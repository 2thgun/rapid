#include "rapid/pairing.hpp"
#include <cmath>
#include <iomanip>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <sstream>

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
                         {}, now + 120, false};
  request.code = verification_code(device_id_, certificate_fingerprint_,
                                   request.transaction_id, request.nonce,
                                   request.companion_public_key);
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
} // namespace rapid::native
