#include "rapid/native.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <openssl/crypto.h>
#include <openssl/hmac.h>

namespace rapid::native {
namespace {
constexpr std::array<const char *, 48> channels = {"elapsed",
                                                   "throttle",
                                                   "brake",
                                                   "fuel",
                                                   "gear",
                                                   "rpm",
                                                   "steering_angle",
                                                   "speed_kmh",
                                                   "velocity_x",
                                                   "velocity_y",
                                                   "velocity_z",
                                                   "g_x",
                                                   "g_y",
                                                   "g_z",
                                                   "wheel_slip_fl",
                                                   "wheel_slip_fr",
                                                   "wheel_slip_rl",
                                                   "wheel_slip_rr",
                                                   "pressure_fl",
                                                   "pressure_fr",
                                                   "pressure_rl",
                                                   "pressure_rr",
                                                   "wheel_speed_fl",
                                                   "wheel_speed_fr",
                                                   "wheel_speed_rl",
                                                   "wheel_speed_rr",
                                                   "core_temp_fl",
                                                   "core_temp_fr",
                                                   "core_temp_rl",
                                                   "core_temp_rr",
                                                   "suspension_fl",
                                                   "suspension_fr",
                                                   "suspension_rl",
                                                   "suspension_rr",
                                                   "tc",
                                                   "heading",
                                                   "pitch",
                                                   "roll",
                                                   "damage_front",
                                                   "damage_rear",
                                                   "damage_left",
                                                   "damage_right",
                                                   "damage_center",
                                                   "pit_limiter",
                                                   "abs_activity",
                                                   "lap_number",
                                                   "current_lap_ms",
                                                   "lap_position"};
void check(bool ok, const char *reason) {
  if (!ok)
    throw std::runtime_error(reason);
}
std::uint64_t le(const std::string &bytes, std::size_t at, std::size_t size) {
  check(at <= bytes.size() && size <= bytes.size() - at, "truncated v4 packet");
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < size; ++i)
    value |= std::uint64_t(static_cast<unsigned char>(bytes[at + i]))
             << (8 * i);
  return value;
}
std::string wire_text(const std::string &bytes, std::size_t &at,
                      std::size_t end) {
  check(at + 2 <= end, "truncated v4 text");
  auto size = le(bytes, at, 2);
  at += 2;
  check(size <= 512 && at + size <= end, "invalid v4 text length");
  auto value = bytes.substr(at, size);
  at += size;
  check(value.find('\0') == std::string::npos, "NUL in v4 text");
  // JSON serialization verifies UTF-8 before any persistent state is changed.
  (void)Json(value).dump();
  return value;
}
} // namespace

std::string telemetry_key(std::string hex) {
  if (hex.empty())
    return {};
  check(hex.size() == 64, "companion key must contain 64 hex characters");
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    throw std::runtime_error("companion key must be hexadecimal");
  };
  std::string key(32, '\0');
  for (std::size_t i = 0; i < 32; ++i)
    key[i] = char(digit(hex[i * 2]) * 16 + digit(hex[i * 2 + 1]));
  return key;
}

