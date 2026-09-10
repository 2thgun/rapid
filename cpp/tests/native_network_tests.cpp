#include "rapid/native.hpp"
#include "rapid/setup_auth.hpp"
#include <argon2.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <csignal>
#include <fcntl.h>
#include <iostream>
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
int main(int argc, char **argv) {
  try {
    require(argc == 5, "Pi binary, assets, archive binary and setup binary required");
    auto root =
        fs::temp_directory_path() / ("rapid-network-tests-" + unique_id());
    fs::create_directories(root);
    int port = 18000 + (getpid() % 10000), udp = port + 10000;
    std::string token = "native-network-test-only",
                salt = "native-network-test-salt";
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
    require(request(port, http::verb::get, "/")
                    .body()
                    .find("id=\"network-mode\"") != std::string::npos,
            "dashboard assets");
    require(request(port, http::verb::get, "/telemetry")
                    .body()
                    .find("new WebSocket") != std::string::npos,
            "engineering assets");
    const auto wheel = request(port, http::verb::get, "/steering-wheel-cartoon.png");
    require(wheel.result_int() == 200 && wheel[http::field::content_type] == "image/png" &&
                wheel.body() == read_file(fs::path(argv[2]) / "steering-wheel-cartoon.png"),
            "steering wheel asset served intact with PNG content type");
    require(request(port, http::verb::put, "/api/v1/session/upload",
                    "{\"enabled\":\"false\"}")
                    .result_int() == 400,
            "upload setting rejects strings");
    asio::io_context io;
    asio::ip::udp::socket sender(io, asio::ip::udp::v4());
    asio::ip::udp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), udp);
    sender.send_to(asio::buffer(std::string("[]")), endpoint);
    for (int i = 0; i < 12; ++i) {
      Json m = {{"version", 3},
                {"simulator", "AC"},
                {"session_id", "network-test"},
                {"sequence", i},
                {"sample_rate_hz", 10},
                {"monotonic_us", i * 100000},
                {"telemetry",
                 {{"rpm", 5000 + i},
                  {"steering_angle", .1},
                  {"g_x", 0},
                  {"g_y", 1},
                  {"g_z", 0},
                  {"throttle", .5},
                  {"lap_number", i < 10 ? 1 : 2},
                  {"completed_lap_ms", i < 10 ? 0 : 1000}}}};
      sender.send_to(asio::buffer(m.dump()), endpoint);
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
    {
      SetupStore owner(root / "owner");
      SetupAuth auth(owner, port + 2);
      require(auth.enroll("network-test-owner-password"), "isolated owner enrolled");
    }
    Process management{fork()};
    if (management.pid == 0) {
      int fd = ::open((root / "setup.log").c_str(), O_WRONLY | O_CREAT, 0600);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      ::close(fd);
      execl(argv[4], argv[4], "--state-directory", (root / "owner").c_str(),
            "--assets", argv[2], "--port", std::to_string(port + 2).c_str(), nullptr);
      _exit(127);
    }
    bool management_ready = false;
    for (int i = 0; i < 100; ++i) {
      try {
        management_ready = request(port + 2, http::verb::get, "/api/v1/setup").result_int() == 200;
        if (management_ready) break;
      } catch (...) {}
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    require(management_ready, "loopback management server ready");
    auto page = request(port + 2, http::verb::get, "/setup");
    require(page.result_int() == 200 && page.body().find("Owner sign-in") != std::string::npos &&
            !page[http::field::content_security_policy].empty(), "setup page served with security policy");
    require(request(port + 2, http::verb::get, "/api/v1/settings").result_int() == 401,
            "HTTP settings require authentication");
    const auto login = request(port + 2, http::verb::post, "/api/v1/auth/login",
        Json{{"password", "network-test-owner-password"}}.dump(),
        {{"Origin", "http://127.0.0.1:" + std::to_string(port + 2)}});
    require(login.result_int() == 200, "HTTP owner login succeeds");
    const auto cookie = std::string(login[http::field::set_cookie]);
    require(request(port + 2, http::verb::get, "/api/v1/settings", "",
                    {{"Cookie", cookie.substr(0, cookie.find(';'))}}).result_int() == 200,
            "HTTP session cookie authorizes settings read");
    std::cout << "Native network: HTTP, UDP, WebSocket history, disconnect "
                 "finalization and authenticated recorder-to-archive upload "
                 "passed\nEvidence: "
              << root << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
