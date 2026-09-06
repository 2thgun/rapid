#include "rapid/protocol.hpp"

#include <cassert>

int main() {
  const auto waiting = rapid::parse_companion_packet(
      R"({"version":3,"type":"status","state":"waiting","simulator":null})");
  assert(waiting && waiting->state == "waiting" && waiting->simulator.empty());

  const auto telemetry = rapid::parse_companion_packet(R"({"version":3,"type":"telemetry","simulator":"ACC","telemetry":{"rpm":6500,"steering_angle":-0.3,"g_x":0.7,"g_y":1.0,"g_z":-0.4,"throttle":0.82,"gear":4,"speed_kmh":198.2,"lap_number":7}})");
  assert(telemetry && telemetry->frame && telemetry->frame->rpm == 6500);
  assert(telemetry->frame->gear && *telemetry->frame->gear == 4);

  assert(!rapid::parse_companion_packet(R"({"version":3,"type":"telemetry","simulator":"ACC","rpm":6500,"steering_angle":0,"g_x":0,"g_y":0,"g_z":0})"));
  assert(!rapid::parse_companion_packet(R"({"version":3,"type":"telemetry","simulator":"ACC","telemetry":{"rpm":6500.5,"steering_angle":0,"g_x":0,"g_y":0,"g_z":0}})"));
}
