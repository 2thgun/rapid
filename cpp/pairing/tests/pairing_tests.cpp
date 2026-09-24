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
// #71: the companion polls for the approved envelope for kPairingPollSeconds;
// it must outlast the Pi's request lifetime, or it gives up while the Pi would
// still accept the approval. The companion is a separate Windows build that
// cannot include pairing.hpp, so this reads its constant from source.
void companion_outlasts_request(const fs::path &companion_source) {
  const auto source = read_file(companion_source);
  const std::string marker = "kPairingPollSeconds = ";
  const auto at = source.find(marker);
  require(at != std::string::npos, "#71: companion declares kPairingPollSeconds");
  const int poll = std::stoi(source.substr(at + marker.size()));
  require(poll >= kPairingRequestSeconds + 30,
          "#71: companion poll budget exceeds the Pi request lifetime by at least 30 s");
}
int main(int argc, char **argv) {
  try {
    if (argc == 2) companion_outlasts_request(argv[1]);
    else std::cout << "Companion source not in this tree; poll-budget check not run\n";
    // RFC 7748 Section 6.1 X25519 known-answer test, independent of
    // OpenSSL/Windows-CNG interop (#14): pins this Pi-side implementation's
    // byte order against the published test vector, so a regression here is
    // caught even if the Windows self-test's own RFC KAT and the cross-
    // platform fixtures were somehow both broken the same way. Values are
    // the RFC's own Alice/Bob keys, never a real device identity.
    const std::string rfc_alice_private =
        "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
    const std::string rfc_alice_public =
        "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
    const std::string rfc_bob_private =
        "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
    const std::string rfc_bob_public =
        "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
    const std::string rfc_shared_secret =
        "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";
    require(x25519_shared_secret(rfc_alice_private, rfc_bob_public) == rfc_shared_secret,
            "RFC 7748 6.1 X25519 known-answer vector (Alice private x Bob public)");
    require(x25519_shared_secret(rfc_bob_private, rfc_alice_public) == rfc_shared_secret,
            "RFC 7748 6.1 X25519 known-answer vector (Bob private x Alice public)");

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
    { std::ofstream extra(approval_file);
      extra << Json{{"transaction_id", coordinated.transaction_id},
                    {"code", coordinated.code}, {"extra", true}}.dump(); }
    require(coordinator.pending(2) && !coordinator.pending(2)->approved &&
                !fs::exists(approval_file),
            "extra panel approval fields are discarded without changing request");
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
    coordinator.open(5);
    PairingTransport transport(coordinator, [] { return 6.0; });
    Request malformed_transport{"POST", "/api/v1/pairing/request", "{}", {}};
    require(transport.handle(malformed_transport).status == 415,
            "core pairing transport requires JSON without setup auth ownership");
    Request transport_request{"POST", "/api/v1/pairing/request",
                              Json{{"label", "Main Program PC"},
                                   {"companion_public_key", derive_public_key(private_key)}}.dump(),
                              {{"content-type", "application/json"}}};
    const auto transport_created = transport.handle(transport_request);
    const auto transport_json = Json::parse(transport_created.body);
    require(transport_created.status == 201 &&
                transport_json["transaction_id"].is_string() &&
                !transport_json.contains("telemetry_key"),
            "core pairing transport creates a key-free companion request");
    require(coordinator.approve(transport_json["transaction_id"],
                                coordinator.pending(6)->code, 6),
            "main program coordinator accepts local approval for transport request");
    Request transport_result{"GET", "/api/v1/pairing/result?transaction_id=" +
                                  transport_json["transaction_id"].get<std::string>(), "", {}};
    const auto transport_completed = transport.handle(transport_result);
    require(transport_completed.status == 200 &&
                !Json::parse(transport_completed.body).contains("telemetry_key"),
            "core pairing transport delivers only an encrypted envelope");
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
    require(!coordinator.pending(21 + kPairingRequestSeconds + 1) && !fs::exists(panel_file),
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
    const double expired = 31 + kPairingRequestSeconds + 1;
    require(!window.approve(pending.transaction_id, pending.code, expired) && !window.active(expired),
            "expired transaction closes pairing window");
    // #71: a person needs time to walk to the PC, type the address, then read
    // and compare the code. One minute of typing then three minutes of
    // comparing must still pair, and a request that arrives just before the
    // window would close still gets its whole approval time.
    window.open(1000);
    pending = window.request("Slow PC", public_key, 1060);
    require(window.approve(pending.transaction_id, pending.code, 1060 + 180),
            "#71: approval three minutes after a request one minute into the window succeeds");
    require(window.consume_approved(1240).has_value(), "#71: the slow approval is consumed");
    window.open(2000);
    pending = window.request("Late PC", public_key, 2000 + kPairingWindowSeconds - 5);
    require(pending.expires_at == 2000 + kPairingWindowSeconds - 5 + kPairingRequestSeconds,
            "#71: the request carries its full approval time");
    require(window.active(pending.expires_at) && window.pending(pending.expires_at - 1),
            "#71: a late request keeps the window open until it expires");
    require(window.approve(pending.transaction_id, pending.code, pending.expires_at - 1),
            "#71: a late request can still be approved near the end of its own time");
    window.cancel();
    window.open(200);
    pending = window.request("Driver PC", public_key, 201);
    require(window.approve(pending.transaction_id, pending.code, 201),
            "matching physical approval is accepted");
    require(window.consume_approved(201)->transaction_id == pending.transaction_id &&
                !window.consume_approved(201),
            "approved transaction is consumed exactly once");
    window.cancel();
    require(!window.pending(201) && !window.active(201), "cancel clears pairing state");
    const auto ipc_root = fs::temp_directory_path() / ("rapid-pairing-ipc-" + unique_id());
    const auto ipc_panel = ipc_root / "panel.json";
    const auto ipc_approval = ipc_root / "approval.json";
    const auto ipc_state = ipc_root / "state.json";
    const auto ipc_control = ipc_root / "control.json";
    SetupStore ipc_store(ipc_root);
    PairingCoordinator ipc_coordinator(ipc_store, device, certificate, ipc_panel,
                                       ipc_approval, ipc_state, ipc_control);
    atomic_file(ipc_control, Json{{"action", "open"}}.dump());
    const auto ipc_request = ipc_coordinator.request("IPC PC", public_key, 1);
    require(Json::parse(read_file(ipc_state))["active"] == true &&
                Json::parse(read_file(ipc_state))["pending"] == true &&
                ipc_request.code.size() == 8,
            "main-program coordinator consumes private setup control handoff");
    atomic_file(ipc_control, Json{{"action", "cancel"}}.dump());
    require(!ipc_coordinator.pending(2) &&
                Json::parse(read_file(ipc_state))["active"] == false,
            "main-program coordinator consumes setup cancellation handoff");
    fs::remove_all(ipc_root, cleanup_error);
    std::cout << "Pairing window: code, expiry, cancellation and failure limits passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
