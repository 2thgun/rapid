#include "rapid/setup_auth.hpp"
#include <argon2.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <array>
#include <iomanip>
#include <sstream>

namespace rapid::native {
namespace {
std::string header(const Request &request, const std::string &key) {
  const auto found = request.headers.find(key);
  return found == request.headers.end() ? "" : found->second;
}
Response reply(int code, const Json &body) { return {code, body.dump()}; }
bool queued(const fs::path &path) {
  std::error_code error;
  return !path.empty() && fs::is_regular_file(path, error);
}
bool equal(const std::string &left, const std::string &right) {
  return left.size() == right.size() && CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}
std::string cookie_token(const Request &request) {
  auto cookie = header(request, "cookie");
  std::string token;
  while (!cookie.empty()) {
    auto end = cookie.find(';');
    auto part = cookie.substr(0, end);
    auto first = part.find_first_not_of(" \t");
    if (first != std::string::npos)
      part.erase(0, first);
    if (part.starts_with("rapid_setup=")) {
      if (!token.empty()) return ""; // Ambiguous duplicate cookies are rejected.
      token = part.substr(12);
      if (token.size() != 64 || token.find_first_not_of("0123456789abcdef") != std::string::npos)
        return "";
    }
    if (end == std::string::npos) break;
    cookie.erase(0, end + 1);
  }
  return token;
}
// The SSID rapid-provision chose ("rapid", or "rapid-NNNN" when another
// "rapid" was in range). Anything else, or a missing file, is omitted.
std::string network_ssid(const fs::path &path) {
  if (path.empty()) return {};
  try {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || fs::file_size(path, error) > 64) return {};
    auto value = read_file(path);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    const bool suffixed = value.size() == 10 && value.starts_with("rapid-") &&
                          value.find_first_not_of("0123456789", 6) == std::string::npos;
    return value == "rapid" || suffixed ? value : std::string{};
  } catch (const std::exception &) {
    return {};
  }
}
// #10: the SHA-256 the setup page shows for the bundled companion, so the
// owner (or the release notes) can tell whether the served artifact matches
// the companion build the Pi's protocol expects. A mismatch is visible, not
// silent.
std::string sha256_hex(const std::string &bytes) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("cannot hash companion artifact");
  std::ostringstream out;
  for (unsigned int i = 0; i < length; ++i)
    out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(digest[i]);
  return out.str();
}
} // namespace

void SetupAuth::set_network_ssid_file(fs::path ssid_file) {
  std::lock_guard lock(mutex_);
  network_ssid_file_ = std::move(ssid_file);
}

void SetupAuth::set_companion_artifact(fs::path artifact) {
  std::lock_guard lock(mutex_);
  companion_artifact_.clear();
  companion_sha256_.clear();
  companion_filename_.clear();
  if (artifact.empty()) return;
  try {
    std::error_code error;
    // The packaged path is a directory (share/rapid/companion) so a release
    // can swap a portable .exe for an .msi without touching the unit. Exactly
    // one regular file is required: ambiguity serves nothing rather than a
    // guess, which is the same fail-closed rule as a missing file.
    fs::path file = artifact;
    if (fs::is_directory(artifact, error)) {
      file.clear();
      for (fs::directory_iterator entry(artifact, error), end; !error && entry != end; entry.increment(error)) {
        if (!entry->is_regular_file(error)) continue;
        const auto name = entry->path().filename().string();
        if (name.empty() || name.front() == '.') continue;
        if (!file.empty()) return;
        file = entry->path();
      }
      if (error || file.empty()) return;
    }
    if (!fs::is_regular_file(file, error)) return;
    companion_artifact_ = std::move(file);
    companion_filename_ = companion_artifact_.filename().string();
    companion_sha256_ = sha256_hex(read_file(companion_artifact_));
  } catch (const std::exception &) {
    companion_artifact_.clear();
    companion_sha256_.clear();
    companion_filename_.clear();
  }
}

