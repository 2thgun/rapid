#include "rapid/native.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace rapid::native;
namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::invalid_argument(message);
}
bool hex(const std::string &value, std::size_t size) {
  return value.size() == size &&
      value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
int command(const fs::path &program, const std::vector<std::string> &arguments) {
  const auto child = fork();
  if (child < 0) throw std::runtime_error("cannot start NetworkManager command");
  if (child == 0) {
    const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null >= 0) {
      dup2(null, STDOUT_FILENO);
      dup2(null, STDERR_FILENO);
      if (null > STDERR_FILENO) ::close(null);
    }
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char *>(program.c_str()));
    for (const auto &argument : arguments)
      argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    execv(program.c_str(), argv.data());
    _exit(127);
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child)
    throw std::runtime_error("cannot wait for NetworkManager command");
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
void run(const fs::path &program, std::initializer_list<std::string> arguments) {
  if (command(program, std::vector<std::string>(arguments)) != 0)
    throw std::runtime_error("NetworkManager command failed");
}
void ipv4(const std::string &address) {
  in_addr parsed{};
  require(::inet_pton(AF_INET, address.c_str(), &parsed) == 1,
          "setup address must be a numeric IPv4 address");
}
} // namespace

int main(int argc, char **argv) {
  try {
    fs::path status_file, nmcli = "/usr/bin/nmcli";
    std::string connection = "rapid-setup";
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "rapid-provision --status-file PATH [--connection NAME] [--nmcli PATH]\n"
                     "Creates or updates the fixed NetworkManager setup-AP profile.\n";
        return 0;
      } else if (option == "--status-file" && i + 1 < argc) status_file = argv[++i];
      else if (option == "--connection" && i + 1 < argc) connection = argv[++i];
      else if (option == "--nmcli" && i + 1 < argc) nmcli = argv[++i];
      else throw std::invalid_argument("unknown or incomplete argument; use --help");
    }
    require(!status_file.empty(), "status file is required");
    require(connection == "rapid-setup", "only the rapid-setup connection may be provisioned");
    const auto status = Json::parse(read_file(status_file));
    require(status.is_object(), "invalid first-boot status");
    if (!status.contains("bootstrap")) {
      require(status.value("owner_configured", false),
              "first-boot status has no setup credentials");
      log("INFO provision: owner already configured; retaining existing network profile");
      return 0;
    }
    require(status["bootstrap"].is_object(), "first-boot setup credentials are invalid");
    const auto &bootstrap = status["bootstrap"];
    require(bootstrap.size() == 7 && bootstrap.contains("setup_address") &&
                bootstrap.contains("ssid") && bootstrap.contains("access_point_password"),
                "invalid first-boot setup credentials");
    const auto address = bootstrap.at("setup_address").get<std::string>();
    const auto ssid = bootstrap.at("ssid").get<std::string>();
    const auto password = bootstrap.at("access_point_password").get<std::string>();
    require(hex(bootstrap.at("certificate_fingerprint").get<std::string>(), 64),
            "invalid setup certificate fingerprint");
    ipv4(address);
    require(ssid.size() == 12 && ssid.starts_with("rapid-") && hex(ssid.substr(6), 6),
            "invalid setup SSID");
    require(hex(password, 16), "invalid setup AP password");
    require(fs::is_regular_file(nmcli) && ::access(nmcli.c_str(), X_OK) == 0,
            "NetworkManager executable is unavailable");
    if (command(nmcli, {"connection", "show", connection}) != 0)
      run(nmcli, {"connection", "add", "type", "wifi", "ifname", "wlan0",
                  "con-name", connection, "autoconnect", "yes", "ssid", ssid});
    run(nmcli, {"connection", "modify", connection, "802-11-wireless.mode", "ap",
                "wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", password,
                "ipv4.method", "shared", "ipv4.addresses", address + "/24",
                "ipv6.method", "disabled", "connection.autoconnect", "yes"});
    run(nmcli, {"radio", "wifi", "on"});
    run(nmcli, {"-w", "15", "connection", "up", connection, "ifname", "wlan0"});
    log("INFO provision: setup AP profile is active");
    return 0;
  } catch (const std::exception &error) {
    log(std::string("ERROR provision: ") + error.what());
    return 1;
  }
}
