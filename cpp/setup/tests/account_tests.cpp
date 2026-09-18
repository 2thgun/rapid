// #23: owner-chosen device access. Covers the shared validation rules, the
// authenticated setup API (TLS, CSRF, rate limit), the root rapid-account helper
// against a fake root with fake chpasswd/sshd/systemctl, and that the plaintext
// password never lands in any artifact the flow produces, logs included.
#include "rapid/account.hpp"
#include "rapid/setup_auth.hpp"
#include <crypt.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace rapid::native;
namespace account = rapid::native::account;

namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}

struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path() / ("rapid-account-tests-" + unique_id());
  TemporaryDirectory() {
    fs::create_directory(path);
    fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec);
  }
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
  }
};

std::string u32(std::size_t value) {
  return {static_cast<char>((value >> 24) & 0xff), static_cast<char>((value >> 16) & 0xff),
          static_cast<char>((value >> 8) & 0xff), static_cast<char>(value & 0xff)};
}
std::string field(const std::string &value) { return u32(value.size()) + value; }
std::string base64(const std::string &bytes) {
  std::string out(4 * ((bytes.size() + 2) / 3) + 1, '\0');
  const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                                reinterpret_cast<const unsigned char *>(bytes.data()),
                                static_cast<int>(bytes.size()));
  out.resize(static_cast<std::size_t>(n));
  return out;
}
std::string ed25519_key(char fill, const std::string &comment = "owner@laptop") {
  const auto line = "ssh-ed25519 " + base64(field("ssh-ed25519") + field(std::string(32, fill)));
  return comment.empty() ? line : line + " " + comment;
}
std::string rsa_key(std::size_t modulus_bytes) {
  std::string modulus(1, '\0');
  modulus += '\xc5';
  modulus += std::string(modulus_bytes - 1, '\x5a');
  return "ssh-rsa " + base64(field("ssh-rsa") + field(std::string("\x01\x00\x01", 3)) + field(modulus)) +
         " rsa@host";
}
std::string ecdsa_key() {
  std::string point(1, '\x04');
  point += std::string(64, '\x11');
  return "ecdsa-sha2-nistp256 " +
         base64(field("ecdsa-sha2-nistp256") + field("nistp256") + field(point));
}

Request request(const std::string &path, const std::string &method = "GET", const Json &body = {}) {
  return {method, path, method == "GET" ? "" : body.dump(),
          {{"host", "127.0.0.1:8002"}, {"origin", "https://127.0.0.1:8002"},
           {"content-type", "application/json"}}};
}
std::string cookie(const Response &response) {
  for (const auto &[name, value] : response.headers)
    if (name == "Set-Cookie") return value.substr(0, value.find(';'));
  throw std::runtime_error("missing session cookie");
}

void write_text(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}
void write_script(const fs::path &path, const std::string &body) {
  write_text(path, "#!/bin/sh\n" + body);
  fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                            fs::perms::others_read | fs::perms::others_exec);
}
unsigned mode_of(const fs::path &path) {
  struct stat info {};
  require(::lstat(path.c_str(), &info) == 0, "stat " + path.string());
  return info.st_mode & 07777;
}
uid_t owner_of(const fs::path &path) {
  struct stat info {};
  require(::lstat(path.c_str(), &info) == 0, "stat " + path.string());
  return info.st_uid;
}

// A fake device root with the tools the helper drives.
struct FakeDevice {
  fs::path root, tools, log_directory;
  uid_t uid;
  gid_t gid;
  std::string helper;

  FakeDevice(const fs::path &base, std::string helper_path) : helper(std::move(helper_path)) {
    root = base / "device";
    tools = base / "tools";
    log_directory = base / "logs";
    uid = ::geteuid() == 0 ? 4242 : ::geteuid();
    gid = ::geteuid() == 0 ? 4242 : ::getegid();
    fs::create_directories(log_directory);
    reset("rapid:!:19000:0:99999:7:::\n", "/bin/bash");
    write_script(tools / "chpasswd",
                 "echo \"$@\" >> '" + (log_directory / "chpasswd.args").string() + "'\n"
                 "cat > '" + (log_directory / "chpasswd.input").string() + "'\n"
                 "awk -F: -v OFS=: 'NR==FNR { user=$1; hash=substr($0, length(user)+2); next } "
                 "$1==user { $2=hash } { print }' '" + (log_directory / "chpasswd.input").string() + "' '" +
                     (root / "etc/shadow").string() + "' > '" + (root / "etc/shadow.new").string() + "' && "
                 "mv '" + (root / "etc/shadow.new").string() + "' '" + (root / "etc/shadow").string() + "'\n");
    write_script(tools / "systemctl",
                 "echo \"$@\" >> '" + (log_directory / "systemctl.log").string() + "'\n"
                 "if [ \"$1\" = is-active ]; then exit \"$(cat '" + (tools / "socket-active").string() + "')\"; fi\n"
                 "exit 0\n");
    write_script(tools / "sshd",
                 "echo \"$@\" >> '" + (log_directory / "sshd.log").string() + "'\n"
                 "exit \"$(cat '" + (tools / "sshd-exit").string() + "')\"\n");
    write_script(tools / "ssh-keygen", "echo \"$@\" >> '" + (log_directory / "ssh-keygen.log").string() + "'\n");
    write_text(tools / "socket-active", "3\n");
    write_text(tools / "sshd-exit", "0\n");
  }

