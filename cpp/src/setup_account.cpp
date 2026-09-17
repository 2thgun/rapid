// Owner-chosen device access (#23): the setup page sets the local account's
// login/sudo password and optionally adds an SSH public key.
//
// The browser's plaintext password lives only in this request's memory. It is
// hashed here with yescrypt and only the hash is queued, in a 0600 file, for
// the root rapid-account helper, which consumes it immediately. The settings
// database, the result file and the log never see the password or the hash.
#include "rapid/account.hpp"
#include "rapid/setup_auth.hpp"
#include <crypt.h>
#include <fcntl.h>
#include <memory>
#include <openssl/crypto.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rapid::native {
namespace {
constexpr double kAccountWindowSeconds = 600;
constexpr std::size_t kAccountAttempts = 5;

Response json_reply(int code, const Json &body) { return {code, body.dump()}; }

bool header_equal(const Request &request, const std::string &key, const std::string &expected) {
  const auto found = request.headers.find(key);
  return found != request.headers.end() && found->second.size() == expected.size() && !expected.empty() &&
         CRYPTO_memcmp(found->second.data(), expected.data(), expected.size()) == 0;
}

void cleanse(std::string &text) { OPENSSL_cleanse(text.data(), text.size()); }

// yescrypt with a kernel-random salt; sha512-crypt only where libxcrypt lacks it.
std::string hash_account_password(const std::string &password) {
  char setting[CRYPT_GENSALT_OUTPUT_SIZE];
  if (!crypt_gensalt_rn("$y$", 0, nullptr, 0, setting, sizeof setting) &&
      !crypt_gensalt_rn("$6$", 0, nullptr, 0, setting, sizeof setting))
    throw std::runtime_error("password hashing is unavailable");
  auto data = std::make_unique<crypt_data>();
  const char *hashed = crypt_rn(password.c_str(), setting, data.get(), sizeof *data);
  std::string result = hashed && hashed[0] != '*' ? hashed : "";
  OPENSSL_cleanse(data.get(), sizeof *data);
  if (!account::valid_crypt_hash(result)) throw std::runtime_error("password hashing failed");
  return result;
}

// Private to the service user (root reads it regardless) and never a link.
void write_private_request(const fs::path &path, std::string &contents) {
  const auto temporary = path.string() + "." + unique_id() + ".part";
  const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (file < 0) {
    cleanse(contents);
    throw std::runtime_error("cannot queue account request");
  }
  std::size_t written = 0;
  while (written < contents.size()) {
    const auto n = ::write(file, contents.data() + written, contents.size() - written);
    if (n <= 0) break;
    written += static_cast<std::size_t>(n);
  }
  const bool ok = written == contents.size() && ::fchmod(file, 0600) == 0 && ::fsync(file) == 0;
  ::close(file);
  cleanse(contents);
  if (!ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    throw std::runtime_error("cannot queue account request");
  }
}
} // namespace

void SetupAuth::set_account_files(fs::path request_file, fs::path result_file) {
  std::lock_guard lock(mutex_);
  account_request_file_ = std::move(request_file);
  account_result_file_ = std::move(result_file);
}

Response SetupAuth::handle_account(const Request &request, const std::string &,
                                   const std::string &csrf, double time) {
  if (account_request_file_.empty() || account_result_file_.empty())
    return json_reply(503, {{"detail", "device access setup is unavailable"}});
  // The password crosses the network only inside TLS.
  if (!secure_transport_)
    return json_reply(403, {{"detail", "device access can only be changed over HTTPS"}});
  std::error_code error;
  const bool queued = fs::is_regular_file(account_request_file_, error);

  if (request.method == "GET") {
    Json response{{"available", true}, {"queued", queued}};
    try {
      const auto result = Json::parse(read_file(account_result_file_));
      if (result.is_object()) {
        // Allowlist: only status fields ever reach the browser.
        Json status = Json::object();
        for (const char *key : {"request_id", "status", "password_set", "password_changed", "ssh_key", "ssh",
                                "ssh_password_login", "error"})
          if (result.contains(key) && (result[key].is_string() || result[key].is_boolean()))
            status[key] = result[key];
        response["result"] = status;
      }
    } catch (const std::exception &) {}
    return json_reply(200, response);
  }

  if (!header_equal(request, "x-csrf-token", csrf))
    return json_reply(403, {{"detail", "invalid CSRF token"}});
  while (!account_attempts_.empty() && account_attempts_.front() <= time - kAccountWindowSeconds)
    account_attempts_.pop_front();
  if (account_attempts_.size() >= kAccountAttempts)
    return {429, "{\"detail\":\"too many device access changes; try again in a few minutes\"}",
            "application/json", {{"Retry-After", "600"}}};
  account_attempts_.push_back(time);
  if (queued)
    return json_reply(409, {{"detail", "a device access change is still being applied"}});

  auto body = Json::parse(request.body, nullptr, false);
  const auto scrub = [&] {
    if (body.is_object() && body.contains("password") && body["password"].is_string())
      cleanse(body["password"].get_ref<std::string &>());
  };
  const auto reject = [&](const std::string &detail) {
    scrub();
    return json_reply(400, {{"detail", detail}});
  };
  if (!body.is_object() || body.empty()) return reject("password or SSH public key required");
  for (const auto &item : body.items())
    if (item.key() != "password" && item.key() != "ssh_public_key" &&
        item.key() != "replace_existing_password")
      return reject("unexpected field");
  const bool has_password = body.contains("password");
  const bool has_key = body.contains("ssh_public_key");
  if ((has_password && !body["password"].is_string()) || (has_key && !body["ssh_public_key"].is_string()) ||
      (body.contains("replace_existing_password") && !body["replace_existing_password"].is_boolean()))
    return reject("invalid field type");
  if (!has_password && !has_key) return reject("password or SSH public key required");

  Json queued_request{{"request_id", unique_id()}};
  if (has_key) {
    std::string problem;
    const auto key = account::parse_public_key(body["ssh_public_key"].get<std::string>(), &problem);
    if (!key) return reject(problem);
    queued_request["authorized_key"] = key->line();
  }
  if (has_password) {
    auto &password = body["password"].get_ref<std::string &>();
    if (const auto problem = account::password_problem(password); !problem.empty())
      return reject(problem);
    try {
      queued_request["password_hash"] = hash_account_password(password);
    } catch (const std::exception &) {
      scrub();
      return json_reply(503, {{"detail", "password hashing is unavailable"}});
    }
    scrub();
    queued_request["replace_existing_password"] = body.value("replace_existing_password", false);
  }
  const auto id = queued_request["request_id"].get<std::string>();
  auto serialized = queued_request.dump();
  if (queued_request.contains("password_hash"))
    cleanse(queued_request["password_hash"].get_ref<std::string &>());
  try {
    write_private_request(account_request_file_, serialized);
  } catch (const std::exception &) {
    return json_reply(503, {{"detail", "device access queue is unavailable"}});
  }
  return json_reply(202, {{"queued", true}, {"request_id", id}});
}
} // namespace rapid::native
