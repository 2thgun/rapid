#include "rapid/setup_auth.hpp"
#include <arpa/inet.h>
#include <csignal>
#include <iostream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <iomanip>
#include <sstream>

using namespace rapid::native;
namespace {
std::string certificate_fingerprint(const fs::path &path) {
  FILE *file = ::fopen(path.c_str(), "rb");
  if (!file) throw std::runtime_error("cannot open TLS certificate");
  X509 *certificate = PEM_read_X509(file, nullptr, nullptr, nullptr);
  ::fclose(file);
  if (!certificate) throw std::runtime_error("cannot parse TLS certificate");
  unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int length = 0;
  const bool valid = X509_digest(certificate, EVP_sha256(), digest, &length) == 1;
  X509_free(certificate);
  if (!valid || length != 32) throw std::runtime_error("cannot fingerprint TLS certificate");
  std::ostringstream result;
  for (unsigned int i = 0; i < length; ++i)
    result << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(digest[i]);
  return result.str();
}

std::string enrollment_token(const fs::path &path) {
  if (path.empty()) return {};
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (descriptor < 0) throw std::runtime_error("cannot open enrollment token file");
  struct stat info {};
  const bool safe = ::fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) &&
      info.st_uid == ::geteuid() && (info.st_mode & 0077) == 0 && info.st_nlink == 1;
  char data[66];
  const auto length = safe ? ::read(descriptor, data, sizeof data) : -1;
  ::close(descriptor);
  if (!safe || length < 64 || length > 65 || (length == 65 && data[64] != '\n'))
    throw std::runtime_error("enrollment token must be a private service-owned file containing 64 hexadecimal characters");
  return std::string(data, 64);
}
}
int main(int argc, char **argv) {
  try {
    fs::path directory, assets = "cpp/assets", token_file, apply_request_file,
             apply_result_file, firstboot_status_file, tls_certificate,
             tls_private_key, calibration_file, calibration_request_file, display_confirm_file;
    std::string pairing_device_id, pairing_certificate_fingerprint;
    fs::path wifi_request_file, wifi_result_file;
    fs::path account_request_file, account_result_file;
    fs::path network_ssid_file;
    fs::path companion_artifact;
    int port = 8002;
    bool enroll = false;
    std::string listen_host = "127.0.0.1";
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "rapid-setup-server --state-directory PATH [--assets PATH] [--port PORT] [--set-owner] [--enrollment-token-file PATH] [--apply-request-file PATH] [--apply-result-file PATH] [--firstboot-status-file PATH] [--calibration-file PATH] [--calibration-request-file PATH] [--display-confirm-file PATH] [--account-request-file PATH --account-result-file PATH] [--ssid-file PATH] [--companion-artifact PATH] [--listen IPV4] [--tls-certificate PATH --tls-private-key PATH] [--device-id HEX32 --certificate-fingerprint HEX64]\n"
                     "Defaults to loopback. A non-loopback listener requires an enrollment token. "
                     "--set-owner reads a new owner password from standard input.\n";
        return 0;
      } else if (option == "--set-owner") enroll = true;
      else if (option == "--state-directory" && i + 1 < argc) directory = argv[++i];
      else if (option == "--assets" && i + 1 < argc) assets = argv[++i];
      else if (option == "--enrollment-token-file" && i + 1 < argc) token_file = argv[++i];
      else if (option == "--apply-request-file" && i + 1 < argc) apply_request_file = argv[++i];
      else if (option == "--apply-result-file" && i + 1 < argc) apply_result_file = argv[++i];
      else if (option == "--wifi-request-file" && i + 1 < argc) wifi_request_file = argv[++i];
      else if (option == "--wifi-result-file" && i + 1 < argc) wifi_result_file = argv[++i];
      else if (option == "--account-request-file" && i + 1 < argc) account_request_file = argv[++i];
      else if (option == "--account-result-file" && i + 1 < argc) account_result_file = argv[++i];
      else if (option == "--ssid-file" && i + 1 < argc) network_ssid_file = argv[++i];
      else if (option == "--companion-artifact" && i + 1 < argc) companion_artifact = argv[++i];
      else if (option == "--firstboot-status-file" && i + 1 < argc) firstboot_status_file = argv[++i];
      else if (option == "--calibration-file" && i + 1 < argc) calibration_file = argv[++i];
      else if (option == "--calibration-request-file" && i + 1 < argc) calibration_request_file = argv[++i];
      else if (option == "--display-confirm-file" && i + 1 < argc) display_confirm_file = argv[++i];
      else if (option == "--tls-certificate" && i + 1 < argc) tls_certificate = argv[++i];
      else if (option == "--tls-private-key" && i + 1 < argc) tls_private_key = argv[++i];
      else if (option == "--device-id" && i + 1 < argc) pairing_device_id = argv[++i];
      else if (option == "--certificate-fingerprint" && i + 1 < argc) pairing_certificate_fingerprint = argv[++i];
      else if (option == "--listen" && i + 1 < argc) {
        listen_host = argv[++i];
        in_addr address{};
        if (::inet_pton(AF_INET, listen_host.c_str(), &address) != 1)
          throw std::invalid_argument("listen address must be a numeric IPv4 address");
      }
      else if (option == "--port" && i + 1 < argc) {
        const std::string value = argv[++i];
        std::size_t consumed;
        port = std::stoi(value, &consumed);
        if (consumed != value.size() || port < 1024 || port > 65535)
          throw std::invalid_argument("port must be 1024 to 65535");
      } else throw std::invalid_argument("unknown or incomplete argument; use --help");
    }
    SetupStore store(directory);
    // Once claimed, the owner database is authoritative. Removing the bootstrap
    // file must not prevent normal sign-in after a service restart.
    // Keep validating the bootstrap file after the owner claim: it remains the
    // explicit authorization for the AP listener, while SetupAuth receives no
    // token after enrollment and therefore cannot reopen owner creation.
    const auto bootstrap_token = enrollment_token(token_file);
    if (listen_host != "127.0.0.1" && bootstrap_token.empty())
      throw std::invalid_argument("a non-loopback listener requires an enrollment token");
    if (tls_certificate.empty() != tls_private_key.empty())
      throw std::invalid_argument("TLS requires both certificate and private key");
    if (pairing_device_id.empty() != pairing_certificate_fingerprint.empty())
      throw std::invalid_argument("pairing requires both device ID and certificate fingerprint");
    if (!pairing_device_id.empty() && tls_certificate.empty())
      throw std::invalid_argument("pairing requires HTTPS certificate and private key");
    if (!tls_certificate.empty() && pairing_device_id.empty()) {
      pairing_device_id = store.snapshot().at("device_id").get<std::string>();
      pairing_certificate_fingerprint = certificate_fingerprint(tls_certificate);
    }
    std::unique_ptr<PairingCoordinator> pairing;
    if (!pairing_device_id.empty())
      pairing = std::make_unique<PairingCoordinator>(store, pairing_device_id,
                                                      pairing_certificate_fingerprint,
                                                      "/run/rapid/pairing.json",
                                                      "/run/rapid/pairing-approval.json",
                                                      "/run/rapid/pairing-state.json",
                                                      "/run/rapid/pairing-control.json", true);
    const auto token = store.owner_hash().empty() ? bootstrap_token : std::string{};
    SetupAuth auth(store, port, monotonic, token, listen_host, apply_request_file,
                   apply_result_file, firstboot_status_file, wifi_request_file,
                   wifi_result_file, pairing.get(), !tls_certificate.empty(),
                   pairing_certificate_fingerprint, false, calibration_file,
                   calibration_request_file, display_confirm_file);
    if (account_request_file.empty() != account_result_file.empty())
      throw std::invalid_argument("device access requires both account request and result files");
    if (!account_request_file.empty()) auth.set_account_files(account_request_file, account_result_file);
    if (!network_ssid_file.empty()) auth.set_network_ssid_file(network_ssid_file);
    if (!companion_artifact.empty()) auth.set_companion_artifact(companion_artifact);
    if (enroll) {
      std::string password;
      if (!std::getline(std::cin, password) || !auth.enroll(password))
        throw std::runtime_error("owner already configured or password input unavailable");
      std::cout << "Owner configured.\n";
      return 0;
    }
    const auto page = read_file(assets / "setup.html");
    std::signal(SIGINT, [](int) { stopping = true; });
    std::signal(SIGTERM, [](int) { stopping = true; });
    std::signal(SIGPIPE, SIG_IGN);
    const Handler handler = [&](const Request &request) -> Response {
      if (request.method == "GET" && (request.target == "/" || request.target == "/setup"))
        return {200, page, "text/html; charset=utf-8", {
            {"Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"},
            {"X-Content-Type-Options", "nosniff"}, {"Referrer-Policy", "no-referrer"}}};
      return auth.handle(request);
    };
    if (tls_certificate.empty())
      serve(listen_host, port, handler);
    else
      serve_tls(listen_host, port, handler, tls_certificate, tls_private_key);
    return 0;
  } catch (const std::exception &error) {
    log(std::string("ERROR setup: ") + error.what());
    return 1;
  }
}