  void reset(const std::string &shadow, const std::string &shell) {
    std::error_code error;
    fs::remove_all(root, error);
    for (const auto *name : {"chpasswd.args", "chpasswd.input", "systemctl.log", "sshd.log", "ssh-keygen.log"})
      fs::remove(log_directory / name, error);
    write_text(root / "etc/passwd", "root:x:0:0:root:/root:/bin/bash\nrapid:x:" + std::to_string(uid) + ":" +
                                        std::to_string(gid) + ":raPId:/home/rapid:" + shell + "\n");
    write_text(root / "etc/shadow", "root:*:19000:0:99999:7:::\n" + shadow);
    fs::create_directories(root / "etc/ssh/sshd_config.d");
    fs::create_directories(root / "home/rapid");
    fs::create_directories(root / "run/rapid-apply");
    if (::geteuid() == 0) require(::chown((root / "home/rapid").c_str(), uid, gid) == 0, "chown fake home");
  }

  fs::path request_file() const { return root / "run/rapid-apply/account-request.json"; }
  fs::path result_file() const { return root / "run/rapid-apply/account-result.json"; }
  fs::path drop_in() const { return root / "etc/ssh/sshd_config.d/10-rapid-owner.conf"; }
  fs::path keys() const { return root / "home/rapid/.ssh/authorized_keys"; }
  std::string shadow() const { return read_file(root / "etc/shadow"); }

  int run(const std::vector<std::string> &extra = {}) const {
    std::vector<std::string> arguments{helper, "--request-file", request_file().string(), "--result-file",
                                       result_file().string(), "--root", root.string(), "--chpasswd",
                                       (tools / "chpasswd").string(), "--systemctl",
                                       (tools / "systemctl").string(), "--sshd", (tools / "sshd").string(),
                                       "--ssh-keygen", (tools / "ssh-keygen").string()};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    const auto child = fork();
    require(child >= 0, "fork helper");
    if (child == 0) {
      const int out = ::open((log_directory / "helper.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (out >= 0) {
        ::dup2(out, STDOUT_FILENO);
        ::dup2(out, STDERR_FILENO);
      }
      std::vector<char *> argv;
      for (auto &argument : arguments) argv.push_back(argument.data());
      argv.push_back(nullptr);
      ::execv(argv[0], argv.data());
      _exit(127);
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status), "helper exits");
    return WEXITSTATUS(status);
  }
  Json result() const { return Json::parse(read_file(result_file())); }
};

std::string yescrypt(const std::string &password) {
  char setting[CRYPT_GENSALT_OUTPUT_SIZE];
  require(crypt_gensalt_rn("$y$", 0, nullptr, 0, setting, sizeof setting) != nullptr, "yescrypt salt");
  crypt_data data{};
  return crypt_rn(password.c_str(), setting, &data, sizeof data);
}

// Recursively searches every regular file below a directory for a byte string.
std::vector<fs::path> files_containing(const fs::path &directory, const std::string &needle) {
  std::vector<fs::path> found;
  for (const auto &entry : fs::recursive_directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;
    if (read_file(entry.path()).find(needle) != std::string::npos) found.push_back(entry.path());
  }
  return found;
}

void policy_tests() {
  for (const auto *weak : {"short1A!", "aaaaaaaaaaaaaaaa", "abababababab", "lowercaseonly", "Password1234",
                           "myraspberrypi22", "abcdefghijklmnopqrstuvwxyz"})
    require(!account::password_problem(weak).empty(), std::string("weak password rejected: ") + weak);
  require(!account::password_problem("Aa1-" + std::string(125, 'x')).empty(), "overlong password rejected");
  require(!account::password_problem(std::string("Good-pass\n2024")).empty(), "control characters rejected");
  require(!account::password_problem(std::string("Good-pass\x00" "2024x", 14)).empty(), "NUL rejected");
  require(!account::password_problem("Good-pass-\xff\xfe" "2024").empty(), "invalid UTF-8 rejected");
  for (const auto *good : {"Tr4ck-day-at-Spa", "correct horse battery staple", "caf\xc3\xa9-lap-time-9"})
    require(account::password_problem(good).empty(), std::string("acceptable password: ") + good);
  const auto problem = account::password_problem("S3cr3t!");
  require(problem.find("S3cr3t") == std::string::npos, "the reason never echoes the password");

  const auto ed = account::parse_public_key(ed25519_key('\x07') + "\r\n");
  require(ed && ed->type == "ssh-ed25519" && ed->comment == "owner@laptop" &&
              ed->line() == ed25519_key('\x07'),
          "Ed25519 key accepted and normalized");
  require(account::parse_public_key(ed25519_key('\x07', "")).has_value(), "comment is optional");
  require(account::parse_public_key(rsa_key(256)).has_value(), "2048-bit RSA accepted");
  require(account::parse_public_key(ecdsa_key()).has_value(), "ECDSA P-256 accepted");
  std::string reason;
  require(!account::parse_public_key(rsa_key(128), &reason) && !reason.empty(), "1024-bit RSA rejected");
  require(!account::parse_public_key("command=\"sh\" " + ed25519_key('\x07')), "authorized_keys options rejected");
  require(!account::parse_public_key(ed25519_key('\x07') + "\n" + ed25519_key('\x08')), "two keys rejected");
  require(!account::parse_public_key("ssh-dss " + base64(field("ssh-dss") + field("x"))), "DSA rejected");
  require(!account::parse_public_key("ssh-rsa " + base64(field("ssh-ed25519") + field(std::string(32, 'a')))),
          "blob type must match the stated type");
  require(!account::parse_public_key("ssh-ed25519 " + base64(field("ssh-ed25519") + field(std::string(31, 'a')))),
          "truncated Ed25519 key rejected");
  require(!account::parse_public_key("ssh-ed25519 " + base64(field("ssh-ed25519") + field(std::string(32, 'a')) + "x")),
          "trailing blob bytes rejected");
  require(!account::parse_public_key("ssh-ed25519 AAAA@@@@"), "invalid base64 rejected");
  require(!account::parse_public_key(ed25519_key('\x07', "tab\tcomment")), "control characters in comment rejected");

  const auto hash = yescrypt("Tr4ck-day-at-Spa");
  require(account::valid_crypt_hash(hash) && hash.starts_with("$y$"), "yescrypt hash accepted");
  require(account::valid_crypt_hash("$6$saltsalt$" + std::string(86, 'a')), "sha512-crypt accepted");
  require(!account::valid_crypt_hash(hash + "\nroot:" + hash), "hash cannot inject a chpasswd line");
  require(!account::valid_crypt_hash(hash + ":0"), "hash cannot inject a shadow field");
  require(!account::valid_crypt_hash("$1$abcdefgh$abcdefghijklmnopqrstuv"), "MD5-crypt rejected");
  require(!account::valid_crypt_hash("Tr4ck-day-at-Spa-plaintext"), "plaintext is never a hash");
  require(!account::shadow_password_usable("") && !account::shadow_password_usable("!") &&
              !account::shadow_password_usable("*") && !account::shadow_password_usable("!$y$abc") &&
              account::shadow_password_usable(hash),
          "shadow field states");
}

