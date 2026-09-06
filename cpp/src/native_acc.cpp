#include "rapid/native.hpp"
#include <arpa/inet.h>
#include <bit>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rapid::native {
namespace {
struct Reader {
  std::string_view data;
  std::size_t pos = 0;
  unsigned u8() {
    if (pos >= data.size())
      throw std::runtime_error("truncated ACC packet");
    return static_cast<unsigned char>(data[pos++]);
  }
  unsigned u16() {
    auto a = u8();
    return a | (u8() << 8);
  }
  int i16() { return std::int16_t(u16()); }
  int i32() {
    auto a = u16();
    return std::int32_t(a | (u16() << 16));
  }
  float f32() { return std::bit_cast<float>(i32()); }
  std::string text() {
    std::uint32_t size = 0;
    unsigned shift = 0;
    for (;;) {
      auto b = u8();
      size |= (b & 127) << shift;
      if (!(b & 128))
        break;
      shift += 7;
      if (shift > 28)
        throw std::runtime_error("ACC string length");
    }
    if (size > data.size() - pos)
      throw std::runtime_error("ACC string truncated");
    auto result = std::string(data.substr(pos, size));
    pos += size;
    return result;
  }
};
Json lap(Reader &r) {
  Json result;
  result["lap_time_ms"] = r.i32();
  result["car_index"] = r.u16();
  result["driver_index"] = r.u16();
  auto n = r.u8();
  if (n > 8)
    throw std::runtime_error("ACC split count");
  result["splits"] = Json::array();
  for (unsigned i = 0; i < n; ++i)
    result["splits"].push_back(r.i32());
  result["valid"] = !r.u8();
  result["valid_for_best"] = bool(r.u8());
  result["is_outlap"] = bool(r.u8());
  result["is_inlap"] = bool(r.u8());
  return result;
}
Json decode(int type, Reader &r) {
  Json p;
  if (type == 1) {
    p["connection_id"] = r.i32();
    p["success"] = bool(r.u8());
  } else if (type == 2) {
    p["event_index"] = r.i16();
    p["session_index"] = r.i16();
    p["session_type"] = r.u8();
    p["phase"] = r.u8();
    p["session_time_ms"] = r.i32();
    p["remaining_time_ms"] = r.i32();
    p["focused_car_index"] = r.i32();
  } else if (type == 3) {
    p["car_index"] = r.u16();
    p["driver_index"] = r.u16();
    p["driver_count"] = r.u8();
    p["gear_raw"] = r.u8();
    p["world_pos_x"] = r.f32();
    p["world_pos_y"] = r.f32();
    p["yaw"] = r.f32();
    p["car_location"] = r.u8();
    p["speed_kmh"] = r.u16();
    p["position"] = r.u16();
    p["cup_position"] = r.u16();
    p["track_position"] = r.u16();
    p["track_relative_position"] = r.f32();
    p["laps"] = r.u16();
    p["delta_ms"] = r.i32();
    p["best_session_lap"] = lap(r);
    p["last_lap"] = lap(r);
    p["current_lap"] = lap(r);
  } else if (type == 4) {
    p["connection_id"] = r.i32();
    auto n = r.u16();
    p["car_indices"] = Json::array();
    for (unsigned i = 0; i < n; ++i)
      p["car_indices"].push_back(r.u16());
  } else if (type == 5) {
    p["connection_id"] = r.i32();
    p["track_name"] = r.text();
    p["track_id"] = r.i32();
    p["track_meters"] = r.i32();
  } else if (type == 6) {
    p["connection_id"] = r.i32();
    p["car_index"] = r.u16();
    p["car_model"] = r.u8();
    p["team_name"] = r.text();
    p["race_number"] = r.i32();
    p["cup_category"] = r.u8();
    p["current_driver_index"] = r.u8();
    p["nationality"] = r.u16();
    p["drivers"] = Json::array();
    if (r.pos < r.data.size()) {
      auto n = r.u8();
      for (unsigned i = 0; i < n; ++i) {
        Json d;
        d["first_name"] = r.text();
        d["last_name"] = r.text();
        d["short_name"] = r.text();
        d["category"] = r.u8();
        d["nationality"] = r.u16();
        p["drivers"].push_back(d);
      }
    }
  } else
    throw std::runtime_error("unknown ACC packet");
  return p;
}
} // namespace
void acc_loop(Runtime &runtime, const Config &config) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    throw std::runtime_error("ACC socket failed");
  sockaddr_in local{}, remote{};
  local.sin_family = remote.sin_family = AF_INET;
  local.sin_port = htons(config.acc_local_port);
  remote.sin_port = htons(config.acc_port);
  if (inet_pton(AF_INET, config.acc_host.c_str(), &remote.sin_addr) != 1 ||
      bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof local)) {
    ::close(fd);
    throw std::runtime_error("ACC endpoint invalid or unavailable");
  }
  std::string registration;
  auto little = [&](std::uint32_t n, int size) {
    for (int i = 0; i < size; ++i)
      registration.push_back(char((n >> (i * 8)) & 255));
  };
  auto text = [&](const std::string &s) {
    if (s.size() > 65535)
      throw std::runtime_error("ACC registration too long");
    little(s.size(), 2);
    registration += s;
  };
  little(1, 1);
  little(config.protocol_version, 1);
  text(config.display_name);
  text(config.acc_password);
  little(config.interval_ms, 4);
  little(0, 2);
  double last = 0;
  while (!stopping) {
    if (monotonic() - last > 1) {
      sendto(fd, registration.data(), registration.size(), 0,
             reinterpret_cast<sockaddr *>(&remote), sizeof remote);
      last = monotonic();
    }
    pollfd item{fd, POLLIN, 0};
    if (poll(&item, 1, 100) > 0) {
      char data[8192];
      sockaddr_in source{};
      socklen_t len = sizeof source;
      auto size = recvfrom(fd, data, sizeof data, 0,
                           reinterpret_cast<sockaddr *>(&source), &len);
      if (size <= 0 || source.sin_addr.s_addr != remote.sin_addr.s_addr)
        continue;
      try {
        Reader r{std::string_view(data, size)};
        int type = r.u8();
        runtime.acc(type, decode(type, r));
        last = monotonic();
      } catch (const std::exception &) { /* Reject malformed packets without
                                            terminating reception. */
      }
    }
  }
  ::close(fd);
}
} // namespace rapid::native
