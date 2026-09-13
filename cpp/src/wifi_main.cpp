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
} // namespace

int main(int argc, char **argv) {
  fs::path request_file, result_file;
  fs::path nmcli = "/usr/bin/nmcli";
  std::optional<std::int64_t> revision;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--request-file" && i + 1 < argc) request_file = argv[++i];
      else if (option == "--result-file" && i + 1 < argc) result_file = argv[++i];
      else if (option == "--nmcli" && i + 1 < argc) nmcli = argv[++i];
      else throw std::invalid_argument("use --request-file PATH --result-file PATH [--nmcli PATH]");
    }
    require(!request_file.empty() && !result_file.empty(), "request and result files are required");
    const auto request = Json::parse(read_file(request_file));
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
    // Recreate one fixed profile. No browser-provided profile name or command is used.
    run(nmcli, {"connection", "delete", "rapid-home"}, false);
    run(nmcli, {"connection", "add", "type", "wifi", "ifname", "wlan0", "con-name", "rapid-home", "ssid", ssid});
    run(nmcli, {"connection", "modify", "rapid-home", "wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", password,
                "connection.autoconnect", "yes", "connection.autoconnect-priority", "100"});
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