void api_tests(const fs::path &base) {
  SetupStore store(base / "api-state");
  double time = 0;
  const auto queue = base / "api-queue";
  fs::create_directories(queue);
  const auto request_file = queue / "account-request.json";
  const auto result_file = queue / "account-result.json";
  SetupAuth auth(store, 8002, [&] { return time; }, {}, "127.0.0.1", {}, {}, {}, {}, {}, nullptr, true);
  auth.set_account_files(request_file, result_file);
  const std::string owner = "test-only-owner-password";
  require(auth.enroll(owner), "enroll owner");
  auto login = auth.handle(request("/api/v1/auth/login", "POST", {{"password", owner}}));
  require(login.status == 200, "owner signs in over HTTPS");
  auto session = cookie(login);
  auto csrf = Json::parse(login.body)["csrf_token"].get<std::string>();
  const auto authed = [&](Request value) {
    value.headers["cookie"] = session;
    value.headers["x-csrf-token"] = csrf;
    return value;
  };
  const std::string password = "Tr4ck-day-at-Spa";

  require(auth.handle(request("/api/v1/account")).status == 401, "status requires the owner session");
  require(auth.handle(request("/api/v1/account", "POST", {{"password", password}})).status == 401 &&
              !fs::exists(request_file),
          "unauthenticated change rejected");
  auto no_csrf = authed(request("/api/v1/account", "POST", {{"password", password}}));
  no_csrf.headers.erase("x-csrf-token");
  require(auth.handle(no_csrf).status == 403 && !fs::exists(request_file), "change requires the CSRF token");
  auto wrong_csrf = authed(request("/api/v1/account", "POST", {{"password", password}}));
  wrong_csrf.headers["x-csrf-token"] = std::string(64, '0');
  require(auth.handle(wrong_csrf).status == 403, "wrong CSRF token rejected");
  auto cross = authed(request("/api/v1/account", "POST", {{"password", password}}));
  cross.headers["origin"] = "https://attacker.example";
  require(auth.handle(cross).status == 403, "cross-origin change rejected");
  auto form = authed(request("/api/v1/account", "POST", {{"password", password}}));
  form.headers["content-type"] = "application/x-www-form-urlencoded";
  require(auth.handle(form).status == 403, "simple form encoding rejected");
  auto site = authed(request("/api/v1/account", "POST", {{"password", password}}));
  site.headers["sec-fetch-site"] = "cross-site";
  require(auth.handle(site).status == 403, "cross-site fetch metadata rejected");
  require(!fs::exists(request_file), "no rejected request is queued");

  {
    SetupStore plain_store(base / "plain-state");
    SetupAuth plain(plain_store, 8002, [&] { return time; });
    plain.set_account_files(queue / "plain-request.json", queue / "plain-result.json");
    require(plain.enroll(owner), "enroll plain owner");
    auto plain_login = request("/api/v1/auth/login", "POST", {{"password", owner}});
    plain_login.headers["origin"] = "http://127.0.0.1:8002";
    const auto response = plain.handle(plain_login);
    auto change = request("/api/v1/account", "POST", {{"password", password}});
    change.headers["origin"] = "http://127.0.0.1:8002";
    change.headers["cookie"] = cookie(response);
    change.headers["x-csrf-token"] = Json::parse(response.body)["csrf_token"].get<std::string>();
    require(plain.handle(change).status == 403 && !fs::exists(queue / "plain-request.json"),
            "device access is never accepted without TLS");
    SetupAuth unconfigured(store, 8002, [&] { return time; }, {}, "127.0.0.1", {}, {}, {}, {}, {}, nullptr, true);
    const auto other_login = unconfigured.handle(request("/api/v1/auth/login", "POST", {{"password", owner}}));
    auto get = request("/api/v1/account");
    get.headers["cookie"] = cookie(other_login);
    require(unconfigured.handle(get).status == 503, "device access is unavailable unless wired");
  }

  // Validation. Each rejected attempt counts toward the rate limit.
  auto weak = authed(request("/api/v1/account", "POST", {{"password", "Weak1"}}));
  const auto weak_response = auth.handle(weak);
  require(weak_response.status == 400 && weak_response.body.find("Weak1") == std::string::npos &&
              !fs::exists(request_file),
          "weak password rejected server-side without echoing it");
  auto unknown = authed(request("/api/v1/account", "POST", {{"password", password}, {"user", "root"}}));
  require(auth.handle(unknown).status == 400 && !fs::exists(request_file), "unexpected fields rejected");
  auto bad_key = authed(request("/api/v1/account", "POST", {{"ssh_public_key", "command=\"sh\" " + ed25519_key(1)}}));
  require(auth.handle(bad_key).status == 400 && !fs::exists(request_file), "key options rejected");
  auto wrong_type = authed(request("/api/v1/account", "POST", {{"password", password}, {"replace_existing_password", "yes"}}));
  require(auth.handle(wrong_type).status == 400, "confirmation must be a boolean");

  auto oversized = authed(request("/api/v1/account", "POST", {{"ssh_public_key", std::string(4100, 'a')}}));
  require(auth.handle(oversized).status == 413, "account body is bounded");

  // A valid change: hash only, private request file.
  time = 1000;
  const auto key = rsa_key(1024);
  auto change = authed(request("/api/v1/account", "POST",
                               {{"password", password}, {"ssh_public_key", key + "\n"},
                                {"replace_existing_password", true}}));
  require(change.body.size() > 1024, "a large RSA key and password fit the account body limit");
  const auto accepted = auth.handle(change);
  require(accepted.status == 202, "valid device access change is queued");
  const auto id = Json::parse(accepted.body)["request_id"].get<std::string>();
  require(fs::is_regular_file(request_file) && mode_of(request_file) == 0600, "request file is private (0600)");
  const auto contents = read_file(request_file);
  require(contents.find(password) == std::string::npos, "request never contains the plaintext password");
  const auto queued = Json::parse(contents);
  require(queued.size() == 4 && queued["request_id"] == id && queued["authorized_key"] == key &&
              queued["replace_existing_password"] == true,
          "request carries id, normalized key and explicit confirmation");
  const auto hash = queued["password_hash"].get<std::string>();
  crypt_data data{};
  require(hash.starts_with("$y$") && std::string(crypt_rn(password.c_str(), hash.c_str(), &data, sizeof data)) == hash,
          "queued yescrypt hash verifies the chosen password");
  require(auth.handle(change).status == 409, "a second change waits for the helper");
  auto status = Json::parse(auth.handle(authed(request("/api/v1/account"))).body);
  require(status["queued"] == true && !status.contains("result"), "status reports the queued change");

  atomic_file(result_file, Json{{"request_id", id}, {"status", "applied"}, {"password_set", true},
                                {"ssh", "enabled"}, {"password_hash", hash}, {"extra", {{"x", 1}}}}.dump());
  fs::remove(request_file);
  status = Json::parse(auth.handle(authed(request("/api/v1/account"))).body);
  require(status["queued"] == false && status["result"]["status"] == "applied" &&
              status["result"]["ssh"] == "enabled" && !status["result"].contains("password_hash") &&
              !status["result"].contains("extra"),
          "only allowlisted status fields reach the browser");

  // Rate limit: 5 attempts in 10 minutes, counted after authentication and CSRF.
  time = 5000;
  login = auth.handle(request("/api/v1/auth/login", "POST", {{"password", owner}}));
  session = cookie(login);
  csrf = Json::parse(login.body)["csrf_token"].get<std::string>();
  auto key_only = authed(request("/api/v1/account", "POST", {{"ssh_public_key", ed25519_key(2)}}));
  int accepted_count = 0;
  for (int i = 0; i < 5; ++i) {
    const auto response = auth.handle(key_only);
    require(response.status == 202 || response.status == 409,
            "attempt within limit: " + std::to_string(response.status) + " " + response.body);
    accepted_count += response.status == 202;
    fs::remove(request_file);
  }
  require(accepted_count == 5, "five changes are allowed in the window");
  const auto limited = auth.handle(key_only);
  require(limited.status == 429 && !fs::exists(request_file), "sixth change in the window is rate limited");
  no_csrf.headers["cookie"] = session;
  require(auth.handle(no_csrf).status == 403, "requests without CSRF are rejected before the limiter");
  time = 5601;
  require(auth.handle(key_only).status == 202, "the limit window expires");
  fs::remove(request_file);
}

