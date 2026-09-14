#include "rapid/native.hpp"
#include "rapid/setup.hpp"
#include "rapid/pairing.hpp"
#include <csignal>
#include <iostream>
#include <memory>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <iomanip>
#include <sstream>
#include <thread>

using namespace rapid::native;
namespace {
std::string certificate_fingerprint(const fs::path &path) {
  FILE *file = ::fopen(path.c_str(), "rb");
  if (!file) throw std::runtime_error("cannot open pairing TLS certificate");
  X509 *certificate = PEM_read_X509(file, nullptr, nullptr, nullptr);
  ::fclose(file);
  if (!certificate) throw std::runtime_error("cannot parse pairing TLS certificate");
  unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int length = 0;
  const bool valid = X509_digest(certificate, EVP_sha256(), digest, &length) == 1;
  X509_free(certificate);
  if (!valid || length != 32) throw std::runtime_error("cannot fingerprint pairing TLS certificate");
  std::ostringstream result;
  for (unsigned int i = 0; i < length; ++i)
    result << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(digest[i]);
  return result.str();
}
}
int main(int argc, char **argv) {
  try {
    if (argc > 1 && std::string(argv[1]) == "--help") {
      std::cout << "rapid-pi [--config path]\nConfiguration: TOML plus RAPID_* "
                   "environment overrides.\n";
      return 0;
    }
    fs::path config = env("RAPID_CONFIG", "config.toml");
    if (argc == 3 && std::string(argv[1]) == "--config")
      config = argv[2];
    else if (argc != 1)
      throw std::runtime_error("use --config path or --help");
    auto settings = Config::load(config);
    std::unique_ptr<SetupStore> setup;
    std::unique_ptr<PairingCoordinator> pairing;
    std::unique_ptr<PairingTransport> pairing_transport;
    std::string pairing_certificate_fingerprint;
    if (!settings.setup_directory.empty()) {
      setup = std::make_unique<SetupStore>(settings.setup_directory);
      const auto paired_keys = setup->peer_keys();
      if (!paired_keys.empty() || settings.pairing_enabled) {
        settings.companion_keys = paired_keys;
        settings.paired_key_mode = true;
      }
      const auto certificate = settings.setup_directory / "device.crt";
      const auto private_key = settings.setup_directory / "device.key";
      if (settings.pairing_enabled && fs::is_regular_file(certificate) && fs::is_regular_file(private_key)) {
        const auto device_id = setup->snapshot().at("device_id").get<std::string>();
        pairing_certificate_fingerprint = certificate_fingerprint(certificate);
        pairing = std::make_unique<PairingCoordinator>(
            *setup, device_id, pairing_certificate_fingerprint,
            "/run/rapid/pairing.json", "/run/rapid/pairing-approval.json",
            "/run/rapid/pairing-state.json", "/run/rapid/pairing-control.json");
        pairing_transport = std::make_unique<PairingTransport>(*pairing);
      }
    }
    Runtime runtime(settings);
    auto dashboard = read_file(settings.assets / "dashboard.html"),
         telemetry = read_file(settings.assets / "telemetry.html"),
         steering_wheel = read_file(settings.assets / "steering-wheel-cartoon.png");
    std::signal(SIGINT, [](int) { stopping = true; });
    std::signal(SIGTERM, [](int) { stopping = true; });
    std::signal(SIGPIPE, SIG_IGN);
    std::atomic_bool failed = false;
    std::vector<std::thread> threads;
    auto start = [&](auto task) {
      threads.emplace_back([task, &failed] {
        try {
          task();
        } catch (const std::exception &e) {
          log(std::string("ERROR runtime worker: ") + e.what());
          failed = true;
          stopping = true;
        }
      });
    };
    if (pairing && pairing_transport) {
      start([&] {
        serve_tls(settings.pairing_host, settings.pairing_port,
                  [&](const Request &request) -> Response {
                    const auto path = request.target.substr(0, request.target.find('?'));
                    if (request.method == "GET" && path == "/api/v1/setup") {
                      auto status = setup_status(setup->snapshot());
                      status["certificate_fingerprint"] = pairing_certificate_fingerprint;
                      return {200, status.dump(), "application/json",
                              {{"Cache-Control", "no-store"}}};
                    }
                    return pairing_transport->handle(request);
                  }, settings.setup_directory / "device.crt",
                  settings.setup_directory / "device.key");
      });
      log("INFO pairing: main-program TLS listener active on port " +
          std::to_string(settings.pairing_port));
    }
    start([&] { udp_loop(runtime, settings); });
    if (settings.acc_enabled)
      start([&] { acc_loop(runtime, settings); });
    if (settings.upload_enabled)
      start([&] { upload_loop(runtime, settings); });
    if (settings.paired_key_mode)
      start([&] {
        while (!stopping) {
          // SetupAuth writes the same private SQLite store. Reloading under
          // SetupStore's mutex makes revocation effective without a runtime
          // restart while keeping keys out of HTTP and control files.
          runtime.replace_paired_keys(setup->peer_keys());
          for (int i = 0; i < 4 && !stopping; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
      });
    start([&] {
      while (!stopping) {
        runtime.power();
        for (int i = 0; i < 10 && !stopping; ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
    int result = 0;
    try {
      serve(
          settings.host, settings.port,
          [&](const Request &req) -> Response {
            auto path = req.target.substr(0, req.target.find('?'));
            if (req.method == "GET" && path == "/")
              return {200, dashboard, "text/html; charset=utf-8"};
            if (req.method == "GET" && path == "/telemetry")
              return {200, telemetry, "text/html; charset=utf-8"};
            if (req.method == "GET" && path == "/steering-wheel-cartoon.png")
              return {200, steering_wheel, "image/png"};
            if (req.method == "GET" &&
                (path == "/api/live" || path == "/api/v1/status"))
              return {200, runtime.snapshot().dump()};
            if (req.method == "GET" && path == "/healthz")
              return {200, "{\"status\":\"ok\",\"runtime\":\"cpp\"}"};
            if (path == "/api/v1/setup") {
              if (!setup)
                return {503, "{\"available\":false}"};
              if (req.method != "GET")
                return {405, "{\"detail\":\"setup is read-only\"}", "application/json",
                        {{"Allow", "GET"}}};
              return {200, setup_status(setup->snapshot()).dump(), "application/json",
                      {{"Cache-Control", "no-store"}}};
            }
            if (req.method == "GET" && path == "/api/v1/network/mode") {
              auto state = settings.network_control / "state";
              if (!fs::exists(state))
                return {503, "{\"available\":false}"};
              auto value = Json::parse(read_file(state));
              if (!value.is_object() || !value.contains("mode") ||
                  !value["mode"].is_string())
                return {503, "{\"available\":false}"};
              value["available"] = true;
              return {200, value.dump()};
            }
            if (req.method == "POST" && path == "/api/v1/network/mode") {
              auto body = Json::parse(req.body);
              if (!body.is_object() || !body.contains("mode") ||
                  !body["mode"].is_string())
                return {400, "{\"detail\":\"mode must be off, ap or home\"}"};
              auto mode = body["mode"].get<std::string>();
              if (mode != "off" && mode != "ap" && mode != "home")
                return {400, "{\"detail\":\"mode must be off, ap or home\"}"};
              if (!fs::is_directory(settings.network_control))
                return {503, "{\"available\":false}"};
              atomic_file(settings.network_control / "request",
                          Json{{"mode", mode}}.dump());
              return {202, Json{{"mode", mode}, {"queued", true}}.dump()};
            }
            if (req.method == "PUT" && path == "/api/v1/session/upload") {
              auto body = Json::parse(req.body);
              if (!body.is_object() || !body.contains("enabled") ||
                  !body["enabled"].is_boolean())
                return {400, "{\"detail\":\"enabled must be boolean\"}"};
              runtime.upload(body["enabled"]);
              return {200, Json{{"enabled", body["enabled"]}}.dump()};
            }
            return {404, "{\"detail\":\"not found\"}"};
          },
          &runtime);
    } catch (const std::exception &e) {
      log(std::string("ERROR server: ") + e.what());
      result = 1;
    }
    stopping = true;
    for (auto &thread : threads)
      thread.join();
    runtime.finish();
    return result || failed ? 1 : 0;
  } catch (const std::exception &e) {
    log(std::string("ERROR startup: ") + e.what());
    return 1;
  }
}
