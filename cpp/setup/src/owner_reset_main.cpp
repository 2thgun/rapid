// rapid-owner-reset: the root helper behind the panel's physical owner reset
// (#44). The touchscreen panel is the trust boundary — anyone standing at the
// device can reopen enrollment — so the trigger is a deliberate hold on the
// panel, and the privileged work runs here, through the same shared
// /run/rapid-apply request/result queue as rapid-apply/wifi/account.
//
// Scope: the owner password hash only. The device identity, paired PCs,
// settings, calibration and device access (SSH) are untouched. The enrollment
// token is reissued so the old one cannot be reused, and the new token reaches
// the panel through the first-boot status document exactly as at first boot.
// The result file carries status only; no password, hash or token travels
// through the queue.
#include "rapid/native.hpp"
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <openssl/crypto.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace rapid::native;

namespace {
struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void require(bool value, const char *message) {
  if (!value) throw Failure(message);
}

void write_public_file(const fs::path &path, const std::string &contents) {
  atomic_file(path, contents);
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                            fs::perms::others_read,
                  fs::perm_options::replace);
}

// Reads the request without following links and consumes it immediately, so a
// stale request is never acted on twice.
std::string consume_request(const fs::path &path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  std::error_code ignored;
  if (descriptor < 0) {
    fs::remove(path, ignored);
    throw Failure("owner reset request is unavailable");
  }
  struct stat info {};
  const bool safe = ::fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1 &&
                    info.st_size > 0 && info.st_size <= 8192;
  std::string contents;
  if (safe) {
    contents.resize(static_cast<std::size_t>(info.st_size));
    std::size_t got = 0;
    while (got < contents.size()) {
      const auto n = ::read(descriptor, contents.data() + got, contents.size() - got);
      if (n <= 0) break;
      got += static_cast<std::size_t>(n);
    }
    contents.resize(got);
  }
  ::close(descriptor);
  fs::remove(path, ignored);
  require(safe && !contents.empty(), "owner reset request is not a private regular file");
  return contents;
}

// Reissues the enrollment token as a private file owned by the setup service
// user (the owner of the state directory), so the unprivileged setup server
// can read and validate it. Returns the new token.
std::string reset_enrollment_token(const fs::path &state_directory) {
  struct stat directory {};
  require(::lstat(state_directory.c_str(), &directory) == 0 && S_ISDIR(directory.st_mode),
          "setup state directory is unavailable");
  const std::string token = unique_id() + unique_id();
  const auto token_file = state_directory / "enrollment.token";
  const auto temporary = token_file.string() + "." + unique_id() + ".part";
  const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  require(descriptor >= 0, "cannot replace the enrollment token");
  const std::string text = token + "\n";
  bool ok = ::write(descriptor, text.data(), text.size()) == static_cast<ssize_t>(text.size());
  if (ok) ok = ::fchmod(descriptor, 0600) == 0;
  if (ok) ok = ::fchown(descriptor, directory.st_uid, directory.st_gid) == 0;
  if (ok) ok = ::fsync(descriptor) == 0;
  require(::close(descriptor) == 0, "cannot replace the enrollment token");
  if (ok) ok = ::rename(temporary.c_str(), token_file.c_str()) == 0;
  if (!ok) {
    ::unlink(temporary.c_str());
    throw Failure("cannot replace the enrollment token");
  }
  sync_file(state_directory);
  return token;
}

// Removes the owner password hash only. Every other table — device identity,
// settings, paired PCs — is left exactly as it was.
bool clear_owner(Database &store) {
  const auto tables = store.query("SELECT name FROM sqlite_master WHERE type='table' AND name='setup_owner'");
  if (tables.empty()) return false;
  store.exec("DELETE FROM setup_owner WHERE id=1");
  return store.query("SELECT changes() AS count")[0]["count"].get<int>() == 1;
}

