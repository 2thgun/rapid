#include "rapid/native.hpp"
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <sys/wait.h>
#include <unistd.h>

using namespace rapid::native;
namespace {
void require(bool value, const char *message) {
  if (!value) throw std::invalid_argument(message);
}

bool printable(const std::string &text) {
  for (unsigned char c : text)
    if (c < 0x20 || c == 0x7f) return false;
  return true;
}

bool hexadecimal(const std::string &text) {
  return text.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

void run(const fs::path &program, const std::vector<std::string> &arguments, bool required = true) {
  const auto child = fork();
  if (child < 0) throw std::runtime_error("cannot start NetworkManager command");
  if (child == 0) {
    const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null >= 0) { dup2(null, STDOUT_FILENO); dup2(null, STDERR_FILENO); if (null > 2) ::close(null); }
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char *>(program.c_str()));
    for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    execv(program.c_str(), argv.data());
    _exit(127);
  }
  int status = 0;
  const bool succeeded = waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (required && !succeeded)
    throw std::runtime_error("NetworkManager command failed");
}

// #26: GLib key-file values need escaping only for a literal backslash; ssid
// and password are already restricted to printable, non-control characters
// (validated in main() before either ever reaches this function), so that is
// the only byte that would otherwise change the value NetworkManager parses
// back out of the file.
std::string escape_keyfile_value(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char c : value) {
    if (c == '\\') escaped += '\\';
    escaped += c;
  }
  return escaped;
}

// Not a secret: only needs to be a stable, syntactically valid identifier for
// the one profile this program owns.
std::string connection_uuid() {
  const auto id = unique_id();
  return id.substr(0, 8) + "-" + id.substr(8, 4) + "-" + id.substr(12, 4) + "-" +
         id.substr(16, 4) + "-" + id.substr(20, 12);
}

// #26: writes the NetworkManager keyfile connection profile for the Home
// network directly and atomically, mode 0600, so the passphrase never
// becomes an nmcli command-line argument -- readable by any other local
// process via /proc/<pid>/cmdline for as long as nmcli runs. Once the
// profile is active NetworkManager keeps the same secret in the same file at
// the same permissions anyway (that is how nmcli's own "connection modify
// ... wifi-sec.psk" ended up persisting it before); this only changes how
// the secret gets onto disk, never whether it ends up there.
void write_home_connection(const fs::path &directory, const std::string &ssid,
                           const std::string &password) {
  fs::create_directories(directory);
  const std::string content =
      "[connection]\n"
      "id=rapid-home\n"
      "uuid=" + connection_uuid() + "\n"
      "type=wifi\n"
      "interface-name=wlan0\n"
      "autoconnect=true\n"
      "autoconnect-priority=100\n"
      "\n"
      "[wifi]\n"
      "mode=infrastructure\n"
      "ssid=" + escape_keyfile_value(ssid) + "\n"
      "\n"
      "[wifi-security]\n"
      "key-mgmt=wpa-psk\n"
      "psk=" + escape_keyfile_value(password) + "\n"
      "\n"
      "[ipv4]\n"
      "method=auto\n"
      "\n"
      "[ipv6]\n"
      "method=auto\n"
      "addr-gen-mode=default\n";
  const auto path = directory / "rapid-home.nmconnection";
  auto temp = path;
  temp += "." + unique_id() + ".part";
  // Mode 0600 from creation (not chmod afterwards): the secret is never on
  // disk at a wider permission, even for an instant. 0600 has no group/other
  // bits, so umask cannot widen it.
  const int descriptor = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0) throw std::runtime_error("cannot create NetworkManager connection file");
  bool ok = true;
  for (std::size_t written = 0; ok && written < content.size();) {
    const auto sent = ::write(descriptor, content.data() + written, content.size() - written);
    if (sent <= 0) { ok = false; break; }
    written += static_cast<std::size_t>(sent);
  }
  if (ok) ok = ::fsync(descriptor) == 0;
  if (::close(descriptor) != 0) ok = false;
  if (!ok) {
    std::error_code ignored;
    fs::remove(temp, ignored);
    throw std::runtime_error("cannot write NetworkManager connection file");
  }
  std::error_code rename_error;
  fs::rename(temp, path, rename_error);
  if (rename_error) {
    std::error_code ignored;
    fs::remove(temp, ignored);
    throw std::runtime_error("cannot install NetworkManager connection file");
  }
  sync_file(directory);
}
} // namespace

