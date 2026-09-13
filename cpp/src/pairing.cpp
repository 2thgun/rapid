#include "rapid/pairing.hpp"
#include <cmath>
#include <iomanip>
#include <openssl/crypto.h>
#include <sstream>

namespace rapid::native {
namespace {
bool lower_hex(const std::string &value, std::size_t length) {
  return value.size() == length &&
      value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
void valid_label(const std::string &label) {
  if (label.empty() || label.size() > 64 ||
      label.find_first_of("\r\n") != std::string::npos ||
      label.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid paired PC label");
}
} // namespace

void PairingWindow::clear_pending() {
  if (!pending_) return;
  for (auto *value : {&pending_->transaction_id, &pending_->nonce,
                      &pending_->companion_public_key, &pending_->code}) {
    OPENSSL_cleanse(value->data(), value->size());
    value->clear();
  }
  pending_.reset();
}

PairingWindow::PairingWindow(std::string device_id,
                             std::string certificate_fingerprint)
    : device_id_(std::move(device_id)),
      certificate_fingerprint_(std::move(certificate_fingerprint)) {
  if (!lower_hex(device_id_, 32) || !lower_hex(certificate_fingerprint_, 64))
    throw std::invalid_argument("invalid pairing device identity");
}

PairingWindow::~PairingWindow() { cancel(); }

std::string PairingWindow::verification_code(
    const std::string &device_id, const std::string &certificate_fingerprint,
    const std::string &transaction_id, const std::string &nonce,
    const std::string &companion_public_key) {
  if (!lower_hex(device_id, 32) || !lower_hex(certificate_fingerprint, 64) ||
      !lower_hex(transaction_id, 32) || !lower_hex(nonce, 32) ||
      !lower_hex(companion_public_key, 64))
    throw std::invalid_argument("invalid pairing code inputs");
  const auto digest = hash_text("rapid-pairing-v1|" + device_id + "|" +
                                certificate_fingerprint + "|" + transaction_id +
                                "|" + nonce + "|" + companion_public_key);
  const auto value = std::stoull(digest.substr(0, 16), nullptr, 16) % 100000000ULL;
  std::ostringstream code;
  code << std::setw(8) << std::setfill('0') << value;
  return code.str();
}

void PairingWindow::open(double now) {
  if (!std::isfinite(now)) throw std::invalid_argument("invalid pairing clock");
  window_open_ = true;
  window_expires_at_ = now + 120;
  failures_ = 0;
  clear_pending();
}

void PairingWindow::cancel() {
  clear_pending();
  window_open_ = false;
  window_expires_at_ = 0;
  failures_ = 0;
}

bool PairingWindow::open(double now) const {
  return window_open_ && std::isfinite(now) && now <= window_expires_at_ &&
      failures_ < 5;
}

void PairingWindow::fail(double now, bool erase_pending) {
  ++failures_;
  if (erase_pending)
    clear_pending();
  if (failures_ >= 5 || now > window_expires_at_)
    cancel();
}

PendingPairing PairingWindow::request(const std::string &label,
                                      const std::string &companion_public_key,
                                      double now) {
  if (!open(now)) {
    fail(now);
    throw std::runtime_error("pairing window is closed or expired");
  }
  if (pending_) throw std::runtime_error("pairing request already pending");
  valid_label(label);
  if (!lower_hex(companion_public_key, 64))
    throw std::invalid_argument("invalid companion public key");
  PendingPairing request{unique_id(), unique_id(), label, companion_public_key,
                         {}, now + 120, false};
  request.code = verification_code(device_id_, certificate_fingerprint_,
                                   request.transaction_id, request.nonce,
                                   request.companion_public_key);
  pending_ = request;
  return request;
}

bool PairingWindow::approve(const std::string &transaction_id,
                            const std::string &code, double now) {
  if (!pending_ || pending_->approved || !open(now) || now > pending_->expires_at ||
      transaction_id != pending_->transaction_id || code != pending_->code) {
    fail(now, false);
    return false;
  }
  pending_->approved = true;
  return true;
}

std::optional<PendingPairing> PairingWindow::consume_approved(double now) {
  if (!pending_ || !pending_->approved || !open(now) || now > pending_->expires_at) {
    if (pending_) fail(now);
    return {};
  }
  auto result = std::move(pending_);
  pending_.reset();
  OPENSSL_cleanse(result->code.data(), result->code.size());
  result->code.clear();
  return result;
}

std::optional<PendingPairing> PairingWindow::pending(double now) {
  if (pending_ && (!open(now) || now > pending_->expires_at))
    fail(now);
  return pending_;
}
} // namespace rapid::native