// Publishes the new token where the panel and the setup page expect it: the
// first-boot status document. The bootstrap address/fingerprint stay; only
// the activation token and the enrollment flags change.
void publish_status(const fs::path &status_file, const std::string &token) {
  Json status = Json::parse(read_file(status_file));
  require(status.is_object(), "first-boot status is unavailable");
  status["owner_configured"] = false;
  status["state"] = "owner_enrollment_required";
  if (!status.contains("bootstrap") || !status["bootstrap"].is_object())
    status["bootstrap"] = Json::object();
  status["bootstrap"]["activation_token"] = token;
  // #31: this file can still hold bootstrap/activation material, so it stays
  // group-rapid readable (never group/other writable) and keeps its owner.
  struct stat info {};
  require(::lstat(status_file.c_str(), &info) == 0, "first-boot status is unavailable");
  atomic_file(status_file, status.dump() + "\n",
              fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
  require(::chown(status_file.c_str(), info.st_uid, info.st_gid) == 0, "cannot publish the enrollment token");
}
} // namespace

int main(int argc, char **argv) {
  std::signal(SIGPIPE, SIG_IGN);
  fs::path request_file, result_file, state_directory = "/var/lib/rapid-setup",
           status_file = "/run/rapid/firstboot.json";
  std::string request_id;
  Json result{{"status", "failed"}};
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "rapid-owner-reset --request-file PATH --result-file PATH [--state-directory PATH] "
                     "[--status-file PATH]\n"
                     "Clears the owner password and reissues the enrollment token for the physical "
                     "panel reset (#44). Never accepts a password; the result carries status only.\n";
        return 0;
      } else if (option == "--request-file" && i + 1 < argc) request_file = argv[++i];
      else if (option == "--result-file" && i + 1 < argc) result_file = argv[++i];
      else if (option == "--state-directory" && i + 1 < argc) state_directory = argv[++i];
      else if (option == "--status-file" && i + 1 < argc) status_file = argv[++i];
      else throw Failure("unknown or incomplete argument; use --help");
    }
    require(!request_file.empty() && !result_file.empty(), "request and result files are required");
    require(!state_directory.empty() && !status_file.empty(), "state directory and status file are required");
    auto contents = consume_request(request_file);
    auto request = Json::parse(contents, nullptr, false);
    OPENSSL_cleanse(contents.data(), contents.size());
    require(request.is_object() && request.contains("request_id") && request["request_id"].is_string(),
            "invalid owner reset request");
    request_id = request["request_id"].get<std::string>();
    require(request_id.size() == 32 && request_id.find_first_not_of("0123456789abcdef") == std::string::npos,
            "invalid owner reset request");
    for (const auto &[key, value] : request.items())
      require(key == "request_id", "invalid owner reset request");
    result["request_id"] = request_id;

    // Validate everything that can fail before mutating anything, so a reset
    // never leaves the device half-reset.
    const auto database_file = state_directory / "setup.db";
    require(fs::is_regular_file(database_file), "setup database is unavailable");
    Json status = Json::parse(read_file(status_file));
    require(status.is_object(), "first-boot status is unavailable");

    // Rotate the token before clearing the owner: if anything later fails, the
    // old password still works and enrollment simply stays closed.
    const auto token = reset_enrollment_token(state_directory);
    bool owner_reset = false;
    {
      Database store(database_file);
      owner_reset = clear_owner(store);
    }
    publish_status(status_file, token);

    result = {{"request_id", request_id},
              {"status", "applied"},
              {"owner_reset", owner_reset},
              {"enrollment", "reopened"}};
    write_public_file(result_file, result.dump() + "\n");
    log(std::string("INFO owner-reset: owner ") + (owner_reset ? "cleared" : "already absent") +
        "; enrollment token reissued");
    return 0;
  } catch (const std::exception &failure) {
    // Messages are fixed strings or filesystem paths; no request content.
    if (!request_file.empty()) {
      std::error_code ignored;
      fs::remove(request_file, ignored);
    }
    if (!result_file.empty()) {
      try {
        Json failed{{"status", "failed"}, {"error", "owner reset could not be applied"}};
        if (!request_id.empty()) failed["request_id"] = request_id;
        write_public_file(result_file, failed.dump() + "\n");
      } catch (const std::exception &) {}
    }
    log(std::string("ERROR owner-reset: ") +
        (dynamic_cast<const Failure *>(&failure) ? failure.what() : "owner reset failed"));
    return 1;
  }
}
