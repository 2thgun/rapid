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
// Part B speaks the authenticated v4 schema-3 wire. The former AC1 delta
// synthesis was deleted with #52: the wire now says explicitly whether a delta
// exists, so the tests assert that a real delta (including 0) is carried as a
// number and an absent one stays null, and that the recorder writes the
// completed lap's `valid` into the manifest (#16).
#include "rapid/lap_boundary.hpp"
#include "rapid/native.hpp"
#include "v4_stream.hpp"
#include "../../../companion/native/acc_lap_validity.hpp"
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

struct AccBoundary {
  AccLapTransition reducer;
  AccLapTransition::Result step(int c, double t, double p, bool has) {
    AccLapTransition::Observation o;
    o.counter=c; o.timer=t; o.position=p; o.has_position=has;
    return reducer.step(o);
  }
};
void test_lap_boundary_debounces_late_counter() {
  AccBoundary b;
  b.step(3, 100, .01, true);
  for (int i = 0; i < 12; ++i)
    b.step(3, 50000 + i * 4000, .35 + i * .05, true);
  require(b.step(3, 10, .01, true).closed, "physical ACC wrap closes");
  for (int i = 1; i <= 20; ++i)
    require(!b.step(3, 10 + i * 20, .01 + i * .001, true).closed,
            "new-lap samples do not close");
  require(!b.step(4, 450, .04, true).closed,
          "late counter after ten-sample cooldown is still the same physical crossing");
  for (int i = 0; i < 12; ++i)
    b.step(4, 50000 + i * 4000, .35 + i * .05, true);
  require(b.step(5, 10, .01, true).closed,
          "late-counter debounce must not suppress the following real lap");
}

void test_lap_boundary_counter_catchup_after_gap() {
  AccBoundary boundary;
  boundary.step(3, 50000, .35, true);
  for (int i = 0; i < 12; ++i)
    boundary.step(3, 60000 + i * 4000, .4 + i * .05, true);
  require(boundary.step(3, 5, .01, true).closed, "stale counter physical wrap closes");
  for (int i = 0; i < 12; ++i)
    boundary.step(3, 25 + i * 20, .02, true);
  require(!boundary.step(4, 30000, .35, true).closed,
          "counter catchup first seen mid-lap after a packet gap cannot close twice");
  for (int i = 0; i < 12; ++i)
    boundary.step(4, 60000 + i * 4000, .4 + i * .05, true);
  require(boundary.step(5, 5, .01, true).closed,
          "gap catchup attribution must not suppress the following physical lap");
}

void test_acc_validity_at_physical_wrap() {
  rapid::AccLapValidity validity;
  validity.update(3, 50000, .4, false);
  for (int i = 0; i < 12; ++i)
    validity.update(3, 60000 + i * 4000, .4 + i * .05, false);
  validity.update(3, 10, .01, true); // timer/position reset, counter stale
  require(validity.present && !validity.valid,
          "ACC physical wrap retains the invalidity before current flag resets");
  for (int i = 0; i < 20; ++i)
    validity.update(3, 30 + i * 20, .02, true);
  validity.update(4, 450, .04, true);
  require(validity.present && !validity.valid,
          "late ACC counter cannot overwrite physically completed validity");
  for (int i = 0; i < 12; ++i)
    validity.update(4, 60000 + i * 4000, .4 + i * .05, true);
  validity.update(5, 10, .01, false); // next lap invalid from its first sample
  require(validity.present && validity.valid,
          "new lap's invalidity must not leak into the valid completed lap");
  validity.update(1, 0, .01, true);
  require(!validity.present, "ACC session restart clears completed validity");
}

// --- Part A: real-recording regression fixtures ---------------------------
void test_acc_validity_counter_before_timer_reset() {
  for (int next_phase : {0, 1, 2}) {
    rapid::AccLapValidity validity;
    for (int i = 0; i < 13; ++i)
      validity.update(3, 179760 + i * 20, .35 + i * .05, false);
    validity.update(4, 180000, .99, false);
    require(validity.present && !validity.valid,
            "counter-first close retains invalid finishing lap");
    // Phase 0: the flag resets with the timer. Phase 1: the new lap is
    // genuinely invalid on its first timer sample. Phase 2: flag reset and
    // early invalidity arrive before the timer reset and must be retained.
    if (next_phase == 2) {
      validity.update(4, 180010, .99, true);
      validity.update(4, 180020, .99, false);
    }
    validity.update(4, 5, .01, next_phase != 1);
    for (int i = 0; i < 13; ++i)
      validity.update(4, 99760 + i * 20, .35 + i * .05, true);
    validity.update(5, 5, .01, true);
    if (next_phase != 2)
      require(!validity.present, "approved policy: untagged boundary false stays unknown even after later true");
    else
      require(validity.present && !validity.valid,
              "true then false inside an established flag phase retains invalidity");
  }
}

