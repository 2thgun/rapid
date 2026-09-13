#pragma once
#include "rapid/native.hpp"
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

class PairingWindow {
  std::string device_id_, certificate_fingerprint_;
  double window_expires_at_ = 0;
  int failures_ = 0;
  bool window_open_ = false;
  std::optional<PendingPairing> pending_;
  void fail(double now);

public:
  PairingWindow(std::string device_id, std::string certificate_fingerprint);
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
  bool open(double now) const;
};
} // namespace rapid::native
