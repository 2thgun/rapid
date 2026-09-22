// Test-only builder for authenticated v4 datagrams, byte-for-byte as parsed
// by native_v4.cpp's decode_v4. The legacy JSON v3 transport has been retired,
// so runtime tests that previously injected v3 JSON now speak real v4 instead
// of depending on the removed path.
#pragma once
#include "rapid/native.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <openssl/hmac.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rapid::test {

// Zero-based telemetry mask bits (wire order), for readability in the tests.
enum V4Channel {
  ch_elapsed = 0,
  ch_throttle = 1,
  ch_brake = 2,
  ch_fuel = 3,
  ch_gear = 4,
  ch_rpm = 5,
  ch_steering = 6,
  ch_speed = 7,
  ch_g_x = 11,
  ch_g_y = 12,
  ch_g_z = 13,
  ch_lap_number = 45,
  ch_current_lap_ms = 46,
  ch_lap_position = 47
};

inline std::uint64_t v4_mask(std::initializer_list<int> bits) {
  std::uint64_t mask = 0;
  for (int bit : bits) mask |= (std::uint64_t{1} << bit);
  return mask;
}

// The five channels decode_v4 requires to be present and finite.
inline std::uint64_t v4_primary_mask() {
  return v4_mask({ch_rpm, ch_steering, ch_g_x, ch_g_y, ch_g_z});
}

inline void v4_put(std::string &b, std::size_t at, std::uint64_t v,
                   std::size_t size) {
  for (std::size_t i = 0; i < size; ++i)
    b[at + i] = char((v >> (8 * i)) & 255);
}

struct V4Stream {
  std::string key = std::string(32, '\x11');
  std::string run;
  int rate = 50;
  int simulator = 2; // 1 ACC, 2 AC, 3 ACE, 4 iRacing
  std::uint64_t sequence = 0;

  explicit V4Stream(std::string run_id) : run(std::move(run_id)) {
    if (run.size() != 16)
      throw std::runtime_error("v4 test run id must be 16 bytes");
  }

  std::string header(int type, int flags, std::size_t end,
                     std::uint64_t time_us) {
    std::string b(end + 32, '\0');
    b.replace(0, 4, "RPD4");
    v4_put(b, 4, 4, 1); // version
    v4_put(b, 5, std::uint64_t(type), 1);
    v4_put(b, 6, std::uint64_t(simulator), 1);
    v4_put(b, 7, std::uint64_t(flags), 1);
    v4_put(b, 8, 52, 2);
    v4_put(b, 10, end - 52, 2);
    v4_put(b, 12, 2, 2); // schema 2: lap-validity/delta-presence revision
    v4_put(b, 14, 48, 2);
    v4_put(b, 16, std::uint64_t(rate), 2);
    v4_put(b, 18, 0, 2);
    b.replace(20, 16, run);
    v4_put(b, 36, ++sequence, 8);
    v4_put(b, 44, time_us, 8);
    return b;
  }

  std::string sign(std::string b) const {
    unsigned char mac[32];
    unsigned int size = 0;
    HMAC(EVP_sha256(), key.data(), int(key.size()),
         reinterpret_cast<const unsigned char *>(b.data()), b.size() - 32, mac,
         &size);
    b.replace(b.size() - 32, 32, reinterpret_cast<char *>(mac), 32);
    return b;
  }

  // Five length-prefixed strings in wire order; an empty lock means "unknown".
  std::string metadata(std::uint64_t time_us,
                       const std::string &track = "Test Track",
                       const std::string &car = "Test Car",
                       const std::string &driver = "Test Driver",
                       const std::string &session = "Practice",
                       const std::string &lock = "900") {
    std::string text;
    for (const std::string *s : {&track, &car, &driver, &session, &lock}) {
      text += char(s->size() & 255);
      text += char(0);
      text += *s;
    }
    auto b = header(2, 1, 52 + text.size(), time_us);
    b.replace(52, text.size(), text);
    return sign(b);
  }

  // lap_valid: -1 absent (the simulator exposed no lap-valid signal), 0 the
  // completed lap was invalid, 1 it was valid. delta_present distinguishes a
  // real delta (including 0) from "the simulator provided none".
  std::string telemetry(std::uint64_t time_us, std::uint64_t mask,
                        const std::vector<std::pair<int, float>> &channels,
                        std::int32_t completed_lap_ms = 0,
                        std::int32_t delta_ms = 0,
                        int lap_valid = -1,
                        bool delta_present = false) {
    std::uint64_t flags = 1;
    if (lap_valid >= 0) flags |= 0x04 | (lap_valid ? 0x08 : 0);
    if (delta_present) flags |= 0x10;
    auto b = header(1, int(flags), 260, time_us);
    v4_put(b, 52, mask, 8);
    v4_put(b, 60, std::uint32_t(completed_lap_ms), 4);
    v4_put(b, 64, std::uint32_t(delta_ms), 4);
    for (const auto &[index, value] : channels)
      v4_put(b, 68 + 4 * std::size_t(index),
             std::bit_cast<std::uint32_t>(value), 4);
    return sign(b);
  }

  // state: 0 waiting, 1 ready, 2 driving, 3 ended, 4 paused.
  std::string status(int state, std::uint64_t time_us,
                     const std::string &text = "") {
    const int flags = state < 2 ? 0 : state == 2 ? 1 : state == 4 ? 1 : 3;
    const auto length = std::min<std::size_t>(text.size(), 512);
    std::string body;
    body += char(state & 255);
    body += char(0);
    body += char(length & 255);
    body += char((length >> 8) & 255);
    const std::uint64_t sent = 0;
    for (int i = 0; i < 8; ++i) body += char((sent >> (8 * i)) & 255);
    body += text.substr(0, length);
    auto b = header(3, flags, 52 + body.size(), time_us);
    b.replace(52, body.size(), body);
    return sign(b);
  }
};

// A fresh random 16-byte run identifier, distinct from the text session IDs
// the retired transport used.
inline std::string v4_run_id() {
  const auto hex = native::unique_id();
  std::string raw;
  for (std::size_t i = 0; i < 32; i += 2)
    raw += char(std::stoul(hex.substr(i, 2), nullptr, 16));
  return raw;
}

} // namespace rapid::test
