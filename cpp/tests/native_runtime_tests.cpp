#include "rapid/native.hpp"
#include <bit>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace rapid::native;
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
Json sample(int sequence = 0, int lap = 1) {
  return {{"version", 3},
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
    Runtime runtime(c);
    require(!runtime.receive("[]", "127.0.0.1"), "non-object rejected");
    auto bad = sample();
    bad["telemetry"]["rpm"] = true;
    require(!runtime.receive(bad.dump(), "127.0.0.1"), "boolean RPM rejected");
    require(runtime.receive(sample().dump(), "127.0.0.1"),
            "valid telemetry after malformed");
    require(!runtime.receive(sample().dump(), "127.0.0.1"), "replay rejected");
    require(!runtime.receive(sample(1).dump(), "192.168.1.5"),
            "sender pinning");
    for (int i = 1; i < 10; ++i)
      require(runtime.receive(sample(i).dump(), "127.0.0.1"), "session sample");
    require(runtime.receive(sample(10, 2).dump(), "127.0.0.1"),
            "lap transition");
    require(runtime.snapshot()["recording"] == true, "recording state wired");
    require(runtime.snapshot()["recorded_samples"] == 11, "sample count wired");
    std::uint64_t cursor = 0;
    require(runtime.events(cursor)["events"].size() == 11, "live broker");
    runtime.upload(false);
    runtime.finish();
    auto path =
        fs::path(runtime.snapshot().at("last_bundle_path").get<std::string>());
    auto manifest = Json::parse(read_file(path / "manifest.json"));
    require(manifest["laps"].size() == 1, "completed lap exported");
    require(manifest["metadata"]["upload_after_session"] == false,
            "manual upload choice persisted");
    auto legacy = sample(0);
    legacy.erase("session_id");
    legacy.erase("sequence");
    legacy.erase("monotonic_us");
    legacy["telemetry"]["abs"] = 0.25;
    legacy["telemetry"]["companion_connected"] = false;
    require(runtime.receive(legacy.dump(), "127.0.0.1"),
            "current v3 companion without envelope identity");
    auto legacy_state = runtime.snapshot();
    auto legacy_id = legacy_state["session_id"];
    require(legacy_id.is_string() && !legacy_id.get<std::string>().empty(),
            "generated session identity");
    require(legacy_state["last_monotonic_us"].is_number_unsigned(),
            "generated chart timestamp");
    require(legacy_state["companion_connected"] == true &&
                legacy_state["abs_activity"] == 0.25,
            "status protected and ABS mapped");
    require(runtime.receive(legacy.dump(), "127.0.0.1"),
            "subsequent legacy sample");
    require(runtime.snapshot()["session_id"] == legacy_id,
            "legacy session identity stable");
    auto invalid_optional = legacy;
    invalid_optional["telemetry"]["fuel"] = "invalid";
    require(!runtime.receive(invalid_optional.dump(), "127.0.0.1"),
            "invalid optional channel type");
    require(runtime.receive(Json{{"version", 3},
                                 {"type", "status"},
                                 {"state", "waiting"},
                                 {"simulator", nullptr}}
                                .dump(),
                            "127.0.0.1"),
            "legacy session ended");
    require(runtime.receive(legacy.dump(), "127.0.0.1"),
            "legacy driving resumed");
    require(runtime.snapshot()["session_id"] != legacy_id,
            "legacy session identity resets after waiting");
    runtime.finish();
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
    fs::path spool;
    for (const auto &entry : fs::directory_iterator(failure))
      if (entry.path().filename().string().starts_with(".rapid-spool-"))
        spool = entry.path();
    auto channel = spool / "01.bin";
    fs::rename(channel, spool / "01.bin.old");
    bool failed = false;
    try {
      recorder.finish();
    } catch (...) {
      failed = true;
    }
    require(failed && fs::exists(spool / "spool.json"),
            "publication failure preserves spool");
    fs::rename(spool / "01.bin.old", channel);
    recorder.finish();
    std::cout << "Native runtime: validation, source pinning, replay, state, "
                 "broker, LD, lap, hashes, crash recovery and publication "
                 "retry passed\nEvidence: "
              << root << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
