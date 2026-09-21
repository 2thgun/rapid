// Reads back the .ld/.ldx files that Recorder::finish() publishes, using a
// binary parser written from the MoTeC i2 LD layout (community `ldparser`
// (gotzl) channel-header field order: prev u32, next u32, data_ptr u32, n
// u32, counter u16, dtype_type u16, dtype u16, rate u16, shift i16, mul i16,
// scale i16, dec i16, name 32s, short 8s, unit 12s, 40 pad) rather than by
// calling into write_ld/write_ldx. This is the LD read-back test required by
// #19: it checks header pointers, the event block, every channel header and
// sample values against the input spool, for a full session and a lap range,
// plus .ldx beacons against the recorded laps.
#include "rapid/native.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

using namespace rapid::native;

namespace {

void require(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}

// Independent little-endian readers; not shared with the writer.
std::uint32_t u32(const std::string &d, std::size_t off) {
  std::uint32_t n = 0;
  for (int i = 0; i < 4; ++i)
    n |= std::uint32_t(static_cast<unsigned char>(d.at(off + i))) << (i * 8);
  return n;
}
std::uint16_t u16(const std::string &d, std::size_t off) {
  std::uint16_t n = 0;
  for (int i = 0; i < 2; ++i)
    n |= std::uint16_t(static_cast<unsigned char>(d.at(off + i))) << (i * 8);
  return n;
}
std::int16_t i16(const std::string &d, std::size_t off) {
  return static_cast<std::int16_t>(u16(d, off));
}
float f32(const std::string &d, std::size_t off) {
  return std::bit_cast<float>(u32(d, off));
}
std::string fixed_str(const std::string &d, std::size_t off,
                       std::size_t len) {
  auto s = d.substr(off, len);
  auto pos = s.find('\0');
  if (pos != std::string::npos)
    s.resize(pos);
  return s;
}

// channels.inc is the runtime table write_ld reads; re-including it here gives
// the scale used to compute expected sample values. The intended channel
// metadata (name/short name/unit/dec) is pinned separately in pinned_channels
// below, so drifting the canonical table fails the gate instead of updating
// code and expectation together.
struct ExpectedChannel {
  const char *name, *short_name, *unit, *key;
  double scale;
  int dec;
};
const ExpectedChannel expected_channels[] = {
#include "rapid/channels.inc"
};
constexpr std::size_t expected_channel_count = std::size(expected_channels);

// The intended on-disk channel metadata, pinned here independently of
// channels.inc so that drifting the canonical table away from the agreed
// layout fails the gate rather than silently updating both sides. Names are
// the real MoTeC ADL identifiers the owner's ACC-derived workspace binds
// (THROTTLE, BRAKE, GEAR, RPMS, SPEED, G_LAT, G_LON, SUS_TRAVEL_*,
// TYRE_PRESS_*); the remaining channels keep canonical raPId/MoTeC names.
// dec is pinned 0 for every channel because the known-good ACC/MoTeC ADL
// export writes dec=0 on all of its float32 channels, while the community
// ldparser applies (raw/scale * 10^-dec + shift) * mul to every dtype; a
// nonzero dec therefore risks i2 scaling the stored value. Rationale and
// per-sim sources: cpp/runtime/include/rapid/channels.md (#29).
struct PinnedChannel {
  const char *name, *short_name, *unit;
  int dec;
};
const PinnedChannel pinned_channels[] = {
    {"Time", "Time", "s", 0},
    {"THROTTLE", "Throttle", "%", 0},
    {"BRAKE", "Brake", "%", 0},
    {"Fuel Level", "Fuel", "l", 0},
    {"GEAR", "Gear", "", 0},
    {"RPMS", "RPM", "1/min", 0},
    {"Steered Angle", "Steer", "", 0},
    {"SPEED", "Speed", "km/h", 0},
    {"Velocity Lat", "Vel Lat", "m/s", 0},
    {"Velocity Vert", "Vel Vert", "m/s", 0},
    {"Velocity Long", "Vel Long", "m/s", 0},
    {"G_LAT", "G Lat", "g", 0},
    {"G Force Vert", "G Vert", "g", 0},
    {"G_LON", "G Long", "g", 0},
    {"Wheel Slip FL", "Slip FL", "", 0},
    {"Wheel Slip FR", "Slip FR", "", 0},
    {"Wheel Slip RL", "Slip RL", "", 0},
    {"Wheel Slip RR", "Slip RR", "", 0},
    {"TYRE_PRESS_LF", "Press FL", "psi", 0},
    {"TYRE_PRESS_RF", "Press FR", "psi", 0},
    {"TYRE_PRESS_LR", "Press RL", "psi", 0},
    {"TYRE_PRESS_RR", "Press RR", "psi", 0},
    {"Wheel Speed FL", "WhlSp FL", "rad/s", 0},
    {"Wheel Speed FR", "WhlSp FR", "rad/s", 0},
    {"Wheel Speed RL", "WhlSp RL", "rad/s", 0},
    {"Wheel Speed RR", "WhlSp RR", "rad/s", 0},
    {"Tyre Temp FL", "Temp FL", "C", 0},
    {"Tyre Temp FR", "Temp FR", "C", 0},
    {"Tyre Temp RL", "Temp RL", "C", 0},
    {"Tyre Temp RR", "Temp RR", "C", 0},
    {"SUS_TRAVEL_LF", "Susp FL", "m", 0},
    {"SUS_TRAVEL_RF", "Susp FR", "m", 0},
    {"SUS_TRAVEL_LR", "Susp RL", "m", 0},
    {"SUS_TRAVEL_RR", "Susp RR", "m", 0},
    {"TC", "TC", "", 0},
    {"Heading", "Heading", "rad", 0},
    {"Pitch", "Pitch", "rad", 0},
    {"Roll", "Roll", "rad", 0},
    {"Damage Front", "Dmg F", "", 0},
    {"Damage Rear", "Dmg R", "", 0},
    {"Damage Left", "Dmg L", "", 0},
    {"Damage Right", "Dmg Rgt", "", 0},
    {"Damage Center", "Dmg C", "", 0},
    {"Pit Limiter", "Pit Lim", "", 0},
    {"ABS", "ABS", "", 0},
    {"Lap Number", "Lap", "", 0},
    {"Lap Time", "Lap Time", "s", 0},
    {"Lap Position", "Lap Pos", "%", 0},
};
static_assert(std::size(pinned_channels) == expected_channel_count,
              "pinned channel table must cover every channel.inc row");

constexpr int kRate = 50;
constexpr std::size_t kLap1Samples = 60, kLap2Samples = 60, kTailSamples = 30;
constexpr std::size_t kTotalSamples = kLap1Samples + kLap2Samples + kTailSamples;
constexpr int kLap1Ms = 1200, kLap2Ms = 1000; // lap 2 is the fastest

// The same deterministic per-channel/per-sample raw telemetry value used to
// build the recording and to check it back. channel_index is the 0-based
// position in channels.inc (1..44 use the generic formula; 45/46/47 are the
// lap-number/lap-time/lap-position channels driving the lap boundaries).
double lap_number_at(std::size_t sample) {
  if (sample < kLap1Samples)
    return 1;
  if (sample < kLap1Samples + kLap2Samples)
    return 2;
  return 3;
}
std::size_t lap_start_at(std::size_t sample) {
  if (sample < kLap1Samples)
    return 0;
  if (sample < kLap1Samples + kLap2Samples)
    return kLap1Samples;
  return kLap1Samples + kLap2Samples;
}
std::size_t lap_length_at(std::size_t sample) {
  if (sample < kLap1Samples)
    return kLap1Samples;
  if (sample < kLap1Samples + kLap2Samples)
    return kLap2Samples;
  return kTailSamples;
}
double expected_raw(std::size_t channel_index, std::size_t sample) {
  if (channel_index == 45) // Lap Number
    return lap_number_at(sample);
  if (channel_index == 46) { // Lap Time (current_lap_ms, ms within lap)
    auto within = sample - lap_start_at(sample);
    return double(within) * (1000.0 / kRate);
  }
  if (channel_index == 47) { // Lap Position (0..1 fraction within lap)
    auto within = sample - lap_start_at(sample);
    return double(within) / double(lap_length_at(sample));
  }
  return double(channel_index) + 0.001 * double(sample);
}
double expected_stored(std::size_t channel_index, std::size_t global_sample,
                        std::size_t local_index) {
  if (channel_index == 0) // Time: n/rate within *this file*, always
    return double(local_index) / kRate;
  return expected_raw(channel_index, global_sample) *
         expected_channels[channel_index].scale;
}

Json build_frame(std::size_t sample) {
  Json telemetry = Json::object();
  for (std::size_t i = 1; i < expected_channel_count; ++i) {
    if (i == 45 || i == 46 || i == 47)
      continue;
    telemetry[expected_channels[i].key] = expected_raw(i, sample);
  }
  telemetry["lap_number"] = lap_number_at(sample);
  telemetry["current_lap_ms"] = expected_raw(46, sample);
  telemetry["lap_position"] = expected_raw(47, sample);
  if (sample == kLap1Samples)
    telemetry["completed_lap_ms"] = kLap1Ms;
  else if (sample == kLap1Samples + kLap2Samples)
    telemetry["completed_lap_ms"] = kLap2Ms;
  Json message = {{"telemetry", telemetry},
                  {"sample_rate_hz", kRate},
                  {"simulator", "ACC"},
                  {"driver_name", "Test Driver"},
                  {"car_model", "GT3 EVO"},
                  {"track_name", "Spa"},
                  {"session_name", "Race"},
                  {"session_id", "motec-recorder-test"},
                  {"started_at", "2026-09-16T12:34:56"}};
  return message;
}

// Parses one .ld file's header/event block/channel headers/sample data with
// a fresh implementation of the layout, and checks it against the expected
// sample count/rate and the input spool (via expected_stored).
void check_ld(const std::string &data, std::size_t sample_count,
              std::size_t global_start, const std::string &label) {
  require(u32(data, 0) == 0x40, label + ": header magic");
  auto metadata_pointer = u32(data, 8);
  auto data_pointer = u32(data, 12);
  auto event_pointer = u32(data, 36);
  require(event_pointer == 1762, label + ": header size");
  require(metadata_pointer - event_pointer == 1154, label + ": event size");
  require(u16(data, 64) == 1, label + ": constant 1");
  require(u16(data, 66) == 0x4240, label + ": format marker 0x4240");
  require(u16(data, 68) == 0xf, label + ": constant 0xf");
  require(u32(data, 70) == 0x1f44, label + ": constant 0x1f44");
  require(fixed_str(data, 74, 8) == "ADL", label + ": ADL marker");
  require(u16(data, 82) == 420, label + ": constant 420");
  require(u16(data, 84) == 0xadb0, label + ": constant 0xadb0");
  require(u32(data, 86) == expected_channel_count, label + ": channel count");
  require(fixed_str(data, 94, 16) == "16/09/2026", label + ": date");
  require(fixed_str(data, 126, 16) == "12:34:56", label + ": clock");
  require(fixed_str(data, 158, 64) == "Test Driver", label + ": driver");
  require(fixed_str(data, 222, 64) == "GT3 EVO", label + ": vehicle");
  require(fixed_str(data, 350, 64) == "Spa", label + ": venue");
  require(u32(data, 1502) == 0xc81a4, label + ": constant 0xc81a4");
  require(fixed_str(data, 1572, 64) == "raPId ACC telemetry",
          label + ": tool string");
  require(std::size_t(event_pointer) == 1762, label + ": header total size");

  // Event block.
  require(fixed_str(data, event_pointer, 64) == "ACC", label + ": event simulator");
  require(fixed_str(data, event_pointer + 64, 64) == "Race",
          label + ": event session");
  require(fixed_str(data, event_pointer + 128, 1024) == "Recorded by raPId",
          label + ": event note");
  require(u16(data, event_pointer + 1152) == 0, label + ": event trailer");
  require(metadata_pointer == event_pointer + 1154, label + ": metadata pointer");
  require(data_pointer ==
              metadata_pointer + expected_channel_count * 124,
          label + ": data pointer");

  // Channel headers, in channels.inc order.
  for (std::size_t i = 0; i < expected_channel_count; ++i) {
    auto off = metadata_pointer + i * 124;
    auto prev = u32(data, off), next = u32(data, off + 4),
         data_ptr = u32(data, off + 8), n = u32(data, off + 12);
    auto dtype_type = u16(data, off + 18), dtype = u16(data, off + 20),
         rate = u16(data, off + 22);
    auto shift = i16(data, off + 24), mul = i16(data, off + 26),
         scale_field = i16(data, off + 28), dec = i16(data, off + 30);
    auto name = fixed_str(data, off + 32, 32);
    auto short_name = fixed_str(data, off + 64, 8);
    auto unit = fixed_str(data, off + 72, 12);
    std::string ch = label + " channel " + std::to_string(i) + " (" +
                     expected_channels[i].key + ")";
    require(prev == (i == 0 ? 0 : metadata_pointer + (i - 1) * 124),
            ch + ": prev pointer");
    require(next == (i + 1 == expected_channel_count
                          ? 0
                          : metadata_pointer + (i + 1) * 124),
            ch + ": next pointer");
    require(data_ptr == data_pointer + i * sample_count * 4,
            ch + ": data pointer");
    require(n == sample_count, ch + ": sample count");
    require(dtype_type == 7 && dtype == 4, ch + ": dtype");
    require(rate == kRate, ch + ": rate");
    require(shift == 0 && mul == 1 && scale_field == 1, ch + ": shift/mul/scale");
    // Float32 channels must declare dec=0: the known-good ACC/MoTeC ADL export
    // does on all 55 channels, and ldparser applies 10^-dec to every dtype, so
    // a nonzero dec risks i2 scaling the stored value. See pinned_channels.
    require(dtype_type != 7 || dec == 0, ch + ": float dec must be 0");
    require(dec == pinned_channels[i].dec, ch + ": decimal places");
    require(name == pinned_channels[i].name, ch + ": name");
    require(short_name == pinned_channels[i].short_name, ch + ": short name");
    require(unit == pinned_channels[i].unit, ch + ": unit");

    for (std::size_t s = 0; s < sample_count; ++s) {
      float value = f32(data, data_ptr + s * 4);
      double want = expected_stored(i, global_start + s, s);
      require(std::abs(double(value) - want) < 1e-3,
              ch + " sample " + std::to_string(s) + ": value " +
                  std::to_string(value) + " != " + std::to_string(want));
    }
  }
  require(data.size() ==
              data_pointer + expected_channel_count * sample_count * 4,
          label + ": file size matches header/channel/data layout");
}

// Parses full-session.ldx's beacon markers and summary strings from scratch.
void check_ldx(const std::string &xml,
               const std::vector<std::pair<std::size_t, std::size_t>> &laps) {
  std::set<std::size_t> expected_samples;
  for (auto [start, end] : laps) {
    expected_samples.insert(start);
    expected_samples.insert(end);
  }
  std::vector<double> found;
  std::size_t pos = 0;
  while ((pos = xml.find("Time=\"", pos)) != std::string::npos) {
    pos += 6;
    auto end = xml.find('"', pos);
    found.push_back(std::stod(xml.substr(pos, end - pos)));
    pos = end;
  }
  require(found.size() == expected_samples.size(), "ldx: beacon count");
  std::size_t idx = 0;
  for (auto sample : expected_samples) {
    double want = double(sample) * 1000000.0 / kRate;
    require(std::abs(found.at(idx) - want) < 1e-6, "ldx: beacon time");
    ++idx;
  }
  require(xml.find("Id=\"Total Laps\" Value=\"" +
                    std::to_string(laps.size()) + "\"") != std::string::npos,
          "ldx: total laps");
  // Lap 2 (index 1) is the fastest by construction (kLap2Ms < kLap1Ms).
  require(xml.find("Id=\"Fastest Lap\" Value=\"2\"") != std::string::npos,
          "ldx: fastest lap number");
  std::ostringstream fastest_time;
  fastest_time << std::fixed << (kLap2Ms / 60000) << ":" << std::setw(6)
               << std::setfill('0') << std::setprecision(3)
               << double(kLap2Ms % 60000) / 1000;
  require(xml.find("Id=\"Fastest Time\" Value=\"" + fastest_time.str() +
                    "\"") != std::string::npos,
          "ldx: fastest time");
}

} // namespace