Json fixture_message(const std::string &session, int rate, int lap_number,
                     int current_lap_ms, double lap_position_pct) {
  return {{"version", 4},
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
    recorder.wait_idle(); // publication runs on the recorder's writer (#17)
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
    recorder.wait_idle(); // publication runs on the recorder's writer (#17)
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

// ACC graphics metadata can arrive after the physical position/timer wrap.
// This deterministic sequence reproduces the saved 125865 ms being attached
// to a lap whose current timer ended at 104365 ms. Validity on the wrap packet
// still describes the previous completed lap too.
void test_acc_delayed_completed_metadata(const fs::path &root) {
  Recorder recorder(root / ("acc-delayed-" + unique_id()), "all");
  auto send = [&](int lap, int timer, double position, int completed, bool valid) {
    auto m = fixture_message("acc-delayed", 50, lap, timer, position);
    m["simulator"] = "ACC";
    m["telemetry"]["completed_lap_ms"] = completed;
    m["telemetry"]["lap_valid"] = valid;
    recorder.record(m);
  };
  for (int i = 0; i < 12; ++i)
    send(3, 50000 + i * 4000, 35 + i * 5, 125865, true);
  send(3, 104365, 99.9, 125865, true);
  send(3, 4, 0.01, 125865, true); // physical wrap; all lap metadata stale
  send(4, 24, 0.03, 104372, false); // fresh completed time/validity later
  recorder.finish();
  recorder.wait_idle();
  auto path = fs::path(recorder.status().at("last_bundle_path").get<std::string>());
  auto manifest = Json::parse(read_file(path / "manifest.json"));
  require(manifest["laps"].size() == 1, "ACC split crossing closes only one lap");
  require(manifest["laps"][0]["time_ms"] == 104372,
          "ACC delayed completed time belongs to the wrapped lap, not its predecessor");
  require(manifest["laps"][0]["valid"] == false,
          "ACC delayed invalidity belongs to the wrapped lap, not its predecessor");
  require(manifest["laps"][0]["end_sample"] == 13,
          "delayed metadata must not move the physical sample boundary");
}

void test_acc_unresolved_metadata_uses_sim_timer(const fs::path &root) {
  Recorder recorder(root / ("acc-unresolved-" + unique_id()), "all");
  auto send = [&](int timer, double position, int gap = 0) {
    auto m = fixture_message("acc-unresolved", 50, 5, timer, position);
    m["simulator"] = "ACC";
    m["_packet_gap"] = gap;
    m["telemetry"]["completed_lap_ms"] = 104372;
    m["telemetry"]["lap_valid"] = true; // held predecessor's validity
    recorder.record(m);
  };
  for (int i = 0; i < 12; ++i)
    send(50000 + i * 4000, 35 + i * 5);
  send(109170, 99.9);
  send(109170, 99.9, 500); // pause/gap changes sample clock, not sim timer
  send(10, 0.01);
  send(30, 0.03); // metadata never catches up
  recorder.finish();
  recorder.wait_idle();
  auto path = fs::path(recorder.status().at("last_bundle_path").get<std::string>());
  auto manifest = Json::parse(read_file(path / "manifest.json"));
  require(manifest["laps"].size() == 1, "unresolved ACC wrap closes one lap");
  require(manifest["laps"][0]["time_ms"] == 109170,
          "unresolved ACC completion uses the sim timer, not stale time or paused sample clock");
  require(manifest["laps"][0]["valid"].is_null(),
          "unresolved ACC metadata never borrows the predecessor's validity");
  require(manifest["laps"][0]["time_source"] == "current_lap_timer",
          "ACC timer estimate is explicitly identified, not labelled exact");
}

void test_acc_validity_tracker_to_recorder(const fs::path &root) {
  for (bool finished_valid : {false, true}) {
    Recorder recorder(root / ("acc-latch-" + unique_id()), "all");
    rapid::AccLapValidity validity;
    auto send = [&](int lap, int timer, double position, int completed, bool current_valid) {
      validity.update(lap, timer, position / 100.0, current_valid);
      auto m = fixture_message("acc-latch", 50, lap, timer, position);
      m["simulator"] = "ACC";
      m["telemetry"]["completed_lap_ms"] = completed;
      if (validity.present)
        m["telemetry"]["lap_valid"] = validity.valid;
      recorder.record(m);
    };
    // Synthetic early-lap observation supplies the validity coverage this
    // end-to-end full-lap test requires; partial joins are separately unknown.
    send(3,100,1,125865,finished_valid);
    for (int i = 0; i < 12; ++i)
      send(3, 50000 + i * 4000, 35 + i * 5, 125865, finished_valid);
    send(3, 104365, 99.9, 125865, finished_valid);
    send(3, 4, .01, 125865, !finished_valid);
    // The source flag now describes the opposite validity of the new lap.
    // Graphics completed time arrives before the counter; the counter is
    // delayed past the old ten-sample debounce period.
    for (int i = 0; i < 20; ++i)
      send(3, 24 + i * 20, .03 + i * .01, 104372, !finished_valid);
    send(4, 444, .24, 104372, !finished_valid);
    recorder.finish();
    recorder.wait_idle();
    auto path = fs::path(recorder.status().at("last_bundle_path").get<std::string>());
    auto manifest = Json::parse(read_file(path / "manifest.json"));
    require(manifest["laps"].size() == 1, "late ACC metadata retains one physical segment");
    require(manifest["laps"][0]["time_ms"] == 104372 &&
                manifest["laps"][0]["time_source"] == "simulator",
            "tracker/recorder preserves delayed simulator candidate without generation certification");
    require(manifest["laps"][0]["valid"] == finished_valid,
            "tracker/recorder attributes finished validity, not the new lap's flag");
  }
}

void test_acc_counter_before_timer_reset(const fs::path &root, double crossing_position=.99, bool next_counter=false) {
  Recorder recorder(root / ("acc-counter-first-" + unique_id()), "all");
  auto send = [&](int lap, int timer, double position, int completed) {
    auto m = fixture_message("acc-counter-first", 50, lap, timer, position);
    m["simulator"] = "ACC";
    m["telemetry"]["completed_lap_ms"] = completed;
    recorder.record(m);
  };
  for (int i = 0; i < 13; ++i)
    send(3, 179760 + i * 20, .35 + i * .05, 120000);
  send(4, 180000, crossing_position, 180010); // O2 (.99) and N1 (.01)
  send(4, 5, .01, 180010);
  for (int i = 0; i < 13; ++i)
    send(4, 99760 + i * 20, .35 + i * .05, 180010);
  send(next_counter ? 5 : 4, 5, .01, 180010); // completion still held
  recorder.finish();
  recorder.wait_idle();
  auto path = fs::path(recorder.status().at("last_bundle_path").get<std::string>());
  auto laps = Json::parse(read_file(path / "manifest.json"))["laps"];
  require(laps.size() == 2, "counter-first timer sequence closes two physical laps");
  require(laps[0]["time_ms"] == 180010, "counter-first finished time is retained");
  require(laps[1]["time_ms"] == 100000 && laps[1]["time_source"] == "current_lap_timer" &&
              laps[1]["valid"].is_null(),
          "next ACC lap peak excludes the stale counter-first crossing timer");
}

void test_acc_held_completion_stays_pending(const fs::path &root) {
  for (bool fresh_later : {true, false}) {
    Recorder recorder(root / ("acc-held-" + unique_id()), "all");
    auto send = [&](int lap, int timer, double position, int completed) {
      auto m = fixture_message("acc-held", 50, lap, timer, position);
      m["simulator"] = "ACC";
      m["telemetry"]["completed_lap_ms"] = completed;
      m["telemetry"]["lap_valid"] = false;
      recorder.record(m);
    };
    for (int i = 0; i < 13; ++i)
      send(3, 99260 + i * 20, .35 + i * .05, 100000);
    send(4, 5, .01, 100000);
    if (fresh_later)
      send(4, 25, .02, 99520);
    recorder.finish();
    recorder.wait_idle();
    auto path = fs::path(recorder.status().at("last_bundle_path").get<std::string>());
    auto laps = Json::parse(read_file(path / "manifest.json"))["laps"];
    require(laps.size() == 1, "held completion does not add a lap");
    if (fresh_later) {
      require(laps[0]["time_ms"] == 99520 && laps[0]["time_source"] == "simulator" &&
                  laps[0]["valid"] == false,
              "fresh ACC completion repairs held metadata within the old one-second tolerance");
    } else {
      require(laps[0]["time_ms"] == 100000 &&
                  laps[0]["time_source"] == "held_completed_time" && laps[0]["valid"] == false,
              "held ACC time stays provisional while the counter's completed-lap validity is retained");
    }
  }
}

void test_acc_provisional_completion_candidates(const fs::path &root) {
  // Known matching timer/completion pairs from the audited ACC recording,
  // plus a genuine repeated time. Never replace them with sample durations.
  const int times[][3] = {{125857, 125865, 90000}, {104365, 104372, 125865},
                          {175837, 175842, 104372}, {104365, 104372, 104372}};
  for (const auto &t : times) {
    Recorder recorder(root / ("acc-exact-" + unique_id()), "all");
    for (int i = 0; i < 13; ++i) {
      auto m = fixture_message("acc-exact", 50, 3, t[0] - (12 - i) * 20,
                               35 + i * 5);
      m["simulator"] = "ACC";
      m["telemetry"]["completed_lap_ms"] = t[2];
      recorder.record(m);
    }
    auto crossing = fixture_message("acc-exact", 50, 4, 3, .01);
    crossing["simulator"] = "ACC";
    crossing["telemetry"]["completed_lap_ms"] = t[1];
    crossing["telemetry"]["lap_valid"] = true;
    recorder.record(crossing);
    recorder.finish();
    recorder.wait_idle();
    auto path = fs::path(recorder.status().at("last_bundle_path").get<std::string>());
    auto manifest = Json::parse(read_file(path / "manifest.json"));
    require(manifest["laps"].size() == 1 && manifest["laps"][0]["time_ms"] == t[1],
            "ACC matching and repeated simulator millisecond values are preserved");
    require(manifest["laps"][0]["time_status"] == (t[1] == t[2] ? "provisional" : "inferred") &&
                manifest["laps"][0]["attribution"]["ranking_eligible"] == false,
            "numeric ACC candidates never gain authoritative ranking from equality or change");
    if (t[1] == t[2])
      require(manifest["laps"][0]["time_source"] == "held_completed_time" &&
                  manifest["laps"][0]["valid"] == true,
              "repeated time remains provisional without discarding the completed-lap wire validity");
    else
      require(manifest["laps"][0]["time_source"] == "simulator" &&
                  manifest["laps"][0]["valid"] == true,
              "changed ACC completion remains an inferred candidate, not authoritative");
  }
}

void test_acc_recovery_preserves_uncertainty(const fs::path &root) {
  const auto target=root / ("acc-recovery-" + unique_id());
  Json durable;
  {
    Recorder recorder(target,"all");
    for(int i=0;i<13;++i) {
      auto m=fixture_message("acc-recovery",50,3,99260+i*20,35+i*5);
      m["simulator"]="ACC"; m["telemetry"]["completed_lap_ms"]=100000;
      recorder.record(m);
    }
    auto m=fixture_message("acc-recovery",50,4,5,.01);
    m["simulator"]="ACC"; m["telemetry"]["completed_lap_ms"]=100000;
    m["telemetry"]["lap_valid"]=true;
    recorder.record(m); recorder.set_upload(false); recorder.wait_idle();
    for(const auto &entry:fs::directory_iterator(target))
      if(entry.path().filename().string().starts_with(".rapid-spool-"))
        durable=Json::parse(read_file(entry.path()/"spool.json"));
    require(durable["laps"][0]["time_status"]=="provisional" &&
            durable["laps"][0]["validity_status"]=="authoritative" &&
            durable.contains("acc_attribution"), "checkpoint independently persists time/validity provenance");
    // No finish: recovery publishes the durable prefix on the next start.
  }
  fs::path bundle;
  Recorder recovery(target,"all",[&](const fs::path &path){bundle=path;});
  require(!bundle.empty(),"unfinished checkpoint recovered");
  auto manifest=Json::parse(read_file(bundle/"manifest.json"));
  require(manifest["laps"]==durable["laps"] && manifest["acc_attribution"]==durable["acc_attribution"],
          "recovery cannot promote or discard unresolved lap provenance");
  require(fs::exists(bundle/"full-session.ldx"), "legacy all-lap LDX export remains available after recovery");
}

// --- Part B: AC1 delta -----------------------------------------------------

// Part B speaks the authenticated v4 wire (the v3 JSON transport is gone).
// The stream shape mirrors the old synthetic JSON frame, one sample per call.
// Schema 2 carries the delta-present flag and the completed lap's validity, so
// these tests cover both presence states and the manifest's per-lap `valid`.
struct DeltaStream {
  rapid::test::V4Stream stream;
  std::uint64_t time_us = 0;
  explicit DeltaStream(std::string run) : stream(std::move(run)) {}
  std::string metadata() {
    return stream.metadata(0, "Test Track", "Test Car", "Test Driver",
                           "Practice", "900");
  }
  // lap_valid: -1 absent, 0 invalid, 1 valid.
  std::string frame(int lap_number, double current_lap_ms,
                    double lap_position_pct, std::int32_t delta_ms = 0,
                    bool delta_present = false, int lap_valid = -1,
                    std::int32_t completed_lap_ms = 0) {
    time_us += 20000;
    const auto mask = rapid::test::v4_mask(
        {rapid::test::ch_throttle, rapid::test::ch_brake,
         rapid::test::ch_gear, rapid::test::ch_rpm,
         rapid::test::ch_steering, rapid::test::ch_speed,
         rapid::test::ch_g_x, rapid::test::ch_g_y, rapid::test::ch_g_z,
         rapid::test::ch_lap_number, rapid::test::ch_current_lap_ms,
         rapid::test::ch_lap_position});
    return stream.telemetry(
        time_us, mask,
        {{rapid::test::ch_rpm, 6000.f},
         {rapid::test::ch_steering, 0.f},
         {rapid::test::ch_g_x, 0.f},
         {rapid::test::ch_g_y, 0.f},
         {rapid::test::ch_g_z, 0.f},
         {rapid::test::ch_throttle, .8f},
         {rapid::test::ch_brake, 0.f},
         {rapid::test::ch_speed, 200.f},
         {rapid::test::ch_gear, 4.f},
         {rapid::test::ch_lap_number, float(lap_number)},
         {rapid::test::ch_current_lap_ms, float(current_lap_ms)},
         {rapid::test::ch_lap_position, float(lap_position_pct)}},
        completed_lap_ms, delta_ms, lap_valid, delta_present);
  }
};

// The former two-lap AC1 delta fixture replayed legacy v3 JSON frames with no
// delta field, which is how the runtime used to recognise "the sim did not
// provide a delta" and synthesise one. Schema 2 makes that presence explicit on
// the wire and the AC1 synthesis was deleted with #52; what remains to verify
// is that a present delta (including a real 0) stays a number and an absent one
// stays null.

// The authenticated flag identifies completed-lap validity independently of
// lastTime freshness. A held time must remain pending for later repair without
// discarding true/false (or inventing a value for an absent flag).
void test_acc_wire_validity_with_held_time(const fs::path &assets, const fs::path &root) {
  for (int valid : {-1, 0, 1}) {
    for (bool fresh_later : {false, true}) {
      const auto name = "acc-wire-held-" + unique_id();
      Config c;
      c.assets = assets;
      c.database = root / (name + ".db");
      c.telemetry = root / name;
      c.queue = root / (name + "-queue.db");
      c.companion_key = std::string(32, '\x11');
      Runtime runtime(c);
      DeltaStream delta(rapid::test::v4_run_id());
      delta.stream.simulator = 1;
      require(runtime.receive(delta.metadata(), "127.0.0.1"), "held-time ACC metadata accepted");
      for (int i = 0; i < 13; ++i)
        require(runtime.receive(delta.frame(3, 99260 + i * 20, 35 + i * 5,
                                           0, false, valid == 1 ? 0 : 1, 100000),
                                "127.0.0.1"), "held-time ACC sample accepted");
      require(runtime.receive(delta.frame(4, 5, .01, 0, false, valid, 100000),
                              "127.0.0.1"), "held-time ACC crossing accepted");
      if (fresh_later)
        require(runtime.receive(delta.frame(4, 25, .02, 0, false, valid, 99520),
                                "127.0.0.1"), "fresh ACC completion accepted");
      runtime.finish();
      auto bundle = runtime.snapshot()["last_bundle_path"].get<std::string>();
      auto laps = Json::parse(read_file(fs::path(bundle) / "manifest.json"))["laps"];
      require(laps.size() == 1 && laps[0]["end_sample"] == 13,
              "held/fresh wire metadata cannot add or move the physical lap boundary");
      const Json expected = valid < 0 ? Json() : Json(valid != 0);
      require(laps[0]["valid"] == expected,
              "authenticated completed-lap validity is independent of held lastTime");
      require(laps[0]["time_ms"] == (fresh_later ? 99520 : 100000) &&
                  laps[0]["time_source"] == (fresh_later ? "simulator" : "held_completed_time"),
              "wire validity must not finalize held time or prevent a fresh repair");
    }
  }
}

void test_acc_delta_untouched(const fs::path &assets, const fs::path &root) {
  Config c;
  c.assets = assets;
  c.database = root / "acc-delta.db";
  c.telemetry = root / "acc-delta-telemetry";
  c.queue = root / "acc-delta-queue.db";
  c.companion_key = std::string(32, '\x11');
  Runtime runtime(c);
  DeltaStream delta(rapid::test::v4_run_id());
  delta.stream.simulator = 1; // ACC
  require(runtime.receive(delta.metadata(), "127.0.0.1"),
          "ACC metadata accepted");
  require(runtime.receive(delta.frame(1, 40000, 50.0, -250, true), "127.0.0.1"),
          "ACC sample accepted");
  require(runtime.snapshot()["delta_ms"].get<int>() == -250,
          "ACC's own delta_ms is never overwritten (#20)");
  runtime.finish();
}

void test_delta_presence(const fs::path &assets, const fs::path &root) {
  Config c;
  c.assets = assets;
  c.database = root / "delta-presence.db";
  c.telemetry = root / "delta-presence-telemetry";
  c.queue = root / "delta-presence-queue.db";
  c.companion_key = std::string(32, '\x11');
  Runtime runtime(c);
  DeltaStream delta(rapid::test::v4_run_id());
  delta.stream.simulator = 2; // AC1: no delta in shared memory
  require(runtime.receive(delta.metadata(), "127.0.0.1"), "delta metadata");
  require(runtime.receive(delta.frame(1, 40000, 50.0, 0, true), "127.0.0.1"),
          "a real delta of 0 is present");
  require(runtime.snapshot()["delta_ms"].is_number_integer() &&
              runtime.snapshot()["delta_ms"].get<int>() == 0,
          "a real delta 0 stays a real 0, not blank");
  require(runtime.receive(delta.frame(1, 41000, 50.5), "127.0.0.1"),
          "an absent delta is accepted");
  require(runtime.snapshot()["delta_ms"].is_null(),
          "an absent delta is null, never a synthesised 0 (#52)");
  runtime.finish();
}

void test_lap_validity_manifest(const fs::path &root) {
  Recorder recorder(root / ("lap-valid-" + unique_id()), "all");
  auto message = [](int lap_number, int current_lap_ms, const Json &valid) {
    Json m = fixture_message("lap-valid-session", 50, lap_number,
                             current_lap_ms, 0.0);
    m["completed_lap_ms"] = 90000;
    if (!valid.is_null())
      m["telemetry"]["lap_valid"] = valid;
    return m;
  };
  recorder.record(message(1, 40000, true));
  // The shared lap-boundary rule debounces closes by 10 samples (#16), so
  // crossings that must each close a lap are spaced the way real laps are:
  // ten mid-lap samples in between. The pads carry no lap_valid, like a sim
  // with no signal, and must not close anything themselves.
  for (int i = 0; i < 10; ++i)
    recorder.record(message(1, 41000 + i * 1000, Json()));
  recorder.record(message(2, 100, true)); // closes lap 1, valid
  for (int i = 0; i < 10; ++i)
    recorder.record(message(2, 200 + i * 1000, Json()));
  recorder.record(message(3, 100, false)); // closes lap 2, invalid
  for (int i = 0; i < 10; ++i)
    recorder.record(message(3, 300 + i * 1000, Json()));
  recorder.record(message(4, 100, Json())); // closes lap 3, no signal
  recorder.finish();
  recorder.wait_idle();
  auto path =
      fs::path(recorder.status().at("last_bundle_path").get<std::string>());
  auto manifest = Json::parse(read_file(path / "manifest.json"));
  require(manifest["laps"].size() == 3,
          "validity fixtures close exactly three laps");
  require(manifest["laps"][0]["valid"] == true,
          "the manifest records a valid lap as valid (#16)");
  require(manifest["laps"][1]["valid"] == false,
          "the manifest records an invalid lap as invalid (#16)");
  require(manifest["laps"][2]["valid"].is_null(),
          "a lap with no lap-valid signal stays unknown, never invented");
}

// The ideal lap: the sum of the best three sector times. A sector only counts
// from a lap the simulator did not flag invalid, because a cut lap has
// impossibly fast sectors that would make the ideal lap unbeatable.
struct SectorLap {
  int s1, s2, s3;
};
// One lap of samples, as (position %, current_lap_ms). The sample at 34% is
// where sector 1 closes and the one at 67% is where sector 2 closes. The first
// sample of the next lap (lap number + 1) carries the finished lap's time and
// validity.
void drive_lap(Runtime &runtime, DeltaStream &delta, int lap, SectorLap t) {
  const std::pair<double, double> samples[] = {
      {2.0, 500},
      {10.0, t.s1 / 3.0},
      {20.0, t.s1 * 2.0 / 3.0},
      {34.0, t.s1},
      {50.0, t.s1 + t.s2 / 2.0},
      {67.0, t.s1 + t.s2},
      {85.0, t.s1 + t.s2 + t.s3 / 2.0},
      {95.0, t.s1 + t.s2 + t.s3 * 0.9},
      {96.0, t.s1 + t.s2 + t.s3 * 0.93},
      {98.0, t.s1 + t.s2 + t.s3 * 0.97},
  };
  for (const auto &[position, time] : samples)
    require(runtime.receive(delta.frame(lap, std::round(time), position),
                            "127.0.0.1"),
            "sample accepted");
}
void cross_line(Runtime &runtime, DeltaStream &delta, int next_lap,
                int completed_ms, int lap_valid) {
  // drive_lap sends ten samples, more than the boundary's ten-sample cooldown;
  // this is the sample that closes the lap.
  require(runtime.receive(delta.frame(next_lap, 300, 2.0, 0, false, lap_valid,
                                      completed_ms),
                          "127.0.0.1"),
          "lap-closing sample accepted");
}

void test_ideal_lap(const fs::path &assets, const fs::path &root) {
  Config c;
  c.assets = assets;
  c.database = root / "ideal.db";
  c.telemetry = root / "ideal-telemetry";
  c.queue = root / "ideal-queue.db";
  c.companion_key = std::string(32, '\x11');
  Runtime runtime(c);
  DeltaStream delta(rapid::test::v4_run_id());
  require(runtime.receive(delta.metadata(), "127.0.0.1"), "ideal metadata");
  const auto state = [&] { return runtime.snapshot(); };
  const char *best[] = {"best_sector_1_ms", "best_sector_2_ms",
                        "best_sector_3_ms"};

  require(state()["optimal_lap_ms"].is_null() && state()[best[0]].is_null(),
          "no ideal lap before any lap is timed");
  // Lap 1 is the first lap the panel sees, possibly joined mid-lap: it never
  // counts toward the ideal lap.
  drive_lap(runtime, delta, 1, {20000, 20000, 20000});
  cross_line(runtime, delta, 2, 60000, -1);
  require(state()["optimal_lap_ms"].is_null(),
          "the first, possibly partial, lap does not set the ideal lap");

  // Lap 2: validity unknown (AC1 provides none) counts.
  drive_lap(runtime, delta, 2, {30000, 31000, 30000});
  cross_line(runtime, delta, 3, 91000, -1);
  require(state()[best[0]] == 30000 && state()[best[1]] == 31000 &&
              state()[best[2]] == 30000,
          "the best sectors after a lap of unknown validity");
  require(state()["optimal_lap_ms"] == 91000,
          "the ideal lap is the sum of the best sectors");

  // Lap 3: valid, faster in sectors 1 and 3, slower in 2. Each sector keeps
  // its own best, so the ideal lap beats every real lap.
  drive_lap(runtime, delta, 3, {29000, 32000, 29500});
  cross_line(runtime, delta, 4, 90500, 1);
  require(state()[best[0]] == 29000 && state()[best[1]] == 31000 &&
              state()[best[2]] == 29500 && state()["optimal_lap_ms"] == 89500,
          "each sector keeps its own best across laps");
  require(state()["best_lap_ms"] == 90500, "the best real lap is unchanged");

  // Lap 4: flagged invalid, with impossible sectors (a cut). The live sector
  // times still show, but nothing from this lap enters the ideal lap.
  drive_lap(runtime, delta, 4, {25000, 25000, 25000});
  require(state()["sector_1_ms"] == 25000 && state()["sector_2_ms"] == 25000,
          "the live sector times still show an invalid lap");
  cross_line(runtime, delta, 5, 75000, 0);
  require(state()[best[0]] == 29000 && state()[best[1]] == 31000 &&
              state()[best[2]] == 29500 && state()["optimal_lap_ms"] == 89500,
          "an invalid lap never enters the ideal lap");

  // A new session starts over.
  DeltaStream next(rapid::test::v4_run_id());
  require(runtime.receive(next.metadata(), "127.0.0.1"), "second run metadata");
  // The runtime switches session on the new run's first telemetry sample.
  require(runtime.receive(next.frame(1, 1000, 5.0), "127.0.0.1"),
          "second run sample");
  require(state()["optimal_lap_ms"].is_null() && state()[best[0]].is_null() &&
              state()[best[1]].is_null() && state()[best[2]].is_null(),
          "a new session clears the best sectors and the ideal lap");
  runtime.finish();
}

void test_acc_uncertain_live_rankings(const fs::path &assets, const fs::path &root) {
  Config c;
  c.assets=assets; c.database=root / "acc-rank.db";
  c.telemetry=root / "acc-rank"; c.queue=root / "acc-rank-queue.db";
  c.companion_key=std::string(32, '\x11');
  Runtime runtime(c);
  DeltaStream delta(rapid::test::v4_run_id()); delta.stream.simulator=1;
  require(runtime.receive(delta.metadata(), "127.0.0.1"), "ACC ranking metadata");
  drive_lap(runtime,delta,1,{20000,20000,20000}); cross_line(runtime,delta,2,60000,1);
  drive_lap(runtime,delta,2,{30000,31000,30000}); cross_line(runtime,delta,3,60000,1);
  auto state=runtime.snapshot();
  require(state["best_lap_ms"].is_null() && state["optimal_lap_ms"].is_null() &&
          state["best_sector_3_ms"].is_null(),
          "uncertain ACC held completion cannot enter live best or IDEAL");
  runtime.finish();
}

// AC1 reports a physical line crossing in two steps a sample apart: the lap
// number ticks (and the lap timer restarts) while the track position still
// reads the end of the lap, then the position wraps. The first sample of the
// new lap therefore has a position past a third of the lap, and the sector
// split used to fire on it: a "first sector" of a few milliseconds, which then
// set the best first sector, made every later S1 delta huge, and made the ideal
// lap tens of seconds short.
void test_sectors_when_position_wraps_late(const fs::path &assets,
                                           const fs::path &root) {
  Config c;
  c.assets = assets;
  c.database = root / "late-wrap.db";
  c.telemetry = root / "late-wrap-telemetry";
  c.queue = root / "late-wrap-queue.db";
  c.companion_key = std::string(32, '\x11');
  Runtime runtime(c);
  DeltaStream delta(rapid::test::v4_run_id());
  require(runtime.receive(delta.metadata(), "127.0.0.1"), "late-wrap metadata");
  const auto state = [&] { return runtime.snapshot(); };

  drive_lap(runtime, delta, 1, {20000, 20000, 20000});
  cross_line(runtime, delta, 2, 60000, -1);
  drive_lap(runtime, delta, 2, {30000, 31000, 30000});
  // The crossing into lap 3 as the owner's recordings show it (session
  // 38f740e4: the start line sits at about 99.6% of the track position): the
  // lap number ticks and the timer is at a few milliseconds, but the position
  // reads ~99.6% and only wraps to 0 about 0.66 s (33 samples) later.
  require(runtime.receive(delta.frame(3, 22, 99.627, 0, false, -1, 91000),
                          "127.0.0.1"),
          "late-wrap crossing sample accepted");
  require(state()["sector_1_ms"] == 30000 && state()["sector_2_ms"] == 31000,
          "a position that has not wrapped yet is not a sector split");
  for (int k = 1; k <= 33; ++k)
    require(runtime.receive(delta.frame(3, 22 + 20 * k, 99.627 + 0.011 * k),
                            "127.0.0.1"),
            "position still near the end of the lap accepted");
  require(state()["sector_1_ms"] == 30000 && state()["sector_2_ms"] == 31000,
          "no split while the position has not wrapped");
  // Lap 3 then runs normally from the wrap on.
  drive_lap(runtime, delta, 3, {29000, 32000, 29500});
  cross_line(runtime, delta, 4, 90500, 1);
  require(state()["best_sector_1_ms"] == 29000 &&
              state()["best_sector_2_ms"] == 31000 &&
              state()["best_sector_3_ms"] == 29500 &&
              state()["optimal_lap_ms"] == 89500,
          "the ideal lap is built from real sectors after a late wrap");
  require(state()["sector_1_ms"] == 29000 && state()["sector_2_ms"] == 32000 &&
              state()["sector_3_ms"] == 29500,
          "the live sector times of the lap after a late wrap are its own");
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

    test_acc_uncertain_live_rankings(assets, root);
    test_acc_wire_validity_with_held_time(assets, root);
    test_lap_boundary_number_increment();
    test_lap_boundary_wrap_without_number_increment();
    test_lap_boundary_reversal_does_not_double_close();
    test_lap_boundary_debounces_split_signal();
    test_lap_boundary_debounces_late_counter();
    test_lap_boundary_counter_catchup_after_gap();
    test_acc_validity_at_physical_wrap();
    test_acc_validity_counter_before_timer_reset();
    test_session_c28a_lap6_wrap(fixtures, root);
    test_session_4e6de929_six_laps(fixtures, root);
    test_acc_delayed_completed_metadata(root);
    test_acc_unresolved_metadata_uses_sim_timer(root);
    test_acc_validity_tracker_to_recorder(root);
    test_acc_held_completion_stays_pending(root);
    test_acc_counter_before_timer_reset(root);
    test_acc_counter_before_timer_reset(root,.01,true);
    test_acc_provisional_completion_candidates(root);
    test_acc_recovery_preserves_uncertainty(root);
    test_acc_delta_untouched(assets, root);
    test_delta_presence(assets, root);
    test_lap_validity_manifest(root);
    test_ideal_lap(assets, root);
    test_sectors_when_position_wraps_late(assets, root);

    std::cout << "Lap boundary: unit rules, real-recording regression "
                 "fixtures, ACC delta passthrough, delta presence and the "
                 "per-lap manifest validity passed\n"
                 "Evidence: "
              << root << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
