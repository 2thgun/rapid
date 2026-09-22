// Receive-path I/O tests for #17 step 8 (disk I/O off the receive path).
//
// These tests are written against the public Runtime API and the on-disk
// artifacts only, so they hold for the current synchronous implementation and
// for a batched/threaded one alike. Crashes are simulated with fork() plus
// _exit()/SIGKILL, so nothing that a real power cut would skip (destructors,
// stdio flushes, pending writer work) runs in the crashed process.
//
// Slow or failing storage is simulated by interposing fsync()/fdatasync() in
// this executable. Both rapid_native (linked statically) and libsqlite3 resolve
// those symbols here, so no production seam is needed. If interposition is not
// effective on a platform, the timing cases exit 77 (skipped), never pass.
//
// Cases:
//   replay-crash      guard: every accepted v4 packet is still rejected as a
//                     replay after a crash at any point, and the legitimate
//                     sender can resume after a bounded gap.
//   recording-crash   guard: a crash loses at most the last D seconds of
//                     accepted samples; what is recovered is an exact prefix.
//   finalize-race     two runs end back to back on slow storage and the
//                     process shuts down: both bundles complete and distinct,
//                     receive() never waits for publication.
//   write-failure     guard: a failing disk makes the receiver fail closed
//                     within the persisted margin, without hanging.
//   latency           receive() and snapshot() stay fast while every fsync
//                     takes 200 ms (failed against the synchronous I/O before
//                     step 8 phase B).
//   bench [packets]   not a test: prints per-packet cost with real and no-op
//                     sync, for the step 8 before/after measurement.
#include "rapid/native.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <openssl/hmac.h>
#include <random>
#include <sstream>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>

using namespace rapid::native;
using Clock = std::chrono::steady_clock;

