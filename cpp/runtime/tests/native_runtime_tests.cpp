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
  std::string metadata() {
    return stream.metadata(0, "Spa", "GT3", "Test driver", "Race", "900");
  }
  std::string telemetry(int lap, double current_lap_ms, float throttle = .5f,
                        int completed_lap_ms = 0) {
    const auto mask = rapid::test::v4_mask(
        {rapid::test::ch_throttle, rapid::test::ch_brake,
         rapid::test::ch_gear, rapid::test::ch_rpm,
         rapid::test::ch_steering, rapid::test::ch_speed,
         rapid::test::ch_g_x, rapid::test::ch_g_y, rapid::test::ch_g_z,
         rapid::test::ch_lap_number, rapid::test::ch_current_lap_ms,
         rapid::test::ch_lap_position});
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
         {rapid::test::ch_lap_position, 0.f}},
        completed_lap_ms);
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
    require(u32(data, 86) == 48, "channel count");
    require(u32(data, meta + 12) == 11, "LD sample count");
    require(data.size() == start + 48 * 11 * 4, "LD size");
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
