// Pairing interop test (#14): consumes fixtures written by the real Windows
// companion self-test (companion/native/rapid-telemetry-daemon.cpp,
// write_pairing_interop_fixtures()) and checks them against the real
// rapid-pi pairing code (PairingWindow, PairingCoordinator, PairingTransport,
// seal_pairing_key/open_pairing_key) -- never a re-implementation of the
// crypto or the protocol. See developer/handoffs/2026-09-16-step2.md for the
// interop direction this covers and why.
#include "rapid/pairing.hpp"
#include <fstream>
#include <iostream>
#include <openssl/evp.h>
#include <sstream>

using namespace rapid::native;

namespace {
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}

// Windows fixtures store protocol identifiers (device id, fingerprint,
// transaction id, nonce, the verification code) as their literal text, and
// binary material (keys, envelope fields) as hex-encoded bytes. Both forms
// are read as a single trimmed line.
std::string read_text(const fs::path &root, const char *name) {
  auto text = read_file(root / name);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    text.pop_back();
  return text;
}

std::string hex_to_bytes(const std::string &hex) {
  require(hex.size() % 2 == 0, "fixture hex length");
  std::string bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2)
    bytes += static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16));
  return bytes;
}

std::string read_bytes(const fs::path &root, const char *name) {
  return hex_to_bytes(read_text(root, name));
}

// Mirrors pairing.cpp's private base64_encode(): duplicated here rather than
// exported from pairing.hpp, since only this test needs to turn raw fixture
// bytes into the base64 fields PairingEnvelope carries.
std::string base64_encode(const std::string &value) {
  std::string result(4 * ((value.size() + 2) / 3), '\0');
  const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(result.data()),
                                    reinterpret_cast<const unsigned char *>(value.data()),
                                    static_cast<int>(value.size()));
  result.resize(static_cast<std::size_t>(size));
  return result;
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "Windows pairing fixture directory required");
    const fs::path root = argv[1];

    const auto device_id = read_text(root, "device-id.hex");
    const auto fingerprint = read_text(root, "certificate-fingerprint.hex");
    const auto transaction_id = read_text(root, "transaction-id.hex");
    const auto nonce = read_text(root, "nonce.hex");
    const auto verification_code = read_text(root, "verification-code.txt");
    const auto companion_public_key = read_text(root, "companion-public-key.hex");
    const auto companion_private_key = read_text(root, "companion-private-key.hex");
    const auto pi_ephemeral_public_key = read_text(root, "pi-ephemeral-public-key.hex");
    const auto envelope_nonce = read_bytes(root, "envelope-nonce.hex");
    const auto envelope_ciphertext = read_bytes(root, "envelope-ciphertext.hex");
    const auto envelope_tag = read_bytes(root, "envelope-tag.hex");
    const auto envelope_plaintext_hex = read_text(root, "envelope-plaintext.hex");

    // 1. Verification code: the driver compares this on the Pi panel and in
    // the companion, so the two independently-implemented algorithms
    // (Windows CNG SHA-256, Linux OpenSSL SHA-256) must agree bit-for-bit
    // for the real Windows-generated public key and identifiers.
    require(PairingWindow::verification_code(device_id, fingerprint, transaction_id,
                                              nonce, companion_public_key) == verification_code,
            "Linux verification code must match the real Windows CNG code");

    // Negative: wrong certificate fingerprint (a spoofed or MITM Pi) must
    // change the code visibly, rather than silently matching.
    const std::string wrong_fingerprint(64, '9');
    require(wrong_fingerprint != fingerprint, "fixture fingerprint sanity");
    require(PairingWindow::verification_code(device_id, wrong_fingerprint, transaction_id,
                                              nonce, companion_public_key) != verification_code,
            "wrong certificate fingerprint must change the verification code");

    // 2. Envelope round trip: an envelope encrypted with real Windows CNG
    // Curve25519 + HKDF-SHA256 + AES-256-GCM must decrypt with the Linux
    // OpenSSL companion-side implementation (open_pairing_key) and recover
    // the same plaintext, proving the wire format (salt, AAD, nonce and tag
    // conventions) matches across the two independent implementations.
    PairingEnvelope envelope{pi_ephemeral_public_key, base64_encode(envelope_nonce),
                             base64_encode(envelope_ciphertext), base64_encode(envelope_tag)};
    const auto opened = open_pairing_key(device_id, transaction_id, nonce,
                                         companion_private_key, envelope);
    require(opened == envelope_plaintext_hex,
            "Linux must decrypt the real Windows-produced pairing envelope");

    // Negative: a tampered ciphertext must fail AES-GCM authentication.
    {
      auto tampered = envelope;
      auto raw = envelope_ciphertext;
      raw[0] = static_cast<char>(raw[0] ^ 0x01);
      tampered.ciphertext = base64_encode(raw);
      bool rejected = false;
      try { (void)open_pairing_key(device_id, transaction_id, nonce, companion_private_key, tampered); }
      catch (const std::exception &) { rejected = true; }
      require(rejected, "a tampered Windows envelope must be rejected");
    }

    // Negative: a mismatched transaction id changes both the HKDF salt and
    // the AEAD associated data, so decryption must fail closed.
    {
      const auto &wrong_transaction = nonce; // any other well-formed 32 hex chars
      require(wrong_transaction != transaction_id, "fixture transaction/nonce must differ");
      bool rejected = false;
      try { (void)open_pairing_key(device_id, wrong_transaction, nonce, companion_private_key, envelope); }
      catch (const std::exception &) { rejected = true; }
      require(rejected, "a mismatched transaction id must be rejected");
    }

    // 3. Feed the real Windows public key and identifiers through the real
    // rapid-pi pairing listener (PairingCoordinator, the class the setup
    // HTTPS service uses), not just the free crypto functions, and confirm
    // it accepts the real wire format and its own round trip still works
    // end to end with the real Windows companion key.
    const auto store_path = fs::temp_directory_path() / ("rapid-pairing-interop-" + unique_id());
    SetupStore store(store_path);
    PairingCoordinator coordinator(store, device_id, fingerprint);
    coordinator.open(0);
    const auto pending = coordinator.request("Windows self-test PC", companion_public_key, 1);
    require(pending.code == PairingWindow::verification_code(device_id, fingerprint,
                pending.transaction_id, pending.nonce, companion_public_key),
            "rapid-pi listener computes the same verification code for the real Windows key");
    require(coordinator.approve(pending.transaction_id, pending.code, 2),
            "rapid-pi listener approves the real Windows key with its own verification code");
    const auto completed = coordinator.consume(2);
    require(completed.has_value(), "rapid-pi listener completes pairing for the real Windows key");
    require(open_pairing_key(device_id, pending.transaction_id, pending.nonce,
                             companion_private_key, completed->envelope).size() == 64,
            "the real Windows private key decrypts the envelope rapid-pi's own listener sealed");
    std::error_code cleanup_error;
    fs::remove_all(store_path, cleanup_error);

    std::cout << "Pairing interop: Windows-generated request, verification code and envelope "
                 "verified against the real rapid-pi pairing code, including tampered-envelope, "
                 "mismatched-transaction and wrong-fingerprint rejections\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "pairing interop test failed: " << error.what() << '\n';
    return 1;
  }
}