void helper_tests(const fs::path &base, const std::string &helper) {
  FakeDevice device(base, helper);
  const std::string password = "Tr4ck-day-at-Spa";
  const auto hash = yescrypt(password);
  const auto id = unique_id();
  const auto queue = [&](const Json &request) { write_text(device.request_file(), request.dump()); };

  // 1. Fresh image: locked account, password + key.
  queue({{"request_id", id}, {"password_hash", hash}, {"authorized_key", ed25519_key(3)},
         {"replace_existing_password", false}});
  require(device.run() == 0, "helper applies a fresh device's access");
  require(!fs::exists(device.request_file()), "request consumed");
  auto result = device.result();
  require(result["request_id"] == id && result["status"] == "applied" && result["password_set"] == true &&
              result["password_changed"] == true && result["ssh_key"] == "installed" && result["ssh"] == "enabled" &&
              result["ssh_password_login"] == true,
          "result reports status only: " + result.dump());
  require(read_file(device.log_directory / "chpasswd.input") == "rapid:" + hash + "\n" &&
              read_file(device.log_directory / "chpasswd.args") == "-e\n",
          "chpasswd -e receives exactly one user:hash line on stdin");
  require(device.shadow().find("rapid:" + hash + ":19000:") != std::string::npos &&
              device.shadow().starts_with("root:*:"),
          "shadow updated for the account only");
  require(read_file(device.drop_in()) ==
              "# Managed by rapid-account (raPId). Rewritten whenever the owner changes device access\n"
              "# on the setup page; local edits here are replaced.\n"
              "PermitRootLogin no\nPermitEmptyPasswords no\nKbdInteractiveAuthentication no\n"
              "PasswordAuthentication yes\n",
          "sshd drop-in enables password login only now that a password exists");
  require(mode_of(device.drop_in()) == 0644, "drop-in mode 0644");
  require(read_file(device.keys()) == ed25519_key(3) + "\n", "authorized_keys holds the key");
  require(mode_of(device.keys()) == 0600 && mode_of(device.keys().parent_path()) == 0700 &&
              owner_of(device.keys()) == device.uid && owner_of(device.keys().parent_path()) == device.uid,
          "authorized_keys 0600 and .ssh 0700, owned by the account");
  require(read_file(device.log_directory / "systemctl.log") ==
              "is-active --quiet ssh.socket\nunmask ssh.service\nenable ssh.service\nreload-or-restart ssh.service\n",
          "SSH is unmasked, enabled and started once a credential exists");
  require(read_file(device.log_directory / "ssh-keygen.log") == "-A\n", "per-device host keys generated");
  require(!fs::exists(device.log_directory / "sshd.log"), "sshd -t waits for ssh.service when never started");

  // 2. Dev Pi: an existing password is not replaced without confirmation.
  device.reset("rapid:" + yescrypt("Existing-dev-pass1") + ":19000:0:99999:7:::\n", "/bin/bash");
  const auto before = device.shadow();
  queue({{"request_id", id}, {"password_hash", hash}, {"authorized_key", ed25519_key(4)}});
  require(device.run() == 0, "confirmation-required is not a helper failure");
  result = device.result();
  require(result["status"] == "confirmation_required" && result["password_set"] == true &&
              device.shadow() == before && !fs::exists(device.log_directory / "chpasswd.input") &&
              !fs::exists(device.keys()) && !fs::exists(device.drop_in()) &&
              !fs::exists(device.log_directory / "systemctl.log"),
          "without confirmation nothing at all changes");
  queue({{"request_id", id}, {"password_hash", hash}, {"replace_existing_password", true}});
  require(device.run() == 0 && device.result()["status"] == "applied" &&
              device.shadow().find("rapid:" + hash + ":") != std::string::npos,
          "explicit confirmation replaces an existing password");

  // 3. Key only on an account without a password; existing keys are kept.
  device.reset("rapid:*:19000:0:99999:7:::\n", "/bin/bash");
  fs::create_directories(device.keys().parent_path());
  write_text(device.keys(), ed25519_key(5, "existing-dev-key"));
  if (::geteuid() == 0) {
    require(::chown(device.keys().parent_path().c_str(), device.uid, device.gid) == 0 &&
                ::chown(device.keys().c_str(), device.uid, device.gid) == 0,
            "chown existing keys");
  }
  write_text(device.tools / "socket-active", "0\n");
  fs::create_directories(device.root / "run/sshd");
  queue({{"request_id", id}, {"authorized_key", ed25519_key(6)}});
  require(device.run() == 0, "key-only request applies");
  result = device.result();
  require(result["password_set"] == false && result["password_changed"] == false &&
              result["ssh_key"] == "installed" && result["ssh"] == "enabled" && result["ssh_password_login"] == false &&
              !fs::exists(device.log_directory / "chpasswd.input"),
          "key-only request never touches the password: " + result.dump());
  require(read_file(device.drop_in()).ends_with("PasswordAuthentication no\n"),
          "password login stays disabled while no password exists");
  require(read_file(device.keys()) == ed25519_key(5, "existing-dev-key") + "\n" + ed25519_key(6) + "\n",
          "existing key-based access is preserved");
  require(read_file(device.log_directory / "sshd.log") == "-t\n" &&
              read_file(device.log_directory / "systemctl.log") == "is-active --quiet ssh.socket\n",
          "socket-activated SSH is validated and left running as is");
  queue({{"request_id", id}, {"authorized_key", ed25519_key(6, "renamed")}});
  require(device.run() == 0 && device.result()["ssh_key"] == "already_present" &&
              read_file(device.keys()) == ed25519_key(5, "existing-dev-key") + "\n" + ed25519_key(6) + "\n",
          "the same key is not added twice");

  // 4. sshd rejects the configuration: the previous drop-in is restored.
  write_text(device.drop_in(), "# previous\nPasswordAuthentication no\n");
  write_text(device.tools / "sshd-exit", "1\n");
  write_text(device.tools / "socket-active", "3\n");
  fs::remove(device.log_directory / "systemctl.log");
  queue({{"request_id", id}, {"password_hash", hash}});
  require(device.run() == 1, "rejected SSH configuration is a failure");
  result = device.result();
  require(result["status"] == "failed" && result["ssh"] == "failed" && result["password_set"] == true &&
              read_file(device.drop_in()) == "# previous\nPasswordAuthentication no\n" &&
              !fs::exists(device.log_directory / "systemctl.log"),
          "rejected drop-in rolled back without reloading SSH");
  write_text(device.tools / "sshd-exit", "0\n");

  // 5. Links planted in the account-writable home cannot redirect the write.
  device.reset("rapid:!:19000:0:99999:7:::\n", "/bin/bash");
  const auto victim = base / "victim";
  // The link target is writable by the account, so only O_NOFOLLOW (not the
  // privilege drop, which separately protects root-owned targets) stops it.
  write_text(victim, "link target content\n");
  if (::geteuid() == 0) require(::chown(victim.c_str(), device.uid, device.gid) == 0, "chown link target");
  fs::create_directories(device.keys().parent_path());
  fs::create_symlink(victim, device.keys());
  if (::geteuid() == 0)
    require(::lchown(device.keys().parent_path().c_str(), device.uid, device.gid) == 0, "chown .ssh");
  queue({{"request_id", id}, {"authorized_key", ed25519_key(7)}});
  require(device.run() == 1 && device.result()["ssh_key"] == "failed" &&
              read_file(victim) == "link target content\n",
          "authorized_keys symlink is refused");
  // A root-owned target is additionally protected by writing as the account.
  const auto root_victim = base / "root-victim";
  write_text(root_victim, "root content\n");
  fs::remove(device.keys());
  fs::create_hard_link(root_victim, device.keys());
  queue({{"request_id", id}, {"authorized_key", ed25519_key(7)}});
  require(device.run() == 1 && read_file(root_victim) == "root content\n", "authorized_keys hard link is refused");
  device.reset("rapid:!:19000:0:99999:7:::\n", "/bin/bash");
  fs::remove_all(device.root / "home/rapid");
  fs::create_directory_symlink(base / "victim-home", device.root / "home/rapid");
  fs::create_directories(base / "victim-home");
  queue({{"request_id", id}, {"authorized_key", ed25519_key(7)}});
  require(device.run() == 1 && !fs::exists(base / "victim-home/.ssh"), "home directory symlink is refused");

  // 6. Invalid or hostile requests change nothing and are consumed.
  const auto rejected = [&](const std::string &what, const std::string &text, std::vector<std::string> extra = {},
                            const std::string &shell = "/bin/bash") {
    device.reset("rapid:!:19000:0:99999:7:::\n", shell);
    const auto shadow = device.shadow();
    write_text(device.request_file(), text);
    require(device.run(extra) == 1, what + ": helper fails");
    require(!fs::exists(device.request_file()), what + ": request consumed");
    require(device.shadow() == shadow && !fs::exists(device.log_directory / "chpasswd.input") &&
                !fs::exists(device.drop_in()) && !fs::exists(device.log_directory / "systemctl.log"),
            what + ": nothing changed");
    require(device.result()["status"] == "failed", what + ": failure recorded");
  };
  rejected("chpasswd line injection", Json{{"request_id", id}, {"password_hash", hash + "\nroot:" + hash}}.dump());
  rejected("plaintext password field", Json{{"request_id", id}, {"password", password}}.dump());
  rejected("malformed JSON", "{\"request_id\": \"" + id + "\", \"password_hash\": ");
  rejected("empty request", Json{{"request_id", id}}.dump());
  rejected("bad request id", Json{{"request_id", "../x"}, {"password_hash", hash}}.dump());
  rejected("key with options", Json{{"request_id", id}, {"authorized_key", "from=\"*\" " + ed25519_key(8)}}.dump());
  rejected("root account", Json{{"request_id", id}, {"password_hash", hash}}.dump(), {"--user", "root"});
  rejected("service account without login shell", Json{{"request_id", id}, {"password_hash", hash}}.dump(), {},
           "/usr/sbin/nologin");
  device.reset("rapid:!:19000:0:99999:7:::\n", "/bin/bash");
  write_text(base / "linked-request.json", Json{{"request_id", id}, {"password_hash", hash}}.dump());
  fs::create_symlink(base / "linked-request.json", device.request_file());
  require(device.run() == 1 && !fs::exists(device.log_directory / "chpasswd.input") &&
              fs::exists(base / "linked-request.json"),
          "a symlinked request is refused without following it");
}

