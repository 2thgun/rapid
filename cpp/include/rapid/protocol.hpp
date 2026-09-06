#pragma once

#include "rapid/state.hpp"

#include <optional>
#include <string_view>

namespace rapid {

struct CompanionPacket {
  std::string simulator;
  std::string state;
  std::optional<TelemetryFrame> frame;
};

// Strict parser for the daemon's schema-v3 status and telemetry envelope.
// It rejects unknown schema versions and non-finite/out-of-range primary values.
std::optional<CompanionPacket> parse_companion_packet(std::string_view json);

}  // namespace rapid
