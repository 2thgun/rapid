#include "rapid/pairing.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <sstream>
#include <sys/stat.h>

namespace rapid::native {
namespace {
bool lower_hex(const std::string &value, std::size_t length) {
  return value.size() == length &&
      value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool secret_equal(const std::string &left, const std::string &right) {
  return left.size() == right.size() &&
      CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}
std::string hex_bytes(const std::string &value, std::size_t bytes) {
  if (value.size() != bytes * 2 ||
      value.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid pairing hexadecimal value");
  std::string result(bytes, '\0');
  for (std::size_t i = 0; i < bytes; ++i)
    result[i] = static_cast<char>(std::stoi(value.substr(i * 2, 2), nullptr, 16));
  return result;
}
std::string hex_text(const std::string &value) {
  std::ostringstream out;
  for (unsigned char byte : value)
    out << std::hex << std::setw(2) << std::setfill('0') << int(byte);
  return out.str();
}
std::string base64_encode(const std::string &value) {
  std::string result(4 * ((value.size() + 2) / 3), '\0');
  const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(result.data()),
                                    reinterpret_cast<const unsigned char *>(value.data()),
                                    static_cast<int>(value.size()));
  result.resize(size);
  return result;
}
std::string base64_decode(const std::string &value) {
  if (value.empty() || value.size() % 4 != 0)
    throw std::invalid_argument("invalid pairing envelope encoding");
  std::string result(3 * value.size() / 4, '\0');
  const auto size = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(result.data()),
                                    reinterpret_cast<const unsigned char *>(value.data()),
                                    static_cast<int>(value.size()));
  if (size < 0) throw std::invalid_argument("invalid pairing envelope encoding");
  const std::size_t padding = value.ends_with("==") ? 2 : value.ends_with('=') ? 1 : 0;
  result.resize(static_cast<std::size_t>(size) - padding);
  return result;
}
std::string random_bytes(std::size_t size) {
  std::string value(size, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char *>(value.data()), static_cast<int>(size)) != 1)
    throw std::runtime_error("pairing random generation failed");
  return value;
}
std::string shared_secret(const std::string &private_key, const std::string &public_key) {
  EVP_PKEY *private_pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
      reinterpret_cast<const unsigned char *>(private_key.data()), private_key.size());
  EVP_PKEY *public_pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
      reinterpret_cast<const unsigned char *>(public_key.data()), public_key.size());
  if (!private_pkey || !public_pkey) {
    EVP_PKEY_free(private_pkey); EVP_PKEY_free(public_pkey);
    throw std::runtime_error("pairing key construction failed");
  }
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(private_pkey, nullptr);
  std::string result(32, '\0');
  std::size_t size = result.size();
  if (!ctx || EVP_PKEY_derive_init(ctx) <= 0 || EVP_PKEY_derive_set_peer(ctx, public_pkey) <= 0 ||
      EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char *>(result.data()), &size) <= 0 || size != result.size()) {
    EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(private_pkey); EVP_PKEY_free(public_pkey);
    throw std::runtime_error("pairing shared-secret derivation failed");
  }
  EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(private_pkey); EVP_PKEY_free(public_pkey);
  return result;
}
std::string hkdf(const std::string &secret, const std::string &salt) {
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  std::string result(32, '\0');
  std::size_t size = result.size();
  const std::string info = "rapid-pairing-envelope-v1";
  if (!ctx || EVP_PKEY_derive_init(ctx) <= 0 || EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_salt(ctx, reinterpret_cast<const unsigned char *>(salt.data()), static_cast<int>(salt.size())) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_key(ctx, reinterpret_cast<const unsigned char *>(secret.data()), static_cast<int>(secret.size())) <= 0 ||
      EVP_PKEY_CTX_add1_hkdf_info(ctx, reinterpret_cast<const unsigned char *>(info.data()), static_cast<int>(info.size())) <= 0 ||
      EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char *>(result.data()), &size) <= 0 || size != result.size()) {
    EVP_PKEY_CTX_free(ctx); throw std::runtime_error("pairing HKDF failed");
  }
  EVP_PKEY_CTX_free(ctx);
  return result;
}
std::string envelope_aad(const std::string &device_id, const std::string &transaction_id,
                         const std::string &nonce, const std::string &companion_public_key,
                         const std::string &ephemeral_public_key) {
  return "rapid-pairing-envelope-v1|" + device_id + "|" + transaction_id + "|" +
         nonce + "|" + companion_public_key + "|" + ephemeral_public_key;
}
std::string gcm(bool encrypt, const std::string &key, const std::string &iv,
                const std::string &aad, const std::string &input, std::string *tag) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  std::string output(input.size() + 16, '\0');
  int length = 0, total = 0;
  bool ok = ctx && EVP_CipherInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr,
                                     encrypt ? 1 : 0) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) == 1 &&
      EVP_CipherInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char *>(key.data()),
                        reinterpret_cast<const unsigned char *>(iv.data()), -1) == 1 &&
      EVP_CipherUpdate(ctx, nullptr, &length, reinterpret_cast<const unsigned char *>(aad.data()),
                       static_cast<int>(aad.size())) == 1 &&
      EVP_CipherUpdate(ctx, reinterpret_cast<unsigned char *>(output.data()), &length,
                       reinterpret_cast<const unsigned char *>(input.data()), static_cast<int>(input.size())) == 1;
  if (ok) total = length;
  if (!encrypt && ok)
    ok = tag && tag->size() == 16 && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag->data()) == 1;
  if (ok && EVP_CipherFinal_ex(ctx, reinterpret_cast<unsigned char *>(output.data()) + total, &length) == 1) {
    total += length;
    if (encrypt && tag) {
      tag->assign(16, '\0');
      ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag->data()) == 1;
    }
  } else ok = false;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) throw std::runtime_error("pairing envelope authentication failed");
  output.resize(total);
  return output;
}
void valid_label(const std::string &label) {
  if (label.empty() || label.size() > 64 ||
      label.find_first_of("\r\n") != std::string::npos ||
      label.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid paired PC label");
}
} // namespace

