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
//
// Device-access policy: the hard minimum is 4 characters, so a 4-digit PIN is
// accepted for this local-only account; 12+ characters or an SSH key are
// recommended, not required (see account_policy.cpp for the threat model).
// The owner setup login has its own separate 12-character minimum.
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

// Lowercase hex SHA-256 of the decoded key blob, used to identify an enrolled
// key for listing and removal without ever handling private material. Empty
// when the base64 is not a decodable key blob.
std::string key_fingerprint(const std::string &blob_base64);

// A yescrypt ($y$) or sha512-crypt ($6$) hash with only crypt(3) characters,
// so it can never inject another chpasswd line or field.
bool valid_crypt_hash(const std::string &hash);

// Whether a shadow password field allows password authentication. Empty,
// locked ("!...") and disabled ("*...") fields do not.
bool shadow_password_usable(const std::string &field);

} // namespace rapid::native::account
