// Regression tests for #16 (lap boundaries) and #20 (AC1 delta).
//
// Part A replays two fixtures decimated from the real 2026-09-13 AC1
// recordings named in #16 (lap-fixtures/*.json, containing only the Lap
// Number, Lap Time and Lap Position sample arrays) through the real
// Recorder, and checks the laps it actually closes:
//   - session-c28a1574-lap6-wrap.json: lap_number stays 6 for the whole
//     file while lap_position/current_lap_ms wrap once (sample 10317 in the
//     original recording) -- the old code never closed this lap at all.
//   - session-4e6de929-six-laps.json: six real crossings, five of them
//     visible as a lap_number increment and one (the very first) visible
//     only as a position/time wrap with lap_number stuck at 1. The old code
//     dropped four of the five lap_number crossings because completed_lap_ms
//     was 0 in the exact packet that carried the increment, and never saw
//     the sixth crossing at all.
//
// Part B is a synthetic two-lap fixture for the AC1 delta (#20): lap 1 has
// no reference yet, so delta_ms must stay blank; lap 2 is computed against
// lap 1's lap-position -> elapsed-time trace. ACC's own delta_ms is left
// untouched.
#include "rapid/lap_boundary.hpp"
#include "rapid/native.hpp"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <system_error>
#include <unistd.h>

using namespace rapid::native;

void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

// Redirects stderr (where log() writes) to a temp file for the duration of
// fn, so a test can check that a lap close actually logged -- the mechanism
// the dashboard's "Log updated" notice (#16) depends on.
std::string capture_stderr(const std::function<void()> &fn) {
  auto path = fs::temp_directory_path() / ("rapid-lap-stderr-" + unique_id());
  std::fflush(stderr);
  int saved = dup(fileno(stderr));
  require(saved >= 0, "dup stderr");
  require(std::freopen(path.string().c_str(), "w", stderr) != nullptr,
          "redirect stderr");
  try {
    fn();
  } catch (...) {
    std::fflush(stderr);
    dup2(saved, fileno(stderr));
    ::close(saved);
    throw;
  }
  std::fflush(stderr);
  dup2(saved, fileno(stderr));
  ::close(saved);
  auto captured = read_file(path);
  std::error_code ec;
  fs::remove(path, ec);
  return captured;
}

// --- Part A: direct LapBoundary unit tests -------------------------------

void test_lap_boundary_number_increment() {
  LapBoundary b;
  b.step(1, 0, 0.05, true);   // establishes state, no close possible yet
  for (int i = 1; i <= 20; ++i)
    b.step(1, i * 5000.0, 0.05 + i * 0.045, true);
  auto result = b.step(2, 100.0, 0.0, true);
  require(result.closed && result.via_lap_number,
          "lap_number increment closes a lap");
}

void test_lap_boundary_wrap_without_number_increment() {
  LapBoundary b;
  b.step(6, 0, 0.05, true);
  for (int i = 1; i <= 20; ++i)
    b.step(6, i * 5000.0, 0.05 + i * 0.045, true); // climbs through mid-band, arms
  auto result = b.step(6, 50.0, 0.01, true); // time resets, position wraps
  require(result.closed && !result.via_lap_number,
          "position wrap plus time reset closes a lap even without a "
          "lap_number increment (session-c28a1574...)");
}

void test_lap_boundary_reversal_does_not_double_close() {
  LapBoundary b;
  b.step(1, 0, 0.02, true);
  // The car sits right at the line and flickers back and forth without ever
  // driving out to mid-lap: no legitimate lap has happened, so this must not
  // arm the wrap heuristic.
  int closes = 0;
  for (int i = 0; i < 30; ++i) {
    double pos = (i % 2 == 0) ? 0.95 : 0.02;
    double time = (i % 2 == 0) ? 90000.0 : 50.0;
    if (b.step(1, time, pos, true).closed)
      ++closes;
  }
  require(closes == 0,
          "reversing at the line without visiting mid-lap cannot manufacture a lap");
}

void test_lap_boundary_debounces_split_signal() {
  // Real telemetry (session-4e6de929..., samples 124782-124783) shows the
  // position/time reset and the lap_number increment for one physical
  // crossing arrive a sample apart. Both must not independently close a lap.
  LapBoundary b;
  b.step(2, 0, 0.05, true);
  for (int i = 1; i <= 20; ++i)
    b.step(2, i * 5000.0, 0.05 + i * 0.045, true);
  auto first = b.step(2, 0.0, 0.0, true);  // wrap fires, lap_number not yet updated
  auto second = b.step(3, 20.0, 0.0, true); // lap_number ticks one sample later
  require(first.closed && !second.closed,
          "the wrap close and the following lap_number increment count as one lap");
}

