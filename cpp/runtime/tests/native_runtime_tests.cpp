#include "rapid/native.hpp"
#include "v4_stream.hpp"
#include <bit>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace rapid::native;
using rapid::test::V4Stream;
using rapid::test::v4_run_id;
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::uint32_t u32(const std::string &data, std::size_t offset) {
  std::uint32_t n = 0;
  for (int i = 0; i < 4; ++i)
    n |= std::uint32_t(static_cast<unsigned char>(data.at(offset + i)))
         << (i * 8);
  return n;
}
// Recorder-level payload. The recorder consumes this internal decoded shape
// directly (it is not a wire packet), so the crash-recovery and publication
// tests keep using it while every Runtime::receive test now speaks v4.
Json sample(int sequence = 0, int lap = 1) {
  return {{"version", 4},
          {"type", "telemetry"},
          {"simulator", "ACC"},
          {"session_id", "native-test"},
          {"sequence", sequence},
          {"monotonic_us", sequence * 100000},
          {"sample_rate_hz", 10},
          {"driver_name", "Test driver"},
          {"car_model", "GT3"},
          {"track_name", "Spa"},
          {"session_name", "Race"},
          {"telemetry",
           {{"rpm", 6000},
            {"steering_angle", -.2},
            {"g_x", .4},
            {"g_y", 1.0},
            {"g_z", -.1},
            {"throttle", .5},
            {"brake", .1},
            {"speed_kmh", 160},
            {"gear", 3},
            {"lap_number", lap},
            {"current_lap_ms", sequence * 100},
            {"completed_lap_ms", lap > 1 ? 1000 : 0}}}};
}
// A v4 stream plus the one telemetry shape the runtime tests share. Time is
// advanced on every call so the decoder's monotonic timestamp check passes.
struct Wire {
  V4Stream stream;
  std::uint64_t time_us = 0;
  explicit Wire(std::string run) : stream(std::move(run)) {}
  std::uint64_t tick() { return time_us += 20000; }
  std::string metadata(const std::string &lock = "900") {
    return stream.metadata(tick(), "Spa", "GT3", "Test driver", "Race", lock);
  }
  std::string telemetry(int lap, double current_lap_ms, float throttle = .5f,
                        int completed_lap_ms = 0) {
    const auto mask = rapid::test::v4_mask(
        {rapid::test::ch_throttle, rapid::test::ch_brake,
         rapid::test::ch_gear, rapid::test::ch_rpm,
         rapid::test::ch_steering, rapid::test::ch_speed,
         rapid::test::ch_g_x, rapid::test::ch_g_y, rapid::test::ch_g_z,
         rapid::test::ch_lap_number, rapid::test::ch_current_lap_ms,
         rapid::test::ch_lap_position, rapid::test::ch_yaw_rate});
    return stream.telemetry(
        tick(), mask,
        {{rapid::test::ch_rpm, 6000.f},
         {rapid::test::ch_steering, -.2f},
         {rapid::test::ch_g_x, .4f},
         {rapid::test::ch_g_y, 1.f},
         {rapid::test::ch_g_z, -.1f},
         {rapid::test::ch_throttle, throttle},
         {rapid::test::ch_brake, .1f},
         {rapid::test::ch_speed, 160.f},
         {rapid::test::ch_gear, 3.f},
         {rapid::test::ch_lap_number, float(lap)},
         {rapid::test::ch_current_lap_ms, float(current_lap_ms)},
         {rapid::test::ch_lap_position, 0.f},
         {rapid::test::ch_yaw_rate, .25f}},
        completed_lap_ms);
  }
  std::string telemetry_with_direct_degrees(int lap, double current_lap_ms,
                                             float degrees) {
    auto packet = telemetry(lap, current_lap_ms);
    packet[58] = char(static_cast<unsigned char>(packet[58]) | 0x01); // bit 48
    rapid::test::v4_put(packet, 68 + rapid::test::ch_steering_deg * 4,
                        std::bit_cast<std::uint32_t>(degrees), 4);
    return stream.sign(std::move(packet));
  }
};
int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("asset path required");
    auto root =
        fs::temp_directory_path() / ("rapid-native-tests-" + unique_id());
    fs::create_directories(root);
    Config c;
    c.assets = argv[1];
    c.database = root / "state.db";
    c.telemetry = root / "telemetry";
    c.queue = root / "queue.db";
    // Authenticated v4 is the only transport, so the runtime under test and
    // the synthetic stream must share the HMAC key.
    c.companion_key = std::string(32, '\x11');
    Runtime runtime(c);
    {
      Config freshness = c;
      freshness.database = root / "freshness.db";
      freshness.telemetry = root / "freshness-telemetry";
      Runtime live(freshness);
      require(!live.snapshot()["telemetry_fresh"].get<bool>(), "no sample is not fresh");
      Wire wire(v4_run_id());
      require(live.receive(wire.metadata(), "127.0.0.1"), "freshness metadata");
      auto first = wire.telemetry(1, 100);
      require(live.receive(first, "127.0.0.1"), "freshness sample");
      require(live.snapshot()["telemetry_fresh"] == true, "new sample is fresh");
      require(live.snapshot()["process_ms"]["p50"].is_number() &&
                  live.snapshot()["process_ms"]["p95"].is_number(),
              "processing latency percentiles exposed");
      std::this_thread::sleep_for(std::chrono::milliseconds(1600));
      require(live.receive(wire.stream.status(2, wire.tick()), "127.0.0.1"),
              "driving heartbeat");
      require(live.snapshot()["companion_connected"] == true &&
                  live.snapshot()["telemetry_fresh"] == false &&
                  number(live.snapshot(), "telemetry_age_ms") >= 1500,
              "heartbeat cannot freshen retained telemetry");
      auto resumed = wire.telemetry(1, 200);
      require(live.receive(resumed, "127.0.0.1"), "samples resume");
      require(live.snapshot()["telemetry_fresh"] == true, "resumed sample is fresh");
      std::this_thread::sleep_for(std::chrono::milliseconds(1600));
      require(!live.receive(resumed, "127.0.0.1"), "replayed sample rejected");
      live.expire();
      require(live.snapshot()["companion_connected"] == false &&
                  live.snapshot()["recording"] == false,
              "replayed sample cannot extend connection lifetime");
      require(live.receive(wire.telemetry(1, 300), "127.0.0.1"),
              "reconnect after expiry");
      // Waiting/ready heartbeats travel on their own random control stream
      // (v4 contract), so a ready status is a fresh control stream rather than
      // a status packet on the still-active driving run.
      V4Stream control(v4_run_id());
      require(live.receive(control.status(1, 20000), "127.0.0.1"),
              "ready heartbeat");
      require(live.snapshot()["telemetry_fresh"] == false &&
                  live.snapshot()["telemetry_age_ms"].is_null(),
              "ready clears sample freshness");
    }
    {
      // #15: a "paused" heartbeat (sim pause/menu overlay/alt-tab) keeps the
      // spool/session open past telemetry going quiet, up to
      // Config::not_live_timeout_seconds (a small value here for a fast,
      // deterministic test; production defaults to 10 minutes). Heartbeats
      // arrive under 1.5s apart, like the real companion's 1 Hz cadence, so
      // the unrelated full-silence disconnect check never preempts this.
      Config pause_config = c;
      pause_config.database = root / "pause.db";
      pause_config.telemetry = root / "pause-telemetry";
      pause_config.queue = root / "pause-queue.db";
      pause_config.not_live_timeout_seconds = 2.5;
      Runtime live(pause_config);
      Wire wire(v4_run_id());
      require(live.receive(wire.metadata(), "127.0.0.1"), "pause-test metadata");
      require(live.receive(wire.telemetry(1, 100), "127.0.0.1"),
              "pause-test telemetry sample");
      require(live.snapshot()["recording"] == true, "recording starts on telemetry");
      for (int second = 0; second < 2; ++second) {
        require(live.receive(wire.stream.status(4, wire.tick()), "127.0.0.1"),
                "paused heartbeat accepted");
        require(live.snapshot()["recording"] == true &&
                    live.snapshot()["session_active"] == true &&
                    live.snapshot()["companion_daemon_state"] == "paused" &&
                    live.snapshot()["connected"] == true,
                "paused heartbeat keeps the session open and reports paused, not idle");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        live.expire();
        require(live.snapshot()["recording"] == true,
                "still within the not-live timeout while heartbeats keep arriving");
      }
      require(live.receive(wire.stream.status(4, wire.tick()), "127.0.0.1"),
              "final paused heartbeat");
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      live.expire();
      require(live.snapshot()["recording"] == false,
              "recording ends once telemetry has been silent past not_live_timeout_seconds, "
              "even though heartbeats kept arriving (#15)");
    }
    {
      // An explicit non-open status (e.g. the companion's session-ended /
      // main-menu heartbeat) still finalizes immediately, regardless of the
      // not-live timeout (#15).
      Config end_config = c;
      end_config.database = root / "explicit-end.db";
      end_config.telemetry = root / "explicit-end-telemetry";
      end_config.queue = root / "explicit-end-queue.db";
      Runtime live(end_config);
      Wire wire(v4_run_id());
      require(live.receive(wire.metadata(), "127.0.0.1"), "explicit-end metadata");
      require(live.receive(wire.telemetry(1, 100), "127.0.0.1"),
              "explicit-end telemetry sample");
      V4Stream control(v4_run_id());
      require(live.receive(control.status(1, 20000), "127.0.0.1"),
              "non-open status accepted");
      require(live.snapshot()["recording"] == false,
              "an explicit non-open status still finalizes immediately, unlike paused (#15)");
    }
    {
      // #47: a session-identity change while still connected must drop the
      // previous session's steering lock, exactly like a disconnect does, so a
      // car/session change cannot show the old lock for a tick.
      Config lock_config = c;
      lock_config.database = root / "steering-lock.db";
      lock_config.telemetry = root / "steering-lock-telemetry";
      lock_config.queue = root / "steering-lock-queue.db";
      lock_config.steering_lock = root / "sim-authoritative-lock.json";
      std::ofstream(lock_config.steering_lock) << "{\"lock_to_lock_deg\":540}";
      Runtime live(lock_config);
      Wire wire(v4_run_id());
      require(live.receive(wire.metadata(), "127.0.0.1"),
              "lock-test metadata with a 900 deg lock");
      require(live.receive(wire.telemetry(1, 100), "127.0.0.1"),
              "lock-test telemetry sample");
      require(live.snapshot()["steering_lock_deg"] == 900,
              "metadata lock exposed before the session change");
      require(std::abs(number(live.snapshot(), "steering_angle_deg") - (-90.0)) < 1e-5,
              "simulator lock derives steering degrees");
      std::ofstream(lock_config.steering_lock) << "{\"lock_to_lock_deg\":720}";
      require(live.receive(wire.metadata(), "127.0.0.1") &&
                  live.receive(wire.telemetry(1, 200), "127.0.0.1"),
              "simulator-lock heartbeat and sample accepted");
      require(std::abs(number(live.snapshot(), "steering_angle_deg") - (-90.0)) < 1e-5,
              "simulator metadata remains authoritative over owner changes");
      // A waiting status arrives on a new run id: the identity changes with no
      // fresh metadata, so the retained lock must be dropped rather than shown
      // for the next tick.
      V4Stream next(v4_run_id());
      require(live.receive(next.status(0, 20000), "127.0.0.1"),
              "new-session waiting status accepted");
      require(live.snapshot()["steering_lock_deg"].is_null(),
              "steering lock cleared on session-identity change (#47)");
      require(live.snapshot()["steering_angle_deg"].is_null(),
              "steering degrees cleared on session reset");
      // The next car/session opens a fresh run whose metadata supplies the new
      // lock; the change must not leave it null once fresh metadata arrives.
      V4Stream fresh(v4_run_id());
      require(live.receive(fresh.metadata(20000, "Spa", "GT3", "Test driver",
                                          "Race", "540"),
                           "127.0.0.1"),
              "new-session metadata accepted");
      require(live.snapshot()["steering_lock_deg"] == 540,
              "new session's lock replaces the previous session's (#47)");
    }
    {
      // The owner fallback is refreshed by the once-per-second metadata
      // heartbeat, not by every 50 Hz telemetry packet. Changes and deletion
      // therefore take effect during an active run without filesystem I/O on
      // the hot path.
      Config owner = c;
      owner.database = root / "owner-lock.db";
      owner.telemetry = root / "owner-lock-telemetry";
      owner.queue = root / "owner-lock-queue.db";
      owner.steering_lock = root / "owner-lock.json";
      std::ofstream(owner.steering_lock) << "{\"lock_to_lock_deg\":540}";
      Runtime live(owner);
      Wire wire(v4_run_id());
      require(live.receive(wire.metadata(""), "127.0.0.1") &&
                  live.receive(wire.telemetry(1, 100), "127.0.0.1"),
              "owner-lock run starts");
      require(std::abs(number(live.snapshot(), "steering_angle_deg") - (-54.0)) < 1e-5,
              "initial owner lock derives steering degrees");
      std::ofstream(owner.steering_lock) << "{\"lock_to_lock_deg\":720}";
      require(live.receive(wire.metadata(""), "127.0.0.1"),
              "changed owner-lock heartbeat accepted");
      require(std::abs(number(live.snapshot(), "steering_angle_deg") - (-72.0)) < 1e-5,
              "changed owner lock applies on the active-run heartbeat");
      require(live.receive(wire.telemetry(1, 200), "127.0.0.1"),
              "changed owner-lock sample accepted");
      fs::remove(owner.steering_lock);
      require(live.receive(wire.metadata(""), "127.0.0.1"),
              "deleted owner-lock heartbeat accepted");
      require(live.snapshot()["steering_angle_deg"].is_null(),
              "deleting owner lock clears derived degrees on the heartbeat");
      require(live.receive(wire.telemetry(1, 300), "127.0.0.1"),
              "deleted owner-lock sample accepted");
      std::ofstream(owner.steering_lock) << "{\"lock_to_lock_deg\":600}";
      require(live.receive(wire.metadata(""), "127.0.0.1"),
              "recreated owner-lock heartbeat accepted");
      require(std::abs(number(live.snapshot(), "steering_angle_deg") - (-60.0)) < 1e-5,
              "recreated owner lock applies on the active-run heartbeat");
      require(live.receive(wire.telemetry(1, 400), "127.0.0.1"),
              "recreated owner-lock sample accepted");
      require(live.receive(wire.stream.status(3, wire.tick()), "127.0.0.1"),
              "owner-lock run ended");
      require(live.snapshot()["steering_angle_deg"].is_null(),
              "ended status clears derived steering degrees");
      Json bundle;
      for (int i = 0; i < 100 && !bundle.is_string(); ++i) {
        bundle = live.snapshot()["last_bundle_path"];
        if (!bundle.is_string())
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      require(bundle.is_string(), "owner-lock recording published");
      const auto published = fs::path(bundle.get<std::string>());
      const auto manifest = Json::parse(read_file(published / "manifest.json"));
      require(manifest["quality"]["channel_available_samples"]["steering_angle_deg"] == 3 &&
                  manifest["quality"]["channel_available_samples"]["tyre_air_temp_fl"] == 0 &&
                  manifest["quality"]["channel_available_samples"]["tyre_air_temp_fr"] == 0 &&
                  manifest["quality"]["channel_available_samples"]["tyre_air_temp_rl"] == 0 &&
                  manifest["quality"]["channel_available_samples"]["tyre_air_temp_rr"] == 0 &&
                  manifest["quality"]["channel_available_samples"]["wheel_speed_mps_fl"] == 0 &&
                  manifest["quality"]["channel_available_samples"]["wheel_speed_mps_rr"] == 0 &&
                  manifest["quality"]["channel_available_samples"]["yaw_rate"] == 4,
              "schema-3 recorder availability covers derived, unavailable, and bit-62 channels");
      const auto ld = read_file(published / "full-session.ld");
      const auto data = u32(ld, 12);
      const auto first_steer_deg = std::bit_cast<float>(u32(ld, data + 48 * 4 * 4));
      require(std::abs(first_steer_deg - (-54.f)) < 1e-4,
              "owner-derived STEERANGLE stored in the LD channel");
    }
    {
      // A direct degree value from ACE/iRacing outranks the owner fallback.
      // Heartbeats still refresh the cached fallback, but must not replace or
      // clear the live direct value until telemetry withdraws that channel.
      Config direct = c;
      direct.database = root / "direct-steering.db";
      direct.telemetry = root / "direct-steering-telemetry";
      direct.queue = root / "direct-steering-queue.db";
      direct.steering_lock = root / "direct-steering-owner.json";
      std::ofstream(direct.steering_lock) << "{\"lock_to_lock_deg\":540}";
      Runtime live(direct);
      Wire wire(v4_run_id());
      require(live.receive(wire.metadata(""), "127.0.0.1") &&
                  live.receive(wire.telemetry_with_direct_degrees(1, 100, -33.f),
                               "127.0.0.1"),
              "direct-degree stream starts");
      require(std::abs(number(live.snapshot(), "steering_angle_deg") - (-33.0)) < 1e-6,
              "direct steering degrees reach live state");
      std::ofstream(direct.steering_lock) << "{\"lock_to_lock_deg\":720}";
      require(live.receive(wire.metadata(""), "127.0.0.1"),
              "direct-degree owner-change heartbeat accepted");
      require(std::abs(number(live.snapshot(), "steering_angle_deg") - (-33.0)) < 1e-6,
              "owner change does not overwrite direct steering degrees");
      fs::remove(direct.steering_lock);
      require(live.receive(wire.metadata(""), "127.0.0.1"),
              "direct-degree owner-deletion heartbeat accepted");
      require(std::abs(number(live.snapshot(), "steering_angle_deg") - (-33.0)) < 1e-6,
              "owner deletion does not clear direct steering degrees");
      require(live.receive(wire.telemetry(1, 200), "127.0.0.1"),
              "direct-degree unavailable transition accepted");
      require(live.snapshot()["steering_angle_deg"].is_null(),
              "fallback resumes as unavailable when direct degrees disappear and owner is absent");
      std::ofstream(direct.steering_lock) << "{\"lock_to_lock_deg\":600}";
      require(live.receive(wire.metadata(""), "127.0.0.1"),
              "post-direct fallback heartbeat accepted");
      require(std::abs(number(live.snapshot(), "steering_angle_deg") - (-60.0)) < 1e-5,
              "owner fallback resumes after direct degrees become unavailable");
    }
    {
      // A new run can announce active metadata directly, without an ended or
      // waiting packet for the old run. Its metadata must invalidate both the
      // previous direct-degree authority and the old normalized sample before
      // considering the new run's fallback.
      Config direct_change = c;
      direct_change.database = root / "direct-session-change.db";
      direct_change.telemetry = root / "direct-session-change-telemetry";
      direct_change.queue = root / "direct-session-change-queue.db";
      direct_change.steering_lock = root / "direct-session-change-owner.json";
      std::ofstream(direct_change.steering_lock) << "{\"lock_to_lock_deg\":600}";
      Runtime live(direct_change);
      Wire first(v4_run_id());
      require(live.receive(first.metadata(""), "127.0.0.1") &&
                  live.receive(first.telemetry_with_direct_degrees(1, 100, -33.f),
                               "127.0.0.1"),
              "first direct-degree run starts");
      require(std::abs(number(live.snapshot(), "steering_angle_deg") - (-33.0)) < 1e-6 &&
                  live.snapshot()["steering_angle"].is_number(),
              "first run exposes direct and normalized steering");
      Wire second(v4_run_id());
      require(live.receive(second.metadata(""), "127.0.0.1"),
              "new active metadata accepted without an end packet");
      const auto between_runs = live.snapshot();
      require(between_runs["steering_angle"].is_null() &&
                  between_runs["steering_angle_deg"].is_null() &&
                  between_runs["steering_lock_deg"].is_null(),
              "new active metadata clears prior steering and does not derive fallback before telemetry");
      require(live.receive(second.telemetry(1, 100), "127.0.0.1"),
              "new run's first telemetry accepted");
      require(std::abs(number(live.snapshot(), "steering_angle_deg") - (-60.0)) < 1e-5,
              "new run derives fallback only from its fresh telemetry");
    }
    {
      Config expiry = c;
      expiry.database = root / "owner-expire.db";
      expiry.telemetry = root / "owner-expire-telemetry";
      expiry.queue = root / "owner-expire-queue.db";
      expiry.steering_lock = root / "owner-expire.json";
      std::ofstream(expiry.steering_lock) << "{\"lock_to_lock_deg\":540}";
      Runtime live(expiry);
      Wire wire(v4_run_id());
      require(live.receive(wire.metadata(""), "127.0.0.1") &&
                  live.receive(wire.telemetry(1, 100), "127.0.0.1"),
              "expiry steering sample accepted");
      std::this_thread::sleep_for(std::chrono::milliseconds(1600));
      live.expire();
      require(live.snapshot()["steering_angle_deg"].is_null(),
              "connection expiry clears derived steering degrees");
    }
    // A datagram that is not authenticated v4 is rejected and counted, exactly
    // like malformed v4 -- the retired unauthenticated path is gone.
    require(!runtime.receive("[]", "127.0.0.1"), "non-object rejected");
    require(runtime.snapshot()["packets_invalid"] == 1,
            "non-v4 datagram counted invalid");
    Wire wire(v4_run_id());
    wire.stream.rate = 10;
    require(runtime.receive(wire.metadata(), "127.0.0.1"), "main metadata");
    auto first = wire.telemetry(1, 0);
    require(runtime.receive(first, "127.0.0.1"),
            "valid telemetry after malformed");
    auto session = runtime.snapshot()["session_id"];
    require(session.is_string() && session.get<std::string>().size() == 32,
            "v4 wire run id exposed as the session id");
    require(!runtime.receive(first, "127.0.0.1"), "replay rejected");
    // A valid v4 packet from an unexpected host is dropped before it can
    // change state.
    require(!runtime.receive(wire.telemetry(1, 100), "192.168.1.5"),
            "sender pinning");
    for (int i = 1; i < 10; ++i)
      require(runtime.receive(wire.telemetry(1, i * 100), "127.0.0.1"),
              "session sample");
    require(runtime.receive(wire.telemetry(2, 1000, .5f, 1000), "127.0.0.1"),
            "lap transition");
    require(runtime.snapshot()["recording"] == true, "recording state wired");
    require(runtime.snapshot()["recorded_samples"] == 11, "sample count wired");
    std::uint64_t cursor = 0;
    require(runtime.events(cursor)["events"].size() == 11, "live broker");
    require(runtime.events(cursor)["events"].empty(), "no repeated events");
    {
      Config burst_config = c;
      burst_config.database = root / "burst.db";
      burst_config.telemetry = root / "burst-telemetry";
      burst_config.queue = root / "burst-queue.db";
      Runtime burst(burst_config);
      Wire burst_wire(v4_run_id());
      burst_wire.stream.rate = 10;
      require(burst.receive(burst_wire.metadata(), "127.0.0.1"), "burst metadata");
      for (int i = 0; i < 301; ++i)
        require(burst.receive(burst_wire.telemetry(1, i * 100), "127.0.0.1"),
                "burst sample");
      std::uint64_t burst_cursor = 0;
      auto batch = burst.events(burst_cursor, 120);
      require(batch["events"].size() == 250 && batch["dropped"] == 51,
              "initial history capped with exact drop count");
      require(batch["events"].front()["sequence"] == 53 &&
                  batch["events"].back()["sequence"] == 302 && burst_cursor == 301,
              "newest events returned in order with final cursor");
      auto empty = burst.events(burst_cursor);
      require(empty["events"].empty() && empty["dropped"] == 0,
              "caught-up cursor produces no duplicates or drops");
      burst_cursor = 1;
      batch = burst.events(burst_cursor);
      require(batch["events"].size() == 250 && batch["dropped"] == 50,
              "lagging cursor accounts only unseen dropped events");
      require(burst.receive(burst_wire.telemetry(1, 30100), "127.0.0.1"),
              "incremental sample");
      batch = burst.events(burst_cursor);
      require(batch["events"].size() == 1 && batch["dropped"] == 0 &&
                  batch["events"].front()["sequence"] == 303,
              "incremental delivery after catch-up");
      burst.finish();
    }
    runtime.upload(false);
    runtime.finish();
    auto path =
        fs::path(runtime.snapshot().at("last_bundle_path").get<std::string>());
    auto manifest = Json::parse(read_file(path / "manifest.json"));
    require(manifest["laps"].size() == 1, "completed lap exported");
    require(manifest["metadata"]["upload_after_session"] == false,
            "manual upload choice persisted");
    auto data = read_file(path / "full-session.ld");
    auto meta = u32(data, 8), start = u32(data, 12);
    require(meta == 2916, "LD event pointer");
    require(u32(data, 86) == 63, "channel count");
    require(u32(data, meta + 12) == 11, "LD sample count");
    require(data.size() == start + 63 * 11 * 4, "LD size");
    require(std::abs(std::bit_cast<float>(u32(data, start + 11 * 4)) - 50) <
                .001,
            "throttle scaling");
    for (const auto &a : manifest["artifacts"]) {
      auto artifact = path / a["relative_path"].get<std::string>();
      require(hash_file(artifact) == string(a, "sha256"), "artifact digest");
    }
    auto lap = read_file(
        path / manifest["artifacts"][2]["relative_path"].get<std::string>());
    require(u32(lap, meta + 12) == 10, "lap excludes next lap sample");
    // Abrupt process exit bypasses destructors; recover only durable samples.
    auto recovery = root / "recovery";
    pid_t child = fork();
    if (child == 0) {
      try {
        Recorder recorder(recovery, "races");
        for (int i = 0; i < 10; ++i)
          recorder.record(sample(i));
        // The checkpoint at sample 10 is written by the recorder's writer
        // thread (#17); crash once it is durable. Bounded crash loss while
        // the writer is behind is covered by rapid-io-recording-crash.
        recorder.wait_idle();
        _exit(0);
      } catch (...) {
        _exit(1);
      }
    }
    int status = 0;
    waitpid(child, &status, 0);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "crash fixture");
    int recovered = 0;
    Recorder restored(recovery, "races", [&](const fs::path &bundle) {
      auto m = Json::parse(read_file(bundle / "manifest.json"));
      require(m["quality"]["recorded_samples"] == 10, "recovery sample count");
      ++recovered;
    });
    require(recovered == 1, "recovery callback");
    // Failed publication must leave a readable spool, then succeed on retry.
    auto failure = root / "failure";
    Recorder recorder(failure, "manual");
    for (int i = 0; i < 10; ++i)
      recorder.record(sample(i));
    recorder.wait_idle();
    fs::path spool;
    for (const auto &entry : fs::directory_iterator(failure))
      if (entry.path().filename().string().starts_with(".rapid-spool-"))
        spool = entry.path();
    auto channel = spool / "01.bin";
    fs::rename(channel, spool / "01.bin.old");
    bool failed = false;
    try {
      recorder.finish();
      recorder.wait_idle();
    } catch (...) {
      failed = true;
    }
    require(failed && fs::exists(spool / "spool.json") &&
                recorder.status()["last_bundle_path"].is_null(),
            "publication failure preserves spool");
    fs::rename(spool / "01.bin.old", channel);
    recorder.finish();
    recorder.wait_idle();
    require(recorder.status()["last_bundle_path"].is_string(),
            "failed publication succeeds on retry");
    {
      // Lost packets must leave the recorded timeline intact. The LD file has
      // no timestamps: sample n is at n / rate. A telemetry packet that never
      // arrives used to leave a hole that every later sample slid across, so
      // data near the end of a session sat early by the total time lost.
      // Sequence gaps count lost packets of any kind, and control packets
      // (heartbeats) share the sequence with telemetry, so a gap alone does
      // not say how many samples are missing; the sender's timestamps do.
      Config lossy = c;
      lossy.database = root / "loss.db";
      lossy.telemetry = root / "loss-telemetry";
      lossy.queue = root / "loss-queue.db";
      Runtime loss(lossy);
      Wire wire(v4_run_id());
      require(loss.receive(wire.metadata(), "127.0.0.1"), "loss metadata");
      int delivered = 0, generated = 0;
      const auto sample = [&](bool deliver) {
        const auto packet = wire.telemetry(1, 1000 + generated * 20.0);
        ++generated;
        if (!deliver) return;
        require(loss.receive(packet, "127.0.0.1"), "loss sample accepted");
        ++delivered;
      };
      for (int i = 0; i < 20; ++i) sample(true);
      // A lost heartbeat shares the sequence but is not a sample; it is sent
      // between samples, so no sample time is spent on it.
      (void)wire.stream.metadata(wire.time_us, "Spa", "GT3", "Test driver", "Race", "900");
      for (int i = 0; i < 20; ++i) sample(true);
      // Five samples lost in a row, then a heartbeat that arrives (so the gap
      // is split across two received packets), then one more lost on its own.
      for (int i = 0; i < 5; ++i) sample(false);
      require(loss.receive(wire.stream.metadata(wire.time_us, "Spa", "GT3", "Test driver", "Race", "900"),
                           "127.0.0.1"),
              "heartbeat after a burst accepted");
      for (int i = 0; i < 20; ++i) sample(true);
      sample(false);
      for (int i = 0; i < 20; ++i) sample(true);
      require(loss.receive(wire.stream.status(3, wire.time_us), "127.0.0.1"), "loss run ended");
      Json bundle;
      for (int i = 0; i < 200 && !bundle.is_string(); ++i) {
        bundle = loss.snapshot()["last_bundle_path"];
        if (!bundle.is_string())
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      require(bundle.is_string(), "loss recording published");
      const auto manifest = Json::parse(read_file(fs::path(bundle.get<std::string>()) / "manifest.json"));
      require(delivered == generated - 6, "the fixture lost six samples");
      require(manifest["quality"]["missing_packets"] == 6 && manifest["quality"]["substituted_samples"] == 6 &&
                  manifest["quality"]["unfilled_missing_packets"] == 0,
              "six lost samples are counted and filled, the lost heartbeat is not");
      require(manifest["quality"]["recorded_samples"] == generated &&
                  manifest["quality"]["received_samples"] == delivered,
              "every sample slot is on the timeline: recorded equals sent, received equals delivered");
    }
    std::cout << "Native runtime: v4 validation, source pinning, replay, state, "
                 "broker, LD, lap, hashes, crash recovery and publication "
                 "retry passed\nEvidence: "
              << root << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
