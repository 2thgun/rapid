// #26: the Home Wi-Fi passphrase must reach NetworkManager without ever
// becoming an nmcli command-line argument (readable by any other local
// process via /proc/<pid>/cmdline while nmcli runs), and it must not persist
// in nmcli's environment, in rapid-wifi's own logs, in its result file, or in
// an error message on the failure path. This drives the real rapid-wifi
// binary against a fake nmcli that records both its argv and its
// environment, then -- following the same pattern as rapid-account-tests'
// secret_tests()/files_containing() -- greps every artifact the flow
// produced for the plaintext password.
#include "rapid/native.hpp"
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace rapid::native;

namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}

struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path() / ("rapid-wifi-secret-tests-" + unique_id());
  TemporaryDirectory() { fs::create_directory(path); }
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
  }
};

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

// Recursively searches every regular file below a directory for a byte
// string, exactly like rapid-account-tests' helper of the same name.
std::vector<fs::path> files_containing(const fs::path &directory, const std::string &needle) {
  std::vector<fs::path> found;
  std::error_code error;
  if (!fs::exists(directory, error)) return found;
  for (const auto &entry : fs::recursive_directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;
    if (read_file(entry.path()).find(needle) != std::string::npos) found.push_back(entry.path());
  }
  return found;
}

std::string require_only(const std::vector<fs::path> &found, const fs::path &allowed,
                         const std::string &label) {
  std::string listing;
  for (const auto &path : found) {
    listing += " " + path.string();
    require(path == allowed, label + " persists outside the NetworkManager keyfile: " + path.string());
  }
  return listing;
}

// One fake nmcli for every scenario below: it records its own argv and
// environment, then answers according to environment variables the test
// sets before each fork (like network_mode_tests.sh's fake nmcli, but
// per-subcommand exit codes are read from the environment instead of a
// scenario name, since these tests fork the real binary, not a shell
// function).
fs::path write_fake_nmcli(const fs::path &path) {
  write_script(path,
               "printf '%s\\n' \"$*\" >> \"$RAPID_TEST_NMCLI_LOG\"\n"
               "{ env; printf -- '----\\n'; } >> \"$RAPID_TEST_NMCLI_ENV\"\n"
               "case \"$*\" in\n"
               "  \"connection delete \"*) exit 0 ;;\n"
               "  \"connection load \"*) exit \"${RAPID_TEST_NMCLI_LOAD_EXIT:-0}\" ;;\n"
               "  \"-w 30 connection up rapid-home ifname wlan0\") exit \"${RAPID_TEST_NMCLI_UP_EXIT:-0}\" ;;\n"
               "  \"-w 15 connection up rapid-setup ifname wlan0\") exit \"${RAPID_TEST_NMCLI_AP_EXIT:-0}\" ;;\n"
               "  *) echo \"unexpected nmcli invocation: $*\" >&2; exit 1 ;;\n"
               "esac\n");
  return path;
}

struct Flow {
  fs::path root, nmcli, nmcli_log, nmcli_env, connections, wifi_log;
  std::string helper;

  Flow(fs::path base, std::string helper_path) : root(std::move(base)), helper(std::move(helper_path)) {
    fs::create_directories(root);
    nmcli = write_fake_nmcli(root / "nmcli");
    nmcli_log = root / "nmcli.log";
    nmcli_env = root / "nmcli.env";
    connections = root / "nm-connections";
    wifi_log = root / "wifi-stderr.log";
  }
  fs::path request_file() const { return root / "wifi-request.json"; }
  fs::path result_file() const { return root / "wifi-result.json"; }
  fs::path connection_file() const { return connections / "rapid-home.nmconnection"; }

