#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace rapid {

struct TelemetryFrame {
  int rpm{};
  double steering_angle{};
  double g_x{};
  double g_y{};
  double g_z{};
  std::optional<double> throttle;
  std::optional<double> brake;
  std::optional<int> gear;
  std::optional<double> speed_kmh;
  std::optional<int> lap_number;
};

struct LiveSnapshot {
  bool companion_connected{};
  std::string simulator;
  std::string daemon_state;
  std::string source_host;
  TelemetryFrame frame;
  std::uint64_t samples_received{};
  std::uint64_t packets_invalid{};
};

class LiveState {
 public:
  void update(std::string simulator, std::string daemon_state, std::string source_host,
              const TelemetryFrame& frame);
  void mark_invalid();
  [[nodiscard]] LiveSnapshot snapshot() const;

 private:
  mutable std::mutex mutex_;
  LiveSnapshot snapshot_;
};

}  // namespace rapid
