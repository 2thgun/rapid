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

SetupAuth::SetupAuth(SetupStore &store, int port, std::function<double()> clock)
    : store_(store), clock_(std::move(clock)),
      origin_("http://127.0.0.1:" + std::to_string(port)),
      authority_("127.0.0.1:" + std::to_string(port)) {}

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
  // This server is deliberately loopback-only until trusted HTTPS/AP bootstrap
  // exists. Exact Host/Origin checks also reject browser DNS-rebinding requests.
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
    return reply(200, status);
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
    return reply(200, {{"revision", state.at("revision")}, {"settings", state.at("settings")},
                       {"applied", false}});
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