// #22 follow-up for f5-open-ap: the public setup status names the AP in use.
void network_ssid_tests(const fs::path &base) {
  SetupStore store(base / "ssid-state");
  const auto ssid_file = base / "network-ssid";
  SetupAuth auth(store, 8002, monotonic, {}, "127.0.0.1", {}, {}, {}, {}, {}, nullptr, true);
  const auto status = [&] { return Json::parse(auth.handle(request("/api/v1/setup")).body); };
  require(!status().contains("network_ssid"), "no SSID without a configured file");
  auth.set_network_ssid_file(ssid_file);
  require(!status().contains("network_ssid"), "no SSID before rapid-provision chose one");
  for (const auto *valid : {"rapid", "rapid-0427"}) {
    write_text(ssid_file, std::string(valid) + "\n");
    require(status()["network_ssid"] == valid, std::string("setup status publishes SSID ") + valid);
  }
  for (const auto *invalid : {"", "rapid-12", "rapid-abcd", "evil<script>", "rapid-04271"}) {
    write_text(ssid_file, std::string(invalid) + "\n");
    require(!status().contains("network_ssid"), std::string("invalid SSID omitted: ") + invalid);
  }
  write_text(ssid_file, "rapid\n");
  const auto response = auth.handle(request("/api/v1/setup"));
  require(response.status == 200 && Json::parse(response.body)["network_ssid"] == "rapid",
          "the SSID is public setup status (no session needed)");
}

