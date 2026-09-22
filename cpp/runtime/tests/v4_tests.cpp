#include "rapid/native.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <openssl/hmac.h>
#include <thread>

using namespace rapid::native;
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
std::string fixture(const fs::path &root, const char *name) {
  auto hex = read_file(root / name);
  std::string bytes;
  while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r'))
    hex.pop_back();
  require(hex.size() % 2 == 0, "fixture hex length");
  for (std::size_t i = 0; i < hex.size(); i += 2)
    bytes += char(std::stoul(hex.substr(i, 2), nullptr, 16));
  return bytes;
}
void resign(std::string &bytes, char key_byte = '\x11') {
  std::string key(32, key_byte);
  unsigned char hash[32];
  unsigned int size = 0;
  require(HMAC(EVP_sha256(), key.data(), 32,
               reinterpret_cast<const unsigned char *>(bytes.data()),
               bytes.size() - 32, hash, &size) &&
              size == 32,
          "sign test packet");
  bytes.replace(bytes.size() - 32, 32, reinterpret_cast<char *>(hash), 32);
}
int main(int argc, char **argv) {
  try {
    require(argc == 3, "assets and Windows fixture paths required");
    const auto root = fs::temp_directory_path() / ("rapid-v4-" + unique_id());
    fs::create_directories(root);
    Config c;
    c.assets = argv[1];
    c.database = root / "state.db";
    c.telemetry = root / "telemetry";
    c.queue = root / "queue.db";
    c.companion_key = telemetry_key(std::string(64, '1'));
    const auto metadata = fixture(argv[2], "metadata.hex"),
               telemetry = fixture(argv[2], "telemetry.hex"),
               driving = fixture(argv[2], "driving.hex"),
               paused = fixture(argv[2], "paused.hex"),
               next = fixture(argv[2], "next.hex"),
               ended = fixture(argv[2], "ended.hex"),
               ready = fixture(argv[2], "ready.hex"),
               ac_metadata = fixture(argv[2], "ac-metadata.hex"),
               ac_telemetry = fixture(argv[2], "ac-telemetry.hex"),
               delta_zero = fixture(argv[2], "delta-zero.hex"),
               delta_absent = fixture(argv[2], "delta-absent.hex"),
               lap_valid_true = fixture(argv[2], "lap-valid-true.hex"),
               lap_valid_false = fixture(argv[2], "lap-valid-false.hex"),
               lap_valid_unknown = fixture(argv[2], "lap-valid-unknown.hex");
    {
      Runtime r(c);
      // The retired v3 JSON transport must be gone, not quietly accepted: a
      // v3-shaped datagram is rejected and counted exactly like malformed v4.
      require(!r.receive(
                  "{\"version\":3,\"type\":\"status\",\"state\":\"waiting\"}",
                  "127.0.0.1"),
              "no v3 downgrade");
      require(r.snapshot()["packets_invalid"] == 1,
              "a v3-shaped JSON packet is rejected and counted");
      auto corrupt = metadata;
      corrupt.back() ^= 1;
      require(!r.receive(corrupt, "127.0.0.1"), "bad HMAC rejected");
      require(!r.receive(metadata.substr(0, metadata.size() - 1), "127.0.0.1"),
              "truncation rejected");
      require(!r.receive(telemetry, "127.0.0.1"), "metadata required");
      auto bad = metadata;
      bad[12] = 1;
      resign(bad);
      require(!r.receive(bad, "127.0.0.1"), "signed old-schema packet rejected");
      require(r.receive(metadata, "127.0.0.1"),
              "Windows metadata accepted after bad packets");
      require(!r.receive(metadata, "127.0.0.1"), "metadata replay rejected");
      bad = telemetry;
      bad[60] = 0;
      bad[68 + 5 * 4] = 0;
      bad[69 + 5 * 4] = 0;
      bad[70 + 5 * 4] = char(0x80);
      bad[71 + 5 * 4] = char(0x7f);
      resign(bad);
      require(!r.receive(bad, "127.0.0.1"),
              "nonfinite primary channel rejected");
      bad = telemetry;
      bad[52] &= char(~(1 << 5));
      resign(bad);
      require(!r.receive(bad, "127.0.0.1"),
              "missing primary validity rejected");
      bad = telemetry;
      bad[58] = 1;
      resign(bad);
      require(!r.receive(bad, "127.0.0.1"), "unknown validity bit rejected");
      bad = telemetry;
      bad[7] = char(0x20 | (bad[7] & 0x1f));
      resign(bad);
      require(!r.receive(bad, "127.0.0.1"), "unknown telemetry flag bit rejected");
      bad = telemetry;
      bad[10] = 0;
      resign(bad);
      require(!r.receive(bad, "127.0.0.1"), "signed invalid length rejected");
      require(r.receive(telemetry, "127.0.0.1"), "Windows telemetry accepted");
      auto state = r.snapshot();
      require(state["schema_version"] == 4 && state["rpm"] == 6500 &&
                  state["gear"] == 4,
              "v4 dashboard channels");
      require(state["delta_ms"] == -125,
              "a present wire delta stays a number, including the sign");
      require(std::abs(number(state, "throttle") - .8) < 1e-6 &&
                  state["car_model"] == "V4 Car",
              "pedals and metadata");
      require(std::abs(number(state, "steering_angle") - (-.3)) < 1e-6,
              "steering channel is normalised, not degrees or radians");
      require(state["steering_lock_deg"] == 900,
              "known steering lock parsed from v4 metadata");
      require(state["recording"] == true && state["recorded_samples"] == 1,
              "v4 recording active");
      require(state["sender_lag_ms"]["p50"].is_number() &&
                  state["sender_lag_ms"]["p95"].is_number() &&
                  state["sender_lag_ms"]["p95"] >= state["sender_lag_ms"]["p50"] &&
                  state["process_ms"]["p50"].is_number() &&
                  state["process_ms"]["p95"].is_number(),
              "v4 latency percentiles exposed");
      require(r.receive(driving, "127.0.0.1"),
              "authenticated driving heartbeat");
      require(r.receive(paused, "127.0.0.1"),
              "authenticated paused heartbeat (#15)");
      require(r.snapshot()["recording"] == true &&
                  r.snapshot()["session_active"] == true &&
                  r.snapshot()["companion_daemon_state"] == "paused" &&
                  r.snapshot()["connected"] == true,
              "a paused heartbeat keeps the spool/session open and is "
              "distinguishable from idle (#15)");
      require(r.receive(next, "127.0.0.1"), "telemetry after paused heartbeat");
      require(r.snapshot()["recording"] == true &&
                  r.snapshot()["recorded_samples"] == 2,
              "live telemetry resuming after a pause continues the same "
              "recording rather than starting a new one (#15)");
      require(r.snapshot()["packets_lost"] == 0,
              "metadata/heartbeat/paused do not count as lost telemetry");
      require(!r.receive(telemetry, "127.0.0.1"), "telemetry replay rejected");
      require(r.receive(ended, "127.0.0.1"), "end marker accepted");
      require(r.snapshot()["recording"] == false,
              "end marker finalizes recording");
      // Publication runs on the recorder's writer thread, off the receive
      // path (#17); it must complete promptly.
      Json bundle;
      for (int i = 0; i < 100 && !bundle.is_string(); ++i) {
        bundle = r.snapshot()["last_bundle_path"];
        if (!bundle.is_string())
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      require(bundle.is_string(), "end marker publishes the bundle");
      auto manifest = Json::parse(
          read_file(fs::path(bundle.get<std::string>()) / "manifest.json"));
      require(manifest["quality"]["recorded_samples"] == 2,
              "v4 samples exported");
      require(r.receive(ready, "127.0.0.1"), "new authenticated idle stream");
    }
    // AC1 exposes no static steering-lock field, so its wire metadata carries
    // no lock text -- the receiver must report that explicitly as unknown
    // rather than inventing a value or reusing the previous simulator's lock.
    {
      Config ac = c;
      ac.database = root / "ac.db";
      ac.telemetry = root / "ac";
      Runtime r(ac);
      require(r.receive(ac_metadata, "127.0.0.1"), "AC1 metadata accepted");
      require(r.snapshot()["steering_lock_deg"].is_null(),
              "unknown steering lock is null, not a guessed default");
      require(r.receive(ac_telemetry, "127.0.0.1"), "AC1 telemetry accepted");
      require(std::abs(number(r.snapshot(), "steering_angle") - (-.4)) < 1e-6,
              "AC1 steering channel is already normalised, unchanged by receipt");
      // AC1's shared-memory page exposes no delta, so the schema-2 wire says
      // "absent" and the Pi must show blank rather than 0 (#52).
      require(r.snapshot()["delta_ms"].is_null(),
              "AC1's absent delta is null, not 0 (#52)");
    }
    // v4 schema 2 delta presence: a real 0 stays a number; an absent delta
    // stays null, so the dashboard never shows an invented 0 (#20/#52).
    {
      Config deltas = c;
      deltas.database = root / "delta.db";
      deltas.telemetry = root / "delta";
      Runtime r(deltas);
      require(r.receive(metadata, "127.0.0.1") &&
                  r.receive(delta_zero, "127.0.0.1"),
              "delta-present packet accepted");
      require(r.snapshot()["delta_ms"].is_number_integer() &&
                  r.snapshot()["delta_ms"].get<int>() == 0,
              "a real delta 0 is carried as 0, not blank");
      require(r.receive(delta_absent, "127.0.0.1"),
              "delta-absent packet accepted");
      require(r.snapshot()["delta_ms"].is_null(),
              "an absent delta is null, not a fabricated 0 (#52)");
    }
    // v4 schema 2 lap validity: the crossing sample carries the completed
    // lap's validity and the manifest records it per lap (#16). The real
    // fixture packets were produced by one companion run, so each expected
    // validity is replayed as the crossing sample of its own fresh run: a
    // single crossing needs no room for the recorder's 10-sample debounce,
    // and every byte the runtime reads is exactly what the Windows encoder
    // signed. The multi-lap sequence is covered by the synthetic streams in
    // rapid-lap-boundary-tests.
    auto check_lap_validity = [&](const std::string &name,
                                  const std::string &crossing,
                                  const Json &expected) {
      Config laps = c;
      laps.database = root / (name + ".db");
      laps.telemetry = root / name;
      Runtime r(laps);
      require(r.receive(metadata, "127.0.0.1") &&
                  r.receive(telemetry, "127.0.0.1") &&
                  r.receive(crossing, "127.0.0.1"),
              (name + " stream accepted").c_str());
      r.finish();
      auto bundle = r.snapshot()["last_bundle_path"];
      require(bundle.is_string(), (name + " run published").c_str());
      auto manifest = Json::parse(
          read_file(fs::path(bundle.get<std::string>()) / "manifest.json"));
      require(manifest["laps"].size() == 1,
              (name + " records exactly the completed lap").c_str());
      require(manifest["laps"][0]["valid"] == expected,
              (name + " records the wire validity in the manifest").c_str());
    };
    check_lap_validity("lap-valid-true", lap_valid_true, true);
    check_lap_validity("lap-valid-false", lap_valid_false, false);
    check_lap_validity("lap-valid-unknown", lap_valid_unknown, Json());
    // Lost control packets must not create invented telemetry samples. Missing
    // optional channels must clear retained readings when their validity
    // clears.
    {
      Config gaps = c;
      gaps.database = root / "gaps.db";
      gaps.telemetry = root / "gaps";
      Runtime r(gaps);
      require(r.receive(metadata, "127.0.0.1") &&
                  r.receive(telemetry, "127.0.0.1"),
              "gap setup");
      auto sparse = next;
      sparse[52] &= char(~((1 << 1) | (1 << 2)));
      resign(sparse);
      require(r.receive(sparse, "127.0.0.1"),
              "optional channels may be absent");
      auto state = r.snapshot();
      require(state["throttle"].is_null() && state["brake"].is_null(),
              "no retained invalid pedals");
      // This block never receives driving.hex or paused.hex, so "sparse"
      // (built from next.hex) has a two-packet gap in the wire sequence.
      require(state["packets_lost"] == 2 && state["recorded_samples"] == 2,
              "wire gap does not synthesize samples");
      require(r.receive(ready, "127.0.0.1"),
              "idle retires active stream even if end packet was lost");
      require(!r.receive(ended, "127.0.0.1"),
              "delayed retired stream rejected");
    }
    {
      Runtime restarted(c);
      require(!restarted.receive(metadata, "127.0.0.1"),
              "old run rejected after restart");
      require(!restarted.receive(ready, "127.0.0.1"),
              "control replay rejected after restart");
      auto resumed = ready;
      resumed[36] = 1;
      resign(resumed);
      require(restarted.receive(resumed, "127.0.0.1"),
              "fresh control sequence after restart");
    }
    c.database = root / "wrong-key.db";
    c.telemetry = root / "wrong-key-telemetry";
    c.companion_key = std::string(32, '\x22');
    Runtime wrong(c);
    require(!wrong.receive(metadata, "127.0.0.1"), "wrong key rejected");
    require(wrong.snapshot()["packets_auth_failed"] == 1,
            "authentication counter");
    Config paired = c;
    paired.database = root / "paired-keys.db";
    paired.telemetry = root / "paired-keys-telemetry";
    paired.companion_keys = {telemetry_key(std::string(64, '1')),
                             telemetry_key(std::string(64, '4'))};
    paired.paired_key_mode = true;
    Runtime paired_runtime(paired);
    require(paired_runtime.receive(metadata, "127.0.0.1"),
            "one paired key accepts its own Windows v4 packet");
    auto same_run_other_peer = metadata;
    resign(same_run_other_peer, '\x44');
    require(paired_runtime.receive(same_run_other_peer, "127.0.0.1"),
            "different paired key has independent replay watermark");
    const auto paired_session =
        paired_runtime.snapshot()["session_id"].get<std::string>();
    require(paired_session.size() == 32 &&
                paired_session.find(':') == std::string::npos,
            "paired v4 session ID remains the wire run ID");
    paired_runtime.replace_paired_keys({});
    require(!paired_runtime.receive(metadata, "127.0.0.1"),
            "revoking every paired key rejects future packets without fallback");
    require(!paired_runtime.receive(
                "{\"version\":3,\"type\":\"status\",\"state\":\"waiting\"}",
                "127.0.0.1"),
            "revoked paired mode rejects unauthenticated v3 fallback");
    std::cout << "Windows v4 fixtures: authentication, state, recording, "
                 "paused/not-live gap (#15), replay persistence passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
