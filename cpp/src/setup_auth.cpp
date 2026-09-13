#include "rapid/setup_auth.hpp"
#include <argon2.h>
#include <openssl/crypto.h>

namespace rapid::native {
namespace {
std::string header(const Request &request, const std::string &key) {
  const auto found = request.headers.find(key);
  return found == request.headers.end() ? "" : found->second;
}
Response reply(int code, const Json &body) { return {code, body.dump()}; }
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
} // namespace

SetupAuth::SetupAuth(SetupStore &store, int port, std::function<double()> clock,
                     std::string enrollment_token, std::string host,
                     fs::path apply_request_file, fs::path apply_result_file,
                     fs::path firstboot_status_file, fs::path wifi_request_file,
                     fs::path wifi_result_file)
    : store_(store), clock_(std::move(clock)),
      origin_("http://" + host + ":" + std::to_string(port)),
      authority_(std::move(host) + ":" + std::to_string(port)),
      enrollment_token_(std::move(enrollment_token)),
      apply_request_file_(std::move(apply_request_file)),
      apply_result_file_(std::move(apply_result_file)),
      wifi_request_file_(std::move(wifi_request_file)),
      wifi_result_file_(std::move(wifi_result_file)),
      firstboot_status_file_(std::move(firstboot_status_file)) {
  if (!enrollment_token_.empty() && (enrollment_token_.size() != 64 ||
      enrollment_token_.find_first_not_of("0123456789abcdef") != std::string::npos))
    throw std::invalid_argument("enrollment token must contain 64 lowercase hexadecimal characters");
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
  if (request.body.size() > 1024)
    return reply(413, {{"detail", "request too large"}});
  if (request.method != "GET" && request.method != "POST")
    return {405, "{\"detail\":\"method not allowed\"}", "application/json", {{"Allow", "GET, POST"}}};
  if (request.method == "POST" &&
      (origin != origin_ || header(request, "content-type") != "application/json"))
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
    return reply(200, status);
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
          status.erase("bootstrap");
          status["owner_configured"] = true;
          status["state"] = "settings_application_required";
          atomic_file(firstboot_status_file_, status.dump() + "\n");
          fs::permissions(firstboot_status_file_,
                          fs::perms::owner_read | fs::perms::owner_write |
                              fs::perms::group_read,
                          fs::perm_options::replace);
        }
      } catch (const std::exception &error) {
        log(std::string("WARN setup: owner configured but cannot clear bootstrap status: ") +
            error.what());
      }
    }
    return reply(201, {{"owner_configured", true}, {"setup_complete", false}});
  }
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
        "; Path=/; HttpOnly; SameSite=Strict; Max-Age=1800");
    return response;
  }
  const auto token = cookie_token(request);
  const auto session = sessions_.find(hash_text(token));
  if (token.empty() || session == sessions_.end())
    return reply(401, {{"detail", "sign in required"}});
  if (path == "/api/v1/auth/session" && request.method == "GET")
    return reply(200, {{"authenticated", true}, {"csrf_token", session->second.csrf}});
  if (path == "/api/v1/settings" && request.method == "GET") {
    const auto state = store_.snapshot();
    Json response{{"revision", state.at("revision")}, {"settings", state.at("settings")},
                  {"applied", false},
                  {"apply_queued", !apply_request_file_.empty()}};
    try {
      if (!apply_result_file_.empty()) {
        const auto result = Json::parse(read_file(apply_result_file_));
        if (result.is_object() && result.value("revision", -1) == state.at("revision"))
          response["application"] = result;
      }
    } catch (const std::exception &) {}
    try {
      if (!wifi_result_file_.empty()) {
        const auto result = Json::parse(read_file(wifi_result_file_));
        if (result.is_object() && result.value("revision", -1) == state.at("revision"))
          response["wifi"] = result;
      }
    } catch (const std::exception &) {}
    return reply(200, response);
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
    try { atomic_file(wifi_request_file_, Json{{"revision", state.at("revision")}, {"ssid", ssid}, {"password", password}}.dump()); }
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
        atomic_file(apply_request_file_,
                    Json{{"revision", state.at("revision")}, {"settings", state.at("settings")}}.dump());
      } catch (const std::exception &) {
        return reply(503, {{"detail", "settings saved but application queue is unavailable"},
                           {"revision", state.at("revision")}, {"settings", state.at("settings")},
                           {"applied", false}});
      }
    }
    return reply(200, {{"revision", state.at("revision")}, {"settings", state.at("settings")},
                       {"applied", false}, {"apply_queued", !apply_request_file_.empty()}});
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
