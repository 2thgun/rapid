#include "rapid/native.hpp"
#include <cmath>
#include <iostream>
#include <openssl/hmac.h>

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
void resign(std::string &bytes) {
  std::string key(32, '\x11');
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
               next = fixture(argv[2], "next.hex"),
               ended = fixture(argv[2], "ended.hex"),
               ready = fixture(argv[2], "ready.hex");
    {
      Runtime r(c);
      require(!r.receive(
                  "{\"version\":3,\"type\":\"status\",\"state\":\"waiting\"}",
                  "127.0.0.1"),
              "no v3 downgrade");
      auto corrupt = metadata;
      corrupt.back() ^= 1;
      require(!r.receive(corrupt, "127.0.0.1"), "bad HMAC rejected");
      require(!r.receive(metadata.substr(0, metadata.size() - 1), "127.0.0.1"),
              "truncation rejected");
      require(!r.receive(telemetry, "127.0.0.1"), "metadata required");
      auto bad = metadata;
      bad[12] = 2;
      resign(bad);
      require(!r.receive(bad, "127.0.0.1"), "signed invalid schema rejected");
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
      bad[10] = 0;
      resign(bad);
      require(!r.receive(bad, "127.0.0.1"), "signed invalid length rejected");
      require(r.receive(telemetry, "127.0.0.1"), "Windows telemetry accepted");
      auto state = r.snapshot();
      require(state["schema_version"] == 4 && state["rpm"] == 6500 &&
                  state["gear"] == 4,
              "v4 dashboard channels");
      require(std::abs(number(state, "throttle") - .8) < 1e-6 &&
                  state["car_model"] == "V4 Car",
              "pedals and metadata");
      require(state["recording"] == true && state["recorded_samples"] == 1,
              "v4 recording active");
      require(r.receive(driving, "127.0.0.1"),
              "authenticated driving heartbeat");
      require(r.receive(next, "127.0.0.1"), "telemetry after heartbeat");
      require(r.snapshot()["packets_lost"] == 0,
              "metadata/heartbeat do not count as lost telemetry");
      require(!r.receive(telemetry, "127.0.0.1"), "telemetry replay rejected");
      require(r.receive(ended, "127.0.0.1"), "end marker accepted");
      require(r.snapshot()["recording"] == false,
              "end marker finalizes recording");
      auto manifest = Json::parse(read_file(
          fs::path(r.snapshot()["last_bundle_path"].get<std::string>()) /
          "manifest.json"));
      require(manifest["quality"]["recorded_samples"] == 2,
              "v4 samples exported");
      require(r.receive(ready, "127.0.0.1"), "new authenticated idle stream");
    }
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
      require(state["packets_lost"] == 1 && state["recorded_samples"] == 2,
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
    std::cout << "Windows v4 fixtures: authentication, state, recording, "
                 "replay persistence passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
