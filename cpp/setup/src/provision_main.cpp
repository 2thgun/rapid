#include "rapid/native.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
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
bool decimal(const std::string &value, std::size_t size) {
  return value.size() == size &&
      value.find_first_not_of("0123456789") == std::string::npos;
}
// #22: the setup AP is always named "rapid" unless a scan finds that name
// already in use, in which case a random 4-digit suffix disambiguates it.
bool valid_ssid(const std::string &value) {
  return value == "rapid" ||
      (value.size() == 10 && value.rfind("rapid-", 0) == 0 && decimal(value.substr(6), 4));
}
std::string trimmed(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
    value.pop_back();
  return value;
}
std::string random_suffix() {
  const auto seed = unique_id().substr(0, 4);
  const auto value = static_cast<unsigned>(std::stoul(seed, nullptr, 16)) % 10000;
  std::ostringstream out;
  out << std::setw(4) << std::setfill('0') << value;
  return out.str();
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
// Only a handful of read-only queries (the collision scan) need output back;
// everything else only needs an exit code, handled by command()/run() above.
std::string capture(const fs::path &program, const std::vector<std::string> &arguments) {
  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) throw std::runtime_error("cannot create pipe for NetworkManager query");
  const auto child = fork();
  if (child < 0) {
    ::close(pipe_fds[0]); ::close(pipe_fds[1]);
    throw std::runtime_error("cannot start NetworkManager command");
  }
  if (child == 0) {
    ::close(pipe_fds[0]);
    dup2(pipe_fds[1], STDOUT_FILENO);
    const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null >= 0) {
      dup2(null, STDERR_FILENO);
      if (null > STDERR_FILENO) ::close(null);
    }
    if (pipe_fds[1] > STDOUT_FILENO) ::close(pipe_fds[1]);
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char *>(program.c_str()));
    for (const auto &argument : arguments)
      argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    execv(program.c_str(), argv.data());
    _exit(127);
  }
  ::close(pipe_fds[1]);
  std::string output;
  char buffer[4096];
  for (ssize_t read_count; (read_count = ::read(pipe_fds[0], buffer, sizeof buffer)) > 0;)
    output.append(buffer, static_cast<std::size_t>(read_count));
  ::close(pipe_fds[0]);
  int status = 0;
  if (waitpid(child, &status, 0) != child)
    throw std::runtime_error("cannot wait for NetworkManager command");
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    throw std::runtime_error("NetworkManager query failed");
  return output;
}
// A boot resumes whatever SSID this provisioner already chose this session
// (ssid_file lives on tmpfs, so it is naturally cleared at reboot). Only the
// first invocation of a boot scans; this is a one-shot edge case, not a
// retried negotiation -- two Pis that both see each other pick independently
// and do not coordinate further.
std::string resolve_ssid(const fs::path &nmcli, const fs::path &ssid_file) {
  std::error_code error;
  if (fs::is_regular_file(ssid_file, error)) {
    const auto existing = trimmed(read_file(ssid_file));
    if (valid_ssid(existing)) return existing;
  }
  bool collision = false;
  std::istringstream lines(capture(nmcli, {"-t", "-f", "SSID", "device", "wifi", "list"}));
  for (std::string line; std::getline(lines, line);)
    if (trimmed(line) == "rapid") { collision = true; break; }
  const std::string ssid = collision ? "rapid-" + random_suffix() : std::string("rapid");
  atomic_file(ssid_file, ssid + "\n");
  if (::chmod(ssid_file.c_str(), 0644) != 0)
    throw std::runtime_error("cannot publish setup SSID");
  return ssid;
}
// #22 upgrade migration. A device that predates the open setup AP can still
// carry a secured access-point profile for the setup SSID -- the pre-firstboot
// worker used the name "rapid-demo", and the first first-boot scheme wrote a
// WPA-PSK "rapid-setup" -- and neither is repaired by creating or updating the
// owned connection, because NetworkManager keeps the old profile alongside it.
// Left in place, either can be activated by the Wi-Fi mode controller or by
// autoconnect and bring the password-protected AP back after an upgrade. Remove
// every unowned access-point profile for the setup SSID. Best-effort: a query
// failure must not stop provisioning, and only profiles that are actually
// access points for "rapid"/"rapid-NNNN" are touched.
void remove_legacy_ap_profiles(const fs::path &nmcli, const std::string &connection) {
  std::string listing;
  try {
    listing = capture(nmcli, {"-t", "-f", "NAME,TYPE", "connection", "show"});
  } catch (const std::exception &) {
    return;
  }
  std::istringstream lines(listing);
  for (std::string line; std::getline(lines, line);) {
    const auto separator = line.find(':');
    if (separator == std::string::npos || separator + 1 >= line.size()) continue;
    const auto name = line.substr(0, separator);
    if (name == connection || line.substr(separator + 1) != "802-11-wireless") continue;
    std::string mode, ssid;
    try {
      mode = trimmed(capture(nmcli, {"-g", "802-11-wireless.mode", "connection", "show", name}));
      if (mode != "ap") continue;
      ssid = trimmed(capture(nmcli, {"-g", "802-11-wireless.ssid", "connection", "show", name}));
    } catch (const std::exception &) {
      continue;
    }
    if (!valid_ssid(ssid)) continue;
    if (command(nmcli, {"connection", "delete", name}) == 0)
      log("INFO provision: removed legacy secured setup AP profile " + name);
  }
}
void ipv4(const std::string &address) {
  in_addr parsed{};
  require(::inet_pton(AF_INET, address.c_str(), &parsed) == 1,
          "setup address must be a numeric IPv4 address");
}
} // namespace

