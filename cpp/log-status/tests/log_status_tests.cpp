#include "rapid/log_status.hpp"
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    rapid::LogStatus status;
    require(!status.observe("ssh.service", 3, "ignore", 0, ""), "other service");
    require(!status.observe("rapid.service", 7, "debug", 0, ""), "debug noise");
    require(!status.observe("rapid.service", 6, "\"GET /api/live HTTP/1.1\" 200 OK", 0, ""), "polling loop");
    require(status.observe("rapid.service", 6, "Recording saved", 0, "2026-09-05T12:00:00Z"), "initial event");
    require(!status.observe("rapid.service", 6, "Connected", 1, ""), "burst coalescing");
    require(status.observe("rapid-display.service", 3, "private error", 1, "2026-09-05T12:00:01Z"), "severity escalation");
    require(status.severity() == "error", "error severity");
    require(status.json().find("private error") == std::string::npos, "raw text leak");
    require(!status.observe("rapid-display.service", 3, "private error", 10, ""), "duplicate suppression");
    require(status.observe("rapid.service", 4, "New warning", 10, ""), "quiet interval");
    require(status.severity() == "warn", "warning severity");
    require(status.sequence() == 3, "sequence increments");
    for (int i = 0; i < 150; ++i) status.observe("rapid.service", 6, std::to_string(i), 40 + i * 5, "");
    require(status.remembered() == 128, "bounded memory");
    rapid::LogStatus streams;
    require(streams.observe("rapid-display.service", 6, "WARNING renderer", 0, ""), "stream warning");
    require(streams.severity() == "warn", "stream severity normalization");
    require(streams.observe("rapid.service", 6, "ERROR recorder", 1, ""), "stream error escalation");
    require(streams.severity() == "error", "stream error severity");
    require(!streams.observe("rapid.service", -1, "invalid", 10, ""), "negative priority");
    require(!streams.observe("rapid.service", 99, "invalid", 10, ""), "unknown priority");
    require(streams.observe("rapid.service", 6, "ERROR recorder", 31, ""), "duplicate window expiry");
    std::cout << "Log status filtering, severity, deduplication and bounded memory passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