namespace {
std::atomic<long> sync_calls{0};
std::atomic<int> sync_delay_ms{0};
std::atomic<bool> sync_fail{false}, sync_skip{false};
int sync_hook() {
  ++sync_calls;
  if (sync_fail) {
    errno = EIO;
    return -1;
  }
  if (int delay = sync_delay_ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
  return sync_skip ? 1 : 0;
}
} // namespace

extern "C" int fsync(int fd) {
  if (int r = sync_hook())
    return r < 0 ? -1 : 0;
  return int(syscall(SYS_fsync, fd));
}
extern "C" int fdatasync(int fd) {
  if (int r = sync_hook())
    return r < 0 ? -1 : 0;
  return int(syscall(SYS_fdatasync, fd));
}

namespace {
constexpr int skipped = 77;
const std::string host = "127.0.0.1";

void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}
double percentile(std::vector<double> values, double rank) {
  if (values.empty())
    return 0;
  std::sort(values.begin(), values.end());
  return values[std::size_t(std::ceil(rank * double(values.size() - 1)))];
}
std::string stats(const std::vector<double> &v) {
  double sum = 0;
  for (double x : v)
    sum += x;
  std::ostringstream out;
  out << std::fixed << std::setprecision(3) << "n=" << v.size()
      << " mean=" << (v.empty() ? 0 : sum / double(v.size()))
      << " p50=" << percentile(v, .5) << " p95=" << percentile(v, .95)
      << " p99=" << percentile(v, .99) << " max=" << percentile(v, 1) << " ms";
  return out.str();
}

// Synthetic Windows-companion v4 stream, byte layout as parsed by
// native_v4.cpp. Deterministic for a given run ID, so a parent process can
// regenerate exactly the packets a crashed child accepted.
struct Stream {
  std::string key = std::string(32, '\x11'), run;
  int rate = 50;
  explicit Stream(std::string run_id) : run(std::move(run_id)) {}
  static void put(std::string &b, std::size_t at, std::uint64_t v,
                  std::size_t size) {
    for (std::size_t i = 0; i < size; ++i)
      b[at + i] = char((v >> (8 * i)) & 255);
  }
  static std::uint64_t time_us(std::uint64_t sequence) {
    return 1000000 + sequence * 20000;
  }
  std::string header(int type, int flags, std::size_t end,
                     std::uint64_t sequence) const {
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
    put(b, 36, sequence, 8);
    put(b, 44, time_us(sequence), 8);
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
  // Sequence 1 is the metadata packet; telemetry sample i uses sequence i + 1.
  std::string metadata() const {
    std::string text;
    // track, car, driver, session, steering lock in degrees (#18)
    for (const char *s :
         {"Test Track", "Test Car", "Test Driver", "Practice", "900"}) {
      text += char(std::strlen(s) & 255);
      text += char(0);
      text += s;
    }
    auto b = header(2, 1, 52 + text.size(), 1);
    b.replace(52, text.size(), text);
    return sign(b);
  }
  static float rpm_for(std::uint64_t sample) { return float(1000 + sample); }
  std::string telemetry(std::uint64_t sample) const {
    const auto sequence = sample + 2;
    auto b = header(1, 1, 260, sequence);
    std::uint64_t mask = (1ULL << 1) | (1ULL << 4) | (1ULL << 5) | (1ULL << 6) |
                         (1ULL << 11) | (1ULL << 12) | (1ULL << 13) |
                         (1ULL << 45) | (1ULL << 46) | (1ULL << 47);
    put(b, 52, mask, 8);
    auto channel = [&](int i, float v) {
      put(b, 68 + 4 * i, std::bit_cast<std::uint32_t>(v), 4);
    };
    channel(1, .5f);
    channel(4, 3);
    channel(5, rpm_for(sample % 19000));
    channel(6, .1f);
    channel(45, 1);
    channel(46, float(sample % 100000 * 20));
    channel(47, float(double(sample % 100000) / 200000.0));
    return sign(b);
  }
  std::string status(int state, std::uint64_t sequence) const {
    const int flags = state < 2 ? 0 : state == 2 ? 1 : 3;
    auto b = header(3, flags, 64, sequence);
    put(b, 52, state, 1);
    return sign(b);
  }
};
std::string random_run() {
  auto hex = unique_id();
  std::string raw;
  for (std::size_t i = 0; i < 32; i += 2)
    raw += char(std::stoul(hex.substr(i, 2), nullptr, 16));
  return raw;
}

// Measures how long this machine deschedules a thread that is not waiting for
// anything, so a shared CI runner's stalls can be told apart from blocking in
// the code under test. Wall-clock bounds below are strict plus this ambient
// allowance; the percentile and storage-wait assertions never use it.
struct Ambient {
  std::atomic<bool> stop{false};
  std::atomic<double> max_ms{0};
  std::thread thread;
  Ambient() {
    thread = std::thread([this] {
      while (!stop) {
        const auto t = Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const double overshoot = ms_since(t) - 1.0;
        double seen = max_ms.load();
        while (overshoot > seen && !max_ms.compare_exchange_weak(seen, overshoot))
          ;
      }
    });
  }
  double finish() {
    stop = true;
    thread.join();
    return max_ms.load();
  }
};

struct Fixture {
  fs::path root;
  Config config;
  Fixture(const fs::path &assets, const std::string &name) {
    root = fs::temp_directory_path() /
           ("rapid-io-" + name + "-" + unique_id());
    fs::create_directories(root);
    config.assets = assets;
    config.database = root / "state.db";
    config.telemetry = root / "telemetry";
    config.queue = root / "queue.db";
    config.companion_key = std::string(32, '\x11');
  }
};

// Runs body in a child process that ends with _exit (no destructors, no stdio
// flush) and returns its exit status.
template <class F> int crash_child(F body, pid_t *pid_out = nullptr,
                                   int *pipe_out = nullptr) {
  int fds[2] = {-1, -1};
  if (pipe_out)
    require(pipe(fds) == 0, "pipe");
  std::cout.flush();
  std::cerr.flush();
  pid_t pid = fork();
  require(pid >= 0, "fork");
  if (pid == 0) {
    if (pipe_out)
      ::close(fds[0]);
    int code = 0;
    try {
      body(fds[1]);
    } catch (const std::exception &e) {
      std::cerr << "child: " << e.what() << std::endl;
      code = 3;
    }
    _exit(code);
  }
  if (pipe_out) {
    ::close(fds[1]);
    *pipe_out = fds[0];
  }
  if (pid_out) {
    *pid_out = pid;
    return 0;
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// Teardown for a Fixture's temp directory. Every caller has already
// waitpid()-reaped whatever child touched it (crash_child()'s own wait, or an
// explicit waitpid() after a SIGKILL race). Even so, fs::remove_all() has
// intermittently thrown ENOTEMPTY here on WSL: the reaped child's spool
// directory can still show a stale entry for a moment, most likely delayed
// directory-entry propagation on the 9p-backed temp filesystem, not a real
// write failure (#17). Retry with a short backoff, and only let a removal
// that still cannot finish raise, so an actual failure to write is not
// swallowed.
void remove_root(const fs::path &root) {
  std::error_code error;
  for (int attempt = 0; attempt < 10; ++attempt) {
    error.clear();
    fs::remove_all(root, error);
    if (!error && !fs::exists(root))
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(20 * (attempt + 1)));
  }
  // Out of retries: let the real error (or a leftover path) surface as a
  // thrown filesystem_error, same as an unretried fs::remove_all() call.
  fs::remove_all(root);
}

// Legitimate senders resume after a Pi restart well beyond any reservation
// margin: a Pi reboot is tens of seconds, i.e. > 1000 packets at 50 Hz.
constexpr std::uint64_t max_restart_margin = 1024;

int replay_crash(const fs::path &assets) {
  for (const bool slow_disk : {false, true}) {
    Fixture f(assets, slow_disk ? "replay-slow" : "replay");
    const Stream retired(random_run()), active(random_run());
    const std::uint64_t retired_samples = 40, active_samples = 400;
    // Crash immediately after the last accept: the widest possible window
    // between an in-memory watermark and its durable copy.
    int code = crash_child([&](int) {
      if (slow_disk)
        sync_delay_ms = 20;
      Runtime r(f.config);
      require(r.receive(retired.metadata(), host), "retired metadata");
      for (std::uint64_t i = 0; i < retired_samples; ++i)
        require(r.receive(retired.telemetry(i), host), "retired telemetry");
      require(r.receive(active.metadata(), host), "new run retires old run");
      for (std::uint64_t i = 0; i < active_samples; ++i) {
        require(r.receive(active.telemetry(i), host), "active telemetry");
      }
      // Driving heartbeat on the same run, then more telemetry.
      require(r.receive(active.status(2, active_samples + 2), host),
              "driving heartbeat");
    });
    require(code == 0, "crash child did not accept its stream");
    std::vector<std::string> accepted = {retired.metadata(), active.metadata()};
    for (std::uint64_t i = 0; i < retired_samples; ++i)
      accepted.push_back(retired.telemetry(i));
    for (std::uint64_t i = 0; i < active_samples; ++i)
      accepted.push_back(active.telemetry(i));
    accepted.push_back(active.status(2, active_samples + 2));
    {
      Runtime r(f.config);
      for (auto it = accepted.rbegin(); it != accepted.rend(); ++it)
        require(!r.receive(*it, host),
                "replay accepted after crash (reverse order)");
      for (const auto &packet : accepted)
        require(!r.receive(packet, host), "replay accepted after crash");
      auto state = r.snapshot();
      require(number(state, "packets_replayed") == double(2 * accepted.size()) &&
                  number(state, "packets_invalid") == 0,
              "post-crash replays must be counted as replays, not invalid");
      // The real sender keeps counting while the Pi is down.
      const auto resume = active_samples + max_restart_margin + 10000;
      require(r.receive(active.telemetry(resume), host),
              "legitimate sender resumes after restart beyond the margin");
      require(r.snapshot()["last_sequence"] == resume + 2,
              "resumed sample reaches live state");
      const Stream fresh(random_run());
      require(r.receive(fresh.metadata(), host) &&
                  r.receive(fresh.telemetry(0), host),
              "a new run starts after restart");
      require(!r.receive(active.telemetry(resume), host) &&
                  !r.receive(active.telemetry(resume + 1), host),
              "new run retires the resumed run");
    }
    {
      // Clean restart after the resume: nothing accepted before is replayable.
      Runtime r(f.config);
      for (const auto &packet : accepted)
        require(!r.receive(packet, host), "replay accepted after clean restart");
      require(!r.receive(active.telemetry(active_samples + max_restart_margin +
                                          10000),
                         host),
              "resumed packet replay accepted after clean restart");
    }
    remove_root(f.root);
  }

  {
    // Storage that cannot persist at all: another connection holds the
    // database write lock, so nothing the receiver commits from here on can
    // reach disk. Whatever the child still accepts must already be covered
    // by durable state.
    Fixture f(assets, "replay-locked");
    const Stream stream(random_run());
    int progress = -1;
    pid_t pid = 0;
    crash_child(
        [&](int out) {
          Runtime r(f.config);
          auto report = [&](std::uint64_t index) {
            require(write(out, &index, sizeof index) == sizeof index,
                    "progress");
          };
          require(r.receive(stream.metadata(), host), "metadata");
          for (std::uint64_t i = 0; i < 50; ++i) {
            require(r.receive(stream.telemetry(i), host), "telemetry");
            report(i);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(600));
          Database blocker(f.config.database);
          blocker.exec("BEGIN IMMEDIATE");
          const auto until = Clock::now() + std::chrono::seconds(4);
          for (std::uint64_t i = 50; i < 3000 && Clock::now() < until; ++i)
            if (r.receive(stream.telemetry(i), host))
              report(i);
          _exit(0); // lock and pending writes vanish with the process
        },
        &pid, &progress);
    std::vector<std::uint64_t> accepted;
    std::uint64_t index = 0;
    while (read(progress, &index, sizeof index) == sizeof index)
      accepted.push_back(index);
    ::close(progress);
    int status = 0;
    waitpid(pid, &status, 0);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                accepted.size() >= 50,
            "locked-storage child");
    {
      // Scoped so the runtime's writer threads are joined before the
      // directory is removed.
      Runtime r(f.config);
      for (auto i : accepted)
        require(!r.receive(stream.telemetry(i), host),
                "replay accepted after crash while storage was locked "
                "(sample " +
                    std::to_string(i) + ", " +
                    std::to_string(accepted.size()) + " accepted)");
    }
    std::cout << "replay-crash (locked storage): " << accepted.size()
              << " accepted before the crash, none replayable\n";
    remove_root(f.root);
  }

  // Crash at arbitrary points of a paced stream (SIGKILL from outside), then
  // replay everything the child reported as accepted.
  std::mt19937 random(std::random_device{}());
  for (int round = 0; round < 3; ++round) {
    Fixture f(assets, "replay-kill");
    const Stream stream(random_run());
    pid_t pid = 0;
    int progress = -1;
    crash_child(
        [&](int out) {
          Runtime r(f.config);
          require(r.receive(stream.metadata(), host), "metadata");
          auto next = Clock::now();
          for (std::uint64_t i = 0;; ++i) {
            require(r.receive(stream.telemetry(i), host), "telemetry");
            std::uint64_t done = i + 1;
            require(write(out, &done, sizeof done) == sizeof done, "progress");
            next += std::chrono::microseconds(1000000 / 250);
            std::this_thread::sleep_until(next);
          }
        },
        &pid, &progress);
    std::uint64_t accepted = 0, value = 0;
    const auto kill_after =
        std::chrono::milliseconds(300 + random() % 1200);
    const auto start = Clock::now();
    while (Clock::now() - start < kill_after &&
           read(progress, &value, sizeof value) == sizeof value)
      accepted = value;
    kill(pid, SIGKILL);
    // The child may accept one more packet than it reported; it is not
    // replayed because the test cannot know whether it was accepted.
    int status = 0;
    waitpid(pid, &status, 0);
    ::close(progress);
    require(WIFSIGNALED(status) && accepted > 0, "kill child mid-stream");
    {
      Runtime r(f.config);
      require(!r.receive(stream.metadata(), host),
              "metadata replay after kill");
      for (std::uint64_t i = 0; i < accepted; ++i)
        require(!r.receive(stream.telemetry(i), host),
                "replay accepted after SIGKILL at sample " + std::to_string(i) +
                    " of " + std::to_string(accepted));
      require(r.receive(stream.telemetry(accepted + max_restart_margin + 10000),
                        host),
              "sender resumes after SIGKILL restart");
    }
    remove_root(f.root);  // retries: filesystem lag after the writers are joined (#17, g4)
  }
  std::cout << "replay-crash: accepted v4 packets stay rejected after crash "
               "and restart; sender resumes\n";
  return 0;
}

std::string read_ld_channel(const fs::path &ld, const std::string &needle,
                            std::vector<float> &values) {
  const auto data = read_file(ld);
  auto u32 = [&](std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= std::uint32_t(static_cast<unsigned char>(data.at(at + i))) << (8 * i);
    return v;
  };
  for (std::uint32_t at = u32(8); at; at = u32(at + 4)) {
    std::string name(data.data() + at + 32, 32);
    name.resize(std::strlen(name.c_str()));
    if (name.find(needle) == std::string::npos)
      continue;
    const auto pointer = u32(at + 8), count = u32(at + 12);
    values.clear();
    for (std::uint32_t i = 0; i < count; ++i)
      values.push_back(std::bit_cast<float>(u32(pointer + 4 * i)));
    return name;
  }
  throw std::runtime_error("LD channel not found: " + needle);
}
// Returns the recovered sample count after checking that the published
// recording is an exact prefix of the stream.
std::uint64_t recovered_prefix(const Config &config) {
  std::vector<fs::path> bundles;
  for (const auto &entry : fs::directory_iterator(config.telemetry))
    if (entry.is_directory() &&
        entry.path().filename().string().starts_with("session-") &&
        fs::exists(entry.path() / "manifest.json"))
      bundles.push_back(entry.path());
  if (bundles.empty())
    return 0;
  require(bundles.size() == 1, "one crashed run produces one bundle");
  auto manifest = Json::parse(read_file(bundles[0] / "manifest.json"));
  const std::uint64_t count = manifest["quality"]["recorded_samples"];
  std::vector<float> rpm;
  read_ld_channel(bundles[0] / "full-session.ld", "RPM", rpm);
  require(rpm.size() == count, "LD sample count matches manifest");
  for (std::uint64_t i = 0; i < count; ++i)
    require(rpm[i] == Stream::rpm_for(i),
            "recovered sample " + std::to_string(i) +
                " is not the sample that was sent");
  return count;
}

// A sample accepted this long before a crash must survive it. The current
// implementation checkpoints every sample_rate samples (1 s); the step 8 design
// allows 1 s plus writer lag.
constexpr double durable_within_s = 2.0;

int recording_crash(const fs::path &assets) {
  {
    // Stream stops, the receive loop keeps running (expire() every 100 ms as
    // udp_loop does), then the process dies.
    Fixture f(assets, "recording-idle");
    const Stream stream(random_run());
    const std::uint64_t samples = 330;
    int code = crash_child([&](int) {
      Runtime r(f.config);
      require(r.receive(stream.metadata(), host), "metadata");
      for (std::uint64_t i = 0; i < samples; ++i)
        require(r.receive(stream.telemetry(i), host), "telemetry");
      const auto until = Clock::now() + std::chrono::milliseconds(2500);
      while (Clock::now() < until) {
        r.expire();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
    require(code == 0, "idle crash child failed");
    {
      Runtime recovered(f.config);
      require(recovered_prefix(f.config) == samples,
              "every sample accepted before an idle crash is in the recording");
    }
    remove_root(f.root);  // retries: filesystem lag after the writers are joined (#17, g4)
  }
  std::mt19937 random(std::random_device{}());
  for (int round = 0; round < 3; ++round) {
    // Real-time 50 Hz stream killed at an arbitrary instant, optionally while
    // every sync is slow.
    const bool slow_disk = round % 2 == 1;
    Fixture f(assets, "recording-kill");
    const Stream stream(random_run());
    pid_t pid = 0;
    int progress = -1;
    crash_child(
        [&](int out) {
          if (slow_disk)
            sync_delay_ms = 15;
          Runtime r(f.config);
          require(r.receive(stream.metadata(), host), "metadata");
          auto next = Clock::now();
          for (std::uint64_t i = 0;; ++i) {
            require(r.receive(stream.telemetry(i), host), "telemetry");
            std::uint64_t done = i + 1;
            require(write(out, &done, sizeof done) == sizeof done, "progress");
            r.expire();
            next += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(next);
          }
        },
        &pid, &progress);
    std::vector<std::pair<Clock::time_point, std::uint64_t>> reports;
    // Late enough that several checkpoints must have become durable.
    const auto kill_after = std::chrono::milliseconds(4500 + random() % 2000);
    const auto start = Clock::now();
    std::uint64_t value = 0;
    while (Clock::now() - start < kill_after &&
           read(progress, &value, sizeof value) == sizeof value)
      reports.emplace_back(Clock::now(), value);
    const auto killed_at = Clock::now();
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    ::close(progress);
    require(WIFSIGNALED(status) && !reports.empty(), "kill recording child");
    std::uint64_t must_survive = 0;
    for (const auto &[at, count] : reports)
      if (std::chrono::duration<double>(killed_at - at).count() >=
          durable_within_s)
        must_survive = count;
    std::uint64_t count = 0;
    {
      Runtime recovered(f.config);
      count = recovered_prefix(f.config);
    }
    std::cout << "recording-crash round " << round
              << (slow_disk ? " (slow sync)" : "") << ": accepted>="
              << reports.back().second << " must_survive=" << must_survive
              << " recovered=" << count << "\n";
    require(count <= reports.back().second + 1,
            "recording contains samples that were never accepted");
    require(count >= must_survive,
            "samples accepted more than " + std::to_string(durable_within_s) +
                " s before the crash were lost");
    remove_root(f.root);
  }
  std::cout << "recording-crash: recovered recordings are exact prefixes "
               "within the durability bound\n";
  return 0;
}

// Deterministic regression for the #17 durability flake on a *fast* machine.
// With a sync far slower than recording-crash uses (60 ms), a checkpoint that
// fsyncs the ~48 channel files one at a time costs about 3 s -- longer than
// the 1 s sample cadence -- so the writer falls behind and the durable prefix
// slips past the 2 s bound even on an idle host. The production fix syncs the
// channel files concurrently and coalesces pending checkpoints, so the same
// stream stays inside the bound with room to spare. The existing
// rapid-io-recording-crash round guard is not touched.
int recording_slow_sync(const fs::path &assets) {
  Fixture f(assets, "recording-slow");
  const Stream stream(random_run());
  constexpr int sync_ms = 60;
  pid_t pid = 0;
  int progress = -1;
  crash_child(
      [&](int out) {
        sync_delay_ms = sync_ms;
        Runtime r(f.config);
        require(r.receive(stream.metadata(), host), "metadata");
        auto next = Clock::now();
        for (std::uint64_t i = 0;; ++i) {
          require(r.receive(stream.telemetry(i), host), "telemetry");
          std::uint64_t done = i + 1;
          require(write(out, &done, sizeof done) == sizeof done, "progress");
          r.expire();
          next += std::chrono::milliseconds(20);
          std::this_thread::sleep_until(next);
        }
      },
      &pid, &progress);
  std::vector<std::pair<Clock::time_point, std::uint64_t>> reports;
  // Fixed kill time: long enough for several checkpoints on the fixed writer,
  // but only one or two on a sequential one.
  const auto kill_after = std::chrono::milliseconds(6000);
  const auto start = Clock::now();
  std::uint64_t value = 0;
  while (Clock::now() - start < kill_after &&
         read(progress, &value, sizeof value) == sizeof value)
    reports.emplace_back(Clock::now(), value);
  const auto killed_at = Clock::now();
  kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  ::close(progress);
  require(WIFSIGNALED(status) && !reports.empty(),
          "kill slow-sync recording child");
  std::uint64_t must_survive = 0;
  for (const auto &[at, count] : reports)
    if (std::chrono::duration<double>(killed_at - at).count() >=
        durable_within_s)
      must_survive = count;
  std::uint64_t count = 0;
  {
    Runtime recovered(f.config);
    count = recovered_prefix(f.config);
  }
  std::cout << "recording-slow-sync (" << sync_ms
            << " ms/sync): accepted>=" << reports.back().second
            << " must_survive=" << must_survive << " recovered=" << count
            << "\n";
  require(count <= reports.back().second + 1,
          "recording contains samples that were never accepted");
  require(count >= must_survive,
          "samples accepted more than " + std::to_string(durable_within_s) +
              " s before the crash were lost under slow sync");
  remove_root(f.root);
  std::cout << "recording-slow-sync: the durable prefix holds when every "
               "sync is far slower than in recording-crash\n";
  return 0;
}

// A run ends and the next starts while storage is slow, and the process shuts
// down right after: both recordings must be published completely and
// separately, and the receiver must not wait for either publication.
int finalize_race(const fs::path &assets) {
  Fixture f(assets, "finalize-race");
  const Stream first(random_run()), second(random_run());
  const std::uint64_t samples = 60;
  std::vector<double> receive_ms, blocked_ms;
  struct Slow {
    std::string what;
    double duration, waited;
    long syncs;
  };
  std::vector<Slow> slow;
  std::vector<double> excess_ms;
  Ambient ambient;
  {
    Runtime r(f.config);
    auto timed = [&](const std::string &packet, const char *what) {
      // Storage waiting is read from the runtime's own counter, so a slow
      // receive can be attributed to blocking rather than to an unrelated
      // scheduling stall on a loaded machine.
      const auto before = number(r.snapshot(), "replay_wait_ms");
      const auto syncs = sync_calls.load();
      const auto t = Clock::now();
      require(r.receive(packet, host), what);
      const auto duration = ms_since(t);
      const auto waited = number(r.snapshot(), "replay_wait_ms") - before;
      receive_ms.push_back(duration);
      blocked_ms.push_back(waited);
      excess_ms.push_back(duration - waited);
      // Only a packet that starts a run may wait: its replay floor has to be
      // durable before it is admitted. Nothing may ever wait for a
      // checkpoint, a publication or the writer queue.
      const bool starts_run = std::string(what).find("metadata") !=
                                  std::string::npos ||
                              std::string(what).find("starts") !=
                                  std::string::npos;
      require(waited == 0 || starts_run,
              std::string("receive() waited ") + std::to_string(waited) +
                  " ms on storage for a packet that is not a new run: " +
                  what);
      require(waited < 1000,
              std::string("storage wait exceeded the deferral bound: ") +
                  what);
      if (duration >= 50)
        slow.push_back({what, duration, waited, sync_calls.load() - syncs});
    };
    timed(first.metadata(), "first metadata");
    for (std::uint64_t i = 0; i < samples; ++i)
      timed(first.telemetry(i), "first telemetry");
    sync_delay_ms = 30;
    timed(first.status(3, samples + 2), "first run ends");
    timed(second.metadata(), "second run starts during publication");
    for (std::uint64_t i = 0; i < samples; ++i)
      timed(second.telemetry(i), "second telemetry");
    timed(second.status(3, samples + 2), "second run ends");
    // Destruction drains the recorder: no explicit finish() or wait.
  }
  sync_delay_ms = 0;
  std::vector<std::string> sessions;
  for (const auto &entry : fs::directory_iterator(f.config.telemetry)) {
    if (!entry.path().filename().string().starts_with("session-"))
      continue;
    require(fs::exists(entry.path() / "manifest.json"),
            "published bundle has a manifest");
    auto manifest = Json::parse(read_file(entry.path() / "manifest.json"));
    require(manifest["quality"]["recorded_samples"] == samples,
            "each run's bundle holds exactly its own samples");
    std::vector<float> rpm;
    read_ld_channel(entry.path() / "full-session.ld", "RPM", rpm);
    for (std::uint64_t i = 0; i < samples; ++i)
      require(rpm.at(i) == Stream::rpm_for(i), "bundle sample mismatch");
    sessions.push_back(manifest["session_id"]);
  }
  require(sessions.size() == 2 && sessions[0] != sessions[1],
          "two distinct bundles published");
  const double stall = ambient.finish();
  std::cout << "finalize-race: two bundles published across a slow-storage "
               "run change and shutdown; receive " << stats(receive_ms)
            << "\n  storage wait " << stats(blocked_ms) << "\n  excess "
            << stats(excess_ms) << "\n  ambient scheduling stall " << stall
            << " ms\n";
  for (const auto &entry : slow)
    std::cout << "  slow receive: " << entry.what << " " << entry.duration
              << " ms, waited on storage " << entry.waited << " ms, "
              << entry.syncs << " syncs\n";
  // Time not spent waiting on storage is the receiver's own work. It is
  // bounded strictly at the 99th percentile, and per sample only up to what
  // this machine was observed to steal from a thread that waits for nothing.
  require(percentile(excess_ms, .99) < 10 + stall,
          "receive() spends time beyond its own work and the replay floor");
  require(percentile(excess_ms, 1) < 50 + stall,
          "receive() waited for publication, a checkpoint or the queue");
  require(percentile(blocked_ms, .95) == 0,
          "only a new run may wait for storage");
  remove_root(f.root);  // retries: filesystem lag after the writers are joined (#17, g4)
  return 0;
}

int write_failure(const fs::path &assets) {
  Fixture f(assets, "write-failure");
  const Stream stream(random_run());
  Runtime r(f.config);
  require(r.receive(stream.metadata(), host), "metadata");
  for (std::uint64_t i = 0; i < 10; ++i)
    require(r.receive(stream.telemetry(i), host), "telemetry before failure");
  const auto calls = sync_calls.load();
  sync_fail = true;
  std::uint64_t accepted = 0;
  const auto start = Clock::now();
  for (std::uint64_t i = 10; i < 3000; ++i) {
    accepted += r.receive(stream.telemetry(i), host);
    (void)r.snapshot();
    if (i % 50 == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto elapsed = ms_since(start);
  sync_fail = false;
  std::cout << "write-failure: accepted " << accepted
            << " of 2990 packets while every sync failed (" << elapsed
            << " ms, " << sync_calls - calls << " sync calls)\n";
  if (sync_calls == calls)
    return skipped;
  require(accepted <= max_restart_margin,
          "receiver kept accepting beyond the durable replay margin while "
          "storage was failing");
  require(elapsed < 30000, "receiver hung on failing storage");
  return 0;
}

int latency(const fs::path &assets) {
  Fixture f(assets, "latency");
  const Stream stream(random_run());
  Runtime r(f.config);
  require(r.receive(stream.metadata(), host), "metadata");
  for (std::uint64_t i = 0; i < 60; ++i)
    require(r.receive(stream.telemetry(i), host), "warm-up telemetry");
  if (sync_calls == 0) {
    std::cout << "latency: fsync interposition not effective; skipped\n";
    return skipped;
  }
  constexpr int delay = 200, packets = 150;
  sync_delay_ms = delay;
  const auto waited_before = number(r.snapshot(), "replay_wait_ms");
  Ambient ambient;
  std::vector<double> receive_ms, snapshot_ms;
  std::atomic<bool> done{false};
  std::atomic<int> accepted{0}, sent{0};
  std::thread sender([&] {
    const auto start = Clock::now();
    auto next = start;
    for (int i = 0; i < packets; ++i) {
      // Give up once far behind schedule so a synchronous implementation
      // fails in seconds rather than minutes.
      if (Clock::now() - start > std::chrono::seconds(5))
        break;
      const auto t = Clock::now();
      accepted += r.receive(stream.telemetry(60 + i), host);
      receive_ms.push_back(ms_since(t));
      ++sent;
      next += std::chrono::milliseconds(20);
      std::this_thread::sleep_until(next);
    }
    done = true;
  });
  while (!done) {
    const auto t = Clock::now();
    (void)r.snapshot();
    snapshot_ms.push_back(ms_since(t));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  sender.join();
  sync_delay_ms = 0;
  const double stall = ambient.finish();
  const auto waited = number(r.snapshot(), "replay_wait_ms") - waited_before;
  std::cout << "latency (every fsync " << delay << " ms): sent " << sent << "/"
            << packets << " accepted " << accepted << "\n  receive  "
            << stats(receive_ms) << "\n  snapshot " << stats(snapshot_ms)
            << "\n  storage wait " << waited << " ms, ambient scheduling stall "
            << stall << " ms\n";
  require(sent == packets && accepted == packets,
          "receiver could not keep a 50 Hz schedule on a slow disk");
  // Nothing in an established run may wait for storage at all; the wall-clock
  // bounds additionally allow for what this machine steals from a runnable
  // thread (measured above), so a loaded CI runner cannot fake a failure.
  require(waited == 0, "receive() waited for storage inside a live run");
  require(percentile(receive_ms, 1) < 50 + stall &&
              percentile(receive_ms, .95) < 10 + stall,
          "receive() waits for storage");
  require(percentile(snapshot_ms, 1) < 50 + stall &&
              percentile(snapshot_ms, .95) < 5 + stall,
          "snapshot() waits for storage");
  return 0;
}

int bench(const fs::path &assets, int packets) {
  std::cout << "bench: synthetic v4 stream, " << packets
            << " telemetry packets per run, temp "
            << fs::temp_directory_path() << "\n";
  for (const bool skip : {false, true}) {
    Fixture f(assets, "bench");
    const Stream stream(random_run());
    Runtime r(f.config);
    sync_skip = skip;
    require(r.receive(stream.metadata(), host), "metadata");
    std::vector<double> unpaced, snapshots;
    const auto calls = sync_calls.load();
    for (int i = 0; i < packets; ++i) {
      const auto t = Clock::now();
      require(r.receive(stream.telemetry(i), host), "bench telemetry");
      unpaced.push_back(ms_since(t));
    }
    const double syncs = double(sync_calls - calls) / packets;
    for (int i = 0; i < 1000; ++i) {
      const auto t = Clock::now();
      (void)r.snapshot();
      snapshots.push_back(ms_since(t));
    }
    const auto state = r.snapshot();
    std::cout << (skip ? "[sync no-op]" : "[real sync] ")
              << " receive unpaced " << stats(unpaced)
              << "\n              sync calls/packet " << syncs
              << "\n              snapshot uncontended " << stats(snapshots)
              << "\n              step-3 process_ms " << state["process_ms"].dump()
              << "\n";
    // Paced 52 Hz for 5 s with a 200 Hz snapshot reader: lock contention.
    std::vector<double> paced, contended;
    std::atomic<bool> done{false};
    std::thread sender([&] {
      auto next = Clock::now();
      for (int i = 0; i < 260; ++i) {
        const auto t = Clock::now();
        (void)r.receive(stream.telemetry(packets + i), host);
        paced.push_back(ms_since(t));
        next += std::chrono::microseconds(19231);
        std::this_thread::sleep_until(next);
      }
      done = true;
    });
    while (!done) {
      const auto t = Clock::now();
      (void)r.snapshot();
      contended.push_back(ms_since(t));
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sender.join();
    std::cout << "              receive paced 52 Hz " << stats(paced)
              << "\n              snapshot during paced stream "
              << stats(contended) << "\n";
    sync_skip = false;
    remove_root(f.root);
  }
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc >= 3, "usage: rapid-io-path-tests <assets> <case> [args]");
    const fs::path assets = argv[1];
    const std::string name = argv[2];
    if (name == "replay-crash")
      return replay_crash(assets);
    if (name == "recording-crash")
      return recording_crash(assets);
    if (name == "recording-slow-sync")
      return recording_slow_sync(assets);
    if (name == "finalize-race")
      return finalize_race(assets);
    if (name == "write-failure")
      return write_failure(assets);
    if (name == "latency")
      return latency(assets);
    if (name == "bench")
      return bench(assets, argc > 3 ? std::stoi(argv[3]) : 1000);
    throw std::runtime_error("unknown case " + name);
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
