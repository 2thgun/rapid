#include "rapid/log_status.hpp"

#include <systemd/sd-journal.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }

void check(int result, const char* operation) {
  if (result < 0) throw std::runtime_error(std::string(operation) + ": " + std::strerror(-result));
}

std::string journal_field(sd_journal* journal, const char* name) {
  const void* data = nullptr;
  std::size_t size = 0;
  if (sd_journal_get_data(journal, name, &data, &size) < 0) return {};
  const std::size_t prefix = std::strlen(name) + 1;
  if (size < prefix) return {};
  return std::string(static_cast<const char*>(data) + prefix, size - prefix);
}

double monotonic_seconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string utc_timestamp() {
  const auto time = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::array<char, 32> buffer{};
  std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer.data();
}

void serve_client(int client, const rapid::LogStatus& status) {
  const timeval timeout{0, 250000};
  setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  std::string request;
  std::array<char, 1024> buffer{};
  const double deadline = monotonic_seconds() + 0.25;
  while (request.size() < 4096 && request.find("\r\n\r\n") == std::string::npos) {
    const int remaining = static_cast<int>((deadline - monotonic_seconds()) * 1000);
    if (remaining <= 0) return;
    pollfd ready{client, POLLIN, 0};
    if (poll(&ready, 1, remaining) <= 0) return;
    const auto count = recv(client, buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (count <= 0) return;
    request.append(buffer.data(), static_cast<std::size_t>(count));
  }
  const bool found = request.starts_with("GET /api/log-status HTTP/1.") &&
                     request.find("\r\n\r\n") != std::string::npos;
  const std::string body = found ? status.json() : "{\"error\":\"not found\"}";
  const std::string response = std::string("HTTP/1.1 ") + (found ? "200 OK" : "404 Not Found") +
      "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
      "Access-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;
  std::size_t sent = 0;
  while (sent < response.size()) {
    const auto count = send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
    if (count <= 0) return;
    sent += static_cast<std::size_t>(count);
  }
}
}  // namespace

int main(int argc, char** argv) {
  int port = 8001;
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "rapid-log-status [--port 8001]\nGET /api/log-status; no raw journal messages\n";
    return 0;
  }
  if (argc != 1) {
    if (argc != 3 || std::string_view(argv[1]) != "--port") return 2;
    const std::string_view value(argv[2]);
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
    if (error != std::errc{} || end != value.data() + value.size() || port < 1 || port > 65535) return 2;
  }
  try {
    sd_journal* raw_journal = nullptr;
    check(sd_journal_open(&raw_journal, SD_JOURNAL_LOCAL_ONLY), "open journal");
    std::unique_ptr<sd_journal, decltype(&sd_journal_close)> journal(raw_journal, sd_journal_close);
    check(sd_journal_add_match(journal.get(), "_SYSTEMD_UNIT=rapid.service", 0), "match application");
    check(sd_journal_add_match(journal.get(), "_SYSTEMD_UNIT=rapid-display.service", 0), "match display");
    check(sd_journal_add_disjunction(journal.get()), "match lifecycle events");
    check(sd_journal_add_match(journal.get(), "UNIT=rapid.service", 0), "match application lifecycle");
    check(sd_journal_add_match(journal.get(), "UNIT=rapid-display.service", 0), "match display lifecycle");
    check(sd_journal_set_data_threshold(journal.get(), 4096), "bound journal fields");
    check(sd_journal_seek_tail(journal.get()), "seek current journal");
    check(sd_journal_previous(journal.get()), "skip historical events");

    const int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (server < 0) throw std::runtime_error("cannot create HTTP socket");
    const int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(server, 16) < 0) {
      close(server);
      throw std::runtime_error("cannot listen on status port");
    }
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    rapid::LogStatus status;
    std::cout << "Native log status listening on port " << port << std::endl;
    while (!stopping) {
      const int journal_fd = sd_journal_get_fd(journal.get());
      check(journal_fd, "journal descriptor");
      const int events = sd_journal_get_events(journal.get());
      check(events, "journal events");
      std::array<pollfd, 2> fds{{{server, POLLIN, 0}, {journal_fd, static_cast<short>(events), 0}}};
      const int result = poll(fds.data(), fds.size(), 1000);
      if (result < 0 && errno != EINTR) throw std::runtime_error("poll failed");
      if (stopping) break;
      check(sd_journal_process(journal.get()), "process journal");
      int next = 0;
      while ((next = sd_journal_next(journal.get())) > 0) {
        const std::string priority_text = journal_field(journal.get(), "PRIORITY");
        int priority = 6;
        if (!priority_text.empty()) {
          const auto [end, error] = std::from_chars(priority_text.data(), priority_text.data() + priority_text.size(), priority);
          if (error != std::errc{} || end != priority_text.data() + priority_text.size()) continue;
        }
        std::string unit = journal_field(journal.get(), "_SYSTEMD_UNIT");
        if (unit != "rapid.service" && unit != "rapid-display.service") unit = journal_field(journal.get(), "UNIT");
        status.observe(unit, priority,
                       journal_field(journal.get(), "MESSAGE"), monotonic_seconds(), utc_timestamp());
      }
      check(next, "read journal");
      if (fds[0].revents & POLLIN) {
        const int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
        if (client >= 0) { serve_client(client, status); close(client); }
      }
    }
    close(server);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "rapid-log-status: " << error.what() << '\n';
    return 1;
  }
}
