#include "rapid/state.hpp"

namespace rapid {

void LiveState::update(std::string simulator, std::string daemon_state, std::string source_host,
                       const TelemetryFrame& frame) {
  std::scoped_lock lock(mutex_);
  snapshot_.companion_connected = true;
  snapshot_.simulator = std::move(simulator);
  snapshot_.daemon_state = std::move(daemon_state);
  snapshot_.source_host = std::move(source_host);
  snapshot_.frame = frame;
  ++snapshot_.samples_received;
}

void LiveState::mark_invalid() {
  std::scoped_lock lock(mutex_);
  ++snapshot_.packets_invalid;
}

LiveSnapshot LiveState::snapshot() const {
  std::scoped_lock lock(mutex_);
  return snapshot_;
}

}  // namespace rapid
