// #82: rapid-setup-server listens on the setup access point's fixed address,
// which exists only while Access Point mode is up. On Home Wi-Fi it used to
// exit with a bind error and let systemd restart it every 3 s forever (9,590
// restarts on the dev Pi, and a journal pinned at its size cap). It must
// instead wait quietly for the address, and still stop cleanly on SIGTERM.
#include "rapid/native.hpp"
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace rapid::native;
namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
struct Server {
  pid_t pid = -1;
  ~Server() {
    if (pid > 0) {
      kill(pid, SIGKILL);
      waitpid(pid, nullptr, 0);
    }
  }
  // True while the process has not exited.
  bool running() {
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid) {
      pid = -1;
      exit_status = status;
      return false;
    }
    return true;
  }
  int exit_status = 0;
};
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 3, "rapid-setup-server binary and assets directory required");
    const auto root = fs::temp_directory_path() / ("rapid-setup-wait-" + unique_id());
    fs::create_directories(root);
    const auto token = root / "enrollment.token";
    {
      const int fd = ::open(token.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
      require(fd >= 0, "create the enrollment token file");
      const std::string text(64, 'a');
      require(::write(fd, (text + "\n").data(), 65) == 65, "write the enrollment token");
      ::close(fd);
    }
    const auto log_path = root / "server.log";
    const int port = 22000 + (getpid() % 10000);

    Server server;
    server.pid = fork();
    if (server.pid == 0) {
      const int fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT, 0600);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      ::close(fd);
      // 192.0.2.0/24 is reserved for documentation (RFC 5737): never assigned
      // to a local interface, so binding it fails with "cannot assign
      // requested address", exactly like the AP address on Home Wi-Fi.
      execl(argv[1], argv[1], "--state-directory", (root / "state").c_str(),
            "--assets", argv[2], "--listen", "192.0.2.1", "--port",
            std::to_string(port).c_str(), "--enrollment-token-file", token.c_str(),
            nullptr);
      _exit(127);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    require(server.running(),
            "#82: the setup server keeps running while its address is not up");
    const auto log_text = read_file(log_path);
    require(log_text.find("waiting for 192.0.2.1") != std::string::npos,
            "#82: it says once that it is waiting for the setup address");
    require(log_text.find("ERROR") == std::string::npos,
            "#82: waiting for the address is not logged as an error");
    std::size_t waiting_lines = 0;
    for (auto at = log_text.find("waiting for"); at != std::string::npos;
         at = log_text.find("waiting for", at + 1))
      ++waiting_lines;
    require(waiting_lines == 1, "#82: the wait is announced once, not on every retry");

    const auto stop_started = std::chrono::steady_clock::now();
    kill(server.pid, SIGTERM);
    bool stopped = false;
    while (std::chrono::steady_clock::now() - stop_started < std::chrono::seconds(5)) {
      if (!server.running()) {
        stopped = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(stopped, "#82: SIGTERM stops the waiting setup server within 5 s");
    require(WIFEXITED(server.exit_status) && WEXITSTATUS(server.exit_status) == 0,
            "#82: it exits cleanly when stopped while waiting");

    std::error_code ignored;
    fs::remove_all(root, ignored);
    std::cout << "Setup server waits for its address without restart-looping, and stops cleanly\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
