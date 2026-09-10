#include "rapid/setup_auth.hpp"
#include <csignal>
#include <iostream>

using namespace rapid::native;
int main(int argc, char **argv) {
  try {
    fs::path directory, assets = "cpp/assets";
    int port = 8002;
    bool enroll = false;
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "rapid-setup-server --state-directory PATH [--assets PATH] [--port PORT] [--set-owner]\n"
                     "Loopback only. --set-owner reads a new owner password from standard input.\n";
        return 0;
      } else if (option == "--set-owner") enroll = true;
      else if (option == "--state-directory" && i + 1 < argc) directory = argv[++i];
      else if (option == "--assets" && i + 1 < argc) assets = argv[++i];
      else if (option == "--port" && i + 1 < argc) {
        const std::string value = argv[++i];
        std::size_t consumed;
        port = std::stoi(value, &consumed);
        if (consumed != value.size() || port < 1024 || port > 65535)
          throw std::invalid_argument("port must be 1024 to 65535");
      } else throw std::invalid_argument("unknown or incomplete argument; use --help");
    }
    SetupStore store(directory);
    SetupAuth auth(store, port);
    if (enroll) {
      std::string password;
      if (!std::getline(std::cin, password) || !auth.enroll(password))
        throw std::runtime_error("owner already configured or password input unavailable");
      std::cout << "Owner configured.\n";
      return 0;
    }
    const auto page = read_file(assets / "setup.html");
    std::signal(SIGINT, [](int) { stopping = true; });
    std::signal(SIGTERM, [](int) { stopping = true; });
    std::signal(SIGPIPE, SIG_IGN);
    serve("127.0.0.1", port, [&](const Request &request) -> Response {
      if (request.method == "GET" && (request.target == "/" || request.target == "/setup"))
        return {200, page, "text/html; charset=utf-8", {
            {"Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"},
            {"X-Content-Type-Options", "nosniff"}, {"Referrer-Policy", "no-referrer"}}};
      return auth.handle(request);
    });
    return 0;
  } catch (const std::exception &error) {
    log(std::string("ERROR setup: ") + error.what());
    return 1;
  }
}
