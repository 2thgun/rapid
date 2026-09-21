#include "rapid/setup.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace rapid::native;
namespace {
struct OpenSslCleanup {
  EVP_PKEY *key = nullptr; X509 *certificate = nullptr;
  ~OpenSslCleanup() { EVP_PKEY_free(key); X509_free(certificate); }
};

void provision_certificate(const fs::path &key_path, const fs::path &certificate_path,
                           const std::string &device_id, const std::string &address) {
  const bool have_key = fs::exists(key_path), have_certificate = fs::exists(certificate_path);
  if (have_key || have_certificate) {
    if (!have_key || !have_certificate)
      throw std::runtime_error("TLS certificate and private key must be provisioned together");
    return;
  }
  OpenSslCleanup material;
  EVP_PKEY_CTX *key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  if (!key_context || EVP_PKEY_keygen_init(key_context) <= 0 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048) <= 0 ||
      EVP_PKEY_keygen(key_context, &material.key) <= 0) {
    EVP_PKEY_CTX_free(key_context); throw std::runtime_error("cannot generate TLS private key");
  }
  EVP_PKEY_CTX_free(key_context);
  material.certificate = X509_new();
  if (!material.certificate || X509_set_version(material.certificate, 2) != 1 ||
      ASN1_INTEGER_set(X509_get_serialNumber(material.certificate), 1) != 1 ||
      !X509_gmtime_adj(X509_get_notBefore(material.certificate), 0) ||
      !X509_gmtime_adj(X509_get_notAfter(material.certificate), 60L * 60L * 24L * 365L) ||
      X509_set_pubkey(material.certificate, material.key) != 1)
    throw std::runtime_error("cannot create TLS certificate");
  X509_NAME *name = X509_get_subject_name(material.certificate);
  if (!name || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                           reinterpret_cast<const unsigned char *>(device_id.c_str()),
                                           -1, -1, 0) != 1 ||
      X509_set_issuer_name(material.certificate, name) != 1)
    throw std::runtime_error("cannot set TLS certificate identity");
  X509V3_CTX extension_context;
  X509V3_set_ctx_nodb(&extension_context);
  X509V3_set_ctx(&extension_context, material.certificate, material.certificate, nullptr, nullptr, 0);
  const std::string san = "IP:" + address;
  X509_EXTENSION *extension = X509V3_EXT_conf_nid(nullptr, &extension_context, NID_subject_alt_name, san.c_str());
  if (!extension || X509_add_ext(material.certificate, extension, -1) != 1) {
    X509_EXTENSION_free(extension); throw std::runtime_error("cannot set TLS certificate address");
  }
  X509_EXTENSION_free(extension);
  if (X509_sign(material.certificate, material.key, EVP_sha256()) <= 0)
    throw std::runtime_error("cannot sign TLS certificate");
  BIO *key_bio = BIO_new(BIO_s_mem()), *certificate_bio = BIO_new(BIO_s_mem());
  if (!key_bio || !certificate_bio || PEM_write_bio_PrivateKey(key_bio, material.key, nullptr, nullptr, 0, nullptr, nullptr) != 1 ||
      PEM_write_bio_X509(certificate_bio, material.certificate) != 1) {
    BIO_free(key_bio); BIO_free(certificate_bio); throw std::runtime_error("cannot serialize TLS certificate");
  }
  auto read_bio = [](BIO *bio) {
    char *data = nullptr; const long length = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<std::size_t>(length));
  };
  const auto key_text = read_bio(key_bio), certificate_text = read_bio(certificate_bio);
  BIO_free(key_bio); BIO_free(certificate_bio);
  // #31: the private key is key material, created 0600 from the first byte
  // rather than chmod'ed after creation; the certificate is public.
  atomic_file(key_path, key_text, fs::perms::owner_read | fs::perms::owner_write);
  atomic_file(certificate_path, certificate_text,
              fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                  fs::perms::others_read);
}