int main(int argc, char **argv) {
  try {
    fs::path status_file, ssid_file, nmcli = "/usr/bin/nmcli";
    std::string connection = "rapid-setup";
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "rapid-provision --status-file PATH --ssid-file PATH [--connection NAME] [--nmcli PATH]\n"
                     "Creates or updates the fixed, open NetworkManager setup-AP profile.\n";
        return 0;
      } else if (option == "--status-file" && i + 1 < argc) status_file = argv[++i];
      else if (option == "--ssid-file" && i + 1 < argc) ssid_file = argv[++i];
      else if (option == "--connection" && i + 1 < argc) connection = argv[++i];
      else if (option == "--nmcli" && i + 1 < argc) nmcli = argv[++i];
      else throw std::invalid_argument("unknown or incomplete argument; use --help");
    }
    require(!status_file.empty(), "status file is required");
    require(!ssid_file.empty(), "SSID file is required");
    require(connection == "rapid-setup", "only the rapid-setup connection may be provisioned");
    const auto status = Json::parse(read_file(status_file));
    require(status.is_object(), "invalid first-boot status");
    require(fs::is_regular_file(nmcli) && ::access(nmcli.c_str(), X_OK) == 0,
            "NetworkManager executable is unavailable");
    // #22 upgrade migration: this runs whether or not the owner is already
    // configured. A secured legacy AP profile can otherwise be reactivated
    // from the panel's AP mode after an upgrade, not only on the first boot.
    // It runs before the SSID scan so the provisioner cannot mistake its own
    // still-configured legacy AP for a colliding network in range.
    remove_legacy_ap_profiles(nmcli, connection);
    // #59 related: an upgraded device can carry a hand-installed override of
    // the Wi-Fi mode worker at /etc/systemd/system/rapid-network-mode.service
    // (the dev Pi's pointed at /usr/local/sbin/rapid-network-mode, which
    // hardcodes the old "rapid-demo" connection). A fresh image never has that
    // file, so the fix must not depend on removing it. Warn instead: the
    // packaged worker at /usr/lib/rapid/rapid-network-mode is the one that
    // activates the open "rapid-setup" profile this provisioner maintains.
    {
      std::error_code override_error;
      if (fs::exists("/etc/systemd/system/rapid-network-mode.service", override_error))
        log("WARN provision: /etc/systemd/system/rapid-network-mode.service overrides the packaged "
            "Wi-Fi mode worker; if it names a legacy AP connection, remove it so Access Point mode "
            "activates " + connection);
    }
    const bool owner_configured = status.value("owner_configured", false);
    const bool has_bootstrap = status.contains("bootstrap") && status["bootstrap"].is_object();
    const bool has_token = has_bootstrap && status["bootstrap"].contains("activation_token");
    // A status document written before owner_configured existed still carries
    // its activation token, which is enough to know the owner is unenrolled.
    require(owner_configured || has_token, "first-boot status has no setup credentials");
    // The address the open profile serves the setup page on. first boot
    // publishes it; an upgraded status that predates it falls back to the
    // packaged default so the profile is still created and open.
    std::string address = "192.168.1.64";
    if (has_bootstrap) {
      const auto &bootstrap = status["bootstrap"];
      if (bootstrap.contains("setup_address") && bootstrap["setup_address"].is_string())
        address = bootstrap["setup_address"].get<std::string>();
      if (bootstrap.contains("certificate_fingerprint") &&
          bootstrap["certificate_fingerprint"].is_string())
        require(hex(bootstrap["certificate_fingerprint"].get<std::string>(), 64),
                "invalid setup certificate fingerprint");
    }
    ipv4(address);
    const bool exists = command(nmcli, {"connection", "show", connection}) == 0;
    const auto ssid = resolve_ssid(nmcli, ssid_file);
    require(valid_ssid(ssid), "invalid setup SSID");
    if (!exists)
      run(nmcli, {"connection", "add", "type", "wifi", "ifname", "wlan0",
                  "con-name", connection, "autoconnect", "yes", "ssid", ssid});
    // Best-effort: a fresh connection never has this setting, so there is
    // nothing to remove; an upgraded device may carry the old device-specific
    // WPA-PSK setting, which this drops rather than leaving it configured
    // alongside (or instead of) the open profile below.
    command(nmcli, {"connection", "modify", connection, "remove", "802-11-wireless-security"});
    // The SSID is set on every run, not just on `connection add`: an upgraded
    // device already has a profile, and the old scheme's name differed (a
    // device-specific "rapid-<hex>"), so the open profile must be renamed to
    // the name the panel and the setup card publish.
    run(nmcli, {"connection", "modify", connection, "802-11-wireless.ssid", ssid,
                "802-11-wireless.mode", "ap",
                "ipv4.method", "shared", "ipv4.addresses", address + "/24",
                "ipv6.method", "disabled", "connection.autoconnect", "yes"});
    if (owner_configured && !has_token) {
      // #59: the owner is already configured. The open profile and the
      // published SSID now exist for the panel's Access Point mode, but the
      // device is not forced onto the setup AP at boot -- Home Wi-Fi, or the
      // owner's explicit choice, decides. This is the upgrade path: a device
      // that only ever had the legacy secured "rapid-demo" profile now has the
      // open "rapid-setup" one the packaged Wi-Fi mode worker activates.
      log("INFO provision: owner already configured; open setup AP profile retained, SSID " + ssid);
      return 0;
    }
    run(nmcli, {"radio", "wifi", "on"});
    run(nmcli, {"-w", "15", "connection", "up", connection, "ifname", "wlan0"});
    log("INFO provision: open setup AP profile is active, SSID " + ssid);
    return 0;
  } catch (const std::exception &error) {
    log(std::string("ERROR provision: ") + error.what());
    return 1;
  }
}
