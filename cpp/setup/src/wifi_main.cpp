#include "rapid/native.hpp"
#include <algorithm>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <sstream>
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

// A WPA passphrase: 8-63 printable characters, or exactly 64 hexadecimal
// characters. Empty means "no passphrase" (an open network).
void require_password(const std::string &password) {
  require(password.empty() || (password.size() >= 8 && printable(password) &&
              (password.size() <= 63 || (password.size() == 64 && hexadecimal(password)))),
          "invalid Wi-Fi password");
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

// The inverse of escape_keyfile_value, for reading a value NetworkManager (or
// this program) wrote earlier.
std::string unescape_keyfile_value(const std::string &value) {
  std::string unescaped;
  unescaped.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\') { unescaped += '\\'; ++i; }
    else unescaped += value[i];
  }
  return unescaped;
}

// Not a secret: only needs to be a stable, syntactically valid identifier for
// the one profile this program owns.
std::string connection_uuid() {
  const auto id = unique_id();
  return id.substr(0, 8) + "-" + id.substr(8, 4) + "-" + id.substr(12, 4) + "-" +
         id.substr(16, 4) + "-" + id.substr(20, 12);
}

// Parses a GLib keyfile into section -> (key -> value), unescaping values the
// same way NetworkManager does.
std::map<std::string, std::map<std::string, std::string>> parse_keyfile(const std::string &content) {
  std::map<std::string, std::map<std::string, std::string>> sections;
  std::string current;
  std::istringstream lines(content);
  for (std::string line; std::getline(lines, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    line = line.substr(first);
    if (line.front() == '#' || line.front() == ';') continue;
    if (line.front() == '[' && line.back() == ']') { current = line.substr(1, line.size() - 2); continue; }
    const auto equals = line.find('=');
    if (equals == std::string::npos) continue;
    auto key = line.substr(0, equals);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    sections[current][key] = unescape_keyfile_value(line.substr(equals + 1));
  }
  return sections;
}

// Writes a keyfile atomically at mode 0600, so a passphrase never becomes
// readable by another local process, even for an instant.
void install_keyfile(const fs::path &directory, const std::string &filename, const std::string &content) {
  fs::create_directories(directory);
  const auto path = directory / filename;
  auto temp = path;
  temp += "." + unique_id() + ".part";
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

// The saved wifi connections, read straight from NetworkManager's keyfile
// directory. Only non-secret material comes back: the connection id, its
// SSID, whether it carries a key, and whether it is the device's own setup
// access point (owned by rapid-provision, so the setup page must not offer to
// edit or remove it). A stored passphrase is never read or returned.
Json list_connections(const fs::path &directory) {
  Json connections = Json::array();
  std::error_code error;
  for (const auto &entry : fs::directory_iterator(directory, error)) {
    if (error) break;
    if (!entry.is_regular_file(error)) continue;
    const auto path = entry.path();
    if (path.extension() != ".nmconnection") continue;
    const auto sections = parse_keyfile(read_file(path));
    const auto connection = sections.find("connection");
    if (connection == sections.end()) continue;
    const auto &fields = connection->second;
    const auto id = fields.find("id");
    const auto type = fields.find("type");
    if (id == fields.end() || type == fields.end() || type->second != "wifi") continue;
    std::string ssid, key_mgmt;
    if (const auto wifi = sections.find("wifi"); wifi != sections.end())
      if (const auto found = wifi->second.find("ssid"); found != wifi->second.end()) ssid = found->second;
    if (const auto security = sections.find("wifi-security"); security != sections.end())
      if (const auto found = security->second.find("key-mgmt"); found != security->second.end())
        key_mgmt = found->second;
    connections.push_back({{"name", id->second}, {"ssid", ssid},
                           {"secured", !key_mgmt.empty() && key_mgmt != "none"},
                           {"owned", id->second == "rapid-setup"}});
  }
  return connections;
}

// The UUID an existing connection file already carries, so a rewrite keeps
// NetworkManager's identity for it; empty when there is no such file yet.
std::string existing_uuid(const fs::path &directory, const std::string &id) {
  const auto path = directory / (id + ".nmconnection");
  std::error_code error;
  if (!fs::is_regular_file(path, error)) return {};
  const auto sections = parse_keyfile(read_file(path));
  const auto connection = sections.find("connection");
  if (connection == sections.end()) return {};
  const auto uuid = connection->second.find("uuid");
  return uuid == connection->second.end() ? std::string{} : uuid->second;
}

// #26: writes the NetworkManager keyfile connection profile directly and
// atomically, mode 0600, so the passphrase never becomes an nmcli command-line
// argument -- readable by any other local process via /proc/<pid>/cmdline for
// as long as nmcli runs. Once the profile is active NetworkManager keeps the
// same secret in the same file at the same permissions anyway (that is how
// nmcli's own "connection modify ... wifi-sec.psk" ended up persisting it
// before); this only changes how the secret gets onto disk, never whether it
// ends up there.
void write_connection(const fs::path &directory, const std::string &id, const std::string &uuid,
                      const std::string &ssid, const std::string &password) {
  const std::string content =
      "[connection]\n"
      "id=" + id + "\n"
      "uuid=" + uuid + "\n"
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
  install_keyfile(directory, id + ".nmconnection", content);
}

// The owned setup access point profile. rapid-provision creates and owns it;
// this only rewrites the security section (a WPA key, or none for an open
// network) while preserving the UUID, SSID and address the provisioner
// resolved, so the setup network keeps its identity across the change.
void write_ap_connection(const fs::path &directory, const std::string &uuid, const std::string &ssid,
                         const std::string &address, const std::string &password) {
  std::string content =
      "[connection]\n"
      "id=rapid-setup\n"
      "uuid=" + uuid + "\n"
      "type=wifi\n"
      "interface-name=wlan0\n"
      "autoconnect=true\n"
      "\n"
      "[wifi]\n"
      "mode=ap\n"
      "ssid=" + escape_keyfile_value(ssid) + "\n";
  if (!password.empty())
    content += "\n[wifi-security]\nkey-mgmt=wpa-psk\npsk=" + escape_keyfile_value(password) + "\n";
  content +=
      "\n[ipv4]\n"
      "method=shared\n"
      "ipv4.addresses=" + address + "/24\n"
      "\n"
      "[ipv6]\n"
      "method=disabled\n";
  install_keyfile(directory, "rapid-setup.nmconnection", content);
}
} // namespace

int main(int argc, char **argv) {
  fs::path request_file, result_file;
  fs::path nmcli = "/usr/bin/nmcli";
  fs::path connection_directory = "/etc/NetworkManager/system-connections";
  std::optional<std::int64_t> revision;
  std::string action;
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
    require(request.is_object() && request.contains("action") && request["action"].is_string(),
            "invalid Wi-Fi request");
    action = request["action"].get<std::string>();
    require(action == "list" || action == "save" || action == "remove" || action == "setup_ap",
            "invalid Wi-Fi request");
    require(fs::is_regular_file(nmcli) && ::access(nmcli.c_str(), X_OK) == 0, "nmcli is unavailable");

    if (action == "list") {
      atomic_file(result_file, Json{{"connections", list_connections(connection_directory)}}.dump() + "\n");
      fs::remove(request_file);
      return 0;
    }

    if (action == "remove") {
      require(request.contains("name") && request["name"].is_string(), "invalid Wi-Fi request");
      const auto name = request["name"].get<std::string>();
      require(!name.empty() && name.size() <= 64 && printable(name), "invalid Wi-Fi connection name");
      const auto connections = list_connections(connection_directory);
      const auto found = std::find_if(connections.begin(), connections.end(),
                                      [&](const Json &entry) { return entry["name"] == name; });
      // The setup access point is owned by rapid-provision; the setup page
      // must not offer to remove it.
      require(found != connections.end() && !(*found)["owned"].get<bool>(),
              "the connection cannot be removed");
      run(nmcli, {"connection", "delete", name});
      std::error_code ignored;
      fs::remove(connection_directory / (name + ".nmconnection"), ignored);
      atomic_file(result_file, Json{{"removed", true}}.dump() + "\n");
      fs::remove(request_file);
      return 0;
    }

    if (action == "setup_ap") {
      require(request.contains("password") && request["password"].is_string(), "invalid Wi-Fi request");
      const auto password = request["password"].get<std::string>();
      require_password(password);
      // Rewrite the provisioner-owned profile in place, preserving its UUID,
      // SSID and address so the setup network keeps its identity.
      const auto path = connection_directory / "rapid-setup.nmconnection";
      const auto sections = parse_keyfile(read_file(path));
      const auto connection = sections.find("connection");
      const auto wifi = sections.find("wifi");
      const auto ipv4 = sections.find("ipv4");
      require(connection != sections.end() && wifi != sections.end() && ipv4 != sections.end(),
              "the setup access point profile is unavailable");
      const auto uuid = connection->second.find("uuid");
      const auto ssid = wifi->second.find("ssid");
      const auto address = ipv4->second.find("ipv4.addresses");
      require(uuid != connection->second.end() && ssid != wifi->second.end() &&
                  address != ipv4->second.end(),
              "the setup access point profile is incomplete");
      write_ap_connection(connection_directory, uuid->second, ssid->second,
                          address->second.substr(0, address->second.find('/')), password);
      run(nmcli, {"connection", "load", path.string()});
      run(nmcli, {"-w", "15", "connection", "up", "rapid-setup", "ifname", "wlan0"});
      atomic_file(result_file, Json{{"secured", !password.empty()}, {"ssid", ssid->second}}.dump() + "\n");
      fs::remove(request_file);
      return 0;
    }

    // action == "save": recreate one fixed profile. No browser-provided profile
    // name or command is used. The passphrase reaches NetworkManager only
    // through a private keyfile this process writes directly (never as an nmcli
    // argument) plus "connection load", which takes just a path (#26).
    require(request.is_object() && (request.size() == 4 || request.size() == 5) &&
                request.contains("revision") && request["revision"].is_number_integer() &&
                request.contains("ssid") && request["ssid"].is_string() &&
                request.contains("password") && request["password"].is_string() &&
                (request.size() == 4 || (request.contains("name") && request["name"].is_string())),
            "invalid Wi-Fi request");
    revision = request["revision"].get<std::int64_t>();
    const auto ssid = request["ssid"].get<std::string>(), password = request["password"].get<std::string>();
    require(!ssid.empty() && ssid.size() <= 32 && printable(ssid), "invalid Wi-Fi SSID");
    require_password(password);
    // An optional connection name lets the owner edit another saved network
    // (not just the fixed Home profile); the default stays rapid-home.
    std::string name = "rapid-home";
    if (request.contains("name") && !request["name"].get<std::string>().empty())
      name = request["name"].get<std::string>();
    require(name.size() <= 64 && printable(name), "invalid Wi-Fi connection name");
    const auto uuid = existing_uuid(connection_directory, name);
    run(nmcli, {"connection", "delete", name}, false);
    write_connection(connection_directory, name, uuid.empty() ? connection_uuid() : uuid, ssid, password);
    run(nmcli, {"connection", "load", (connection_directory / (name + ".nmconnection")).string()});
    run(nmcli, {"-w", "30", "connection", "up", name, "ifname", "wlan0"});
    atomic_file(result_file, Json{{"revision", *revision}, {"connected", true}}.dump() + "\n");
    fs::remove(request_file);
    return 0;
  } catch (const std::exception &error) {
    // A failed join must not strand the setup browser on a keyboardless device.
    if (action == "save" || action == "setup_ap") {
      try { if (fs::is_regular_file(nmcli) && ::access(nmcli.c_str(), X_OK) == 0)
        run(nmcli, {"-w", "15", "connection", "up", "rapid-setup", "ifname", "wlan0"}, false); } catch (...) {}
    }
    std::error_code ignored;
    if (!request_file.empty()) fs::remove(request_file, ignored);
    if (!result_file.empty()) {
      try {
        if (action == "save" && revision)
          atomic_file(result_file, Json{{"revision", *revision}, {"connected", false}, {"error", "Wi-Fi connection failed"}}.dump() + "\n");
        else
          atomic_file(result_file, Json{{"error", "Wi-Fi request failed"}}.dump() + "\n");
      } catch (...) {}
    }
    log(std::string("ERROR wifi: ") + error.what());
    return 1;
  }
}
