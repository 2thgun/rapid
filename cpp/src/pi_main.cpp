#include "rapid/native.hpp"
#include <csignal>
#include <iostream>
#include <thread>

using namespace rapid::native;
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
    start([&] { udp_loop(runtime, settings); });
    if (settings.acc_enabled)
      start([&] { acc_loop(runtime, settings); });
    if (settings.upload_enabled)
      start([&] { upload_loop(runtime, settings); });
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
