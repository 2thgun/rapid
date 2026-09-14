#pragma once
#include "rapid/native.hpp"
#include "rapid/setup.hpp"
#include <optional>

namespace rapid::native {
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
  fs::path panel_approval_file_;
  void publish_panel(double now);
  void apply_panel_approval(double now);
  std::optional<CompletedPairing> consume_impl(
      double now, const std::string &expected_transaction);

public:
  PairingCoordinator(SetupStore &store, std::string device_id,
                     std::string certificate_fingerprint, fs::path panel_file = {},
                     fs::path panel_approval_file = {});
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
} // namespace rapid::native