  // Runs the real rapid-wifi binary once against whatever request_file()
  // currently holds. Returns its exit status (WEXITSTATUS).
  int run() const {
    setenv("RAPID_TEST_NMCLI_LOG", nmcli_log.c_str(), 1);
    setenv("RAPID_TEST_NMCLI_ENV", nmcli_env.c_str(), 1);
    const auto child = fork();
    require(child >= 0, "fork rapid-wifi");
    if (child == 0) {
      const int out = ::open(wifi_log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (out >= 0) {
        ::dup2(out, STDOUT_FILENO);
        ::dup2(out, STDERR_FILENO);
      }
      execl(helper.c_str(), helper.c_str(), "--request-file", request_file().c_str(), "--result-file",
            result_file().c_str(), "--nmcli", nmcli.c_str(), "--connection-directory", connections.c_str(),
            nullptr);
      _exit(127);
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status), "rapid-wifi exits");
    unsetenv("RAPID_TEST_NMCLI_LOG");
    unsetenv("RAPID_TEST_NMCLI_ENV");
    return WEXITSTATUS(status);
  }
};

void success_scenario(const fs::path &base, const std::string &helper) {
  Flow flow(base / "success", helper);
  const std::string password = "Unique-" + unique_id().substr(0, 12) + "-Wifi";
  atomic_file(flow.request_file(), Json{{"action", "save"}, {"revision", 1}, {"ssid", "Home Network"}, {"password", password}}.dump());
  require(flow.run() == 0, "rapid-wifi succeeds against a cooperative fake nmcli");
  require(Json::parse(read_file(flow.result_file()))["connected"] == true, "result reports connected");
  require(!fs::exists(flow.request_file()), "request consumed");
  require(fs::is_regular_file(flow.connection_file()), "NetworkManager keyfile installed");
  struct stat info {};
  require(::lstat(flow.connection_file().c_str(), &info) == 0, "stat the installed keyfile");
  require((info.st_mode & 07777) == 0600, "the installed keyfile is mode 0600, not wider");
  require(read_file(flow.connection_file()).find("psk=" + password) != std::string::npos,
          "the installed keyfile carries the passphrase NetworkManager needs to connect");
  const auto found = files_containing(flow.root, password);
  const auto listing = require_only(found, flow.connection_file(), "the passphrase");
  require(!found.empty(), "the passphrase is found at all (sanity: the search itself works)" + listing);
}

// nmcli's own "connection up" fails (e.g. a wrong password). The keyfile
// this process wrote before that call legitimately carries the (bad)
// passphrase -- exactly as nmcli itself would have persisted it to the same
// path via the old "connection modify ... wifi-sec.psk" call -- but nothing
// else may, including nmcli's argv/env, rapid-wifi's own stderr log, and the
// result file's error string.
void activation_failure_scenario(const fs::path &base, const std::string &helper) {
  Flow flow(base / "activation-failure", helper);
  const std::string password = "Unique-" + unique_id().substr(0, 12) + "-Bad";
  atomic_file(flow.request_file(), Json{{"action", "save"}, {"revision", 2}, {"ssid", "Home Network"}, {"password", password}}.dump());
  setenv("RAPID_TEST_NMCLI_UP_EXIT", "1", 1);
  const int status = flow.run();
  unsetenv("RAPID_TEST_NMCLI_UP_EXIT");
  require(status == 1, "rapid-wifi reports failure when nmcli cannot activate the profile");
  require(!fs::exists(flow.request_file()), "request consumed even on failure");
  const auto result = Json::parse(read_file(flow.result_file()));
  require(result["connected"] == false && result.value("error", std::string{}) == "Wi-Fi connection failed",
          "result reports a generic failure, not nmcli's own message");
  require(read_file(flow.nmcli_log).find("-w 15 connection up rapid-setup ifname wlan0") != std::string::npos,
          "a failed join attempts to restore the setup AP");
  const auto found = files_containing(flow.root, password);
  require_only(found, flow.connection_file(), "the passphrase");
}

