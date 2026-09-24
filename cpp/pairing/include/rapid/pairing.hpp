#pragma once
#include "rapid/native.hpp"
#include "rapid/setup.hpp"
#include <optional>

namespace rapid::native {
// #71: the one pairing timeline. The window gives the owner time to walk to the
// PC and type the address; a request then gets its own full approval time, and
// the window is stretched to cover it, so time spent typing never eats into the
// time left to compare and approve. The companion's poll budget
// (kPairingPollSeconds in rapid-telemetry-daemon.cpp) must exceed the request
// lifetime; rapid-pairing-tests enforces that.
inline constexpr double kPairingWindowSeconds = 300;
inline constexpr double kPairingRequestSeconds = 300;

// Private control-plane state for the physical pairing window. It intentionally
// has no HTTP dependency: only the future panel/HTTPS service may decide where
// pending details are displayed or submitted.
struct PendingPairing {
  std::string transaction_id, nonce, label, companion_public_key, code;
  double expires_at = 0;
  bool approved = false;
};

struct PairingEnvelope {
  std::string ephemeral_public_key, nonce, ciphertext, tag;
};

struct SealedPairingKey {
  std::string telemetry_key;
  PairingEnvelope envelope;
};

SealedPairingKey seal_pairing_key(const std::string &device_id,
                                  const std::string &transaction_id,
                                  const std::string &nonce,
                                  const std::string &companion_public_key);
std::string open_pairing_key(const std::string &device_id,
                             const std::string &transaction_id,
                             const std::string &nonce,
                             const std::string &companion_private_key,
                             const PairingEnvelope &envelope);

// Raw X25519 ECDH, no HKDF/AEAD framing on top: the exact primitive
// seal_pairing_key()/open_pairing_key() use internally to derive their
// shared secret. Exposed only so an RFC 7748 Section 6.1 known-answer test
// (#14) can pin this Pi/OpenSSL implementation's byte order independently of
// the Windows CNG side, which needs a byte-reversal OpenSSL does not
// (rapid-telemetry-daemon.cpp's decrypt_pairing_envelope()). Never call this
// from wire-handling code; it has none of seal/open's input validation.
std::string x25519_shared_secret(const std::string &private_key,
                                 const std::string &public_key);

class PairingWindow {
  std::string device_id_, certificate_fingerprint_;
  double window_expires_at_ = 0;
  int failures_ = 0;
  bool window_open_ = false;
  std::optional<PendingPairing> pending_;
  void clear_pending();
  void fail(double now, bool erase_pending = true);

public:
  PairingWindow(std::string device_id, std::string certificate_fingerprint);
  ~PairingWindow();
  PairingWindow(const PairingWindow &) = delete;
  PairingWindow &operator=(const PairingWindow &) = delete;
  static std::string verification_code(const std::string &device_id,
                                       const std::string &certificate_fingerprint,
                                       const std::string &transaction_id,
                                       const std::string &nonce,
                                       const std::string &companion_public_key);
  void open(double now);
  void cancel();
  PendingPairing request(const std::string &label,
                         const std::string &companion_public_key, double now);
  bool approve(const std::string &transaction_id, const std::string &code,
               double now);
  std::optional<PendingPairing> consume_approved(double now);
  std::optional<PendingPairing> pending(double now);
  bool active(double now) const;
};

struct CompletedPairing {
  std::string peer_id, label;
  PairingEnvelope envelope;
};

// Couples the in-memory approval window to private peer storage. Transport
// layers should call consume() only after local approval has succeeded.
class PairingCoordinator {
  SetupStore &store_;
  mutable std::mutex mutex_;
  std::string device_id_;
  PairingWindow window_;
  fs::path panel_file_;
  fs::path state_file_;
  fs::path control_file_;
  fs::path panel_approval_file_;
  bool control_proxy_ = false;
  void publish_panel(double now);
  void apply_control(double now);
  void apply_panel_approval(double now);
  std::optional<CompletedPairing> consume_impl(
      double now, const std::string &expected_transaction);

public:
  PairingCoordinator(SetupStore &store, std::string device_id,
                     std::string certificate_fingerprint, fs::path panel_file = {},
                     fs::path panel_approval_file = {}, fs::path state_file = {},
                     fs::path control_file = {}, bool control_proxy = false);
  void open(double now);
  void cancel();
  PendingPairing request(const std::string &label,
                         const std::string &companion_public_key, double now);
  bool approve(const std::string &transaction_id, const std::string &code,
               double now);
  std::optional<CompletedPairing> consume(double now);
  std::optional<CompletedPairing> consume(double now,
                                          const std::string &transaction_id);
  std::optional<PendingPairing> pending(double now);
  bool active(double now) const;
};

// HTTPS is supplied by the hosting main program. This transport owns only the
// companion request/result protocol, so first-time onboarding never needs to
// issue a telemetry credential itself.
class PairingTransport {
  PairingCoordinator &pairing_;
  std::function<double()> clock_;

public:
  PairingTransport(PairingCoordinator &pairing,
                   std::function<double()> clock = monotonic);
  static bool handles(const Request &request);
  Response handle(const Request &request);
};
} // namespace rapid::native