Json receive_v4(Database &store, const std::string &bytes,
                const std::string &key) {
  if (key.size() != 32 || bytes.size() < 84 || bytes.size() > 4096)
    throw AuthenticationError(
        "v4 authentication unavailable or invalid packet length");
  const auto end = bytes.size() - 32;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (!HMAC(EVP_sha256(), key.data(), int(key.size()),
            reinterpret_cast<const unsigned char *>(bytes.data()), end, digest,
            &length) ||
      length != 32 || CRYPTO_memcmp(digest, bytes.data() + end, 32) != 0)
    throw AuthenticationError("v4 authentication failed");
  check(bytes.substr(0, 4) == "RPD4" && le(bytes, 4, 1) == 4,
        "invalid v4 magic/version");
  auto type = le(bytes, 5, 1), sim = le(bytes, 6, 1), flags = le(bytes, 7, 1);
  check(type >= 1 && type <= 3 && sim >= 1 && sim <= 4,
        "invalid v4 packet type/simulator");
  check(le(bytes, 8, 2) == 52 && le(bytes, 10, 2) == end - 52 &&
            le(bytes, 12, 2) == 1 && le(bytes, 14, 2) == 48 &&
            le(bytes, 18, 2) == 0,
        "invalid v4 header/schema");
  auto rate = le(bytes, 16, 2), sequence = le(bytes, 36, 8),
       time = le(bytes, 44, 8);
  check(rate >= 1 && rate <= 100 && sequence <= 9007199254740991ULL &&
            time <= 9007199254740991ULL,
        "invalid v4 rate/sequence/time");
  check(flags <= 3, "invalid v4 flags");
  const auto raw_id = bytes.substr(20, 16);
  check(raw_id != std::string(16, '\0'), "zero v4 run id");
  std::string id;
  const char *hex = "0123456789abcdef";
  for (unsigned char c : raw_id) {
    id += hex[c >> 4];
    id += hex[c & 15];
  }
  const char *simulators[] = {"", "ACC", "AC", "ACE", "iRacing"};
  Json message = {{"version", 3},
                  {"_wire_version", 4},
                  {"simulator", simulators[sim]},
                  {"session_id", id},
                  {"sequence", sequence},
                  {"monotonic_us", time},
                  {"sample_rate_hz", rate}};
  Json metadata = Json::object();
  bool closed = false;
  if (type == 1) {
    check(flags == 1 && end == 260, "invalid v4 telemetry size/flags");
    const auto mask = le(bytes, 52, 8);
    constexpr std::uint64_t primary =
        (1ULL << 5) | (1ULL << 6) | (1ULL << 11) | (1ULL << 12) | (1ULL << 13);
    check((mask >> 48) == 0 && (mask & primary) == primary,
          "invalid v4 validity mask");
    Json frame = Json::object();
    for (std::size_t i = 0; i < channels.size(); ++i) {
      if (!(mask & (1ULL << i))) {
        frame[channels[i]] = nullptr;
        continue;
      }
      const double value =
          std::bit_cast<float>(std::uint32_t(le(bytes, 68 + 4 * i, 4)));
      check(std::isfinite(value) && std::abs(value) <= 1e12,
            "invalid v4 channel");
      if (i == 1 || i == 2)
        check(value >= 0 && value <= 1.001, "invalid v4 pedal");
      if (i == 5)
        check(value >= 0 && value <= 20000, "invalid v4 RPM");
      if (i == 4 || i == 45 || i == 46) {
        check(std::abs(value) <= 2147483647 && value == std::trunc(value),
              "invalid v4 integer channel");
        frame[channels[i]] = std::int64_t(value);
      } else
        frame[channels[i]] = value;
    }
    frame["completed_lap_ms"] =
        std::bit_cast<std::int32_t>(std::uint32_t(le(bytes, 60, 4)));
    frame["delta_ms"] =
        std::bit_cast<std::int32_t>(std::uint32_t(le(bytes, 64, 4)));
    check(frame["completed_lap_ms"].get<std::int64_t>() >= -2147483647 &&
              frame["delta_ms"].get<std::int64_t>() >= -2147483647,
          "invalid v4 lap timing");
    message["type"] = "telemetry";
    message["telemetry"] = std::move(frame);
  } else if (type == 2) {
    check(flags == 1, "metadata requires active run");
    std::size_t at = 52;
    for (const char *name :
         {"track_name", "car_model", "driver_name", "session_name"})
      metadata[name] = wire_text(bytes, at, end);
    check(at == end, "trailing v4 metadata");
    message["type"] = "status";
    message["state"] = "driving";
  } else {
    check(end >= 64 && le(bytes, 53, 1) == 0 && le(bytes, 54, 2) == end - 64 &&
              end - 64 <= 512,
          "invalid v4 status size");
    const auto state = le(bytes, 52, 1);
    check(state <= 3 &&
              ((state < 2 && flags == 0) || (state == 2 && flags == 1) ||
               (state == 3 && flags == 3)),
          "invalid v4 status flags");
    (void)Json(bytes.substr(64, end - 64)).dump();
    message["type"] = "status";
    message["state"] = state == 2   ? "driving"
                       : state == 0 ? "waiting"
                                    : "ready";
    closed = state == 3;
  }
  // Watermarks survive receiver restarts; retired run IDs cannot reclaim a
  // session.
  auto rows =
      store.query("SELECT sequence,time,simulator,metadata,closed,active FROM "
                  "v4_runs WHERE id=?",
                  {id});
  std::uint64_t gap = 0;
  if (!rows.empty()) {
    const auto &previous = rows[0];
    if (previous["closed"] == 1 ||
        sequence <= previous["sequence"].get<std::uint64_t>())
      throw std::range_error("replayed v4 packet");
    check(time >= previous["time"].get<std::uint64_t>() &&
              previous["simulator"] == sim &&
              previous["active"] == int((flags & 1) != 0),
          "v4 run identity/time changed");
    gap = sequence - previous["sequence"].get<std::uint64_t>() - 1;
    if (type != 2)
      metadata = Json::parse(previous["metadata"].get<std::string>());
  } else {
    check(type == 2 || (type == 3 && flags == 0),
          "v4 metadata required before active data");
  }
  if (type == 1 || (type == 3 && flags & 1))
    check(!metadata.empty(), "missing v4 run metadata");
  message.update(metadata);
  message["_wire_gap"] = std::min<std::uint64_t>(gap, 1000000);
  // The sequence includes control packets, so gaps are not missing sample
  // counts.
  message["_packet_gap"] = 0;
  store.exec("BEGIN IMMEDIATE");
  try {
    if (rows.empty())
      store.exec("UPDATE v4_runs SET closed=1 WHERE closed=0");
    store.exec("INSERT INTO v4_runs VALUES(?,?,?,?,?,?,?) ON CONFLICT(id) DO "
               "UPDATE SET "
               "sequence=excluded.sequence,time=excluded.time,metadata="
               "excluded.metadata,closed=excluded.closed",
               {id, sequence, time, sim, metadata.dump(), int(closed),
                int((flags & 1) != 0)});
    store.exec("COMMIT");
  } catch (...) {
    store.exec("ROLLBACK");
    throw;
  }
  return message;
}
} // namespace rapid::native
