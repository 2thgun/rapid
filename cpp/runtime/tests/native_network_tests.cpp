#include "rapid/native.hpp"
#include "rapid/setup_auth.hpp"
#include "v4_stream.hpp"
#include <argon2.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ssl.hpp>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <set>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace rapid::native;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
void require(bool c, const char *text) {
  if (!c)
    throw std::runtime_error(text);
}
struct Process {
  pid_t pid;
  ~Process() {
    if (pid > 0) {
      kill(pid, SIGTERM);
      for (int i = 0; i < 100; ++i) {
        if (waitpid(pid, nullptr, WNOHANG) == pid)
          return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      kill(pid, SIGKILL);
      waitpid(pid, nullptr, 0);
    }
  }
};
http::response<http::string_body> request(int port, http::verb method,
                                          const std::string &path,
                                          const std::string &body = "",
                                          const std::map<std::string, std::string> &headers = {}) {
  asio::io_context io;
  tcp::socket socket(io);
  socket.connect(
      {asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)});
  http::request<http::string_body> req{method, path, 11};
  req.set(http::field::host, "127.0.0.1:" + std::to_string(port));
  req.set(http::field::content_type, "application/json");
  for (const auto &[name, value] : headers) req.set(name, value);
  req.body() = body;
  req.prepare_payload();
  http::write(socket, req);
  beast::flat_buffer buffer;
  http::response<http::string_body> reply;
  http::read(socket, buffer, reply);
  return reply;
}
http::response<http::string_body> tls_request(int port, http::verb method,
                                              const std::string &path) {
  asio::io_context io;
  asio::ssl::context context(asio::ssl::context::tls_client);
  context.set_verify_mode(asio::ssl::verify_none);
  asio::ssl::stream<tcp::socket> stream(io, context);
  stream.next_layer().connect(
      {asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)});
  stream.handshake(asio::ssl::stream_base::client);
  http::request<http::string_body> req{method, path, 11};
  req.set(http::field::host, "127.0.0.1:" + std::to_string(port));
  req.prepare_payload();
  http::write(stream, req);
  beast::flat_buffer buffer;
  http::response<http::string_body> reply;
  http::read(stream, buffer, reply);
  return reply;
}
void test_tls_material(const fs::path &certificate, const fs::path &private_key) {
  EVP_PKEY_CTX *key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  EVP_PKEY *key = nullptr;
  require(key_context && EVP_PKEY_keygen_init(key_context) > 0 &&
              EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048) > 0 &&
              EVP_PKEY_keygen(key_context, &key) > 0,
          "generate test TLS key");
  EVP_PKEY_CTX_free(key_context);
  X509 *cert = X509_new();
  require(cert && X509_set_version(cert, 2) == 1 &&
              ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1 &&
              X509_gmtime_adj(X509_get_notBefore(cert), 0) &&
              X509_gmtime_adj(X509_get_notAfter(cert), 3600) &&
              X509_set_pubkey(cert, key) == 1,
          "create test TLS certificate");
  X509_NAME *name = X509_get_subject_name(cert);
  require(name &&
              X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                  reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0) == 1 &&
              X509_set_issuer_name(cert, name) == 1 &&
              X509_sign(cert, key, EVP_sha256()) > 0,
          "sign test TLS certificate");
  FILE *cert_file = std::fopen(certificate.c_str(), "wb");
  FILE *key_file = std::fopen(private_key.c_str(), "wb");
  require(cert_file && key_file && PEM_write_X509(cert_file, cert) == 1 &&
              PEM_write_PrivateKey(key_file, key, nullptr, nullptr, 0, nullptr, nullptr) == 1,
          "write test TLS material");
  std::fclose(cert_file);
  std::fclose(key_file);
  X509_free(cert);
  EVP_PKEY_free(key);
  fs::permissions(certificate, fs::perms::owner_read | fs::perms::owner_write);
  fs::permissions(private_key, fs::perms::owner_read | fs::perms::owner_write);
}
int main(int argc, char **argv) {
  try {
    require(argc == 5, "Pi binary, assets, archive binary and setup binary required");
    auto root =
        fs::temp_directory_path() / ("rapid-network-tests-" + unique_id());
    fs::create_directories(root);
    int port = 18000 + (getpid() % 10000), udp = port + 10000;
    std::string token = "native-network-test-only",
                salt = "native-network-test-salt";
    // The runtime only accepts authenticated v4, so the test sender and the
    // rapid-pi process share this HMAC key (32 bytes of 0x11 as 64 hex).
    const std::string companion_key_hex(64, '1');
    char hash[256];
    require(argon2id_hash_encoded(2, 8192, 1, token.data(), token.size(),
                                  salt.data(), salt.size(), 32, hash,
                                  sizeof hash) == ARGON2_OK,
            "test credentials");
    Process archive{fork()};
    if (archive.pid == 0) {
      setenv("RAPID_ARCHIVE_DATA_DIR", (root / "archive").c_str(), 1);
      setenv("RAPID_ARCHIVE_HOST", "127.0.0.1", 1);
      setenv("RAPID_ARCHIVE_PORT", std::to_string(port + 1).c_str(), 1);
      setenv("RAPID_ASSETS_DIRECTORY", argv[2], 1);
      setenv("RAPID_ARCHIVE_OWNER_PASSWORD_HASH", hash, 1);
      setenv("RAPID_ARCHIVE_INGEST_TOKEN_HASH", hash, 1);
      int fd = ::open((root / "archive.log").c_str(), O_WRONLY | O_CREAT, 0600);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      ::close(fd);
      execl(argv[3], argv[3], nullptr);
      _exit(127);
    }
    bool archive_ready = false;
    for (int i = 0; i < 100; ++i) {
      try {
        archive_ready =
            request(port + 1, http::verb::get, "/healthz").result_int() == 200;
        if (archive_ready)
          break;
      } catch (...) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(archive_ready, "archive ready");
    Process child{fork()};
    if (child.pid == 0) {
      setenv("RAPID_CONFIG", "/nonexistent/rapid-test.toml", 1);
      setenv("RAPID_APP_HOST", "127.0.0.1", 1);
      setenv("RAPID_APP_PORT", std::to_string(port).c_str(), 1);
      setenv("RAPID_COMPANION_PORT", std::to_string(udp).c_str(), 1);
      setenv("RAPID_COMPANION_KEY", companion_key_hex.c_str(), 1);
      setenv("RAPID_NETWORK_CONTROL_DIRECTORY",
             (root / "network-control").c_str(), 1);
      setenv("RAPID_ASSETS_DIRECTORY", argv[2], 1);
      setenv("RAPID_DATABASE_PATH", (root / "rapid.db").c_str(), 1);
      setenv("RAPID_SETUP_STATE_DIRECTORY", (root / "setup").c_str(), 1);
      setenv("RAPID_TELEMETRY_DIRECTORY", (root / "telemetry").c_str(), 1);
      setenv("RAPID_UPLOAD_ENABLED", "false", 1);
      setenv("RAPID_ACC_ENABLED", "false", 1);
      setenv("RAPID_UPLOAD_ENABLED", "true", 1);
      setenv("RAPID_UPLOAD_URL",
             ("http://127.0.0.1:" + std::to_string(port + 1)).c_str(), 1);
      setenv("RAPID_UPLOAD_TOKEN", token.c_str(), 1);
      setenv("RAPID_UPLOAD_POLICY", "all", 1);
      setenv("RAPID_UPLOAD_QUEUE_PATH", (root / "queue.db").c_str(), 1);
      int fd = ::open((root / "runtime.log").c_str(), O_WRONLY | O_CREAT, 0600);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      ::close(fd);
      execl(argv[1], argv[1], nullptr);
      _exit(127);
    }
    bool ready = false;
    for (int i = 0; i < 100; ++i) {
      try {
        ready = request(port, http::verb::get, "/healthz").result_int() == 200;
        if (ready)
          break;
      } catch (...) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(ready, "server ready");
    const auto setup_reply = request(port, http::verb::get, "/api/v1/setup");
    require(setup_reply.result_int() == 200 &&
                setup_reply[http::field::cache_control] == "no-store",
            "setup status available without caching");
    const auto setup = Json::parse(setup_reply.body());
    require(setup["setup_complete"] == false && setup["revision"] == 1 &&
                setup["capabilities"]["settings_write"] == false && !setup.contains("settings"),
            "setup status reports incomplete, read-only foundation");
    require(request(port, http::verb::post, "/api/v1/setup", "{}").result_int() == 405,
            "setup mutation unavailable until authenticated management is implemented");
    fs::create_directories(root / "network-control");
    require(request(port, http::verb::post, "/api/v1/network/mode",
                    "{\"mode\":\"invalid\"}")
                    .result_int() == 400,
            "network setting rejects unknown modes");
    for (const auto *mode : {"home", "ap", "off"}) {
      const auto response = request(port, http::verb::post,
          "/api/v1/network/mode", Json{{"mode", mode}}.dump());
      require(response.result_int() == 202, "network mode request accepted");
      require(Json::parse(read_file(root / "network-control" / "request"))["mode"] ==
                  mode, "network mode queued in isolated test directory");
    }
    atomic_file(root / "network-control" / "state", "{\"mode\":\"ap\"}");
    auto network_status = request(port, http::verb::get, "/api/v1/network/mode");
    require(network_status.result_int() == 200 &&
                Json::parse(network_status.body())["available"] == true,
            "network status is available");
    const auto root_reply = request(port, http::verb::get, "/");
    require(root_reply.result_int() == 302 &&
                root_reply[http::field::location] == "/telemetry",
            "root redirects to the live telemetry page (#24, no browser dashboard)");
    require(request(port, http::verb::get, "/telemetry")
                    .body()
                    .find("new WebSocket") != std::string::npos,
            "engineering assets");
    require(request(port, http::verb::put, "/api/v1/session/upload",
                    "{\"enabled\":\"false\"}")
                    .result_int() == 400,
            "upload setting rejects strings");
    asio::io_context io;
    asio::ip::udp::socket sender(io, asio::ip::udp::v4());
    asio::ip::udp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), udp);
    rapid::test::V4Stream stream(rapid::test::v4_run_id());
    stream.rate = 10;
    sender.send_to(asio::buffer(std::string("[]")), endpoint);
    sender.send_to(asio::buffer(stream.metadata(
        0, "Network Track", "Network Car", "Network Driver", "Race", "900")),
        endpoint);
    for (int i = 0; i < 12; ++i) {
      const auto mask = rapid::test::v4_mask(
          {rapid::test::ch_throttle, rapid::test::ch_brake,
           rapid::test::ch_gear, rapid::test::ch_rpm,
           rapid::test::ch_steering, rapid::test::ch_g_x,
           rapid::test::ch_g_y, rapid::test::ch_g_z,
           rapid::test::ch_lap_number});
      auto packet = stream.telemetry(
          std::uint64_t(i + 1) * 100000, mask,
          {{rapid::test::ch_rpm, float(5000 + i)},
           {rapid::test::ch_steering, .1f},
           {rapid::test::ch_g_x, 0.f},
           {rapid::test::ch_g_y, 1.f},
           {rapid::test::ch_g_z, 0.f},
           {rapid::test::ch_throttle, .5f},
           {rapid::test::ch_lap_number, float(i < 10 ? 1 : 2)}});
      sender.send_to(asio::buffer(packet), endpoint);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Json state;
    bool received = false;
    for (int i = 0; i < 50; ++i) {
      state = Json::parse(request(port, http::verb::get, "/api/live").body());
      received = state["runtime"] == "cpp" && state["rpm"] == 5011 &&
                 state["recording"] == true && state["recorded_samples"] == 12;
      if (received)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(received, "UDP to live recording state");
    beast::websocket::stream<tcp::socket> ws(io);
    ws.next_layer().connect({asio::ip::make_address("127.0.0.1"),
                             static_cast<unsigned short>(port)});
    ws.handshake("127.0.0.1", "/api/v1/live?history=30");
    beast::flat_buffer buffer;
    ws.read(buffer);
    auto events = Json::parse(beast::buffers_to_string(buffer.data()));
    require(events["events"].size() == 12, "WebSocket history");
    boost::system::error_code ec;
    ws.next_layer().close(ec);
    // Display clients (Qt panel, browser dashboard) ask for ?mode=state
    // instead: the same JSON shape GET /api/live returns, pushed on its own
    // at ~30 Hz, with no per-client backlog -- the newest snapshot only.
    beast::websocket::stream<tcp::socket> live_state(io);
    live_state.next_layer().connect({asio::ip::make_address("127.0.0.1"),
                                     static_cast<unsigned short>(port)});
    live_state.handshake("127.0.0.1", "/api/v1/live?mode=state");
    beast::flat_buffer state_buffer;
    live_state.read(state_buffer);
    auto pushed = Json::parse(beast::buffers_to_string(state_buffer.data()));
    require(pushed["runtime"] == "cpp" && pushed["rpm"] == 5011 &&
                pushed.contains("sender_lag_ms") && pushed.contains("process_ms"),
            "WebSocket state push sends the live /api/live snapshot shape");
    const auto fast_mask = rapid::test::v4_mask(
        {rapid::test::ch_throttle, rapid::test::ch_brake,
         rapid::test::ch_gear, rapid::test::ch_rpm,
         rapid::test::ch_steering, rapid::test::ch_g_x,
         rapid::test::ch_g_y, rapid::test::ch_g_z,
         rapid::test::ch_lap_number});
    auto fast_packet = stream.telemetry(
        14 * 100000, fast_mask,
        {{rapid::test::ch_rpm, 9001.f},
         {rapid::test::ch_steering, .1f},
         {rapid::test::ch_g_x, 0.f},
         {rapid::test::ch_g_y, 1.f},
         {rapid::test::ch_g_z, 0.f},
         {rapid::test::ch_throttle, .5f},
         {rapid::test::ch_lap_number, 2.f}});
    sender.send_to(asio::buffer(fast_packet), endpoint);
    bool pushed_latest = false;
    for (int i = 0; i < 60 && !pushed_latest; ++i) {
      state_buffer.consume(state_buffer.size());
      live_state.read(state_buffer);
      auto latest = Json::parse(beast::buffers_to_string(state_buffer.data()));
      pushed_latest = latest["rpm"] == 9001;
    }
    require(pushed_latest,
            "WebSocket state push coalesces to the newest runtime snapshot");
    boost::system::error_code state_ec;
    live_state.next_layer().close(state_ec);
    bool finalized = false;
    for (int i = 0; i < 40; ++i) {
      state =
          Json::parse(request(port, http::verb::get, "/api/v1/status").body());
      if (state["recording"] == false &&
          state["last_bundle_path"].is_string()) {
        finalized = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(finalized, "stale sender finalizes session");
    require(fs::exists(fs::path(state["last_bundle_path"].get<std::string>()) /
                       "full-session.ld"),
            "network recording output");
    require(request(port, http::verb::get, "/missing").result_int() == 404,
            "unknown route");
    bool uploaded = false;
    for (int i = 0; i < 100; ++i) {
      auto current =
          Json::parse(request(port, http::verb::get, "/api/v1/status").body());
      if (current["upload_state"] == "complete") {
        uploaded = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(uploaded, "native uploader commits to native archive");
    Database archive_db(root / "archive" / "archive.sqlite3");
    auto rows = archive_db.query(
        "SELECT archive_relpath FROM sessions WHERE status='committed'");
    require(rows.size() == 1, "one archive bundle committed");
    auto archived = root / "archive" / "committed" /
                    rows[0]["archive_relpath"].get<std::string>();
    require(
        hash_file(archived / "full-session.ld") ==
            hash_file(fs::path(state["last_bundle_path"].get<std::string>()) /
                      "full-session.ld"),
        "uploaded LD digest matches recorder");
    const auto activation_token = unique_id() + unique_id();
    const auto token_file = root / "enrollment-token";
    atomic_file(token_file, activation_token + "\n");
    fs::permissions(token_file, fs::perms::owner_read | fs::perms::owner_write);
    Process management{fork()};
    if (management.pid == 0) {
      int fd = ::open((root / "setup.log").c_str(), O_WRONLY | O_CREAT, 0600);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      ::close(fd);
      execl(argv[4], argv[4], "--state-directory", (root / "owner").c_str(),
            "--assets", argv[2], "--port", std::to_string(port + 2).c_str(),
            "--enrollment-token-file", token_file.c_str(), nullptr);
      _exit(127);
    }
    bool management_ready = false;
    for (int i = 0; i < 100; ++i) {
      try {
        const auto setup_response = request(port + 2, http::verb::get, "/api/v1/setup");
        management_ready = setup_response.result_int() == 200 &&
            Json::parse(setup_response.body())["capabilities"]["settings_write"] == true;
        if (management_ready) break;
      } catch (...) {}
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(management_ready, "loopback management server ready");
    auto page = request(port + 2, http::verb::get, "/setup");
    require(page.result_int() == 200 && page.body().find("Owner sign-in") != std::string::npos &&
            !page["Content-Security-Policy"].empty(), "setup page served with security policy");
    require(request(port + 2, http::verb::get, "/api/v1/settings").result_int() == 401,
            "HTTP settings require authentication");
    const auto claimed = request(port + 2, http::verb::post, "/api/v1/auth/enroll",
        Json{{"token", activation_token}, {"password", "network-test-owner-password"}}.dump(),
        {{"Origin", "http://127.0.0.1:" + std::to_string(port + 2)}});
    require(claimed.result_int() == 201 && Json::parse(claimed.body())["setup_complete"] == false,
            "HTTP browser enrollment consumes private file token without claiming setup completion");
    const auto login = request(port + 2, http::verb::post, "/api/v1/auth/login",
        Json{{"password", "network-test-owner-password"}}.dump(),
        {{"Origin", "http://127.0.0.1:" + std::to_string(port + 2)}});
    require(login.result_int() == 200, "HTTP owner login succeeds");
    const auto cookie = std::string(login[http::field::set_cookie]);
    const auto session_cookie = cookie.substr(0, cookie.find(';'));
    require(request(port + 2, http::verb::get, "/api/v1/settings", "",
                    {{"Cookie", session_cookie}}).result_int() == 200,
            "HTTP session cookie authorizes settings read");
    const auto csrf = Json::parse(login.body())["csrf_token"].get<std::string>();
    const auto saved = request(port + 2, http::verb::post, "/api/v1/settings",
        Json{{"revision", 1}, {"settings", {{"hostname", "rapid-network-test"},
             {"rotation", 180}, {"boot_network", "home_then_ap"}}}}.dump(),
        {{"Cookie", session_cookie}, {"Origin", "http://127.0.0.1:" + std::to_string(port + 2)},
         {"X-CSRF-Token", csrf}});
    require(saved.result_int() == 200 && Json::parse(saved.body())["applied"] == false,
            "HTTP owner can save desired settings without applying them");
    const auto tls_certificate = root / "test-certificate.pem";
    const auto tls_private_key = root / "test-private-key.pem";
    test_tls_material(tls_certificate, tls_private_key);
    Process tls_management{fork()};
    if (tls_management.pid == 0) {
      execl(argv[4], argv[4], "--state-directory", (root / "owner-tls").c_str(),
            "--assets", argv[2], "--port", std::to_string(port + 3).c_str(),
            "--enrollment-token-file", token_file.c_str(),
            "--tls-certificate", tls_certificate.c_str(),
            "--tls-private-key", tls_private_key.c_str(), nullptr);
      _exit(127);
    }
    bool tls_ready = false;
    int tls_status = 0;
    std::string tls_cache_control;
    std::string tls_fingerprint;
    std::string tls_error;
    for (int i = 0; i < 100; ++i) {
      try {
        const auto response = tls_request(port + 3, http::verb::get, "/api/v1/setup");
        tls_status = response.result_int();
        tls_cache_control = std::string(response[http::field::cache_control]);
        const auto status = Json::parse(response.body(), nullptr, false);
        tls_fingerprint = status.is_object() ? status.value("certificate_fingerprint", "") : "";
        tls_ready = tls_status == 200 &&
                    tls_cache_control.find("no-store") !=
                        std::string::npos && tls_fingerprint.size() == 64 &&
                    tls_fingerprint.find_first_not_of("0123456789abcdef") == std::string::npos;
        if (tls_ready) break;
      } catch (const std::exception &error) {
        tls_error = error.what();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!tls_ready)
      std::cerr << "HTTPS response status=" << tls_status << " cache="
                << tls_cache_control << " error=" << tls_error << "\n";
    require(tls_ready, "setup transport serves a real HTTPS request");
    std::cout << "Native network: HTTP, UDP, WebSocket history, disconnect "
                 "finalization, HTTPS setup and authenticated recorder-to-archive upload "
                 "passed\nEvidence: "
              << root << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