std::string x25519_shared_secret(const std::string &private_key,
                                 const std::string &public_key) {
  return hex_text(shared_secret(hex_bytes(private_key, 32), hex_bytes(public_key, 32)));
}

void PairingWindow::clear_pending() {
  if (!pending_) return;
  for (auto *value : {&pending_->transaction_id, &pending_->nonce,
                      &pending_->label, &pending_->companion_public_key,
                      &pending_->code}) {
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

SealedPairingKey seal_pairing_key(const std::string &device_id,
                                  const std::string &transaction_id,
                                  const std::string &nonce,
                                  const std::string &companion_public_key) {
  const auto companion = hex_bytes(companion_public_key, 32);
  (void)hex_bytes(device_id, 16);
  (void)hex_bytes(transaction_id, 16);
  (void)hex_bytes(nonce, 16);
  EVP_PKEY_CTX *keygen = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
  EVP_PKEY *ephemeral = nullptr;
  if (!keygen || EVP_PKEY_keygen_init(keygen) <= 0 || EVP_PKEY_keygen(keygen, &ephemeral) <= 0) {
    EVP_PKEY_CTX_free(keygen); throw std::runtime_error("pairing ephemeral key generation failed");
  }
  std::string private_key(32, '\0'), public_key(32, '\0');
  std::size_t private_size = private_key.size(), public_size = public_key.size();
  const bool exported = EVP_PKEY_get_raw_private_key(ephemeral,
      reinterpret_cast<unsigned char *>(private_key.data()), &private_size) > 0 &&
      EVP_PKEY_get_raw_public_key(ephemeral, reinterpret_cast<unsigned char *>(public_key.data()), &public_size) > 0;
  EVP_PKEY_CTX_free(keygen); EVP_PKEY_free(ephemeral);
  if (!exported || private_size != 32 || public_size != 32)
    throw std::runtime_error("pairing ephemeral key export failed");
  const auto key = random_bytes(32), iv = random_bytes(12), public_hex = hex_text(public_key);
  const auto derived = hkdf(shared_secret(private_key, companion),
      "rapid-pairing-salt-v1|" + transaction_id + "|" + nonce);
  std::string tag;
  const auto ciphertext = gcm(true, derived, iv,
      envelope_aad(device_id, transaction_id, nonce, companion_public_key, public_hex), key, &tag);
  OPENSSL_cleanse(private_key.data(), private_key.size());
  return {hex_text(key), {public_hex, base64_encode(iv), base64_encode(ciphertext), base64_encode(tag)}};
}

std::string open_pairing_key(const std::string &device_id,
                             const std::string &transaction_id,
                             const std::string &nonce,
                             const std::string &companion_private_key,
                             const PairingEnvelope &envelope) {
  const auto private_key = hex_bytes(companion_private_key, 32);
  const auto ephemeral = hex_bytes(envelope.ephemeral_public_key, 32);
  const auto iv = base64_decode(envelope.nonce), ciphertext = base64_decode(envelope.ciphertext),
             tag = base64_decode(envelope.tag);
  if (iv.size() != 12 || ciphertext.size() != 32 || tag.size() != 16)
    throw std::invalid_argument("invalid pairing envelope sizes");
  EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
      reinterpret_cast<const unsigned char *>(private_key.data()), private_key.size());
  std::string public_key(32, '\0'); std::size_t public_size = public_key.size();
  if (!key || EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char *>(public_key.data()), &public_size) <= 0) {
    EVP_PKEY_free(key); throw std::runtime_error("pairing public-key derivation failed");
  }
  EVP_PKEY_free(key);
  const auto public_hex = hex_text(public_key), ephemeral_hex = envelope.ephemeral_public_key;
  const auto derived = hkdf(shared_secret(private_key, ephemeral),
      "rapid-pairing-salt-v1|" + transaction_id + "|" + nonce);
  std::string auth_tag = tag;
  const auto plaintext = gcm(false, derived, iv,
      envelope_aad(device_id, transaction_id, nonce, public_hex, ephemeral_hex), ciphertext, &auth_tag);
  if (plaintext.size() != 32) throw std::runtime_error("invalid pairing key size");
  return hex_text(plaintext);
}