SetupAuth::SetupAuth(SetupStore &store, int port, std::function<double()> clock,
                     std::string enrollment_token, std::string host,
                     fs::path apply_request_file, fs::path apply_result_file,
                     fs::path firstboot_status_file, fs::path wifi_request_file,
                     fs::path wifi_result_file, PairingCoordinator *pairing,
                     bool secure_transport, std::string certificate_fingerprint,
                     bool pairing_transport_enabled, fs::path calibration_file,
                     fs::path calibration_request_file, fs::path display_confirm_file)
    : store_(store), clock_(std::move(clock)),
      origin_((secure_transport ? "https://" : "http://") + host + ":" + std::to_string(port)),
      authority_(std::move(host) + ":" + std::to_string(port)),
      enrollment_token_(std::move(enrollment_token)),
      apply_request_file_(std::move(apply_request_file)),
      apply_result_file_(std::move(apply_result_file)),
      wifi_request_file_(std::move(wifi_request_file)),
      wifi_result_file_(std::move(wifi_result_file)),
      firstboot_status_file_(std::move(firstboot_status_file)), calibration_file_(std::move(calibration_file)),
      calibration_request_file_(std::move(calibration_request_file)),
      display_confirm_file_(std::move(display_confirm_file)), pairing_(pairing),
      secure_transport_(secure_transport),
      certificate_fingerprint_(std::move(certificate_fingerprint)) {
  if (!enrollment_token_.empty() && (enrollment_token_.size() != 64 ||
      enrollment_token_.find_first_not_of("0123456789abcdef") != std::string::npos))
    throw std::invalid_argument("enrollment token must contain 64 lowercase hexadecimal characters");
  if (secure_transport_ && pairing_ && pairing_transport_enabled)
    pairing_transport_ = std::make_unique<PairingTransport>(*pairing_, clock_);
}

bool SetupAuth::enroll(const std::string &password) {
  if (password.size() < 12 || password.size() > 256 || password.find('\0') != std::string::npos)
    throw std::invalid_argument("owner password must be 12 to 256 bytes without NUL");
  if (!store_.owner_hash().empty())
    return false;
  const auto salt = unique_id();
  char encoded[256];
  if (argon2id_hash_encoded(3, 65536, 1, password.data(), password.size(),
                            salt.data(), salt.size(), 32, encoded, sizeof encoded) != ARGON2_OK)
    throw std::runtime_error("owner password hashing failed");
  return store_.claim_owner(encoded);
}

void SetupAuth::expire(double time) {
  for (auto i = sessions_.begin(); i != sessions_.end();) {
    if (i->second.expires <= time) i = sessions_.erase(i);
    else ++i;
  }
  while (!attempts_.empty() && attempts_.front() <= time - 60)
    attempts_.pop_front();
}