int main(int argc, char **argv) {
  fs::path request_file, result_file;
  fs::path nmcli = "/usr/bin/nmcli";
  fs::path connection_directory = "/etc/NetworkManager/system-connections";
  std::optional<std::int64_t> revision;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--request-file" && i + 1 < argc) request_file = argv[++i];
      else if (option == "--result-file" && i + 1 < argc) result_file = argv[++i];
      else if (option == "--nmcli" && i + 1 < argc) nmcli = argv[++i];
      else if (option == "--connection-directory" && i + 1 < argc) connection_directory = argv[++i];
      else throw std::invalid_argument(
          "use --request-file PATH --result-file PATH [--nmcli PATH] [--connection-directory PATH]");
    }
    require(!request_file.empty() && !result_file.empty(), "request and result files are required");
    Json request;
    try { request = Json::parse(read_file(request_file)); }
    catch (const std::exception &) {
      // nlohmann's own parse-error message can quote a fragment of the
      // offending bytes; never let that (or the raw file content) reach the
      // log via a rethrown message (#26).
      throw std::invalid_argument("invalid Wi-Fi request");
    }
    require(request.is_object() && request.size() == 3 && request.contains("revision") &&
                request["revision"].is_number_integer() && request.contains("ssid") &&
                request["ssid"].is_string() && request.contains("password") && request["password"].is_string(),
            "invalid Wi-Fi request");
    revision = request["revision"].get<std::int64_t>();
    const auto ssid = request["ssid"].get<std::string>(), password = request["password"].get<std::string>();
    require(!ssid.empty() && ssid.size() <= 32 && printable(ssid), "invalid Wi-Fi SSID");
    require(password.size() >= 8 && printable(password) &&
                (password.size() <= 63 || (password.size() == 64 && hexadecimal(password))),
            "invalid Wi-Fi password");
    require(fs::is_regular_file(nmcli) && ::access(nmcli.c_str(), X_OK) == 0, "nmcli is unavailable");
    // Recreate one fixed profile. No browser-provided profile name or
    // command is used. The passphrase reaches NetworkManager only through a
    // private keyfile this process writes directly (never as an nmcli
    // argument) plus "connection load", which takes just a path (#26).
    run(nmcli, {"connection", "delete", "rapid-home"}, false);
    write_home_connection(connection_directory, ssid, password);
    run(nmcli, {"connection", "load", (connection_directory / "rapid-home.nmconnection").string()});
    run(nmcli, {"-w", "30", "connection", "up", "rapid-home", "ifname", "wlan0"});
    atomic_file(result_file, Json{{"revision", *revision}, {"connected", true}}.dump() + "\n");
    fs::remove(request_file);
    return 0;
  } catch (const std::exception &error) {
    // A failed join must not strand the setup browser on a keyboardless device.
    try { if (fs::is_regular_file(nmcli) && ::access(nmcli.c_str(), X_OK) == 0)
      run(nmcli, {"-w", "15", "connection", "up", "rapid-setup", "ifname", "wlan0"}, false); } catch (...) {}
    std::error_code ignored;
    if (!request_file.empty()) fs::remove(request_file, ignored);
    if (revision && !result_file.empty()) try { atomic_file(result_file, Json{{"revision", *revision}, {"connected", false}, {"error", "Wi-Fi connection failed"}}.dump() + "\n"); } catch (...) {}
    log(std::string("ERROR wifi: ") + error.what());
    return 1;
  }
}
