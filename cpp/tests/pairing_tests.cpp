#include "rapid/pairing.hpp"
#include <iostream>
#include <iomanip>
#include <openssl/evp.h>
#include <sstream>

using namespace rapid::native;
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
std::string derive_public_key(const std::string &private_hex) {
  std::string bytes(32, '\0');
  for (std::size_t i = 0; i < 32; ++i)
    bytes[i] = static_cast<char>(std::stoi(private_hex.substr(i * 2, 2), nullptr, 16));
  EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
      reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
  std::string result(32, '\0'); std::size_t size = result.size();
  require(key && EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char *>(result.data()), &size) > 0,
          "derive test X25519 public key");
  EVP_PKEY_free(key);
  std::ostringstream out;
  for (unsigned char byte : result)
    out << std::hex << std::setw(2) << std::setfill('0') << int(byte);
  return out.str();
}
int main() {
  try {
    const std::string device(32, 'a'), certificate(64, 'b'), public_key(64, 'c');
    const auto code = PairingWindow::verification_code(device, certificate,
        std::string(32, 'd'), std::string(32, 'e'), public_key);
    require(code.size() == 8 && code.find_first_not_of("0123456789") == std::string::npos &&
                code == PairingWindow::verification_code(device, certificate,
                    std::string(32, 'd'), std::string(32, 'e'), public_key),
            "verification code is deterministic and eight numeric digits");
    const std::string private_key(64, '1');
    const auto sealed = seal_pairing_key(device, std::string(32, 'd'),
                                         std::string(32, 'e'), derive_public_key(private_key));
    require(sealed.telemetry_key.size() == 64 && sealed.envelope.tag.size() > 0 &&
                open_pairing_key(device, std::string(32, 'd'), std::string(32, 'e'),
                                 private_key, sealed.envelope) == sealed.telemetry_key,
            "X25519 HKDF AES-GCM pairing envelope round trips");
    auto tampered = sealed.envelope;
    tampered.ciphertext[0] = tampered.ciphertext[0] == 'A' ? 'B' : 'A';
    bool rejected = false;
    try { (void)open_pairing_key(device, std::string(32, 'd'), std::string(32, 'e'), private_key, tampered); }
    catch (const std::exception &) { rejected = true; }
    require(rejected, "tampered pairing envelope is rejected");
    PairingWindow window(device, certificate);
    require(!window.active(0), "pairing starts physically closed");
    window.open(10);
    auto pending = window.request("Driver PC", public_key, 11);
    require(window.pending(11)->code == pending.code && !window.approve(pending.transaction_id,
                "00000000", 11) && window.pending(11),
            "only the displayed transaction code approves");
    require(window.pending(11).has_value(),
            "wrong verification code leaves pending material available");
    for (int i = 0; i < 4; ++i)
      require(!window.approve(pending.transaction_id, "00000000", 12 + i),
              "failed approval is bounded");
    require(!window.active(20), "five failures close physical pairing window");
    require(!window.pending(20), "five failures erase pending material");
    window.open(30);
    pending = window.request("Driver PC", public_key, 31);
    require(!window.approve(pending.transaction_id, pending.code, 152) && !window.active(152),
            "expired transaction closes pairing window");
    window.open(200);
    pending = window.request("Driver PC", public_key, 201);
    require(window.approve(pending.transaction_id, pending.code, 201),
            "matching physical approval is accepted");
    require(window.consume_approved(201)->transaction_id == pending.transaction_id &&
                !window.consume_approved(201),
            "approved transaction is consumed exactly once");
    window.cancel();
    require(!window.pending(201) && !window.active(201), "cancel clears pairing state");
    std::cout << "Pairing window: code, expiry, cancellation and failure limits passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
