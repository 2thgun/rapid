#include "rapid/setup_auth.hpp"
#include <iostream>

using namespace rapid::native;
namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path() / ("rapid-auth-tests-" + unique_id());
  TemporaryDirectory() { fs::create_directory(path); }
  ~TemporaryDirectory() { std::error_code error; fs::remove_all(path, error); }
};
Request request(const std::string &path, const std::string &method = "GET", const Json &body = {}) {
  return {method, path, body.dump(), {{"host", "127.0.0.1:8002"},
      {"origin", "http://127.0.0.1:8002"}, {"content-type", "application/json"}}};
}
std::string cookie(const Response &response) {
  for (const auto &[name, value] : response.headers)
    if (name == "Set-Cookie") return value.substr(0, value.find(';'));
  throw std::runtime_error("missing session cookie");
}
} // namespace

int main() {
  try {
    TemporaryDirectory root;
    // Construct an actual schema-1 database, then migrate without losing identity.
    const auto directory = root.path / "state";
    fs::create_directory(directory);
    fs::permissions(directory, fs::perms::owner_all);
    const auto id = unique_id();
    {
      Database legacy(directory / "setup.db");
      legacy.exec("CREATE TABLE setup_state (id INTEGER PRIMARY KEY, revision INTEGER NOT NULL, document TEXT NOT NULL)");
      legacy.exec("INSERT INTO setup_state VALUES(1,7,?)", {Json{{"device_id", id}, {"setup_complete", false},
          {"settings", {{"hostname", "rapid-test"}, {"rotation", 180}, {"boot_network", "home_then_ap"}}}}.dump()});
      legacy.exec("PRAGMA user_version=1");
    }
    SetupStore store(directory);
    require(store.snapshot()["device_id"] == id && store.snapshot()["revision"] == 7 &&
            store.snapshot()["schema_version"] == 2, "schema migration preserves device identity and revision");
    double time = 0;
    SetupAuth auth(store, 8002, [&] { return time; });
    require(Json::parse(auth.handle(request("/api/v1/setup")).body)["owner_configured"] == false,
            "new device has no default owner");
    require(auth.handle(request("/api/v1/settings")).status == 401, "settings require owner login");
    bool rejected = false;
    try { auth.enroll("short"); } catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "short owner password rejected");
    const std::string password = "test-only-owner-password";
    require(auth.enroll(password) && !auth.enroll("replacement-password"), "owner claimed exactly once");
    require(store.owner_hash().starts_with("$argon2id$") && store.owner_hash().find(password) == std::string::npos,
            "password stored as salted Argon2id hash");
    const auto login_request = request("/api/v1/auth/login", "POST", {{"password", password}});
    auto cross_site = login_request;
    cross_site.headers["origin"] = "https://attacker.example";
    require(auth.handle(cross_site).status == 403, "cross-origin login blocked");
    cross_site = login_request;
    cross_site.headers["host"] = "attacker.example";
    require(auth.handle(cross_site).status == 403, "DNS rebinding host rejected");
    cross_site = login_request;
    cross_site.headers.erase("origin");
    require(auth.handle(cross_site).status == 403, "missing login origin rejected");
    cross_site = login_request;
    cross_site.headers["content-type"] = "text/plain";
    require(auth.handle(cross_site).status == 403, "simple cross-site form encoding rejected");
    auto malformed = login_request;
    malformed.body = "{";
    require(auth.handle(malformed).status == 400, "invalid JSON handled safely");
    const auto login = auth.handle(login_request);
    require(login.status == 200, "owner can log in");
    const auto credentials = Json::parse(login.body);
    const auto session_cookie = cookie(login);
    require(login.headers[0].second.find("HttpOnly; SameSite=Strict") != std::string::npos,
            "session cookie has browser protections");
    auto settings = request("/api/v1/settings");
    settings.headers["cookie"] = session_cookie;
    require(auth.handle(settings).status == 200, "authenticated owner can read desired settings");
    auto response = Json::parse(auth.handle(settings).body);
    require(response["applied"] == false && response.dump().find("argon2") == std::string::npos,
            "settings are explicitly unapplied and omit owner credentials");
    auto logout = request("/api/v1/auth/logout", "POST", Json::object());
    logout.headers["cookie"] = session_cookie;
    require(auth.handle(logout).status == 403, "logout requires CSRF token");
    logout.headers["x-csrf-token"] = credentials["csrf_token"].get<std::string>();
    require(auth.handle(logout).status == 200 && auth.handle(settings).status == 401,
            "logout immediately invalidates session");
    for (int i = 0; i < 3; ++i)
      require(auth.handle(request("/api/v1/auth/login", "POST", {{"password", "wrong"}})).status == 401,
              "incorrect password rejected");
    require(auth.handle(login_request).status == 429, "login attempts bounded including successful attempts");
    time = 61;
    auto renewed = auth.handle(login_request);
    require(renewed.status == 200, "login throttle expires");
    settings.headers["cookie"] = cookie(renewed);
    auto duplicate = settings;
    duplicate.headers["cookie"] += "; " + cookie(renewed);
    require(auth.handle(duplicate).status == 401, "ambiguous duplicate cookies rejected");
    SetupAuth restarted(store, 8002, [&] { return time; });
    require(restarted.handle(settings).status == 401 && restarted.handle(login_request).status == 200,
            "restart revokes sessions but preserves password");
    time += 1800;
    require(auth.handle(settings).status == 401, "session expires at 30 minutes");
    auto oversized = login_request;
    oversized.body = std::string(1025, 'x');
    require(auth.handle(oversized).status == 413, "oversized auth request rejected");
    require(store.snapshot()["setup_complete"] == false, "owner enrollment does not pretend setup is complete");
    std::cout << "setup migration, owner authentication, session and CSRF tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
