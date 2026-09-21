#include "rapid/account.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <openssl/evp.h>
#include <set>
#include <string_view>
#include <vector>

namespace rapid::native::account {
namespace {
bool well_formed_utf8(const std::string &text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    std::size_t extra = 0;
    std::uint32_t code = 0;
    if (c < 0x80) { ++i; continue; }
    if (c >= 0xc2 && c <= 0xdf) { extra = 1; code = c & 0x1f; }
    else if (c >= 0xe0 && c <= 0xef) { extra = 2; code = c & 0x0f; }
    else if (c >= 0xf0 && c <= 0xf4) { extra = 3; code = c & 0x07; }
    else return false;
    if (i + extra >= text.size()) return false;
    for (std::size_t k = 1; k <= extra; ++k) {
      const auto next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 0x3f);
    }
    // Overlong forms, surrogates and values above U+10FFFF.
    if ((extra == 2 && code < 0x800) || (extra == 3 && (code < 0x10000 || code > 0x10ffff)) ||
        (code >= 0xd800 && code <= 0xdfff))
      return false;
    i += extra + 1;
  }
  return true;
}

std::string lower_ascii(std::string text) {
  for (auto &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::optional<std::string> decode_base64(const std::string &text) {
  if (text.empty() || text.size() % 4 != 0 || text.size() > 16384) return std::nullopt;
  const auto first_pad = text.find('=');
  if (first_pad != std::string::npos &&
      (first_pad < text.size() - 2 || text.find_first_not_of('=', first_pad) != std::string::npos))
    return std::nullopt;
  if (text.substr(0, first_pad).find_first_not_of(
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") != std::string::npos)
    return std::nullopt;
  std::string out(text.size() / 4 * 3, '\0');
  const int length = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                                     reinterpret_cast<const unsigned char *>(text.data()),
                                     static_cast<int>(text.size()));
  if (length < 0) return std::nullopt;
  const std::size_t padding = static_cast<std::size_t>(std::count(text.begin(), text.end(), '='));
  out.resize(static_cast<std::size_t>(length) - padding);
  // Canonical form only: the stored line is exactly the re-encoded blob.
  std::string again(4 * ((out.size() + 2) / 3) + 1, '\0');
  const int encoded = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(again.data()),
                                      reinterpret_cast<const unsigned char *>(out.data()),
                                      static_cast<int>(out.size()));
  again.resize(static_cast<std::size_t>(encoded));
  if (again != text) return std::nullopt;
  return out;
}

struct Reader {
  std::string_view data;
  bool fail = false;
  std::string_view field() {
    if (fail || data.size() < 4) { fail = true; return {}; }
    const auto b = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])); };
    const std::uint32_t length = (b(0) << 24) | (b(1) << 16) | (b(2) << 8) | b(3);
    if (length > data.size() - 4) { fail = true; return {}; }
    const auto value = data.substr(4, length);
    data.remove_prefix(4 + length);
    return value;
  }
};

bool valid_blob(const std::string &type, const std::string &blob) {
  Reader reader{blob};
  if (reader.field() != type || reader.fail) return false;
  if (type == "ssh-ed25519") {
    const auto key = reader.field();
    return !reader.fail && key.size() == 32 && reader.data.empty();
  }
  if (type == "sk-ssh-ed25519@openssh.com") {
    const auto key = reader.field();
    const auto application = reader.field();
    return !reader.fail && key.size() == 32 && !application.empty() && reader.data.empty();
  }
  const auto ecdsa = [&](std::string_view curve, std::size_t point, bool sk) {
    const auto name = reader.field();
    const auto q = reader.field();
    if (sk && reader.field().empty()) return false;
    return !reader.fail && name == curve && q.size() == point && q[0] == 0x04 && reader.data.empty();
  };
  if (type == "ecdsa-sha2-nistp256") return ecdsa("nistp256", 65, false);
  if (type == "ecdsa-sha2-nistp384") return ecdsa("nistp384", 97, false);
  if (type == "ecdsa-sha2-nistp521") return ecdsa("nistp521", 133, false);
  if (type == "sk-ecdsa-sha2-nistp256@openssh.com") return ecdsa("nistp256", 65, true);
  if (type == "ssh-rsa") {
    const auto exponent = reader.field();
    auto modulus = reader.field();
    if (reader.fail || !reader.data.empty() || exponent.empty() || modulus.empty()) return false;
    while (!modulus.empty() && modulus[0] == 0) modulus.remove_prefix(1);
    if (modulus.empty()) return false;
    std::size_t bits = (modulus.size() - 1) * 8;
    for (auto top = static_cast<unsigned char>(modulus[0]); top; top >>= 1) ++bits;
    return bits >= 2048 && bits <= 16384;
  }
  return false;
}
} // namespace

