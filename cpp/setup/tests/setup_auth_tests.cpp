#include "rapid/setup_auth.hpp"
#include <cmath>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <openssl/evp.h>
#include <sstream>

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
std::string derive_public_key(const std::string &private_hex) {
  std::string bytes(32, '\0');
  for (std::size_t i = 0; i < 32; ++i)
    bytes[i] = static_cast<char>(std::stoi(private_hex.substr(i * 2, 2), nullptr, 16));
  EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr,
      reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
  std::string result(32, '\0'); std::size_t size = result.size();
  require(key && EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char *>(result.data()), &size) > 0,
          "derive HTTPS pairing test public key");
  EVP_PKEY_free(key);
  std::ostringstream out;
  for (unsigned char byte : result)
    out << std::hex << std::setw(2) << std::setfill('0') << int(byte);
  return out.str();
}
std::string hex_text(const std::string &bytes) {
  std::ostringstream out;
  for (unsigned char byte : bytes)
    out << std::hex << std::setw(2) << std::setfill('0') << int(byte);
  return out.str();
}
} // namespace

int main() {
  try {
    TemporaryDirectory root;
    {
      SetupStore fresh(root.path / "browser-enrollment");
      double enrollment_time = 0;
      const auto token = unique_id() + unique_id();
      SetupAuth disabled(fresh, 8002);
      const auto enroll_request = request("/api/v1/auth/enroll", "POST",
          {{"token", token}, {"password", "browser-owner-password"}});
      require(disabled.handle(enroll_request).status == 403, "browser enrollment is opt-in");
      const auto bootstrap_status = root.path / "bootstrap-status.json";
      atomic_file(bootstrap_status, Json{{"owner_configured", false},
          {"state", "owner_enrollment_required"},
          {"bootstrap", {{"activation_token", token}}}}.dump());
      SetupAuth ap(fresh, 8002, [&] { return enrollment_time; }, token, "192.168.50.1",
                   {}, {}, bootstrap_status);
      auto ap_request = enroll_request;
      ap_request.headers["host"] = "192.168.50.1:8002";
      ap_request.headers["origin"] = "http://192.168.50.1:8002";
      require(ap.handle(ap_request).status == 201,
              "activation-token enrollment supports an exact AP authority");
      const auto cleared_status = Json::parse(read_file(bootstrap_status));
      require(cleared_status["owner_configured"] == true &&
                  cleared_status["state"] == "settings_application_required" &&
                  !cleared_status.contains("bootstrap"),
              "owner enrollment clears physical bootstrap details immediately");
      require(ap.handle(enroll_request).status == 403,
              "AP enrollment rejects a loopback Host or Origin");
      require(fresh.snapshot()["setup_complete"] == false,
              "AP owner enrollment does not complete provisioning");
    }
    {
      SetupStore fresh(root.path / "browser-enrollment-rate-limit");
      double enrollment_time = 0;
      const auto token = unique_id() + unique_id();
      SetupAuth browser(fresh, 8002, [&] { return enrollment_time; }, token);
      const auto enroll_request = request("/api/v1/auth/enroll", "POST",
          {{"token", token}, {"password", "browser-owner-password"}});
      auto status = browser.handle(request("/api/v1/setup"));
      require(Json::parse(status.body)["capabilities"]["browser_owner_enrollment"] == true &&
              status.body.find(token) == std::string::npos, "enrollment capability never exposes token");
      auto unsafe = enroll_request;
      unsafe.headers["origin"] = "https://attacker.example";
      require(browser.handle(unsafe).status == 403, "cross-origin enrollment rejected");
      auto wrong = request("/api/v1/auth/enroll", "POST",
          {{"token", std::string(64, '0')}, {"password", "browser-owner-password"}});
      for (int i = 0; i < 5; ++i)
        require(browser.handle(wrong).status == 403, "wrong activation token rejected");
      require(browser.handle(enroll_request).status == 429, "enrollment guesses rate limited");
      enrollment_time = 61;
      auto weak = request("/api/v1/auth/enroll", "POST", {{"token", token}, {"password", "short"}});
      require(browser.handle(weak).status == 400 && fresh.owner_hash().empty(),
              "invalid password does not consume enrollment");
      require(browser.handle(enroll_request).status == 201, "browser claims owner with activation token");
      require(browser.handle(enroll_request).status == 409, "token cannot replace owner");
      SetupAuth restarted(fresh, 8002, monotonic, token);
      require(restarted.handle(enroll_request).status == 409 &&
          Json::parse(restarted.handle(request("/api/v1/setup")).body)
              ["capabilities"]["browser_owner_enrollment"] == false,
          "restarting with old token cannot reopen enrollment");
      require(restarted.handle(request("/api/v1/auth/login", "POST",
          {{"password", "browser-owner-password"}})).status == 200,
          "browser owner can sign in after restart");
      require(fresh.snapshot()["setup_complete"] == false, "enrollment is not complete provisioning");
    }
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
            store.snapshot()["schema_version"] == 3, "schema migration preserves device identity and revision");
    double time = 0;
    const auto apply_directory = root.path / "apply";
    fs::create_directory(apply_directory);
    const auto apply_result = apply_directory / "result.json";
    const auto wifi_result = apply_directory / "wifi-result.json";
    const auto completion_status = apply_directory / "firstboot-status.json";
    const auto calibration = apply_directory / "touch-calibration.conf";
    atomic_file(calibration, "stale calibration\n");
    const auto calibration_request = apply_directory / "calibration-request.json";
    atomic_file(completion_status, Json{{"owner_configured", true},
                                        {"setup_complete", false},
                                        {"state", "settings_application_required"}}.dump());
    SetupAuth auth(store, 8002, [&] { return time; }, {}, "127.0.0.1",
                   apply_directory / "request.json", apply_result, completion_status,
                   apply_directory / "wifi-request.json", wifi_result, nullptr,
                   false, {}, true, calibration, calibration_request,
                   apply_directory / "display-confirm.json");
    const auto public_setup = Json::parse(auth.handle(request("/api/v1/setup")).body);
    require(public_setup["owner_configured"] == false &&
                public_setup["capabilities"]["settings_write"] == true,
            "new device has no default owner and advertises saved-profile writes");
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
    auto reset = request("/api/v1/calibration/reset", "POST", Json::object());
    reset.headers["cookie"] = session_cookie;
    reset.headers["x-csrf-token"] = credentials["csrf_token"];
    require(auth.handle(reset).status == 200 && !fs::exists(calibration),
            "owner can reset stale touchscreen calibration");
    auto start_calibration = request("/api/v1/calibration/start", "POST", Json::object());
    start_calibration.headers["cookie"] = session_cookie;
    require(auth.handle(start_calibration).status == 403 && !fs::exists(calibration_request),
            "panel calibration start requires the CSRF token");
    start_calibration.headers["x-csrf-token"] = credentials["csrf_token"];
    require(auth.handle(start_calibration).status == 200 && fs::exists(calibration_request) &&
                read_file(calibration_request).find("key") == std::string::npos,
            "owner can start calibration on the Pi display through a key-free request");
    require(login.headers[0].second.find("HttpOnly; SameSite=Strict") != std::string::npos,
            "session cookie has browser protections");
    auto settings = request("/api/v1/settings");
    settings.headers["cookie"] = session_cookie;
    require(auth.handle(settings).status == 200, "authenticated owner can read desired settings");
    auto response = Json::parse(auth.handle(settings).body);
    require(response["applied"] == false && response["apply_queued"] == false &&
                response.dump().find("argon2") == std::string::npos,
            "settings are explicitly unapplied and omit owner credentials");
    auto save = request("/api/v1/settings", "POST", {{"revision", 7}, {"settings",
        {{"hostname", "rapid-renamed"}, {"rotation", 180}, {"boot_network", "home_then_ap"}}}});
    save.headers["cookie"] = session_cookie;
    require(auth.handle(save).status == 403, "settings write requires CSRF token");
    save.headers["x-csrf-token"] = credentials["csrf_token"].get<std::string>();
    require(auth.handle(save).status == 200, "owner can save validated desired settings");
    const auto queued = Json::parse(read_file(apply_directory / "request.json"));
    require(queued["revision"] == 8 && queued["settings"]["hostname"] == "rapid-renamed",
            "validated settings save queues the exact root application request");
    response = Json::parse(auth.handle(settings).body);
    require(response["revision"] == 8 && response["settings"]["hostname"] == "rapid-renamed" &&
                response["settings"]["rotation"] == 180 && response["applied"] == false &&
                response["apply_queued"] == true,
            "settings save is persistent but does not claim application");
    fs::remove(apply_directory / "request.json");
    response = Json::parse(auth.handle(settings).body);
    require(response["apply_queued"] == false,
            "settings status clears its queued flag after the applicator consumes the request");
    atomic_file(apply_result, Json{{"revision", 8}, {"hostname_applied", true},
                                  {"rotation", "awaiting_confirmation"},
                                  {"pending", Json::array({"rotation", "wifi"})}}.dump());
    response = Json::parse(auth.handle(settings).body);
    require(response["application"]["hostname_applied"] == true &&
                response["application"]["rotation"] == "awaiting_confirmation",
            "matching root application result is available to the authenticated owner");
    const auto display_confirm = apply_directory / "display-confirm.json";
    auto confirm_display = request("/api/v1/settings/confirm-display", "POST", {{"revision", 8}});
    confirm_display.headers["cookie"] = session_cookie;
    require(auth.handle(confirm_display).status == 403 && !fs::exists(display_confirm),
            "orientation confirmation requires the CSRF token");
    confirm_display.headers["x-csrf-token"] = credentials["csrf_token"].get<std::string>();
    auto stale_confirm = confirm_display;
    stale_confirm.body = Json{{"revision", 7}}.dump();
    require(auth.handle(stale_confirm).status == 409 && !fs::exists(display_confirm),
            "orientation confirmation must name the current revision");
    atomic_file(wifi_result, Json{{"revision", 8}, {"connected", true}}.dump());
    response = Json::parse(auth.handle(settings).body);
    require(response["setup_complete"] == false,
            "onboarding is not complete while an orientation change awaits confirmation");
    require(auth.handle(confirm_display).status == 202 &&
                Json::parse(read_file(display_confirm))["revision"] == 8,
            "owner can keep a previewed orientation from the browser");
    atomic_file(apply_result, Json{{"revision", 8}, {"hostname_applied", true}, {"rotation", "confirmed"},
                                  {"pending", Json::array({"wifi"})}}.dump());
    require(auth.handle(confirm_display).status == 409, "a kept orientation cannot be confirmed again");
    response = Json::parse(auth.handle(settings).body);
    require(response["setup_complete"] == true && store.snapshot()["setup_complete"] == true,
            "matching settings and Home Wi-Fi completion finish onboarding persistently");
    require(Json::parse(read_file(completion_status))["state"] == "complete",
            "first-boot status records completed onboarding for local recovery surfaces");
    auto retry = request("/api/v1/settings/retry", "POST", {{"revision", 8}});
    retry.headers["cookie"] = session_cookie;
    require(auth.handle(retry).status == 403, "settings retry requires CSRF token");
    retry.headers["x-csrf-token"] = credentials["csrf_token"].get<std::string>();
    require(auth.handle(retry).status == 202 && fs::is_regular_file(apply_directory / "request.json"),
            "owner can retry the current settings application without changing its revision");
    fs::remove(apply_directory / "request.json");
    atomic_file(apply_result, Json{{"revision", 7}, {"hostname_applied", true}}.dump());
    response = Json::parse(auth.handle(settings).body);
    require(!response.contains("application"),
            "stale root application result is hidden from the authenticated owner");
    require(store.remember_peer(std::string(32, 'a'), "Driver PC", std::string(64, 'b'), 1.0),
            "paired PC fixture is stored privately");
    require(store.remember_peer(std::string(32, 'a'), "Driver PC 2", std::string(64, 'c'), 2.0),
            "remembering an existing PC refreshes its record");
    auto refreshed = store.peers();
    require(refreshed.size() == 1 && refreshed[0]["label"] == "Driver PC 2" &&
                refreshed[0]["last_seen"] == 2.0,
            "reconnecting PC updates label and last-seen without duplicating it");
    bool invalid_time = false;
    try { store.remember_peer(std::string(32, 'd'), "Bad clock", std::string(64, 'e'), NAN); }
    catch (const std::invalid_argument &) { invalid_time = true; }
    require(invalid_time, "non-finite paired PC timestamps are rejected");
    auto peers = request("/api/v1/peers");
    peers.headers["cookie"] = session_cookie;
    response = Json::parse(auth.handle(peers).body);
    require(response["peers"].size() == 1 && response.dump().find(std::string(64, 'b')) == std::string::npos,
            "owner peer list excludes telemetry key material");
    auto revoke = request("/api/v1/peers/revoke", "POST", {{"id", std::string(32, 'a')}});
    revoke.headers["cookie"] = session_cookie;
    require(auth.handle(revoke).status == 403, "peer revoke requires CSRF token");
    revoke.headers["x-csrf-token"] = credentials["csrf_token"].get<std::string>();
    require(auth.handle(revoke).status == 200 && store.peers().empty(),
            "owner can revoke one paired PC");
    require(auth.handle(save).status == 409, "stale settings revision cannot overwrite newer values");
    auto malformed_save = save;
    malformed_save.body = Json{{"revision", 8}, {"settings", {{"hostname", "invalid"}}}}.dump();
    require(auth.handle(malformed_save).status == 400, "settings write validates exact settings shape");
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
    require(store.snapshot()["setup_complete"] == true,
            "successful settings and Wi-Fi application remains complete across later auth activity");
    SetupStore pairing_store(root.path / "pairing");
    const auto pairing_panel = root.path / "pairing-panel.json";
    const auto pairing_approval = root.path / "pairing-approval.json";
    PairingCoordinator pairing(pairing_store, std::string(32, '1'), std::string(64, '2'),
                               pairing_panel, pairing_approval);
    SetupAuth pairing_auth(pairing_store, 8443, [&] { return time; }, {}, "127.0.0.1",
                           {}, {}, {}, {}, {}, &pairing, true, std::string(64, '2'));
    require(pairing_auth.enroll(password), "pairing owner enrollment");
    auto secure = [](const std::string &path, const std::string &method = "GET",
                     const Json &body = Json::object()) {
      auto result = request(path, method, body);
      result.headers["host"] = "127.0.0.1:8443";
      result.headers["origin"] = "https://127.0.0.1:8443";
      return result;
    };
    auto pairing_login_request = secure("/api/v1/auth/login", "POST", {{"password", password}});
    const auto pairing_login = pairing_auth.handle(pairing_login_request);
    require(pairing_login.headers.size() > 0 && pairing_login.status == 200,
            "pairing owner login");
    require(pairing_login.headers[0].second.find("Secure") != std::string::npos,
            "HTTPS pairing session cookie is Secure");
    const auto pairing_cookie = cookie(pairing_login);
    const auto pairing_csrf = Json::parse(pairing_login.body)["csrf_token"].get<std::string>();
    auto window = secure("/api/v1/pairing/window", "POST", {{"open", true}});
    window.headers["cookie"] = pairing_cookie;
    window.headers["x-csrf-token"] = pairing_csrf;
    require(pairing_auth.handle(window).status == 200, "owner opens pairing window");
    auto state_request = secure("/api/v1/pairing/state");
    state_request.headers["cookie"] = pairing_cookie;
    require(Json::parse(pairing_auth.handle(state_request).body)["active"] == true,
            "pairing state reports an open window");
    require(Json::parse(pairing_auth.handle(secure("/api/v1/setup")).body)
                ["capabilities"]["pairing"] == true,
            "HTTPS setup advertises configured pairing capability");
    require(Json::parse(pairing_auth.handle(secure("/api/v1/setup")).body)
                ["certificate_fingerprint"] == std::string(64, '2'),
            "HTTPS setup publishes the certificate fingerprint");
    SetupStore insecure_store(root.path / "insecure-pairing");
    PairingCoordinator insecure_pairing(insecure_store, std::string(32, '3'), std::string(64, '4'));
    SetupAuth insecure_auth(insecure_store, 8002, [&] { return time; }, {}, "127.0.0.1",
                            {}, {}, {}, {}, {}, &insecure_pairing, false);
    require(insecure_auth.enroll(password), "insecure pairing test owner enrollment");
    auto insecure_login = request("/api/v1/auth/login", "POST", {{"password", password}});
    const auto insecure_login_response = insecure_auth.handle(insecure_login);
    auto insecure_window = request("/api/v1/pairing/window", "POST", {{"open", true}});
    insecure_window.headers["cookie"] = cookie(insecure_login_response);
    insecure_window.headers["x-csrf-token"] = Json::parse(insecure_login_response.body)["csrf_token"].get<std::string>();
    require(insecure_auth.handle(insecure_window).status == 404,
            "HTTP setup cannot expose pairing routes even with a coordinator");
    auto insecure_pairing_request = request("/api/v1/pairing/request", "POST",
        {{"label", "HTTP PC"}, {"companion_public_key", std::string(64, 'a')}});
    insecure_pairing_request.headers.erase("origin");
    require(insecure_auth.handle(insecure_pairing_request).status == 403,
            "HTTP setup rejects companion pairing requests");
    const std::string companion_private(64, '1');
    const auto companion_public = derive_public_key(companion_private);
    auto pairing_request = secure("/api/v1/pairing/request", "POST",
        {{"label", "Test PC"}, {"companion_public_key", companion_public}});
    pairing_request.headers.erase("origin");
    const auto requested = pairing_auth.handle(pairing_request);
    require(requested.status == 201, "HTTPS companion pairing request accepted");
    const auto transaction = Json::parse(requested.body)["transaction_id"].get<std::string>();
    auto pending_request = secure("/api/v1/pairing/pending");
    pending_request.headers["cookie"] = pairing_cookie;
    const auto pending_response = pairing_auth.handle(pending_request);
    require(pending_response.status == 200, "owner sees pending pairing code");
    const auto pending_json = Json::parse(pending_response.body);
    auto approve_request = secure("/api/v1/pairing/approve", "POST",
        {{"transaction_id", transaction}, {"code", pending_json["code"]}});
    approve_request.headers["cookie"] = pairing_cookie;
    approve_request.headers["x-csrf-token"] = pairing_csrf;
    require(pairing_auth.handle(approve_request).status == 200, "owner approves pairing code");
    auto result_request = secure("/api/v1/pairing/result?transaction_id=" + transaction);
    const auto result = pairing_auth.handle(result_request);
    require(result.status == 200 && Json::parse(result.body)["approved"] == true &&
                pairing_store.peers().size() == 1 &&
                result.body.find("telemetry_key") == std::string::npos,
            "HTTPS pairing returns one-use envelope without telemetry key material");
    const auto result_json = Json::parse(result.body);
    PairingEnvelope envelope{result_json["ephemeral_public_key"].get<std::string>(),
                             result_json["nonce"].get<std::string>(),
                             result_json["ciphertext"].get<std::string>(),
                             result_json["tag"].get<std::string>()};
    require(open_pairing_key(std::string(32, '1'), transaction,
                             Json::parse(requested.body)["nonce"], companion_private, envelope) ==
                hex_text(pairing_store.peer_keys().at(0)),
            "HTTPS pairing envelope decrypts to the private stored telemetry key");
    bool transaction_tamper_rejected = false;
    try {
      auto altered_transaction = transaction;
      altered_transaction[0] = altered_transaction[0] == '0' ? '1' : '0';
      (void)open_pairing_key(std::string(32, '1'), altered_transaction,
                             Json::parse(requested.body)["nonce"], companion_private, envelope);
    } catch (const std::exception&) { transaction_tamper_rejected = true; }
    require(transaction_tamper_rejected,
            "HTTPS pairing envelope binds its transaction identifier");
    require(pairing_auth.handle(result_request).status == 202,
            "pairing envelope cannot be replayed");
    auto malformed_result = secure("/api/v1/pairing/result?transaction_id=bad&extra=1");
    require(pairing_auth.handle(malformed_result).status == 400,
            "malformed pairing transaction is rejected explicitly");
    auto extra_result = secure("/api/v1/pairing/result?transaction_id=" + transaction + "&extra=1");
    require(pairing_auth.handle(extra_result).status == 400,
            "pairing result rejects extra query parameters");
    auto prefixed_result = secure("/api/v1/pairing/result?extra=1&transaction_id=" + transaction);
    require(pairing_auth.handle(prefixed_result).status == 400,
            "pairing result rejects smuggled transaction parameters");
    auto cancel = secure("/api/v1/pairing/window", "POST", {{"open", false}});
    cancel.headers["cookie"] = pairing_cookie;
    cancel.headers["x-csrf-token"] = pairing_csrf;
    require(pairing_auth.handle(cancel).status == 200,
            "owner can close pairing window after handoff");
    state_request = secure("/api/v1/pairing/state");
    state_request.headers["cookie"] = pairing_cookie;
    const auto closed_state = Json::parse(pairing_auth.handle(state_request).body);
    require(closed_state["active"] == false && closed_state["pending"] == false,
            "closing pairing window clears its pending state");
    window = secure("/api/v1/pairing/window", "POST", {{"open", true}});
    window.headers["cookie"] = pairing_cookie;
    window.headers["x-csrf-token"] = pairing_csrf;
    require(pairing_auth.handle(window).status == 200, "owner reopens pairing window");
    const std::string second_private(64, '2');
    const auto second_public = derive_public_key(second_private);
    auto panel_request = secure("/api/v1/pairing/request", "POST",
        {{"label", "Panel PC"}, {"companion_public_key", second_public}});
    panel_request.headers.erase("origin");
    const auto panel_created = pairing_auth.handle(panel_request);
    require(panel_created.status == 201 && fs::exists(pairing_panel),
            "panel approval flow publishes its local handoff");
    const auto panel_created_json = Json::parse(panel_created.body);
    const auto panel_details = Json::parse(read_file(pairing_panel));
    { std::ofstream approval(pairing_approval);
      approval << Json{{"transaction_id", panel_details["transaction_id"]},
                       {"code", panel_details["code"]}}.dump(); }
    auto panel_result = secure("/api/v1/pairing/result?transaction_id=" +
                               panel_details["transaction_id"].get<std::string>());
    const auto panel_response = pairing_auth.handle(panel_result);
    require(panel_response.status == 200 && pairing_store.peers().size() == 2 &&
                panel_response.body.find("telemetry_key") == std::string::npos,
            "physical panel approval completes HTTPS pairing");
    const auto panel_json = Json::parse(panel_response.body);
    PairingEnvelope panel_envelope{panel_json["ephemeral_public_key"].get<std::string>(),
                                   panel_json["nonce"].get<std::string>(),
                                   panel_json["ciphertext"].get<std::string>(),
                                   panel_json["tag"].get<std::string>()};
    require(open_pairing_key(std::string(32, '1'),
                             panel_details["transaction_id"].get<std::string>(),
                             panel_created_json["nonce"].get<std::string>(), second_private, panel_envelope) ==
                hex_text(pairing_store.peer_keys().at(1)),
            "second HTTPS pairing envelope decrypts to its independent telemetry key");
    auto peers_request = secure("/api/v1/peers");
    peers_request.headers["cookie"] = pairing_cookie;
    const auto peers_json = Json::parse(pairing_auth.handle(peers_request).body);
    const auto second_peer_id = peers_json["peers"].at(1)["id"].get<std::string>();
    auto revoke_request = secure("/api/v1/peers/revoke", "POST", {{"id", second_peer_id}});
    revoke_request.headers["cookie"] = pairing_cookie;
    revoke_request.headers["x-csrf-token"] = pairing_csrf;
    require(pairing_auth.handle(revoke_request).status == 200 && pairing_store.peers().size() == 1,
            "revoking the second paired PC preserves the first peer");
    peers_request = secure("/api/v1/peers");
    peers_request.headers["cookie"] = pairing_cookie;
    const auto remaining_peers = pairing_auth.handle(peers_request);
    require(remaining_peers.status == 200 && remaining_peers.body.find("\"key\"") == std::string::npos,
            "peer listing remains free of telemetry keys after revocation");
    std::cout << "setup migration, owner authentication, session and CSRF tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
