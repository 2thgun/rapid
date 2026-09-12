#include "rapid/setup_auth.hpp"
#include <csignal>
#include <iostream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace rapid::native;
namespace {
std::string enrollment_token(const fs::path &path) {
  if (path.empty()) return {};
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (descriptor < 0) throw std::runtime_error("cannot open enrollment token file");
  struct stat info {};
  const bool safe = ::fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) &&
      info.st_uid == ::geteuid() && (info.st_mode & 0077) == 0 && info.st_nlink == 1;
  char data[66];
  const auto length = safe ? ::read(descriptor, data, sizeof data) : -1;
  ::close(descriptor);
  if (!safe || length < 64 || length > 65 || (length == 65 && data[64] != '\n'))
    throw std::runtime_error("enrollment token must be a private service-owned file containing 64 hexadecimal characters");
  return std::string(data, 64);
}
}
int main(int argc, char **argv) {
  try {
    fs::path directory, assets = "cpp/assets", token_file;
    int port = 8002;
    bool enroll = false;
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "rapid-setup-server --state-directory PATH [--assets PATH] [--port PORT] [--set-owner] [--enrollment-token-file PATH]\n"
                     "Loopback only. --set-owner reads a new owner password from standard input.\n";
        return 0;
      } else if (option == "--set-owner") enroll = true;
      else if (option == "--state-directory" && i + 1 < argc) directory = argv[++i];
      else if (option == "--assets" && i + 1 < argc) assets = argv[++i];
      else if (option == "--enrollment-token-file" && i + 1 < argc) token_file = argv[++i];
      else if (option == "--port" && i + 1 < argc) {
        const std::string value = argv[++i];
        std::size_t consumed;
        port = std::stoi(value, &consumed);
        if (consumed != value.size() || port < 1024 || port > 65535)
          throw std::invalid_argument("port must be 1024 to 65535");
      } else throw std::invalid_argument("unknown or incomplete argument; use --help");
    }
    SetupStore store(directory);
    // Once claimed, the owner database is authoritative. Removing the bootstrap
    // file must not prevent normal sign-in after a service restart.
    SetupAuth auth(store, port, monotonic,
                   store.owner_hash().empty() ? enrollment_token(token_file) : std::string{});
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