void PairingWindow::open(double now) {
  if (!std::isfinite(now)) throw std::invalid_argument("invalid pairing clock");
  window_open_ = true;
  window_expires_at_ = now + kPairingWindowSeconds;
  failures_ = 0;
  clear_pending();
}

void PairingWindow::cancel() {
  clear_pending();
  window_open_ = false;
  window_expires_at_ = 0;
  failures_ = 0;
}

bool PairingWindow::active(double now) const {
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
  if (!active(now)) {
    fail(now);
    throw std::runtime_error("pairing window is closed or expired");
  }
  if (pending_) throw std::runtime_error("pairing request already pending");
  valid_label(label);
  if (!lower_hex(companion_public_key, 64))
    throw std::invalid_argument("invalid companion public key");
  PendingPairing request{unique_id(), unique_id(), label, companion_public_key,
                         {}, now + kPairingRequestSeconds, false};
  request.code = verification_code(device_id_, certificate_fingerprint_,
                                   request.transaction_id, request.nonce,
                                   request.companion_public_key);
  // #71: a request gets its full approval time even when it arrives late in the
  // window; otherwise the window closing would expire it early.
  window_expires_at_ = std::max(window_expires_at_, request.expires_at);
  pending_ = request;
  return request;
}

bool PairingWindow::approve(const std::string &transaction_id,
                            const std::string &code, double now) {
  if (!pending_ || pending_->approved || !active(now) || now > pending_->expires_at ||
      transaction_id != pending_->transaction_id || !secret_equal(code, pending_->code)) {
    fail(now, false);
    return false;
  }
  pending_->approved = true;
  return true;
}

