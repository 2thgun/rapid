# Companion pairing contract

This is the implementation contract for GitHub issue #10. It describes the
customer pairing flow that will replace copying a shared `telemetry.key`.
Nothing in the current companion or Pi package implements this protocol yet.

## Goal and boundary

Pairing gives one Windows user account a new random, 256-bit v4 telemetry key.
The key is private to that PC and is never placed in an installer, browser
response, Samba share, log or command line. The Pi stores enough public
information to list, revoke and authenticate that PC. The companion stores its
configuration and private key under its user's application-data directory,
protected with Windows DPAPI.

The pairing service is available only while the Pi has a physical pairing
window open. The panel must show the requesting PC name and the verification
code, and require a local approval. An authenticated owner setup page may open
the same window, but may not approve a request without the displayed code.

## Handshake

The Pi management service must use HTTPS with a device certificate. The
companion deliberately supports a first-use certificate only during an open
pairing window; afterward it pins the certificate's SHA-256 fingerprint along
with the device ID. A changed fingerprint is an error requiring a new physical
pairing, not a silent reconnect.

1. The companion discovers a Pi or accepts a manually entered address. It shows
   the Pi's device ID and certificate fingerprint before pairing.
2. The companion creates an ephemeral X25519 key pair and sends a `POST` pairing
   request containing its public key and a validated display name. It sends no
   telemetry key or Windows identity token.
3. The Pi creates one random transaction identifier and nonce. It derives an
   eight-digit verification code from the transaction identifier, nonce, device
   certificate fingerprint and companion public key. The Pi panel and companion
   both show that code and the requested PC name.
4. The user compares the two screens and approves on the Pi. The request expires
   after two minutes; five failed or expired attempts close the window.
5. The Pi generates a fresh 32-byte telemetry key and returns it only after
   approval, encrypted to the companion's X25519 public key. The envelope uses
   X25519 shared secret, HKDF-SHA-256 with transaction-bound salt and
   AES-256-GCM with the device ID, transaction identifier and companion public
   key as authenticated data.
6. The companion verifies and decrypts the envelope, stores the key with DPAPI,
   writes no plaintext key file, pins the Pi identity/certificate, and starts
   normal v4 telemetry. The Pi records the PC ID, label, creation/last-seen
   times and the distinct telemetry key in service-owned private state. The receiver
   needs that secret to verify the existing symmetric HMAC-SHA256 packet format;
   it must never be exposed through setup responses, logs, another customer's
   backup, or the image. Encryption at rest may protect the state further, but
   a non-secret verifier cannot authenticate v4 packets.

The numeric code proves that the user is looking at the intended physical Pi;
the public-key envelope prevents a network intermediary from learning the
telemetry key. A pairing response must be single-use and its private material
must be erased once it is consumed or expires.

## Required service behavior

- At most one pending request and 16 remembered PCs exist per Pi.
- The panel can cancel a pending request at any time. A timeout, cancellation,
  failed envelope verification or service restart leaves no usable credential.
- A retained PC can be revoked individually. Revocation immediately rejects its
  v4 packets and deletes the local pairing record on the next failed reconnect.
- The receiver selects the correct v4 key before accepting a packet and keeps
  replay watermarks separate for each paired PC. It still permits only one live
  telemetry sender through the existing ownership/inactivity rule.
- Discovery is convenience only. Manual address entry always remains available.
- The MSI and portable companion use the same pairing executable and storage
  format. Portable mode needs no administrator rights and still uses the current
  Windows user's DPAPI scope; it does not make a portable plaintext key file.

## UI states

The companion needs explicit states: `not paired`, `discovering`, `request
pending`, `compare code`, `approved`, `connected`, `revoked`, and `error`.
Normal simulator monitoring may continue only in `connected`; `not paired` must
show an actionable pairing button instead of a v4 key parse error. A failed
pairing must leave the existing working pairing untouched.

## Acceptance tests

Automated tests must cover deterministic code derivation, expiry, one-pending
and five-attempt limits, cancelled/replayed transactions, envelope
authentication failure, key zeroization paths, certificate pin mismatch and
independent replay state for two PC keys. Integration tests must prove that a
packet signed by PC A cannot authenticate as PC B, and that revoking A does not
interrupt B.

On hardware, pair two clean Windows accounts through both discovery and manual
address entry; compare the code on the Pi, reconnect each account, revoke one,
and confirm that only the other can resume telemetry. Repeat with a network
intermediary test fixture to prove the envelope never reveals the key. Verify
MSI and portable onboarding without a terminal, plaintext key, SMB dependency
or administrator prompt.