// The complete flow: browser → setup server → request → helper → result.
void secret_tests(const fs::path &base, const std::string &helper) {
  const auto flow = base / "flow";
  fs::create_directories(flow);
  FakeDevice device(flow, helper);
  const std::string password = "Unique-" + unique_id().substr(0, 12) + "-Pw";
  const auto server_log = flow / "logs/setup-server.log";
  std::ofstream server_log_stream(server_log);
  auto *previous = std::cerr.rdbuf(server_log_stream.rdbuf());
  std::string hash;
  {
    SetupStore store(flow / "setup-state");
    SetupAuth auth(store, 8002, monotonic, {}, "127.0.0.1", {}, {}, {}, {}, {}, nullptr, true);
    auth.set_account_files(device.request_file(), device.result_file());
    require(auth.enroll("test-only-owner-password"), "enroll");
    const auto login = auth.handle(request("/api/v1/auth/login", "POST", {{"password", "test-only-owner-password"}}));
    auto change = request("/api/v1/account", "POST", {{"password", password}, {"ssh_public_key", ed25519_key(9)}});
    change.headers["cookie"] = cookie(login);
    change.headers["x-csrf-token"] = Json::parse(login.body)["csrf_token"].get<std::string>();
    auto weak = change;
    weak.body = Json{{"password", password.substr(0, 8)}}.dump();
    const auto weak_response = auth.handle(weak);
    server_log_stream << weak_response.body << "\n";
    const auto response = auth.handle(change);
    server_log_stream << response.body << "\n";
    require(response.status == 202, "flow change queued");
    hash = Json::parse(read_file(device.request_file()))["password_hash"].get<std::string>();
    require(device.run() == 0, "flow helper applies");
    auto status = request("/api/v1/account");
    status.headers["cookie"] = change.headers["cookie"];
    const auto status_response = auth.handle(status);
    server_log_stream << status_response.body << "\n";
    require(Json::parse(status_response.body)["result"]["status"] == "applied", "flow result visible");
    log("INFO test: flow complete");
  }
  std::cerr.rdbuf(previous);
  server_log_stream.close();
  require(fs::file_size(server_log) > 0 && fs::file_size(device.log_directory / "helper.log") > 0,
          "server and helper logs were captured");
  const auto plaintext = files_containing(base, password);
  std::string listing;
  for (const auto &path : plaintext) listing += " " + path.string();
  require(plaintext.empty(), "the plaintext password persists in:" + listing);
  require(files_containing(base, password.substr(0, 8)).empty(), "not even a password prefix persists");
  // The hash may exist only in the (fake) shadow file and the fake chpasswd's
  // own capture of its stdin; never in results, logs, the drop-in or the DB.
  for (const auto &path : files_containing(flow, hash))
    require(path == device.root / "etc/shadow" || path == device.log_directory / "chpasswd.input",
            "password hash persisted outside shadow: " + path.string());
  require(!fs::exists(device.request_file()), "flow request removed");
}