// The list action reads the saved connections back from NetworkManager's
// keyfile directory. Only non-secret material comes back: name, SSID, whether
// a key is set, and whether the connection is the device's own setup access
// point. A stored passphrase is never read or returned.
void list_scenario(const fs::path &base, const std::string &helper) {
  Flow flow(base / "list", helper);
  const std::string home =
      "[connection]\n"
      "id=rapid-home\n"
      "uuid=11111111-1111-1111-1111-111111111111\n"
      "type=wifi\n"
      "interface-name=wlan0\n"
      "\n"
      "[wifi]\n"
      "mode=infrastructure\n"
      "ssid=Home Network\n"
      "\n"
      "[wifi-security]\n"
      "key-mgmt=wpa-psk\n"
      "psk=secret-password\n"
      "\n"
      "[ipv4]\n"
      "method=auto\n";
  const std::string ap =
      "[connection]\n"
      "id=rapid-setup\n"
      "uuid=22222222-2222-2222-2222-222222222222\n"
      "type=wifi\n"
      "interface-name=wlan0\n"
      "\n"
      "[wifi]\n"
      "mode=ap\n"
      "ssid=rapid\n"
      "\n"
      "[ipv4]\n"
      "method=shared\n"
      "ipv4.addresses=192.168.1.64/24\n";
  write_text(flow.connections / "rapid-home.nmconnection", home);
  write_text(flow.connections / "rapid-setup.nmconnection", ap);
  atomic_file(flow.request_file(), Json{{"action", "list"}}.dump());
  require(flow.run() == 0, "rapid-wifi lists the saved connections");
  const auto result = Json::parse(read_file(flow.result_file()));
  const auto connections = result["connections"];
  require(connections.is_array() && connections.size() == 2, "the list holds both saved connections");
  bool found_home = false, found_ap = false;
  for (const auto &entry : connections) {
    if (entry["name"] == "rapid-home") {
      found_home = true;
      require(entry["ssid"] == "Home Network", "the Home SSID is listed");
      require(entry["secured"] == true, "the Home network is reported secured");
      require(entry["owned"] == false, "the Home network is not owned");
    }
    if (entry["name"] == "rapid-setup") {
      found_ap = true;
      require(entry["ssid"] == "rapid", "the setup SSID is listed");
      require(entry["secured"] == false, "the open setup network is reported unsecured");
      require(entry["owned"] == true, "the setup network is reported owned");
    }
  }
  require(found_home && found_ap, "both connections are listed");
  require(result.dump().find("secret-password") == std::string::npos,
          "the list never returns a stored passphrase");
  require(result.dump().find("psk") == std::string::npos, "the list never returns key material");
}

// The remove action deletes a saved connection through the same helper. The
// setup access point is owned by the provisioner and must be refused.
void remove_scenario(const fs::path &base, const std::string &helper) {
  Flow flow(base / "remove", helper);
  write_text(flow.connections / "rapid-home.nmconnection", "[connection]\nid=rapid-home\ntype=wifi\n");
  write_text(flow.connections / "rapid-setup.nmconnection", "[connection]\nid=rapid-setup\ntype=wifi\n");
  atomic_file(flow.request_file(), Json{{"action", "remove"}, {"name", "rapid-home"}}.dump());
  require(flow.run() == 0, "rapid-wifi removes a saved connection");
  require(Json::parse(read_file(flow.result_file()))["removed"] == true, "the removal is reported");
  require(!fs::exists(flow.connections / "rapid-home.nmconnection"), "the removed keyfile is gone");
  require(fs::exists(flow.connections / "rapid-setup.nmconnection"), "the setup profile is untouched");
  require(read_file(flow.nmcli_log).find("connection delete rapid-home") != std::string::npos,
          "the removal goes through nmcli");
  // The owned setup access point is refused.
  atomic_file(flow.request_file(), Json{{"action", "remove"}, {"name", "rapid-setup"}}.dump());
  require(flow.run() == 1, "rapid-wifi refuses to remove the setup access point");
  require(fs::exists(flow.connections / "rapid-setup.nmconnection"), "the setup profile survives a refused removal");
}

