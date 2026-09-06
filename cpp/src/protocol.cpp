#include "rapid/protocol.hpp"

#include <cmath>
#include <cctype>
#include <regex>
#include <string>

namespace rapid {
namespace {
std::optional<std::string> string_value(std::string_view input, const char* name) {
  const std::regex expression(std::string{"\""} + name + R"regex("\s*:\s*"([^"]*)")regex");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_search(input.begin(), input.end(), match, expression)) return std::nullopt;
  return std::string(match[1].first, match[1].second);
}

std::optional<double> number_value(std::string_view input, const char* name) {
  const std::regex expression(std::string{"\""} + name + R"("\s*:\s*(-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?))");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_search(input.begin(), input.end(), match, expression)) return std::nullopt;
  try { return std::stod(std::string(match[1].first, match[1].second)); } catch (...) { return std::nullopt; }
}

std::optional<std::string_view> object_value(std::string_view input, const char* name) {
  const std::regex expression(std::string{"\""} + name + R"("\s*:\s*)");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_search(input.begin(), input.end(), match, expression)) return std::nullopt;
  auto start = static_cast<std::size_t>(match.position() + match.length());
  while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) ++start;
  if (start == input.size() || input[start] != '{') return std::nullopt;
  bool quoted = false;
  bool escaped = false;
  int depth = 0;
  for (std::size_t index = start; index < input.size(); ++index) {
    const char character = input[index];
    if (quoted) {
      if (escaped) escaped = false;
      else if (character == '\\') escaped = true;
      else if (character == '"') quoted = false;
      continue;
    }
    if (character == '"') quoted = true;
    else if (character == '{') ++depth;
    else if (character == '}' && --depth == 0) return input.substr(start, index - start + 1);
  }
  return std::nullopt;
}

bool finite(const std::optional<double>& value) { return value && std::isfinite(*value); }
bool integral(const double value) { return std::trunc(value) == value; }
}  // namespace

std::optional<CompanionPacket> parse_companion_packet(std::string_view json) {
  const auto version = number_value(json, "version");
  const auto type = string_value(json, "type");
  if (!version || *version != 3 || !type) return std::nullopt;
  CompanionPacket packet;
  if (*type == "status") {
    packet.state = string_value(json, "state").value_or("");
    if (packet.state != "waiting" && packet.state != "ready" && packet.state != "driving") return std::nullopt;
    packet.simulator = string_value(json, "simulator").value_or("");
    if (!packet.simulator.empty() && packet.simulator != "ACC" && packet.simulator != "AC" &&
        packet.simulator != "ACE" && packet.simulator != "iRacing") return std::nullopt;
    return packet;
  }
  if (*type != "telemetry") return std::nullopt;
  packet.simulator = string_value(json, "simulator").value_or("");
  if (packet.simulator != "ACC" && packet.simulator != "AC" && packet.simulator != "ACE" && packet.simulator != "iRacing") return std::nullopt;
  const auto telemetry = object_value(json, "telemetry");
  if (!telemetry) return std::nullopt;
  const auto rpm = number_value(*telemetry, "rpm");
  const auto steer = number_value(*telemetry, "steering_angle");
  const auto gx = number_value(*telemetry, "g_x");
  const auto gy = number_value(*telemetry, "g_y");
  const auto gz = number_value(*telemetry, "g_z");
  if (!finite(rpm) || !finite(steer) || !finite(gx) || !finite(gy) || !finite(gz) || *rpm < 0 || *rpm > 20000 || !integral(*rpm)) return std::nullopt;
  TelemetryFrame frame{};
  frame.rpm = static_cast<int>(*rpm);
  frame.steering_angle = *steer;
  frame.g_x = *gx;
  frame.g_y = *gy;
  frame.g_z = *gz;
  frame.throttle = number_value(*telemetry, "throttle"); frame.brake = number_value(*telemetry, "brake");
  if ((frame.throttle && (!std::isfinite(*frame.throttle) || *frame.throttle < 0 || *frame.throttle > 1.001)) || (frame.brake && (!std::isfinite(*frame.brake) || *frame.brake < 0 || *frame.brake > 1.001))) return std::nullopt;
  if (const auto value = number_value(*telemetry, "gear")) { if (!std::isfinite(*value) || !integral(*value)) return std::nullopt; frame.gear = static_cast<int>(*value); }
  if (const auto value = number_value(*telemetry, "speed_kmh")) { if (!std::isfinite(*value)) return std::nullopt; frame.speed_kmh = *value; }
  if (const auto value = number_value(*telemetry, "lap_number")) { if (!std::isfinite(*value) || !integral(*value) || *value < 0) return std::nullopt; frame.lap_number = static_cast<int>(*value); }
  packet.state = "driving"; packet.frame = frame;
  return packet;
}

}  // namespace rapid
