// rapid-account: the root helper that applies owner-chosen device access (#23).
//
// It is deliberately separate from rapid-apply: it needs write access to /etc
// (shadow, sshd drop-in) and to the account's home directory, which the
// hostname/orientation applicator must not have, and its request carries a
// password hash that must not share a queue with display confirmation waits.
//
// It never receives a plaintext password. The setup server hashes the owner's
// password (yescrypt) and queues only the hash; this helper re-validates every
// field because the request directory is writable by the setup service user.
#include "rapid/account.hpp"
#include "rapid/native.hpp"
#include <csignal>
#include <fcntl.h>
#include <grp.h>
#include <iostream>
#include <openssl/crypto.h>
#include <optional>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace rapid::native;
namespace account = rapid::native::account;

namespace {
constexpr const char *kDropInHeader =
    "# Managed by rapid-account (raPId). Rewritten whenever the owner changes device access\n"
    "# on the setup page; local edits here are replaced.\n";

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void require(bool value, const char *message) {
  if (!value) throw Failure(message);
}

struct Account {
  uid_t uid = 0;
  gid_t gid = 0;
  std::string home, shell;
};

std::vector<std::string> split(const std::string &line, char separator) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  for (;;) {
    const auto end = line.find(separator, start);
    fields.push_back(line.substr(start, end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return fields;
}

std::optional<Account> find_account(const fs::path &passwd, const std::string &user) {
  std::istringstream lines(read_file(passwd));
  for (std::string line; std::getline(lines, line);) {
    const auto fields = split(line, ':');
    if (fields.size() != 7 || fields[0] != user) continue;
    try {
      std::size_t used = 0;
      const auto uid = std::stoul(fields[2], &used);
      if (used != fields[2].size()) return std::nullopt;
      const auto gid = std::stoul(fields[3], &used);
      if (used != fields[3].size()) return std::nullopt;
      return Account{static_cast<uid_t>(uid), static_cast<gid_t>(gid), fields[5], fields[6]};
    } catch (const std::exception &) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

// Only the password field's state is kept; the hash itself is never copied out.
bool password_usable(const fs::path &shadow, const std::string &user) {
  auto contents = read_file(shadow);
  bool usable = false;
  std::istringstream lines(contents);
  for (std::string line; std::getline(lines, line);) {
    const auto colon = line.find(':');
    if (colon == std::string::npos || line.substr(0, colon) != user) {
      OPENSSL_cleanse(line.data(), line.size());
      continue;
    }
    const auto end = line.find(':', colon + 1);
    usable = account::shadow_password_usable(line.substr(colon + 1, end - colon - 1));
    OPENSSL_cleanse(line.data(), line.size());
  }
  OPENSSL_cleanse(contents.data(), contents.size());
  return usable;
}

std::vector<char *> argv_of(const fs::path &program, const std::vector<std::string> &arguments) {
  std::vector<char *> argv;
  argv.push_back(const_cast<char *>(program.c_str()));
  for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
  argv.push_back(nullptr);
  return argv;
}

char *const clean_environment[] = {const_cast<char *>("PATH=/usr/sbin:/usr/bin:/sbin:/bin"),
                                   const_cast<char *>("LC_ALL=C"), nullptr};

// Runs a fixed program with a clean environment and silenced output. Optional
// stdin data travels through a pipe, never through argv or the environment.
int run(const fs::path &program, const std::vector<std::string> &arguments,
        const std::string *input = nullptr) {
  int pipe_fds[2] = {-1, -1};
  if (input && ::pipe2(pipe_fds, O_CLOEXEC) != 0) throw Failure("cannot create helper pipe");
  const auto child = fork();
  if (child < 0) throw Failure("cannot start helper command");
  if (child == 0) {
    const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (input) ::dup2(pipe_fds[0], STDIN_FILENO);
    else if (null >= 0) ::dup2(null, STDIN_FILENO);
    if (null >= 0) {
      ::dup2(null, STDOUT_FILENO);
      ::dup2(null, STDERR_FILENO);
    }
    auto argv = argv_of(program, arguments);
    ::execve(program.c_str(), argv.data(), clean_environment);
    _exit(127);
  }
  if (input) {
    ::close(pipe_fds[0]);
    std::size_t written = 0;
    while (written < input->size()) {
      const auto n = ::write(pipe_fds[1], input->data() + written, input->size() - written);
      if (n <= 0) break;
      written += static_cast<std::size_t>(n);
    }
    ::close(pipe_fds[1]);
  }
  int status = 0;
  if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

bool executable(const fs::path &program) {
  std::error_code error;
  return fs::is_regular_file(program, error) && ::access(program.c_str(), X_OK) == 0;
}

// Appends the key as the account itself, so a symlink or hard link planted in
// the (account-writable) home directory can never redirect a root write.
enum class KeyResult { installed, already_present, failed };
KeyResult install_key(const Account &target, const fs::path &home, const std::string &line) {
  const auto child = fork();
  if (child < 0) return KeyResult::failed;
  if (child == 0) {
    if (::geteuid() == 0) {
      if (::setgroups(0, nullptr) != 0 || ::setgid(target.gid) != 0 || ::setuid(target.uid) != 0 ||
          ::getuid() != target.uid || ::geteuid() != target.uid)
        _exit(10);
    } else if (::geteuid() != target.uid) {
      _exit(11);
    }
    ::umask(077);
    const int home_fd = ::open(home.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info {};
    if (home_fd < 0 || ::fstat(home_fd, &info) != 0 || info.st_uid != target.uid) _exit(12);
    if (::mkdirat(home_fd, ".ssh", 0700) != 0 && errno != EEXIST) _exit(13);
    const int ssh_fd = ::openat(home_fd, ".ssh", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (ssh_fd < 0 || ::fstat(ssh_fd, &info) != 0 || info.st_uid != target.uid || ::fchmod(ssh_fd, 0700) != 0)
      _exit(14);
    const int file = ::openat(ssh_fd, "authorized_keys",
                              O_RDWR | O_CREAT | O_APPEND | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (file < 0 || ::fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != target.uid ||
        info.st_nlink != 1 || info.st_size > 1024 * 1024 || ::fchmod(file, 0600) != 0)
      _exit(15);
    std::string existing(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t got = 0;
    while (got < existing.size()) {
      const auto n = ::pread(file, existing.data() + got, existing.size() - got, static_cast<off_t>(got));
      if (n <= 0) _exit(16);
      got += static_cast<std::size_t>(n);
    }
    // The same key (type and blob) counts as present whatever its comment.
    const auto parsed = account::parse_public_key(line);
    if (!parsed) _exit(17);
    std::istringstream lines(existing);
    for (std::string current; std::getline(lines, current);) {
      const auto other = account::parse_public_key(current);
      if (other && other->type == parsed->type && other->blob_base64 == parsed->blob_base64) _exit(1);
    }
    std::string data = (!existing.empty() && existing.back() != '\n' ? "\n" : "") + line + "\n";
    if (::write(file, data.data(), data.size()) != static_cast<ssize_t>(data.size()) || ::fsync(file) != 0)
      _exit(18);
    _exit(0);
  }
  int status = 0;
  if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) return KeyResult::failed;
  if (WEXITSTATUS(status) == 0) return KeyResult::installed;
  if (WEXITSTATUS(status) == 1) return KeyResult::already_present;
  return KeyResult::failed;
}

std::optional<std::string> read_optional(const fs::path &path) {
  std::error_code error;
  if (!fs::exists(fs::symlink_status(path, error))) return std::nullopt;
  return read_file(path);
}

void write_public_file(const fs::path &path, const std::string &contents) {
  atomic_file(path, contents);
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                            fs::perms::others_read,
                  fs::perm_options::replace);
}

// Reads the request without following links and consumes it immediately, so a
// password hash never outlives this process on disk.
std::string consume_request(const fs::path &path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  std::error_code ignored;
  if (descriptor < 0) {
    fs::remove(path, ignored);
    throw Failure("account request is unavailable");
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
  require(safe && !contents.empty(), "account request is not a private regular file");
  return contents;
}
} // namespace

int main(int argc, char **argv) {
  std::signal(SIGPIPE, SIG_IGN);
  fs::path request_file, result_file, root = "/";
  std::string user = "rapid";
  fs::path chpasswd = "/usr/sbin/chpasswd", systemctl = "/usr/bin/systemctl", sshd = "/usr/sbin/sshd",
           ssh_keygen = "/usr/bin/ssh-keygen";
  std::string request_id;
  Json result{{"status", "failed"}};
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "rapid-account --request-file PATH --result-file PATH [--user NAME] [--root PATH] "
                     "[--chpasswd PATH] [--systemctl PATH] [--sshd PATH] [--ssh-keygen PATH]\n"
                     "Applies an owner-chosen password hash and SSH public key for the local account, "
                     "then enables SSH. Never accepts a plaintext password.\n";
        return 0;
      } else if (option == "--request-file" && i + 1 < argc) request_file = argv[++i];
      else if (option == "--result-file" && i + 1 < argc) result_file = argv[++i];
      else if (option == "--user" && i + 1 < argc) user = argv[++i];
      else if (option == "--root" && i + 1 < argc) root = argv[++i];
      else if (option == "--chpasswd" && i + 1 < argc) chpasswd = argv[++i];
      else if (option == "--systemctl" && i + 1 < argc) systemctl = argv[++i];
      else if (option == "--sshd" && i + 1 < argc) sshd = argv[++i];
      else if (option == "--ssh-keygen" && i + 1 < argc) ssh_keygen = argv[++i];
      else throw Failure("unknown or incomplete argument; use --help");
    }
    require(!request_file.empty() && !result_file.empty(), "request and result files are required");
    require(!user.empty() && user.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos,
            "invalid account name");
    auto contents = consume_request(request_file);
    auto request = Json::parse(contents, nullptr, false);
    OPENSSL_cleanse(contents.data(), contents.size());
    require(request.is_object() && request.contains("request_id") && request["request_id"].is_string(),
            "invalid account request");
    request_id = request["request_id"].get<std::string>();
    require(request_id.size() == 32 && request_id.find_first_not_of("0123456789abcdef") == std::string::npos,
            "invalid account request");
    result["request_id"] = request_id;
    for (const auto &[key, value] : request.items())
      require(key == "request_id" || key == "password_hash" || key == "authorized_key" ||
                  key == "replace_existing_password",
              "invalid account request");
    std::string hash, key_line;
    bool replace = false;
    if (request.contains("password_hash")) {
      require(request["password_hash"].is_string(), "invalid account request");
      hash = request["password_hash"].get<std::string>();
      require(account::valid_crypt_hash(hash), "invalid password hash");
    }
    if (request.contains("authorized_key")) {
      require(request["authorized_key"].is_string(), "invalid account request");
      const auto parsed = account::parse_public_key(request["authorized_key"].get<std::string>());
      require(parsed.has_value(), "invalid SSH public key");
      key_line = parsed->line();
    }
    if (request.contains("replace_existing_password")) {
      require(request["replace_existing_password"].is_boolean(), "invalid account request");
      replace = request["replace_existing_password"].get<bool>();
    }
    if (request.contains("password_hash")) {
      auto &stored = request["password_hash"].get_ref<std::string &>();
      OPENSSL_cleanse(stored.data(), stored.size());
    }
    require(!hash.empty() || !key_line.empty(), "account request has nothing to apply");

    const auto target = find_account(root / "etc/passwd", user);
    require(target.has_value(), "account does not exist");
    require(target->uid != 0, "refusing to manage the root account");
    require(!target->shell.ends_with("/nologin") && !target->shell.ends_with("/false") && !target->shell.empty(),
            "account has no login shell");
    require(!target->home.empty() && target->home.front() == '/', "account has no home directory");
    const auto shadow = root / "etc/shadow";
    const bool had_password = password_usable(shadow, user);

    // Dev-Pi safety: an existing password is only replaced when the owner
    // explicitly confirmed it. Nothing at all is changed otherwise.
    if (!hash.empty() && had_password && !replace) {
      OPENSSL_cleanse(hash.data(), hash.size());
      result = {{"request_id", request_id}, {"status", "confirmation_required"}, {"password_set", true}};
      write_public_file(result_file, result.dump() + "\n");
      log("INFO account: an existing password needs explicit confirmation; nothing changed");
      return 0;
    }

    std::string error;
    bool password_changed = false;
    if (!hash.empty()) {
      require(executable(chpasswd), "chpasswd is unavailable");
      auto input = user + ":" + hash + "\n";
      const int code = run(chpasswd, {"-e"}, &input);
      OPENSSL_cleanse(input.data(), input.size());
      OPENSSL_cleanse(hash.data(), hash.size());
      require(code == 0, "password update failed");
      password_changed = true;
    }
    const bool has_password = password_usable(shadow, user);
    require(!password_changed || has_password, "password update was not recorded");

    std::string key_state = "unchanged";
    if (!key_line.empty()) {
      switch (install_key(*target, root / fs::path(target->home).relative_path(), key_line)) {
      case KeyResult::installed: key_state = "installed"; break;
      case KeyResult::already_present: key_state = "already_present"; break;
      case KeyResult::failed: key_state = "failed"; error = "SSH key installation failed"; break;
      }
    }
    const bool key_available = key_state == "installed" || key_state == "already_present";

    // SSH is enabled only once a credential exists. The drop-in never touches
    // public key settings, so key-based access that already works is kept.
    std::string ssh_state = "unchanged";
    if (has_password || key_available) {
      const auto drop_in = root / "etc/ssh/sshd_config.d/10-rapid-owner.conf";
      if (!executable(sshd) || !fs::is_directory(root / "etc/ssh")) {
        ssh_state = "unavailable";
      } else {
        const auto previous = read_optional(drop_in);
        const std::string desired = std::string(kDropInHeader) + "PermitRootLogin no\n"
                                    "PermitEmptyPasswords no\n"
                                    "KbdInteractiveAuthentication no\n"
                                    "PasswordAuthentication " + (has_password ? "yes" : "no") + "\n";
        fs::create_directories(drop_in.parent_path());
        write_public_file(drop_in, desired);
        const auto restore = [&] {
          std::error_code ignored;
          if (previous) write_public_file(drop_in, *previous);
          else fs::remove(drop_in, ignored);
        };
        // sshd -t needs its privilege separation directory, which only exists
        // once ssh has run; otherwise ssh.service validates on start.
        if (fs::is_directory(root / "run/sshd") && run(sshd, {"-t"}) != 0) {
          restore();
          ssh_state = "failed";
          if (error.empty()) error = "SSH configuration was rejected";
        } else if (executable(systemctl) && run(systemctl, {"is-active", "--quiet", "ssh.socket"}) == 0) {
          // Socket activation reads the configuration per connection.
          ssh_state = "enabled";
        } else if (!executable(systemctl)) {
          ssh_state = "unavailable";
        } else {
          // Per-device host keys: the image ships without any.
          if (executable(ssh_keygen)) run(ssh_keygen, {"-A"});
          const bool ok = run(systemctl, {"unmask", "ssh.service"}) == 0 &&
                          run(systemctl, {"enable", "ssh.service"}) == 0 &&
                          run(systemctl, {"reload-or-restart", "ssh.service"}) == 0;
          if (ok) {
            ssh_state = "enabled";
          } else {
            restore();
            run(systemctl, {"reload-or-restart", "ssh.service"});
            ssh_state = "failed";
            if (error.empty()) error = "SSH could not be enabled";
          }
        }
      }
    }
    result = {{"request_id", request_id},
              {"status", error.empty() ? "applied" : "failed"},
              {"password_set", has_password},
              {"password_changed", password_changed},
              {"ssh_key", key_state},
              {"ssh", ssh_state},
              {"ssh_password_login", has_password && ssh_state == "enabled"}};
    if (!error.empty()) result["error"] = error;
    write_public_file(result_file, result.dump() + "\n");
    log(std::string("INFO account: password ") + (password_changed ? "updated" : "unchanged") +
        "; SSH key " + key_state + "; SSH " + ssh_state);
    return error.empty() ? 0 : 1;
  } catch (const std::exception &failure) {
    // Messages are fixed strings or filesystem paths; no request content.
    if (!request_file.empty()) {
      std::error_code ignored;
      fs::remove(request_file, ignored);
    }
    if (!result_file.empty()) {
      try {
        Json failed{{"status", "failed"}, {"error", "device access could not be applied"}};
        if (!request_id.empty()) failed["request_id"] = request_id;
        write_public_file(result_file, failed.dump() + "\n");
      } catch (const std::exception &) {}
    }
    log(std::string("ERROR account: ") +
        (dynamic_cast<const Failure *>(&failure) ? failure.what() : "device access application failed"));
    return 1;
  }
}