// #28: browser-generated key enrollment. It reuses #23's authenticated,
// CSRF-protected, rate-limited /api/v1/account endpoint (setup_account.cpp) and
// its rapid-account helper. These tests pin that only an OpenSSH public-key
// line is accepted and that no field can carry private-key material.
void ssh_key_enrollment_tests(const fs::path &base) {
  SetupStore store(base / "sshkey-state");
  const auto queue = base / "sshkey-queue";
  fs::create_directories(queue);
  const auto request_file = queue / "account-request.json";
  const auto result_file = queue / "account-result.json";
  double time = 0;
  SetupAuth auth(store, 8002, [&] { return time; }, {}, "127.0.0.1", {}, {}, {}, {}, {}, nullptr, true);
  auth.set_account_files(request_file, result_file);
  const std::string owner = "test-only-owner-password";
  require(auth.enroll(owner), "enroll owner");
  std::string session, csrf;
  const auto sign_in = [&] {
    const auto login = auth.handle(request("/api/v1/auth/login", "POST", {{"password", owner}}));
    require(login.status == 200, "owner signs in");
    session = cookie(login);
    csrf = Json::parse(login.body)["csrf_token"].get<std::string>();
  };
  sign_in();
  const auto authed = [&](const Json &body) {
    auto value = request("/api/v1/account", "POST", body);
    value.headers["cookie"] = session;
    value.headers["x-csrf-token"] = csrf;
    return value;
  };
  const auto post = [&](const Json &body) { return auth.handle(authed(body)); };
  // Every rejected attempt consumes a rate-limit slot, so give each check its
  // own window; otherwise a later check would be hidden behind a 429.
  const auto next_window = [&] { time += 601; sign_in(); };

  // Authentication and CSRF, the same rules as the password path.
  require(auth.handle(request("/api/v1/account", "POST", {{"ssh_public_key", ed25519_key(10)}})).status == 401 &&
              !fs::exists(request_file),
          "an unauthenticated key is refused");
  auto no_csrf = authed({{"ssh_public_key", ed25519_key(10)}});
  no_csrf.headers.erase("x-csrf-token");
  require(auth.handle(no_csrf).status == 403, "a key needs the CSRF token");
  auto wrong_csrf = authed({{"ssh_public_key", ed25519_key(10)}});
  wrong_csrf.headers["x-csrf-token"] = std::string(64, '0');
  require(auth.handle(wrong_csrf).status == 403, "a key with a wrong CSRF token is refused");
  require(!fs::exists(request_file), "no rejected key is queued");

  // The endpoint schema has no field that could carry private-key material:
  // anything outside {password, ssh_public_key, replace_existing_password} is
  // rejected, so the private half cannot even be expressed.
  for (const char *field : {"private_key", "ssh_private_key", "private_key_pem", "seed",
                            "passphrase", "public_key", "authorized_keys"}) {
    next_window();
    Json body;
    body[field] = "-----BEGIN OPENSSH PRIVATE KEY-----";
    const auto response = post(body);
    require(response.status == 400 && !fs::exists(request_file),
            std::string("no request field may carry key material: ") + field);
  }

  // Malformed, wrong-type and oversized keys are refused without queueing.
  next_window();
  require(post({{"ssh_public_key", "not a key"}}).status == 400, "a malformed key is refused");
  next_window();
  require(post({{"ssh_public_key", "ssh-dss " + base64(field("ssh-dss") + field("x"))}}).status == 400,
          "a DSA key is refused");
  next_window();
  require(post({{"ssh_public_key",
                 "ssh-rsa " + base64(field("ssh-ed25519") + field(std::string(32, 'a')))}})
              .status == 400,
          "a key whose blob does not match its type is refused");
  next_window();
  require(post({{"ssh_public_key", std::string(3200, 'a')}}).status == 400,
          "an oversized key line is refused");
  next_window();
  const auto private_paste = post({{"ssh_public_key", "-----BEGIN OPENSSH PRIVATE KEY-----\nAAAA\n"
                                                    "-----END OPENSSH PRIVATE KEY-----"}});
  require(private_paste.status == 400, "a private key cannot be pasted into the public-key field");
  next_window();
  require(post({{"ssh_public_key", std::string(4100, 'a')}}).status == 413,
          "an oversized account body is refused");
  require(!fs::exists(request_file), "no rejected key is queued");

  // A browser-generated Ed25519 key (the #28 path) is accepted and queued with
  // exactly the normalized public line and nothing else.
  const auto generated = ed25519_key('\x2a', "rapid@laptop");
  const auto accepted = post({{"ssh_public_key", generated}});
  require(accepted.status == 202 && fs::is_regular_file(request_file) && mode_of(request_file) == 0600,
          "a generated Ed25519 key is queued in a 0600 request");
  const auto contents = read_file(request_file);
  require(contents.find("PRIVATE KEY") == std::string::npos, "the request has no private material");
  const auto queued = Json::parse(contents);
  require(queued.size() == 2 && queued["authorized_key"] == generated && queued["request_id"].is_string(),
          "the request holds only the id and the normalized public key");
  require(post({{"ssh_public_key", ed25519_key(11)}}).status == 409,
          "a second key waits for the helper");
  fs::remove(request_file);

  // A generated ECDSA P-256 key (the documented fallback) is accepted too.
  const auto ecdsa = ecdsa_key();
  require(post({{"ssh_public_key", ecdsa}}).status == 202 &&
              Json::parse(read_file(request_file))["authorized_key"] == ecdsa,
          "a generated ECDSA P-256 key is accepted");
  fs::remove(request_file);
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "usage: rapid-account-tests PATH-TO-rapid-account");
    TemporaryDirectory root;
    policy_tests();
    std::cout << "ok: password, SSH key and hash validation\n";
    api_tests(root.path);
    std::cout << "ok: setup API auth, TLS, CSRF, validation and rate limit\n";
    network_ssid_tests(root.path);
    std::cout << "ok: setup status publishes the setup AP name in use\n";
    ssh_key_enrollment_tests(root.path);
    std::cout << "ok: browser-generated SSH key enrollment reuses the account endpoint\n";
    helper_tests(root.path / "helper", fs::absolute(argv[1]).string());
    std::cout << "ok: rapid-account helper against a fake root\n";
    secret_tests(root.path / "secret", fs::absolute(argv[1]).string());
    std::cout << "ok: plaintext password persists in no artifact\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << "\n";
    return 1;
  }
}