// The setup_ap action rewrites the provisioner-owned rapid-setup profile with
// a WPA key (or none for an open network), preserving its UUID, SSID and
// address so the setup network keeps its identity.
void setup_ap_scenario(const fs::path &base, const std::string &helper) {
  Flow flow(base / "setup-ap", helper);
  const std::string ap =
      "[connection]\n"
      "id=rapid-setup\n"
      "uuid=33333333-3333-3333-3333-333333333333\n"
      "type=wifi\n"
      "interface-name=wlan0\n"
      "\n"
      "[wifi]\n"
      "mode=ap\n"
      "ssid=rapid\n"
      "\n"
      "[ipv4]\n"
      "method=shared\n"
      "ipv4.addresses=192.168.1.64/24\n";
  write_text(flow.connections / "rapid-setup.nmconnection", ap);
  const std::string password = "Setup-Ap-Passphrase";
  atomic_file(flow.request_file(), Json{{"action", "setup_ap"}, {"password", password}}.dump());
  require(flow.run() == 0, "rapid-wifi applies the setup-AP password");
  const auto result = Json::parse(read_file(flow.result_file()));
  require(result["secured"] == true && result["ssid"] == "rapid", "the setup-AP result reports the secured state");
  const auto keyfile = read_file(flow.connections / "rapid-setup.nmconnection");
  require(keyfile.find("psk=" + password) != std::string::npos, "the setup-AP keyfile carries the WPA key");
  require(keyfile.find("uuid=33333333-3333-3333-3333-333333333333") != std::string::npos,
          "the setup-AP keyfile keeps the provisioner's UUID");
  require(keyfile.find("ssid=rapid") != std::string::npos, "the setup-AP keyfile keeps the SSID");
  require(keyfile.find("ipv4.addresses=192.168.1.64/24") != std::string::npos,
          "the setup-AP keyfile keeps the address");
  // Clearing the password returns the profile to open.
  atomic_file(flow.request_file(), Json{{"action", "setup_ap"}, {"password", ""}}.dump());
  require(flow.run() == 0, "rapid-wifi clears the setup-AP password");
  const auto cleared = Json::parse(read_file(flow.result_file()));
  require(cleared["secured"] == false, "the cleared setup-AP result reports open");
  const auto cleared_keyfile = read_file(flow.connections / "rapid-setup.nmconnection");
  require(cleared_keyfile.find("wifi-security") == std::string::npos,
          "the cleared setup-AP keyfile has no security section");
  require(cleared_keyfile.find("psk") == std::string::npos, "the cleared setup-AP keyfile has no WPA key");
}

// A corrupted request file must not leak a fragment of its own bytes (which
// may include the passphrase) through a JSON library parse-error message
// into rapid-wifi's own log.
void malformed_request_scenario(const fs::path &base, const std::string &helper) {
  Flow flow(base / "malformed-request", helper);
  const std::string password = "Unique-" + unique_id().substr(0, 12) + "-Cut";
  // Truncated mid-value: a byte-offset parse error from nlohmann::json can
  // quote the bytes right around the cut, which is exactly where this
  // password sits.
  const auto whole = Json{{"action", "save"}, {"revision", 3}, {"ssid", "Home Network"}, {"password", password}}.dump();
  write_text(flow.request_file(), whole.substr(0, whole.find(password) + password.size() / 2));
  const int status = flow.run();
  require(status == 1, "rapid-wifi rejects a truncated request");
  require(!fs::exists(flow.connection_file()), "no NetworkManager keyfile is written for an invalid request");
  const auto found = files_containing(flow.root, password.substr(0, password.size() / 2));
  require(found.empty(), "no artifact -- including rapid-wifi's own error log -- quotes the cut request");
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "usage: rapid-wifi-secret-tests PATH-TO-rapid-wifi");
    TemporaryDirectory root;
    const auto helper = fs::absolute(argv[1]).string();
    success_scenario(root.path, helper);
    std::cout << "ok: the passphrase reaches NetworkManager only through its private keyfile\n";
    activation_failure_scenario(root.path, helper);
    std::cout << "ok: a failed activation leaks the passphrase nowhere but the keyfile it already wrote\n";
    malformed_request_scenario(root.path, helper);
    std::cout << "ok: a truncated request never echoes a passphrase fragment\n";
    list_scenario(root.path, helper);
    std::cout << "ok: the saved-connection list returns no secret material\n";
    remove_scenario(root.path, helper);
    std::cout << "ok: a saved connection is removed through the helper and the setup access point is refused\n";
    setup_ap_scenario(root.path, helper);
    std::cout << "ok: the setup-AP password is applied and cleared on the provisioner-owned profile\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
  }
}
