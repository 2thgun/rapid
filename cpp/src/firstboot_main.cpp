#include "rapid/setup.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace rapid::native;
namespace {
std::string private_hex_secret(const fs::path &path, std::size_t size,
                               const std::string &description) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (descriptor >= 0) {
    struct stat info {};
    const bool safe = ::fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_uid == ::geteuid() && (info.st_mode & 0077) == 0 && info.st_nlink == 1;
    std::string data(size + 2, '\0');
    const auto length = safe ? ::read(descriptor, data.data(), data.size()) : -1;
    ::close(descriptor);
    if (!safe || (length != static_cast<ssize_t>(size) &&
                  (length != static_cast<ssize_t>(size + 1) || data[size] != '\n')) ||
        data.substr(0, size).find_first_not_of("0123456789abcdef") != std::string::npos)
      throw std::runtime_error(description + " must be a private service-owned file containing lowercase hexadecimal characters");
    return data.substr(0, size);
  }
  if (errno != ENOENT)
    throw std::runtime_error("cannot open " + description + " file");
  int created = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (created < 0)
    throw std::runtime_error("cannot create " + description + " file");
  try {
    std::string secret;
    while (secret.size() < size) secret += unique_id();
    secret.resize(size);
    const auto text = secret + "\n";
    if (::write(created, text.data(), text.size()) != static_cast<ssize_t>(text.size()) ||
        ::fsync(created) != 0)
      throw std::runtime_error("cannot write " + description + " file");
    if (::close(created) != 0)
      throw std::runtime_error("cannot close " + description + " file");
    created = -1;
    sync_file(path.parent_path());
    return secret;
  } catch (...) {
    if (created >= 0) ::close(created);
    throw;
  }
}

void setup_address(const std::string &value) {
  in_addr address{};
  if (::inet_pton(AF_INET, value.c_str(), &address) != 1)
    throw std::invalid_argument("setup address must be a numeric IPv4 address");
}
} // namespace

int main(int argc, char **argv) {
  try {
    fs::path directory, status_file, token_file;
    std::string address = "192.168.1.64";
    int port = 8002;
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "rapid-firstboot --state-directory PATH [--status-file PATH] "
                     "[--enrollment-token-file PATH] [--setup-address IPV4] [--setup-port PORT]\n"
                     "Initializes private device state and creates a private AP enrollment token.\n";
        return 0;
      }
      if (option == "--state-directory" && i + 1 < argc) directory = argv[++i];
      else if (option == "--status-file" && i + 1 < argc) status_file = argv[++i];
      else if (option == "--enrollment-token-file" && i + 1 < argc) token_file = argv[++i];
      else if (option == "--setup-address" && i + 1 < argc) {
        address = argv[++i];
        setup_address(address);
      } else if (option == "--setup-port" && i + 1 < argc) {
        const std::string value = argv[++i];
        std::size_t consumed;
        port = std::stoi(value, &consumed);
        if (consumed != value.size() || port < 1024 || port > 65535)
          throw std::invalid_argument("setup port must be 1024 to 65535");
      }
      else throw std::invalid_argument("unknown or incomplete argument; use --help");
    }
    SetupStore store(directory);
    const bool owner_configured = !store.owner_hash().empty();
    auto status = provisioning_status(store.snapshot(), owner_configured);
    // The status file is consumed locally by the AP provisioner and physical
    // panel. It is never an HTTP response or journal entry.
    if (!owner_configured) {
      if (token_file.empty()) token_file = directory / "enrollment.token";
      const auto id = store.snapshot().at("device_id").get<std::string>();
      status["bootstrap"] = {{"setup_address", address}, {"setup_port", port},
                             {"setup_url", "http://" + address + ":" + std::to_string(port) + "/setup"},
                             {"ssid", "rapid-" + id.substr(0, 6)},
                             {"access_point_password", private_hex_secret(directory / "ap-password", 16, "access-point password")},
                             {"activation_token", private_hex_secret(token_file, 64, "activation token")}};
    }
    if (!status_file.empty()) {
      atomic_file(status_file, status.dump() + "\n");
      if (::chmod(status_file.c_str(), 0640) != 0)
        throw std::runtime_error("cannot secure first-boot status file");
    }
    std::cout << provisioning_status(store.snapshot(), owner_configured).dump() << '\n';
    log("INFO firstboot: provisioning state is " + status.at("state").get<std::string>() +
        "; bootstrap details are available only to the local provisioner and panel");
    return 0;
  } catch (const std::exception &error) {
    log(std::string("ERROR firstboot: ") + error.what());
    return 1;
  }
}
