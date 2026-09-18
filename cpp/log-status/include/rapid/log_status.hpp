#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace rapid {
// Only fixed metadata is published; journal messages never leave this process.
class LogStatus {
 public:
  bool observe(std::string_view unit, int priority, std::string_view message,
               double now, std::string timestamp) {
    if ((unit != "rapid.service" && unit != "rapid-display.service") ||
        priority < 0 || priority > 6) return false;
    // Stream logs may retain the service's default journal priority.
    if (message.find("ERROR") != std::string_view::npos || message.find("CRITICAL") != std::string_view::npos ||
        message.find("(EE)") != std::string_view::npos) priority = std::min(priority, 3);
    else if (message.find("WARNING") != std::string_view::npos || message.find("(WW)") != std::string_view::npos)
      priority = std::min(priority, 4);
    const auto http = message.find("HTTP/");
    if (priority >= 5 && http != std::string_view::npos &&
        (message.find("\" 2", http) != std::string_view::npos ||
         message.find("\" 3", http) != std::string_view::npos)) return false;
    const std::string signature = std::string(unit) + std::to_string(priority) +
                                  std::string(message.substr(0, 512));
    for (const auto& item : recent_) {
      if (item.first == signature && now - item.second < 30) return false;
    }
    // Three-second display, then a quiet interval; errors may escalate immediately.
    if (now - last_notice_ < 5 && priority >= last_priority_) return false;
    recent_.emplace_back(signature, now);
    if (recent_.size() > 128) recent_.pop_front();
    last_notice_ = now;
    last_priority_ = priority;
    severity_ = priority <= 3 ? "error" : priority == 4 ? "warn" : "info";
    timestamp_ = std::move(timestamp);
    ++sequence_;
    return true;
  }

  std::uint64_t sequence() const { return sequence_; }
  std::string_view severity() const { return severity_; }
  std::size_t remembered() const { return recent_.size(); }
  std::string json() const {
    return "{\"log_sequence\":" + std::to_string(sequence_) +
           ",\"log_updated_at\":" + (timestamp_.empty() ? "null" : "\"" + timestamp_ + "\"") +
           ",\"log_severity\":\"" + severity_ + "\"}";
  }

 private:
  std::uint64_t sequence_ = 0;
  std::string severity_ = "info", timestamp_;
  double last_notice_ = -std::numeric_limits<double>::infinity();
  int last_priority_ = 7;
  std::deque<std::pair<std::string, double>> recent_;
};
}  // namespace rapid
