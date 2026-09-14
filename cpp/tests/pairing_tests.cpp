#include "rapid/pairing.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>
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
    const auto store_path = fs::temp_directory_path() / ("rapid-pairing-coordinator-" + unique_id());
    const auto panel_file = store_path / "pairing.json";
    const auto approval_file = store_path / "pairing-approval.json";
    SetupStore store(store_path);
    atomic_file(panel_file, "stale panel");
    atomic_file(approval_file, "stale approval");
    PairingCoordinator coordinator(store, device, certificate, panel_file, approval_file);
    require(!fs::exists(panel_file) && !fs::exists(approval_file),
            "pairing restart clears stale panel handoffs");
    coordinator.open(0);
    const auto coordinated = coordinator.request("Driver PC", derive_public_key(private_key), 1);
    require(fs::exists(panel_file), "pairing panel state is published");
    const auto panel_permissions = fs::status(panel_file).permissions();
    require((panel_permissions & fs::perms::others_read) == fs::perms::none &&
                (panel_permissions & fs::perms::others_write) == fs::perms::none,
            "pairing panel state is not world-readable");
    const auto panel = Json::parse(read_file(panel_file));
    require(panel["transaction_id"] == coordinated.transaction_id &&
                panel["label"] == "Driver PC" && panel["code"] == coordinated.code &&
                !panel.contains("companion_public_key") && !panel.contains("telemetry_key"),
            "pairing panel state contains display metadata without secrets");
    { std::ofstream malformed(approval_file);
      malformed << "{\"transaction_id\":\"not-a-transaction\",\"code\":\"bad\"}"; }
    require(coordinator.pending(2) && !coordinator.pending(2)->approved &&
                !fs::exists(approval_file),
            "malformed panel approval is discarded without changing request");
    { std::ofstream approval(approval_file);
      approval << Json{{"transaction_id", coordinated.transaction_id},
                       {"code", coordinated.code}}.dump(); }
    require(coordinator.pending(2)->approved && !fs::exists(approval_file),
            "coordinator accepts a physical panel approval");
    const auto completed = coordinator.consume(2);
    require(completed && completed->peer_id.size() == 32 && store.peers().size() == 1 &&
                open_pairing_key(device, coordinated.transaction_id, coordinated.nonce,
                                 private_key, completed->envelope).size() == 64,
            "approved pairing stores one private peer and returns its envelope");
    require(!fs::exists(panel_file), "pairing panel state clears after consumption");
    require(!coordinator.consume(2), "coordinator consumes an approved request once");
    coordinator.open(10);
    (void)coordinator.request("Expiring PC", derive_public_key(private_key), 11);
    { std::ofstream approval(approval_file);
      approval << Json{{"transaction_id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                       {"code", "00000000"}}.dump(); }
    coordinator.cancel();
    require(!fs::exists(panel_file) && !fs::exists(approval_file),
            "cancelling pairing clears queued panel approval");
    coordinator.open(20);
    (void)coordinator.request("Expiring PC", derive_public_key(private_key), 21);
    require(!coordinator.pending(142) && !fs::exists(panel_file),
            "expired pairing clears the panel state");
    std::error_code cleanup_error;
    fs::remove_all(store_path, cleanup_error);
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
