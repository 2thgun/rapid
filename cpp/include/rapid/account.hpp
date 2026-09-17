#pragma once
#include <optional>
#include <string>

// Owner-chosen device access (#23): the login/sudo password of the local
// account and an optional SSH public key. These rules are shared by the setup
// server (which validates and hashes) and the root helper (which re-validates
// everything it is handed, because its request file is writable by the
// unprivileged setup service).
namespace rapid::native::account {

// Empty when the password is acceptable, otherwise a short reason that is safe
// to show in the browser. The reason never contains the password.
std::string password_problem(const std::string &password);

struct PublicKey {
  std::string type;
  std::string blob_base64; // canonical base64 of the key blob
  std::string comment;     // may be empty
  // One authorized_keys line without options and without a trailing newline.
  std::string line() const;
};

// Parses a single OpenSSH public key line ("type base64 [comment]"). Options
// (command=, from=, ...) are rejected, the blob must decode to a well-formed
// key of the stated type and RSA keys must be at least 2048 bits.
std::optional<PublicKey> parse_public_key(const std::string &text, std::string *problem = nullptr);

// A yescrypt ($y$) or sha512-crypt ($6$) hash with only crypt(3) characters,
// so it can never inject another chpasswd line or field.
bool valid_crypt_hash(const std::string &hash);

// Whether a shadow password field allows password authentication. Empty,
// locked ("!...") and disabled ("*...") fields do not.
bool shadow_password_usable(const std::string &field);

} // namespace rapid::native::account