std::string certificate_fingerprint(const fs::path &path) {
  FILE *file = ::fopen(path.c_str(), "rb");
  if (!file) throw std::runtime_error("cannot open TLS certificate for fingerprinting");
  X509 *certificate = PEM_read_X509(file, nullptr, nullptr, nullptr);
  ::fclose(file);
  if (!certificate) throw std::runtime_error("cannot parse TLS certificate for fingerprinting");
  unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int length = 0;
  const bool valid = X509_digest(certificate, EVP_sha256(), digest, &length) == 1;
  X509_free(certificate);
  if (!valid || length != 32) throw std::runtime_error("cannot fingerprint TLS certificate");
  std::ostringstream result;
  for (unsigned int i = 0; i < length; ++i)
    result << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(digest[i]);
  return result.str();
}

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
    fs::path directory, status_file, token_file, tls_certificate, tls_private_key;
    std::string address = "192.168.1.64";
    int port = 8002;
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "rapid-firstboot --state-directory PATH [--status-file PATH] "
                     "[--enrollment-token-file PATH] [--tls-certificate PATH] [--tls-private-key PATH] [--setup-address IPV4] [--setup-port PORT]\n"
                     "Initializes private device state, AP credentials and a device TLS identity.\n";
        return 0;
      }
      if (option == "--state-directory" && i + 1 < argc) directory = argv[++i];
      else if (option == "--status-file" && i + 1 < argc) status_file = argv[++i];
      else if (option == "--enrollment-token-file" && i + 1 < argc) token_file = argv[++i];
      else if (option == "--tls-certificate" && i + 1 < argc) tls_certificate = argv[++i];
      else if (option == "--tls-private-key" && i + 1 < argc) tls_private_key = argv[++i];
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
    // #22: the setup AP is now the fixed open network "rapid" with no
    // per-device passphrase; a device upgraded from the old scheme may still
    // carry its generated AP password on disk. Drop it rather than leave a
    // stale secret sitting unused in private state.
    {
      std::error_code error;
      fs::remove(directory / "ap-password", error);
    }
    if (tls_certificate.empty()) tls_certificate = directory / "device.crt";
    if (tls_private_key.empty()) tls_private_key = directory / "device.key";
    provision_certificate(tls_private_key, tls_certificate,
                          store.snapshot().at("device_id").get<std::string>(), address);
    const bool owner_configured = !store.owner_hash().empty();
    auto status = provisioning_status(store.snapshot(), owner_configured);
    // The status file is consumed locally by the AP provisioner and physical
    // panel. It is never an HTTP response or journal entry.
    //
    // #59 / setup-page-ux: the panel must be able to show the open setup AP's
    // name, address and TLS fingerprint even on an enrolled device, because the
    // owner can still choose Access Point mode (or recovery can restore it)
    // after setup. The activation token is the only secret here and is
    // published only while the owner is unenrolled, so a later boot of a
    // configured device cannot resurrect owner enrollment.
    status["bootstrap"] = {{"setup_address", address}, {"setup_port", port},
                           {"setup_url", "https://" + address + ":" + std::to_string(port) + "/setup"},
                           {"certificate_fingerprint", certificate_fingerprint(tls_certificate)}};
    if (!owner_configured) {
      if (token_file.empty()) token_file = directory / "enrollment.token";
      // #22: the AP's SSID (fixed "rapid", or a disambiguated "rapid-NNNN" if
      // another one is already in range) is resolved by the privileged
      // provisioner, which alone can scan for it; it is published to the
      // panel from its own file, not from this status document.
      status["bootstrap"]["activation_token"] =
          private_hex_secret(token_file, 64, "activation token");
    }
    if (!status_file.empty()) {
      // #31: can carry the bootstrap activation token, so it is created
      // group-rapid readable (for the privileged provisioner) but never
      // group/other writable, from the first byte.
      atomic_file(status_file, status.dump() + "\n",
                  fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
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