int main() {
  try {
    auto root = fs::temp_directory_path() / ("rapid-motec-tests-" + unique_id());
    fs::create_directories(root);
    fs::path published;
    {
      Recorder recorder(root, "all", [&](const fs::path &p) { published = p; });
      for (std::size_t s = 0; s < kTotalSamples; ++s)
        recorder.record(build_frame(s));
      recorder.finish("ended");
    }
    require(!published.empty(), "publish callback fired");
    require(fs::exists(published / "manifest.json"), "manifest published");

    auto manifest = Json::parse(read_file(published / "manifest.json"));
    require(manifest.at("laps").size() == 2, "two laps recorded");
    require(manifest["laps"][0]["number"] == 1 &&
                manifest["laps"][0]["start_sample"] == 0 &&
                manifest["laps"][0]["end_sample"] == kLap1Samples,
            "lap 1 bounds");
    require(manifest["laps"][1]["number"] == 2 &&
                manifest["laps"][1]["start_sample"] == kLap1Samples &&
                manifest["laps"][1]["end_sample"] ==
                    kLap1Samples + kLap2Samples,
            "lap 2 bounds");

    // Full session against the whole input spool.
    auto full = read_file(published / "full-session.ld");
    check_ld(full, kTotalSamples, 0, "full-session");

    // A lap range: lap 2, samples [60, 120), independently of the writer's
    // own bookkeeping -- re-derives the expected slice from the same
    // deterministic fixture used to build the recording.
    auto lap_path = published / "laps" / "lap-002-unknown.ld";
    require(fs::exists(lap_path), "lap 2 file published");
    auto lap2 = read_file(lap_path);
    check_ld(lap2, kLap2Samples, kLap1Samples, "lap-002");

    // Lap 1 too, to cross-check a range that does not start at sample 0.
    auto lap1_path = published / "laps" / "lap-001-unknown.ld";
    require(fs::exists(lap1_path), "lap 1 file published");
    auto lap1 = read_file(lap1_path);
    check_ld(lap1, kLap1Samples, 0, "lap-001");

    auto ldx = read_file(published / "full-session.ldx");
    check_ldx(ldx, {{0, kLap1Samples}, {kLap1Samples, kLap1Samples + kLap2Samples}});

    std::cout << "native recorder LD/LDX read-back tests passed" << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAILED: " << e.what() << std::endl;
    return 1;
  }
}