Response SetupAuth::handle(const Request &request) {
  // Exact configured Host/Origin checks reject browser DNS-rebinding requests.
  // setup_main keeps the listener on loopback unless an activation token
  // explicitly authorizes the AP bootstrap listener.
  if (header(request, "host") != authority_)
    return reply(403, {{"detail", "invalid host"}});
  const auto origin = header(request, "origin");
  const auto fetch_site = header(request, "sec-fetch-site");
  if ((!origin.empty() && origin != origin_) || fetch_site == "cross-site")
    return reply(403, {{"detail", "cross-origin request rejected"}});
  const auto path = request.target.substr(0, request.target.find('?'));
  // A pasted RSA public key plus a password needs more than the default limit.
  if (request.body.size() > (path == "/api/v1/account" ? std::size_t{4096} : std::size_t{1024}))
    return reply(413, {{"detail", "request too large"}});
  if (request.method != "GET" && request.method != "POST")
    return {405, "{\"detail\":\"method not allowed\"}", "application/json", {{"Allow", "GET, POST"}}};
  const bool companion_pairing_request = pairing_transport_ &&
      PairingTransport::handles(request) && request.method == "POST";
  if (request.method == "POST" &&
      ((!companion_pairing_request && origin != origin_) ||
       header(request, "content-type") != "application/json"))
    return reply(403, {{"detail", "same-origin JSON required"}});

  std::lock_guard lock(mutex_);
  const auto time = clock_();
  expire(time);
  if (path == "/api/v1/setup" && request.method == "GET") {
    auto status = setup_status(store_.snapshot());
    status["owner_configured"] = !store_.owner_hash().empty();
    // This loopback management service can save desired profile values. The
    // runtime endpoint uses the same public status builder but remains read-only.
    status["capabilities"]["settings_write"] = true;
    status["capabilities"]["browser_owner_enrollment"] =
        !enrollment_token_.empty() && store_.owner_hash().empty();
    status["capabilities"]["pairing"] = pairing_ != nullptr;
    status["capabilities"]["companion_download"] = !companion_artifact_.empty();
    if (!companion_artifact_.empty() && !companion_sha256_.empty())
      status["companion"] = {{"filename", companion_filename_},
                             {"url", "/companion/download"},
                             {"sha256", companion_sha256_}};
    if (secure_transport_ && !certificate_fingerprint_.empty())
      status["certificate_fingerprint"] = certificate_fingerprint_;
    if (const auto ssid = network_ssid(network_ssid_file_); !ssid.empty())
      status["network_ssid"] = ssid;
    return reply(200, status);
  }
  // #10: a plain, unauthenticated static download of the bundled Windows
  // companion. Deliberately placed above the owner-session gate below, like
  // /api/v1/setup: the artifact is public (no secret, no key material) and the
  // whole point is that a fresh owner can fetch it before pairing. The paired
  // key is only ever negotiated on-device, after the owner approves the PC.
  if (path == "/companion/download" && request.method == "GET") {
    if (companion_artifact_.empty() || companion_filename_.empty())
      return reply(404, {{"detail", "companion download is unavailable"}});
    try {
      const auto name = companion_filename_;
      const bool msi = name.size() > 4 && name.compare(name.size() - 4, 4, ".msi") == 0;
      return {200, read_file(companion_artifact_),
              msi ? "application/x-msi" : "application/vnd.microsoft.portable-executable",
              {{"Content-Disposition", "attachment; filename=\"" + name + "\""},
               {"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"}}};
    } catch (const std::exception &) {
      return reply(503, {{"detail", "companion download is unavailable"}});
    }
  }
  if (path == "/api/v1/auth/enroll" && request.method == "POST") {
    if (!store_.owner_hash().empty())
      return reply(409, {{"detail", "owner already configured; sign in"}});
    if (enrollment_token_.empty())
      return reply(403, {{"detail", "browser enrollment is not activated"}});
    if (attempts_.size() >= 5)
      return {429, "{\"detail\":\"try again shortly\"}", "application/json", {{"Retry-After", "60"}}};
    attempts_.push_back(time);
    const auto body = Json::parse(request.body, nullptr, false);
    if (!body.is_object() || body.size() != 2 || !body.contains("token") ||
        !body["token"].is_string() || !body.contains("password") || !body["password"].is_string())
      return reply(400, {{"detail", "activation token and password required"}});
    if (!equal(body["token"].get<std::string>(), enrollment_token_))
      return reply(403, {{"detail", "invalid activation token"}});
    try {
      if (!enroll(body["password"].get<std::string>()))
        return reply(409, {{"detail", "owner already configured; sign in"}});
    } catch (const std::invalid_argument &error) {
      return reply(400, {{"detail", error.what()}});
    }
    OPENSSL_cleanse(enrollment_token_.data(), enrollment_token_.size());
    enrollment_token_.clear();
    if (!firstboot_status_file_.empty()) {
      try {
        auto status = Json::parse(read_file(firstboot_status_file_));
        if (status.is_object()) {
          // #59 / setup-page-ux: the activation token is the secret that
          // authorizes owner creation, so enrollment removes it immediately.
          // The AP address and TLS fingerprint stay: the panel still shows its
          // setup card while Access Point mode is up on an enrolled device.
          if (status.contains("bootstrap") && status["bootstrap"].is_object())
            status["bootstrap"].erase("activation_token");
          status["owner_configured"] = true;
          status["state"] = "settings_application_required";
          // #31: this file can still hold bootstrap/activation material, and
          // is read back by the privileged provisioner; keep it group-rapid
          // readable (never group/other writable) from creation.
          atomic_file(firstboot_status_file_, status.dump() + "\n",
                      fs::perms::owner_read | fs::perms::owner_write |
                          fs::perms::group_read);
        }
      } catch (const std::exception &error) {
        log(std::string("WARN setup: owner configured but cannot clear bootstrap status: ") +
            error.what());
      }
    }
    return reply(201, {{"owner_configured", true}, {"setup_complete", false}});
  }
  if (pairing_transport_ && PairingTransport::handles(request))
    return pairing_transport_->handle(request);
  if (path == "/api/v1/auth/login" && request.method == "POST") {
    if (attempts_.size() >= 5)
      return {429, "{\"detail\":\"try again shortly\"}", "application/json", {{"Retry-After", "60"}}};
    attempts_.push_back(time);
    const auto body = Json::parse(request.body, nullptr, false);
    if (!body.is_object() || body.size() != 1 || !body.contains("password") || !body["password"].is_string())
      return reply(400, {{"detail", "password required"}});
    const auto password = body["password"].get<std::string>();
    const auto hash = store_.owner_hash();
    if (password.empty() || password.size() > 256 || hash.empty() ||
        argon2id_verify(hash.c_str(), password.data(), password.size()) != ARGON2_OK)
      return reply(401, {{"detail", "invalid credentials"}});
    if (sessions_.size() >= 16)
      return reply(429, {{"detail", "too many active sessions"}});
    const auto token = unique_id() + unique_id(), csrf = unique_id() + unique_id();
    sessions_.emplace(hash_text(token), Session{clock_() + 1800, csrf});
    auto response = reply(200, {{"authenticated", true}, {"csrf_token", csrf}, {"expires_in", 1800}});
    response.headers.emplace_back("Set-Cookie", "rapid_setup=" + token +
        "; Path=/; HttpOnly; SameSite=Strict; Max-Age=1800" + (secure_transport_ ? "; Secure" : ""));
    return response;
  }
  const auto token = cookie_token(request);
  const auto session = sessions_.find(hash_text(token));
  if (token.empty() || session == sessions_.end())
    return reply(401, {{"detail", "sign in required"}});
  if (path == "/api/v1/account")
    return handle_account(request, path, session->second.csrf, time);
  if (pairing_ && secure_transport_ && path == "/api/v1/pairing/window" && request.method == "POST") {
    if (!equal(header(request, "x-csrf-token"), session->second.csrf))
      return reply(403, {{"detail", "invalid CSRF token"}});
    const auto body = Json::parse(request.body, nullptr, false);
    if (!body.is_object() || body.size() != 1 || !body["open"].is_boolean())
      return reply(400, {{"detail", "open boolean required"}});
    if (body["open"]) pairing_->open(time); else pairing_->cancel();
    return reply(200, {{"active", pairing_->active(time)}});
  }
  if (pairing_ && secure_transport_ && path == "/api/v1/pairing/pending" && request.method == "GET") {
    const auto pending = pairing_->pending(time);
    if (!pending) return reply(404, {{"detail", "no pairing request pending"}});
    return reply(200, {{"transaction_id", pending->transaction_id},
                       {"nonce", pending->nonce}, {"label", pending->label},
                       {"companion_public_key", pending->companion_public_key},
                       {"code", pending->code}, {"expires_at", pending->expires_at}});
  }
  if (pairing_ && secure_transport_ && path == "/api/v1/pairing/state" && request.method == "GET") {
    const auto pending = pairing_->pending(time);
    return reply(200, {{"active", pairing_->active(time)}, {"pending", pending.has_value()}});
  }
  if (pairing_ && secure_transport_ && path == "/api/v1/pairing/approve" && request.method == "POST") {
    if (!equal(header(request, "x-csrf-token"), session->second.csrf))
      return reply(403, {{"detail", "invalid CSRF token"}});
    const auto body = Json::parse(request.body, nullptr, false);
    if (!body.is_object() || body.size() != 2 || !body["transaction_id"].is_string() ||
        !body["code"].is_string())
      return reply(400, {{"detail", "transaction_id and code required"}});
    if (!pairing_->approve(body["transaction_id"], body["code"], time))
      return reply(403, {{"detail", "pairing approval rejected"}});
    return reply(200, {{"approved", true}});
  }
  if (path == "/api/v1/auth/session" && request.method == "GET")
    return reply(200, {{"authenticated", true}, {"csrf_token", session->second.csrf}});
  if (path == "/api/v1/calibration/reset" && request.method == "POST") {
    if (!equal(header(request, "x-csrf-token"), session->second.csrf))
      return reply(403, {{"detail", "invalid CSRF token"}});
    const auto body = Json::parse(request.body, nullptr, false);
    if (!body.is_object() || !body.empty())
      return reply(400, {{"detail", "empty JSON object required"}});
    if (calibration_file_.empty())
      return reply(503, {{"detail", "calibration recovery is unavailable"}});
    std::error_code error;
    fs::remove(calibration_file_, error);
    if (error) return reply(503, {{"detail", "calibration reset failed"}});
    return reply(200, {{"reset", true}});
  }
  if (path == "/api/v1/calibration/start" && request.method == "POST") {
    if (!equal(header(request, "x-csrf-token"), session->second.csrf))
      return reply(403, {{"detail", "invalid CSRF token"}});
    const auto body = Json::parse(request.body, nullptr, false);
    if (!body.is_object() || !body.empty())
      return reply(400, {{"detail", "empty JSON object required"}});
    if (calibration_request_file_.empty())
      return reply(503, {{"detail", "panel calibration is unavailable"}});
    // The panel consumes this key-free request and shows the tap targets.
    try {
      // #31: the panel (same service user) reads this to start capture, so a
      // different local user must not be able to plant or alter it.
      atomic_file(calibration_request_file_, Json{{"requested", true}}.dump() + "\n",
                  fs::perms::owner_read | fs::perms::owner_write);
    } catch (const std::exception &) {
      return reply(503, {{"detail", "panel calibration request failed"}});
    }
    return reply(200, {{"requested", true}});
  }
  if (path == "/api/v1/settings" && request.method == "GET") {
    auto state = store_.snapshot();
    std::optional<Json> application, wifi;
    bool settings_applied = false, wifi_connected = false;
    try {
      if (!apply_result_file_.empty()) {
        const auto result = Json::parse(read_file(apply_result_file_));
        if (result.is_object() && result.value("revision", -1) == state.at("revision")) {
          // A changed orientation counts only once the owner has kept it.
          const auto rotation = result.value("rotation", std::string{});
          settings_applied = result.value("hostname_applied", false) &&
                             rotation != "awaiting_confirmation" && rotation != "rolled_back" &&
                             rotation != "failed";
          application = result;
        }
      }
    } catch (const std::exception &) {}
    try {
      if (!wifi_result_file_.empty()) {
        const auto result = Json::parse(read_file(wifi_result_file_));
        if (result.is_object() && result.value("revision", -1) == state.at("revision")) {
          wifi_connected = result.value("connected", false);
          wifi = result;
        }
      }
    } catch (const std::exception &) {}
    if (!state.at("setup_complete") && settings_applied && wifi_connected &&
        store_.mark_setup_complete(state.at("revision"))) {
      state = store_.snapshot();
      if (!firstboot_status_file_.empty()) {
        try {
          auto status = Json::parse(read_file(firstboot_status_file_));
          if (status.is_object()) {
            status["setup_complete"] = true;
            status["state"] = "complete";
            atomic_file(firstboot_status_file_, status.dump() + "\n",
                        fs::perms::owner_read | fs::perms::owner_write |
                            fs::perms::group_read);
          }
        } catch (const std::exception &error) {
          log(std::string("WARN setup: cannot record setup completion status: ") + error.what());
        }
      }
    }
    Json response{{"revision", state.at("revision")}, {"settings", state.at("settings")},
                  {"applied", false},
                  {"apply_queued", queued(apply_request_file_)}};
    response["setup_complete"] = state.at("setup_complete");
    if (application) response["application"] = *application;
    if (wifi) response["wifi"] = *wifi;
    return reply(200, response);
  }
  if (path == "/api/v1/settings/retry" && request.method == "POST") {
    if (!equal(header(request, "x-csrf-token"), session->second.csrf))
      return reply(403, {{"detail", "invalid CSRF token"}});
    const auto body = Json::parse(request.body, nullptr, false);
    if (!body.is_object() || body.size() != 1 || !body.contains("revision") ||
        !body["revision"].is_number_integer())
      return reply(400, {{"detail", "revision required"}});
    const auto state = store_.snapshot();
    if (body["revision"] != state.at("revision"))
      return reply(409, {{"detail", "settings changed; reload and try again"},
                         {"revision", state.at("revision")}});
    if (apply_request_file_.empty())
      return reply(503, {{"detail", "settings application is unavailable"}});
    try {
      // #31: the root rapid-apply helper reads this back to act; create it
      // owner-only so no other local user can influence that action.
      atomic_file(apply_request_file_,
                  Json{{"revision", state.at("revision")},
                       {"settings", state.at("settings")}}.dump(),
                  fs::perms::owner_read | fs::perms::owner_write);
    } catch (const std::exception &) {
      return reply(503, {{"detail", "settings application queue is unavailable"}});
    }
    return reply(202, {{"revision", state.at("revision")}, {"queued", true}});
  }
  if (path == "/api/v1/settings/confirm-display" && request.method == "POST") {
    if (!equal(header(request, "x-csrf-token"), session->second.csrf))
      return reply(403, {{"detail", "invalid CSRF token"}});
    const auto body = Json::parse(request.body, nullptr, false);
    if (!body.is_object() || body.size() != 1 || !body.contains("revision") ||
        !body["revision"].is_number_integer())
      return reply(400, {{"detail", "revision required"}});
    const auto state = store_.snapshot();
    bool awaiting = false;
    try {
      const auto result = Json::parse(read_file(apply_result_file_));
      awaiting = result.is_object() && result.value("revision", -1) == state.at("revision") &&
                 result.value("rotation", std::string{}) == "awaiting_confirmation";
    } catch (const std::exception &) {}
    if (body["revision"] != state.at("revision") || !awaiting)
      return reply(409, {{"detail", "no orientation change is awaiting confirmation"}});
    if (display_confirm_file_.empty())
      return reply(503, {{"detail", "orientation confirmation is unavailable"}});
    try {
      // #31: root rapid-apply polls this to keep a previewed orientation.
      atomic_file(display_confirm_file_, Json{{"revision", state.at("revision")}}.dump(),
                  fs::perms::owner_read | fs::perms::owner_write);
    } catch (const std::exception &) {
      return reply(503, {{"detail", "orientation confirmation is unavailable"}});
    }
    return reply(202, {{"revision", state.at("revision")}, {"confirmation_queued", true}});
  }
  if (path == "/api/v1/peers" && request.method == "GET")
    return reply(200, {{"peers", store_.peers()}});
  if (path == "/api/v1/peers/revoke" && request.method == "POST") {
    if (!equal(header(request, "x-csrf-token"), session->second.csrf))
      return reply(403, {{"detail", "invalid CSRF token"}});
    const auto body = Json::parse(request.body, nullptr, false);
    if (!body.is_object() || body.size() != 1 || !body.contains("id") || !body["id"].is_string())
      return reply(400, {{"detail", "paired PC id required"}});
    const auto id = body["id"].get<std::string>();
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
      return reply(400, {{"detail", "invalid paired PC id"}});
    if (!store_.revoke_peer(id)) return reply(404, {{"detail", "paired PC not found"}});
    return reply(200, {{"revoked", id}});
  }
  if (path == "/api/v1/wifi" && request.method == "POST") {
    if (!equal(header(request, "x-csrf-token"), session->second.csrf))
      return reply(403, {{"detail", "invalid CSRF token"}});
    const auto body = Json::parse(request.body, nullptr, false);
    if (!body.is_object() || body.size() != 3 || !body.contains("revision") ||
        !body["revision"].is_number_integer() || !body.contains("ssid") || !body["ssid"].is_string() ||
        !body.contains("password") || !body["password"].is_string())
      return reply(400, {{"detail", "revision, Wi-Fi name and password required"}});
    const auto ssid = body["ssid"].get<std::string>(), password = body["password"].get<std::string>();
    const auto printable = [](const std::string &value) { for (unsigned char c : value) if (c < 0x20 || c == 0x7f) return false; return true; };
    const bool hexadecimal = password.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
    if (ssid.empty() || ssid.size() > 32 || !printable(ssid) || password.size() < 8 || !printable(password) ||
        (password.size() > 63 && (password.size() != 64 || !hexadecimal)))
      return reply(400, {{"detail", "invalid Wi-Fi name or password"}});
    const auto state = store_.snapshot();
    if (body["revision"] != state.at("revision"))
      return reply(409, {{"detail", "settings changed; reload and try again"}, {"revision", state.at("revision")}});
    if (wifi_request_file_.empty()) return reply(503, {{"detail", "Wi-Fi application is unavailable"}});
    // #26: this file carries the plaintext passphrase until rapid-wifi
    // consumes and deletes it; keep it owner-only so no other local process
    // can read it off disk in that window (rapid-wifi itself runs as root
    // and can read it regardless of group/other bits).
    try {
      atomic_file(wifi_request_file_, Json{{"revision", state.at("revision")}, {"ssid", ssid}, {"password", password}}.dump());
      fs::permissions(wifi_request_file_, fs::perms::owner_read | fs::perms::owner_write,
                      fs::perm_options::replace);
    }
    catch (const std::exception &) { return reply(503, {{"detail", "Wi-Fi application queue is unavailable"}}); }
    return reply(202, {{"revision", state.at("revision")}, {"queued", true}});
  }
  if (path == "/api/v1/settings" && request.method == "POST") {
    if (!equal(header(request, "x-csrf-token"), session->second.csrf))
      return reply(403, {{"detail", "invalid CSRF token"}});
    const auto body = Json::parse(request.body, nullptr, false);
    if (!body.is_object() || body.size() != 2 || !body.contains("revision") ||
        !body["revision"].is_number_integer() || !body.contains("settings"))
      return reply(400, {{"detail", "revision and settings required"}});
    try {
      if (!store_.update(body["revision"].get<std::int64_t>(), body["settings"])) {
        const auto state = store_.snapshot();
        return reply(409, {{"detail", "settings changed; reload and try again"},
                           {"revision", state.at("revision")}, {"settings", state.at("settings")},
                           {"applied", false}});
      }
    } catch (const std::exception &error) {
      return reply(400, {{"detail", error.what()}});
    }
    const auto state = store_.snapshot();
    if (!apply_request_file_.empty()) {
      try {
        // #31: root rapid-apply reads this back to apply settings.
        atomic_file(apply_request_file_,
                    Json{{"revision", state.at("revision")}, {"settings", state.at("settings")}}.dump(),
                    fs::perms::owner_read | fs::perms::owner_write);
      } catch (const std::exception &) {
        return reply(503, {{"detail", "settings saved but application queue is unavailable"},
                           {"revision", state.at("revision")}, {"settings", state.at("settings")},
                           {"applied", false}});
      }
    }
    return reply(200, {{"revision", state.at("revision")}, {"settings", state.at("settings")},
                       {"applied", false}, {"apply_queued", queued(apply_request_file_)}});
  }
  if (path == "/api/v1/auth/logout" && request.method == "POST") {
    if (!equal(header(request, "x-csrf-token"), session->second.csrf))
      return reply(403, {{"detail", "invalid CSRF token"}});
    sessions_.erase(session);
    auto response = reply(200, {{"authenticated", false}});
    response.headers.emplace_back("Set-Cookie", "rapid_setup=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return response;
  }
  return reply(404, {{"detail", "not found"}});
}
} // namespace rapid::native