std::optional<PendingPairing> PairingWindow::consume_approved(double now) {
  if (!pending_ || !pending_->approved || !active(now) || now > pending_->expires_at) {
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
  if (pending_ && (!active(now) || now > pending_->expires_at))
    fail(now);
  return pending_;
}

PairingCoordinator::PairingCoordinator(SetupStore &store, std::string device_id,
                                       std::string certificate_fingerprint, fs::path panel_file,
                                       fs::path panel_approval_file,
                                       fs::path state_file, fs::path control_file,
                                       bool control_proxy)
    : store_(store), device_id_(std::move(device_id)),
      window_(device_id_, std::move(certificate_fingerprint)), panel_file_(std::move(panel_file)),
      state_file_(std::move(state_file)), control_file_(std::move(control_file)),
      panel_approval_file_(std::move(panel_approval_file)), control_proxy_(control_proxy) {
  std::error_code error;
  if (!control_proxy_) {
    if (!panel_file_.empty()) fs::remove(panel_file_, error);
    error.clear();
    if (!panel_approval_file_.empty()) fs::remove(panel_approval_file_, error);
    error.clear();
    if (!state_file_.empty()) fs::remove(state_file_, error);
  }
}

void PairingCoordinator::publish_panel(double now) {
  const auto pending = window_.pending(now);
  std::error_code error;
  if (!panel_file_.empty()) {
    if (!pending) fs::remove(panel_file_, error);
    else {
      atomic_file(panel_file_, Json{{"transaction_id", pending->transaction_id},
                                   {"nonce", pending->nonce}, {"label", pending->label},
                                   {"code", pending->code}, {"expires_at", pending->expires_at}}.dump() + "\n");
      if (::chmod(panel_file_.c_str(), 0640) != 0)
        throw std::runtime_error("cannot secure pairing panel state");
    }
  }
  if (!state_file_.empty()) {
    atomic_file(state_file_, Json{{"active", window_.active(now)},
                                 {"pending", pending.has_value()}}.dump() + "\n");
    ::chmod(state_file_.c_str(), 0640);
  }
}

void PairingCoordinator::apply_control(double now) {
  if (control_file_.empty() || !fs::exists(control_file_)) return;
  try {
    const auto body = Json::parse(read_file(control_file_));
    if (body.is_object() && body.size() == 1 && body["action"].is_string()) {
      const auto action = body["action"].get<std::string>();
      if (action == "open") window_.open(now);
      else if (action == "cancel") window_.cancel();
    }
  } catch (...) {}
  std::error_code error;
  fs::remove(control_file_, error);
  publish_panel(now);
}

void PairingCoordinator::apply_panel_approval(double now) {
  if (panel_approval_file_.empty()) return;
  std::error_code error;
  if (!fs::exists(panel_approval_file_)) return;
  try {
    const auto body = Json::parse(read_file(panel_approval_file_));
    const auto transaction = body.value("transaction_id", std::string{});
    const auto code = body.value("code", std::string{});
    if (body.is_object() && body.size() == 2 && lower_hex(transaction, 32) && code.size() == 8 &&
        code.find_first_not_of("0123456789") == std::string::npos)
      window_.approve(transaction, code, now);
  } catch (...) {
    // A malformed or stale local request must never block pairing.
  }
  fs::remove(panel_approval_file_, error);
  publish_panel(now);
}

void PairingCoordinator::open(double now) {
  std::lock_guard lock(mutex_);
  if (control_proxy_) {
    atomic_file(control_file_, Json{{"action", "open"}}.dump() + "\n");
    return;
  }
  if (!panel_approval_file_.empty()) {
    std::error_code error;
    fs::remove(panel_approval_file_, error);
  }
  if (!control_file_.empty()) { std::error_code error; fs::remove(control_file_, error); }
  window_.open(now); publish_panel(now);
}
void PairingCoordinator::cancel() {
  std::lock_guard lock(mutex_);
  if (control_proxy_) {
    atomic_file(control_file_, Json{{"action", "cancel"}}.dump() + "\n");
    return;
  }
  if (!control_file_.empty()) { std::error_code error; fs::remove(control_file_, error); }
  if (!panel_approval_file_.empty()) {
    std::error_code error;
    fs::remove(panel_approval_file_, error);
  }
  window_.cancel(); publish_panel(monotonic());
}

PendingPairing PairingCoordinator::request(const std::string &label,
                                           const std::string &companion_public_key,
                                           double now) {
  std::lock_guard lock(mutex_);
  apply_control(now);
  const auto result = window_.request(label, companion_public_key, now);
  publish_panel(now); return result;
}

bool PairingCoordinator::approve(const std::string &transaction_id,
                                 const std::string &code, double now) {
  std::lock_guard lock(mutex_);
  if (control_proxy_) {
    if (transaction_id.size() != 32 || transaction_id.find_first_not_of("0123456789abcdef") != std::string::npos ||
        code.size() != 8 || code.find_first_not_of("0123456789") != std::string::npos)
      return false;
    atomic_file(panel_approval_file_, Json{{"transaction_id", transaction_id}, {"code", code}}.dump() + "\n");
    return true;
  }
  apply_control(now);
  const auto result = window_.approve(transaction_id, code, now);
  publish_panel(now); return result;
}

std::optional<CompletedPairing> PairingCoordinator::consume(double now) {
  std::lock_guard lock(mutex_);
  apply_control(now);
  return consume_impl(now, {});
}

std::optional<CompletedPairing> PairingCoordinator::consume(
    double now, const std::string &expected_transaction) {
  std::lock_guard lock(mutex_);
  apply_control(now);
  return consume_impl(now, expected_transaction);
}

std::optional<CompletedPairing> PairingCoordinator::consume_impl(
    double now, const std::string &expected_transaction) {
  apply_panel_approval(now);
  const auto waiting = window_.pending(now);
  if (!waiting || !waiting->approved ||
      (!expected_transaction.empty() && waiting->transaction_id != expected_transaction))
    return {};
  const auto request = window_.consume_approved(now);
  if (!request) return {};
  // The request is one-use; remove its panel representation before any
  // envelope or peer-store work can fail.
  publish_panel(now);
  const auto sealed = seal_pairing_key(
      device_id_, request->transaction_id, request->nonce,
      request->companion_public_key);
  const auto peer_id = hash_text("rapid-pairing-peer-v1|" + request->companion_public_key).substr(0, 32);
  if (!store_.remember_peer(peer_id, request->label, sealed.telemetry_key, now))
    return {};
  return CompletedPairing{peer_id, request->label, sealed.envelope};
}

std::optional<PendingPairing> PairingCoordinator::pending(double now) {
  std::lock_guard lock(mutex_);
  if (control_proxy_) {
    if (panel_file_.empty() || !fs::exists(panel_file_)) return {};
    try {
      const auto body = Json::parse(read_file(panel_file_));
      if (!body.is_object() || !body["transaction_id"].is_string() ||
          !body["nonce"].is_string() || !body["label"].is_string() ||
          !body["code"].is_string() || !body["expires_at"].is_number()) return {};
      return PendingPairing{body["transaction_id"], body["nonce"], body["label"], {},
                            body["code"], body["expires_at"], false};
    } catch (...) { return {}; }
  }
  apply_control(now);
  apply_panel_approval(now);
  const auto result = window_.pending(now);
  publish_panel(now);
  return result;
}

bool PairingCoordinator::active(double now) const {
  std::lock_guard lock(mutex_);
  if (control_proxy_ && !state_file_.empty() && fs::exists(state_file_)) {
    try { return Json::parse(read_file(state_file_)).value("active", false); }
    catch (...) { return false; }
  }
  return window_.active(now);
}
} // namespace rapid::native