// --- Part A: real-recording regression fixtures ---------------------------

Json fixture_message(const std::string &session, int rate, int lap_number,
                     int current_lap_ms, double lap_position_pct) {
  return {{"version", 3},
          {"simulator", "AC"},
          {"session_id", session},
          {"sample_rate_hz", rate},
          {"telemetry", {{"lap_number", lap_number},
                        {"current_lap_ms", current_lap_ms},
                        {"lap_position", lap_position_pct}}}};
}

Json load_fixture(const fs::path &path) {
  return Json::parse(read_file(path));
}

void replay_fixture(Recorder &recorder, const Json &fixture) {
  const auto &lap = fixture.at("lap_number");
  const auto &time = fixture.at("current_lap_ms");
  const auto &pos = fixture.at("lap_position_pct");
  int rate = fixture.at("rate_hz");
  auto session = fixture.at("source").get<std::string>();
  require(lap.size() == time.size() && lap.size() == pos.size(),
          "fixture arrays are parallel");
  for (std::size_t i = 0; i < lap.size(); ++i)
    recorder.record(fixture_message(session, rate, lap[i].get<int>(),
                                    time[i].get<int>(), pos[i].get<double>()));
}

void test_session_c28a_lap6_wrap(const fs::path &fixtures, const fs::path &root) {
  auto fixture = load_fixture(fixtures / "session-c28a1574-lap6-wrap.json");
  Recorder recorder(root / ("c28a-" + unique_id()), "all");
  std::string log_output;
  log_output = capture_stderr([&] {
    replay_fixture(recorder, fixture);
    recorder.finish();
  });
  auto path = fs::path(recorder.status().at("last_bundle_path").get<std::string>());
  auto manifest = Json::parse(read_file(path / "manifest.json"));
  require(manifest["laps"].size() == 1,
          "lap_number stuck at 6 with a real position/time wrap still "
          "closes exactly one lap (#16)");
  const auto &lap = manifest["laps"][0];
  require(lap["valid"].is_null(),
          "a derived lap time (no completed_lap_ms in the fixture) is marked unknown");
  require(lap.at("time_ms").get<int>() > 0, "derived lap time is positive");
  require(lap.at("start_sample").get<int>() < lap.at("end_sample").get<int>(),
          "lap spans a non-empty sample range");
  require(log_output.find("Lap 1 recorded") != std::string::npos,
          "closing the lap logs, which is what drives the dashboard's "
          "\"Log updated\" notice at the line instead of only at the garage");
}

void test_session_4e6de929_six_laps(const fs::path &fixtures, const fs::path &root) {
  auto fixture = load_fixture(fixtures / "session-4e6de929-six-laps.json");
  Recorder recorder(root / ("4e6d-" + unique_id()), "all");
  std::string log_output;
  log_output = capture_stderr([&] {
    replay_fixture(recorder, fixture);
    recorder.finish();
  });
  auto path = fs::path(recorder.status().at("last_bundle_path").get<std::string>());
  auto manifest = Json::parse(read_file(path / "manifest.json"));
  require(manifest["laps"].size() == 6,
          "all six real crossings close a lap: the five visible as a "
          "lap_number increment (four of which the old code dropped because "
          "completed_lap_ms was 0 in that exact packet) plus the one visible "
          "only as a position/time wrap while lap_number stayed at 1 (#16)");
  int previous_end = -1;
  for (int i = 0; i < 6; ++i) {
    const auto &lap = manifest["laps"][i];
    require(lap.at("number").get<int>() == i + 1, "laps are numbered in detection order");
    require(lap["valid"].is_null(), "derived lap time is marked unknown");
    require(lap.at("time_ms").get<int>() > 0, "derived lap time is positive");
    require(lap.at("start_sample").get<int>() >= previous_end,
            "laps do not overlap (adjacent laps share a boundary sample)");
    previous_end = lap.at("end_sample").get<int>();
    require(log_output.find("Lap " + std::to_string(i + 1) + " recorded") !=
                std::string::npos,
            "each lap close logs individually, at the line");
  }
}

// --- Part B: AC1 delta -----------------------------------------------------

