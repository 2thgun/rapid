// Latency measurement tests (#17 fix round, wave 2): sender_lag_ms and
// process_ms/lock_wait_ms in native_runtime.cpp/native_v4.cpp.
//
// Two reviewers (o10-integrate, o8-io) found the step-3 fields (1c6bd95) did
// not measure what #17's pass criterion needs:
//   - sender_lag_ms subtracted the companion's microseconds-since-run-start
//     (v4 monotonic_us) from the Pi's own steady_clock, two unrelated clock
//     origins, so the value carried an arbitrary offset (roughly Pi uptime).
//   - process_ms started timing only after the runtime lock was already
//     held, and was read out before the recorder ran, so it excluded both
//     lock wait and the storage handoff.
//
// These tests inject the two clocks that matter directly: Runtime::receive's
// receipt_monotonic parameter stands in for udp_loop's recvfrom time, and the
// v4 packet's own `time` header field carries the sender's monotonic_us. That
// makes the sender-lag cases fully deterministic without depending on real
// wall-clock timing. The lock-wait case is the exception: it needs genuine
// concurrent access to Runtime's own mutex_, which is only observable through
// real threads.
#include "rapid/native.hpp"
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <openssl/hmac.h>
#include <thread>

using namespace rapid::native;
namespace {
void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
std::string random_run() {
  auto hex = unique_id();
  std::string raw;
  for (std::size_t i = 0; i < 32; i += 2)
    raw += char(std::stoul(hex.substr(i, 2), nullptr, 16));
  return raw;
}

// Minimal v4 packet builder (byte layout as parsed by native_v4.cpp's
// decode_v4), parameterised on the sender's monotonic microseconds so tests
// control the sender clock exactly, independent of when the test runs.
struct Stream {
  std::string key = std::string(32, '\x11'), run;
  int rate = 50;
  std::uint64_t sequence = 0;
  explicit Stream(std::string run_id) : run(std::move(run_id)) {}
  static void put(std::string &b, std::size_t at, std::uint64_t v,
                  std::size_t size) {
    for (std::size_t i = 0; i < size; ++i)
      b[at + i] = char((v >> (8 * i)) & 255);
  }
  std::string header(int type, int flags, std::size_t end,
                     std::uint64_t time_us) {
    std::string b(end + 32, '\0');
    b.replace(0, 4, "RPD4");
    put(b, 4, 4, 1);
    put(b, 5, type, 1);
    put(b, 6, 2, 1); // AC
    put(b, 7, flags, 1);
    put(b, 8, 52, 2);
    put(b, 10, end - 52, 2);
    put(b, 12, 2, 2); // schema 2: lap-validity/delta-presence revision
    put(b, 14, 48, 2);
    put(b, 16, rate, 2);
    put(b, 18, 0, 2);
    b.replace(20, 16, run);
    put(b, 36, ++sequence, 8);
    put(b, 44, time_us, 8);
    return b;
  }
  std::string sign(std::string b) const {
    unsigned char mac[32];
    unsigned int size = 0;
    HMAC(EVP_sha256(), key.data(), 32,
         reinterpret_cast<const unsigned char *>(b.data()), b.size() - 32, mac,
         &size);
    b.replace(b.size() - 32, 32, reinterpret_cast<char *>(mac), 32);
    return b;
  }
  std::string metadata(std::uint64_t time_us) {
    std::string text;
    for (const char *s :
         {"Test Track", "Test Car", "Test Driver", "Practice", "900"}) {
      text += char(std::strlen(s) & 255);
      text += char(0);
      text += s;
    }
    auto b = header(2, 1, 52 + text.size(), time_us);
    b.replace(52, text.size(), text);
    return sign(b);
  }
  std::string telemetry(std::uint64_t time_us) {
    auto b = header(1, 1, 260, time_us);
    std::uint64_t mask = (1ULL << 1) | (1ULL << 4) | (1ULL << 5) |
                         (1ULL << 6) | (1ULL << 11) | (1ULL << 12) |
                         (1ULL << 13) | (1ULL << 45) | (1ULL << 46) |
                         (1ULL << 47);
    put(b, 52, mask, 8);
    auto channel = [&](int i, float v) {
      put(b, 68 + 4 * i, std::bit_cast<std::uint32_t>(v), 4);
    };
    channel(1, .5f);
    channel(4, 3);
    channel(5, 3000.f);
    channel(6, .1f);
    channel(45, 1);
    channel(46, 0.f);
    channel(47, 0.f);
    return sign(b);
  }
};

struct Fixture {
  fs::path root;
  Config config;
  explicit Fixture(const fs::path &assets, const std::string &name) {
    root = fs::temp_directory_path() / ("rapid-latency-" + name + "-" + unique_id());
    fs::create_directories(root);
    config.assets = assets;
    config.database = root / "state.db";
    config.telemetry = root / "telemetry";
    config.queue = root / "queue.db";
    config.companion_key = std::string(32, '\x11');
  }
};

double p95(const Json &field) { return field["p95"].get<double>(); }

// sender_lag_ms: constant clock offset (lag ~= 0), an added delay on top of
// that same offset (lag ~= delay), and a new stream resetting the baseline.
int sender_lag(const fs::path &assets) {
  Fixture f(assets, "sender-lag");
  Runtime r(f.config);
  Stream a(random_run());
  constexpr double sender_start = 10.0, rate_s = 0.02 /* 50 Hz */,
                   offset_a = 3.0 /* arbitrary constant clock difference */;
  // Metadata, then a run of telemetry with exactly the same offset every
  // time: no queueing, just two unrelated monotonic epochs. Excess delay
  // above the (so far constant) baseline should read as ~0.
  double sender = sender_start;
  require(r.receive(a.metadata(std::uint64_t(sender * 1e6)), "127.0.0.1",
                    sender + offset_a),
          "stream A metadata accepted");
  for (int i = 0; i < 30; ++i) {
    sender += rate_s;
    require(r.receive(a.telemetry(std::uint64_t(sender * 1e6)), "127.0.0.1",
                      sender + offset_a),
            "stream A constant-offset telemetry accepted");
  }
  auto state = r.snapshot();
  require(state["sender_lag_ms"]["current"].get<double>() < 0.05 &&
              p95(state["sender_lag_ms"]) < 0.05,
          "a constant clock offset alone reads as ~0 lag, not the offset "
          "itself (" + state["sender_lag_ms"].dump() + ")");
  // Now the same stream develops a steady extra delay (e.g. a queueing
  // backlog): every packet from here arrives `delay` late on top of the same
  // clock offset. The baseline (set above) does not move, so lag should
  // read as ~= delay, both as the latest value and once it dominates the
  // window.
  constexpr double delay = 0.050; // 50 ms
  for (int i = 0; i < 30; ++i) {
    sender += rate_s;
    require(r.receive(a.telemetry(std::uint64_t(sender * 1e6)), "127.0.0.1",
                      sender + offset_a + delay),
            "stream A delayed telemetry accepted");
  }
  state = r.snapshot();
  const double current = state["sender_lag_ms"]["current"].get<double>();
  require(std::abs(current - delay * 1000) < 2.0,
          "an added delay on top of the same offset reads as that delay, "
          "not ~0 or the raw offset (current=" + std::to_string(current) +
              " ms, expected ~" + std::to_string(delay * 1000) + " ms)");
  require(std::abs(p95(state["sender_lag_ms"]) - delay * 1000) < 2.0,
          "p95 also converges on the added delay once it dominates the "
          "window (p95=" + std::to_string(p95(state["sender_lag_ms"])) +
              " ms)");
  // A new stream (new v4 run ID) with a totally different, much larger
  // constant offset (a plausible companion restart: a new process, new
  // uptime-based epoch). Its first packets must not inherit stream A's
  // baseline/lag history -- the baseline resets and lag reads as ~0 again,
  // not the (very large) apparent jump between the two unrelated epochs.
  Stream b(random_run());
  constexpr double offset_b = 5000.0;
  double sender_b = 20.0;
  require(r.receive(b.metadata(std::uint64_t(sender_b * 1e6)), "127.0.0.1",
                    sender_b + offset_b),
          "stream B metadata accepted (retires stream A)");
  state = r.snapshot();
  require(state["sender_lag_ms"]["current"].get<double>() < 0.05,
          "a new stream resets the baseline instead of reporting the huge "
          "apparent offset jump between two unrelated streams (current=" +
              state["sender_lag_ms"]["current"].dump() + ")");
  for (int i = 0; i < 5; ++i) {
    sender_b += rate_s;
    require(r.receive(b.telemetry(std::uint64_t(sender_b * 1e6)), "127.0.0.1",
                      sender_b + offset_b),
            "stream B telemetry accepted");
  }
  state = r.snapshot();
  require(state["sender_lag_ms"]["current"].get<double>() < 0.05 &&
              p95(state["sender_lag_ms"]) < 0.05,
          "stream B's own constant offset continues to read as ~0 lag once "
          "its baseline is established");
  std::cout << "sender_lag_ms: constant offset, added delay and a new-stream "
               "reset all measure correctly\n";
  return 0;
}

// process_ms/lock_wait_ms: real contention on Runtime's mutex_ (background
// threads hammering snapshot()) must show up as nonzero lock wait, and
// process_ms (receipt to visible-in-snapshot) must always be at least as
// large as the lock-wait component it contains -- true by construction of
// how the two are measured, so this also guards against a future change
// re-introducing the original bug (measuring process_ms only after the lock
// was already held).
int held_lock(const fs::path &assets) {
  Fixture f(assets, "held-lock");
  Runtime r(f.config);
  Stream stream(random_run());
  require(r.receive(stream.metadata(1000000), "127.0.0.1"),
          "contention-test metadata accepted");
  std::atomic<bool> stop{false};
  // Several busy readers contending for the same mutex_ that receive() and
  // snapshot() both take, with no sleep, so some receive() calls below are
  // very likely to have to wait for it.
  std::vector<std::thread> readers;
  for (int i = 0; i < 6; ++i)
    readers.emplace_back([&] {
      while (!stop)
        (void)r.snapshot();
    });
  for (int i = 0; i < 400; ++i) {
    const auto sender_us = 1000000 + std::uint64_t(i + 1) * 20000;
    require(r.receive(stream.telemetry(sender_us), "127.0.0.1"),
            "contended telemetry accepted");
  }
  stop = true;
  for (auto &t : readers)
    t.join();
  auto state = r.snapshot();
  const double process_max = state["process_ms"]["max"].get<double>();
  const double lock_wait_max = state["lock_wait_ms"]["max"].get<double>();
  require(state["process_ms"]["p95"].get<double>() >=
              state["process_ms"]["p50"].get<double>() &&
              state["lock_wait_ms"]["p95"].get<double>() >=
                  state["lock_wait_ms"]["p50"].get<double>(),
          "percentiles are ordered");
  // True for every sample by construction (lock_wait_ms is part of the
  // receipt-to-visible interval process_ms measures), so this holds however
  // much real contention the run happened to produce.
  require(process_max >= lock_wait_max,
          "process_ms (receipt to visible) can never be smaller than its "
          "own lock-wait component (process_max=" +
              std::to_string(process_max) +
              " ms, lock_wait_max=" + std::to_string(lock_wait_max) + " ms)");
  require(lock_wait_max > 0.0,
          "six busy readers contending for the runtime lock produced no "
          "measurable lock wait at all (lock_wait_ms=" +
              state["lock_wait_ms"].dump() +
              "); the old process_ms (timed only after the lock was already "
              "held) would have missed this entirely");
  std::cout << "process_ms/lock_wait_ms under contention: max " << process_max
            << " ms process, " << lock_wait_max << " ms of it lock wait\n";
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "usage: rapid-latency-tests <assets>");
    const fs::path assets = argv[1];
    if (int code = sender_lag(assets))
      return code;
    if (int code = held_lock(assets))
      return code;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