// Device-access credential policy (#23/#22, reworked 2026-09-21).
//
// The threat model is local-only: this credential is the `rapid` account's
// SSH/sudo password, the account is not reachable off the local network, SSH
// password login exists only while a password is set, and the setup page needs
// HTTPS plus an authenticated owner session and CSRF token to change it. The
// owner asked for a painless credential, so the hard minimum is 4 characters
// (a 4-digit PIN is accepted) and the page states that 12+ characters or a key
// are recommended, not required. The owner setup login keeps its 12-character
// minimum in setup_auth.cpp: it guards the whole management surface.
//
// This is a deliberate relaxation, not a removal of validation: control
// characters, invalid text, single-character and repeating patterns, straight
// sequences and well-known passwords are still refused, and the same function
// is the single server-side authority the client mirrors.
std::string password_problem(const std::string &password) {
  if (password.size() < 4) return "the password or PIN must be at least 4 characters";
  if (password.size() > 128) return "the password or PIN must be at most 128 bytes";
  for (unsigned char c : password)
    if (c < 0x20 || c == 0x7f) return "the password or PIN must not contain control characters";
  if (!well_formed_utf8(password)) return "the password or PIN must be valid text";
  std::set<unsigned char> distinct(password.begin(), password.end());
  if (distinct.size() < 3) return "the password or PIN must use at least 3 different characters";
  static const std::array<std::string_view, 12> common{
      "password1234", "password12345", "passw0rd1234", "raspberrypi1", "raspberry123",
      "rapidrapid12", "123456789012", "qwertyuiop12", "1q2w3e4r5t6y", "letmein12345",
      "administrator", "changeme1234"};
  const auto lowered = lower_ascii(password);
  if (std::find(common.begin(), common.end(), lowered) != common.end() ||
      lowered.find("raspberry") != std::string::npos || lowered.find("password") != std::string::npos)
    return "the password is too easy to guess";
  // Straight runs such as "abcdefghijkl", "123456789012" or "4321".
  bool ascending = true, descending = true;
  for (std::size_t i = 1; i < password.size() && (ascending || descending); ++i) {
    const auto previous = static_cast<unsigned char>(password[i - 1]);
    const auto current = static_cast<unsigned char>(password[i]);
    ascending = ascending && current == previous + 1;
    descending = descending && previous == current + 1;
  }
  if (ascending || descending) return "the password or PIN is too easy to guess";
  // Short repeating patterns such as "abababab" or "1212".
  for (std::size_t period = 1; period * 2 <= password.size(); ++period) {
    if (password.size() % period != 0) continue;
    bool periodic = true;
    for (std::size_t i = period; i < password.size() && periodic; ++i)
      periodic = password[i] == password[i % period];
    if (periodic) return "the password or PIN repeats a short pattern";
  }
  return {};
}

std::string PublicKey::line() const {
  return comment.empty() ? type + " " + blob_base64 : type + " " + blob_base64 + " " + comment;
}

std::optional<PublicKey> parse_public_key(const std::string &text, std::string *problem) {
  const auto fail = [&](const char *reason) -> std::optional<PublicKey> {
    if (problem) *problem = reason;
    return std::nullopt;
  };
  auto line = text;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
    line.pop_back();
  const auto start = line.find_first_not_of(" \t");
  if (start == std::string::npos) return fail("the SSH public key is empty");
  line.erase(0, start);
  if (line.size() > 3072) return fail("the SSH public key is too long");
  for (unsigned char c : line)
    if (c < 0x20 || c >= 0x7f) return fail("paste exactly one SSH public key line");
  const auto first = line.find(' ');
  if (first == std::string::npos) return fail("the SSH public key is incomplete");
  PublicKey key;
  key.type = line.substr(0, first);
  static const std::array<std::string_view, 7> types{
      "ssh-ed25519", "sk-ssh-ed25519@openssh.com", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
      "ecdsa-sha2-nistp521", "sk-ecdsa-sha2-nistp256@openssh.com", "ssh-rsa"};
  if (std::find(types.begin(), types.end(), key.type) == types.end())
    return fail("unsupported SSH key type; use an Ed25519, ECDSA or RSA (2048+ bit) public key");
  auto rest = line.substr(first + 1);
  const auto second = rest.find(' ');
  key.blob_base64 = rest.substr(0, second);
  if (second != std::string::npos) {
    key.comment = rest.substr(second + 1);
    const auto comment_start = key.comment.find_first_not_of(' ');
    key.comment = comment_start == std::string::npos ? "" : key.comment.substr(comment_start);
  }
  if (key.comment.size() > 256) return fail("the SSH key comment is too long");
  const auto blob = decode_base64(key.blob_base64);
  if (!blob || !valid_blob(key.type, *blob)) return fail("the SSH public key is not valid");
  return key;
}

bool valid_crypt_hash(const std::string &hash) {
  if (hash.size() < 20 || hash.size() > 256) return false;
  if (!hash.starts_with("$y$") && !hash.starts_with("$6$")) return false;
  return hash.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789./$=") ==
         std::string::npos;
}

bool shadow_password_usable(const std::string &field) {
  return !field.empty() && field[0] != '!' && field[0] != '*';
}
} // namespace rapid::native::account