Json ac1_frame(const std::string &session, int sequence, int lap_number,
               double current_lap_ms, double lap_position_pct) {
  return {{"version", 3},
          {"type", "telemetry"},
          {"simulator", "AC"},
          {"session_id", session},
          {"sequence", sequence},
          {"monotonic_us", sequence * 20000},
          {"sample_rate_hz", 50},
          {"telemetry",
           {{"rpm", 6000},
            {"steering_angle", 0.0},
            {"g_x", 0.0},
            {"g_y", 0.0},
            {"g_z", 0.0},
            {"throttle", .8},
            {"brake", .0},
            {"speed_kmh", 200},
            {"gear", 4},
            {"lap_number", lap_number},
            {"current_lap_ms", int(current_lap_ms)},
            {"lap_position", lap_position_pct}}}};
}

void test_ac1_delta_two_lap_fixture(const fs::path &assets, const fs::path &root) {
  Config c;
  c.assets = assets;
  c.database = root / "delta.db";
  c.telemetry = root / "delta-telemetry";
  c.queue = root / "delta-queue.db";
  Runtime runtime(c);
  int sequence = 0;
  const std::string session = "ac1-delta-test";
  // Lap 1: reference lap, a perfectly linear 100 s pace. No reference lap
  // exists yet, so delta_ms must stay blank throughout.
  for (int i = 0; i <= 10; ++i) {
    double position = i * 10.0; // percent, matches the wire's 0..100 form
    double time_ms = i * 10000.0;
    require(runtime.receive(
                ac1_frame(session, sequence++, 1, time_ms, position).dump(),
                "127.0.0.1"),
            "lap 1 sample accepted");
    require(runtime.snapshot()["delta_ms"].is_null(),
            "delta stays blank until a reference lap exists (#20)");
  }
  // Close lap 1 (lap_number increments to 2, current_lap_ms resets).
  require(runtime.receive(
              ac1_frame(session, sequence++, 2, 0.0, 0.0).dump(), "127.0.0.1"),
          "lap 1 closes");
  // Lap 2: same positions, 10% slower (110 s pace). delta_ms should now be
  // populated and match position * 10000 ms, interpolated from lap 1's
  // straight-line trace.
  for (int i = 1; i <= 10; ++i) {
    double position = i * 10.0;
    double time_ms = i * 11000.0;
    require(runtime.receive(
                ac1_frame(session, sequence++, 2, time_ms, position).dump(),
                "127.0.0.1"),
            "lap 2 sample accepted");
    auto delta = runtime.snapshot()["delta_ms"];
    require(!delta.is_null(), "delta is populated once lap 1 is a reference");
    int expected = int(std::lround(position / 100.0 * 10000.0));
    require(std::abs(delta.get<int>() - expected) <= 1,
            "delta matches the interpolated lap 1 trace");
  }
  runtime.finish();
}

void test_acc_delta_untouched(const fs::path &assets, const fs::path &root) {
  Config c;
  c.assets = assets;
  c.database = root / "acc-delta.db";
  c.telemetry = root / "acc-delta-telemetry";
  c.queue = root / "acc-delta-queue.db";
  Runtime runtime(c);
  Json frame = {{"version", 3},
               {"type", "telemetry"},
               {"simulator", "ACC"},
               {"session_id", "acc-delta-test"},
               {"sequence", 0},
               {"monotonic_us", 0},
               {"sample_rate_hz", 50},
               {"telemetry",
                {{"rpm", 6000},
                 {"steering_angle", 0.0},
                 {"g_x", 0.0},
                 {"g_y", 0.0},
                 {"g_z", 0.0},
                 {"lap_number", 1},
                 {"current_lap_ms", 40000},
                 {"lap_position", 50.0},
                 {"delta_ms", -250}}}};
  require(runtime.receive(frame.dump(), "127.0.0.1"), "ACC sample accepted");
  require(runtime.snapshot()["delta_ms"].get<int>() == -250,
          "ACC's own delta_ms is never overwritten (#20)");
  runtime.finish();
}

int main(int argc, char **argv) {
  try {
    if (argc != 3)
      throw std::runtime_error("assets path and lap fixtures path required");
    fs::path assets = argv[1];
    fs::path fixtures = argv[2];
    auto root =
        fs::temp_directory_path() / ("rapid-lap-boundary-tests-" + unique_id());
    fs::create_directories(root);

    test_lap_boundary_number_increment();
    test_lap_boundary_wrap_without_number_increment();
    test_lap_boundary_reversal_does_not_double_close();
    test_lap_boundary_debounces_split_signal();
    test_session_c28a_lap6_wrap(fixtures, root);
    test_session_4e6de929_six_laps(fixtures, root);
    test_ac1_delta_two_lap_fixture(assets, root);
    test_acc_delta_untouched(assets, root);

    std::cout << "Lap boundary and AC1 delta: unit rules, real-recording "
                 "regression fixtures, and two-lap delta fixture passed\n"
                 "Evidence: "
              << root << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
