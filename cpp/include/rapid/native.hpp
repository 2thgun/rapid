#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include "rapid/lap_boundary.hpp"

namespace rapid::native {
using Json = nlohmann::json;
namespace fs = std::filesystem;
extern std::atomic_bool stopping;
std::string now();
double monotonic();
std::string unique_id();
std::string read_file(const fs::path &path);
void sync_file(const fs::path &path);
void atomic_file(const fs::path &path, const std::string &data);
std::string hash_file(const fs::path &path);
std::string hash_text(const std::string &data);
std::string safe_name(std::string value);
std::string env(const std::string &name, const std::string &fallback = "");
void log(const std::string &message);
double number(const Json &j, const std::string &key, double fallback = 0);
std::string string(const Json &j, const std::string &key,
                   const std::string &fallback = "");

class Database {
  sqlite3 *db_{};

public:
  explicit Database(const fs::path &path);
  ~Database();
  Database(const Database &) = delete;
  void exec(const std::string &sql, const Json &args = Json::array());
  Json query(const std::string &sql, const Json &args = Json::array());
};

std::string telemetry_key(std::string hex);
struct AuthenticationError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
// receipt_monotonic is the Pi-side monotonic time the datagram was actually
// read from the socket (udp_loop's recvfrom), used to compute _received_monotonic
// for the sender-lag estimate (#17) with as little added jitter as possible.
// A negative value (the default) falls back to monotonic() taken here, which
// only a synthetic caller (tests) should rely on.
Json receive_v4(Database &store, const std::string &payload,
                const std::string &key, double receipt_monotonic = -1);
Json receive_v4(Database &store, const std::string &payload,
                const std::string &key, const std::string &peer_namespace,
                double receipt_monotonic = -1);
Json receive_v4(Database &store, const std::string &payload,
                const std::vector<std::string> &keys,
                double receipt_monotonic = -1);

// Thrown when a packet is authentic but its replay floor could not be made
// durable in time (slow or failing storage). The packet is rejected.
struct ReplayDeferred : std::runtime_error {
  using std::runtime_error::runtime_error;
};
// v4 replay state held in memory and persisted by a writer thread (#17).
// Invariant: a packet with sequence s is admitted only once the durable row
// for its run already has sequence >= s (a floor reserved `margin` packets
// ahead), so no crash can make an admitted packet acceptable again. The
// v4_runs schema is unchanged; its sequence column holds that floor.
class ReplayGuard {
public:
  struct Run {
    std::uint64_t sequence = 0, time = 0;
    int simulator = 0;
    std::string metadata = "{}";
    bool closed = false, active = false;
  };
  explicit ReplayGuard(const fs::path &database, std::uint64_t margin = 256,
                       double period_s = 0.25, double wait_s = 1.0);
  ~ReplayGuard();
  ReplayGuard(const ReplayGuard &) = delete;
  std::optional<Run> find(const std::string &id) const;
  // Blocks only while the durable floor is behind; throws ReplayDeferred.
  void admit(const std::string &id, const Run &run, bool new_run);
  Json status() const;

private:
  struct Entry {
    Run run;                     // last admitted state
    std::optional<Run> staged;   // new run waiting for its first durable row
    bool admitted = false, persisted = false, dirty = false;
    std::uint64_t durable = 0, target = 0, version = 0;
  };
  Database db_;
  const std::uint64_t margin_;
  const double period_s_, wait_s_;
  mutable std::mutex mutex_;
  std::condition_variable wake_, durable_;
  std::map<std::string, Entry> entries_;
  bool stop_ = false, urgent_ = false;
  std::string error_;
  std::uint64_t commits_ = 0, deferred_ = 0, waits_ = 0;
  double wait_ms_ = 0, max_wait_ms_ = 0;
  std::thread writer_;
  void loop();
  bool commit(bool exact);
};
Json receive_v4(ReplayGuard &guard, const std::string &payload,
                const std::vector<std::string> &keys,
                double receipt_monotonic = -1);

struct Config {
  std::string host = "0.0.0.0", pairing_host = "0.0.0.0", companion_host, acc_host = "192.168.1.89",
              acc_password, companion_key;
  std::vector<std::string> companion_keys;
  std::string display_name = "raPId", upload_url, upload_token,
              upload_policy = "races";
  int port = 8000, companion_port = 9001, acc_port = 9000,
      acc_local_port = 9000, pairing_port = 8003, protocol_version = 4, interval_ms = 100;
  // #15: a recording stays open through a paused/menu/alt-tab gap (kept alive
  // by "paused"/"driving" heartbeats) until this many seconds without a live
  // telemetry sample. Default is the owner-decided 10 minutes.
  double not_live_timeout_seconds = 600.0;
  bool acc_enabled = false, upload_enabled = false, pairing_enabled = false;
  // Set only when the runtime has adopted records from private setup state.
  // In this mode an empty refreshed set means every paired PC was revoked; it
  // must not silently fall back to a legacy configured shared key.
  bool paired_key_mode = false;
  fs::path database = "data/rapid.db", telemetry = "data/telemetry",
           queue = "data/upload-queue.db", assets = "cpp/assets",
           network_control = "/run/rapid-network", setup_directory;
  static Config load(const fs::path &path);
};

// The recorder front (record/finish/status) does CPU work only and is called
// under the runtime lock. Spool writes, fsync, spool.json checkpoints and
// bundle publication run on a writer thread in submission order (#17).
class Recorder {
  struct Writer;
  fs::path root_, spool_;
  bool active_ = false;
  std::vector<float> rows_;
  Json checkpoint_, last_frame_;
  std::function<void(const fs::path &)> callback_;
  std::string policy_;
  LapBoundary lap_boundary_;
  std::size_t lap_start_ = 0;
  int next_lap_number_ = 1;
  bool upload_ = false;
  std::unique_ptr<Writer> writer_;
  void start(const Json &message);
  void write_frame(const Json &frame, bool substituted);
  void checkpoint();
  fs::path publish(const fs::path &spool, Json state,
                   const std::string &reason);

public:
  Recorder(fs::path root, std::string policy,
           std::function<void(const fs::path &)> callback = {});
  ~Recorder();
  void record(const Json &message);
  // Queues finalization and returns. With nothing recording, re-queues
  // publications that failed earlier.
  void finish(const std::string &reason = "ended");
  // Blocks until queued storage work is done; throws the first storage error
  // since the previous call.
  void wait_idle();
  void set_upload(bool enabled);
  Json status() const;
};

class Runtime {
  Config config_;
  mutable std::mutex mutex_;
  Json state_;
  Database store_;
  std::unique_ptr<ReplayGuard> replay_;
  Recorder recorder_;
  std::string source_, session_;
  double last_packet_ = 0, last_sample_ = 0, last_recording_packet_ = 0;
  // #15: last time a heartbeat proved the companion still considers the
  // recording open (state "driving" or "paused"), used by expire() to
  // distinguish a sim pause/menu/alt-tab gap from a lost connection.
  double last_recording_heartbeat_ = 0;
  bool recording_legacy_ = false;
  std::int64_t sequence_ = -1;
  int timing_lap_ = -1, best_lap_ = 0;
  std::vector<int> splits_;
  std::vector<std::string> paired_keys_;
  int best_sectors_[3]{};
  std::deque<std::pair<std::uint64_t, Json>> events_;
  // Per-stream (v4 run ID) sender-lag baseline (#17): the minimum observed
  // (pi_receive_monotonic - sender_monotonic) offset for the current stream,
  // which folds in the arbitrary clock-epoch difference between the two
  // monotonic clocks plus the best-case one-way transit. Reported lag is the
  // current offset above that baseline, so it starts at 0 and only ever
  // measures excess (queueing) delay -- see the comment on record_lag's
  // definition in native_runtime.cpp for what this method cannot see.
  std::string metrics_session_;
  double sender_lag_baseline_ = std::numeric_limits<double>::infinity();
  double last_sender_seconds_ = -std::numeric_limits<double>::infinity();
  double sender_lag_current_ms_ = 0;
  std::deque<double> sender_lag_ms_, process_ms_, lock_wait_ms_;
  // Lap-position (0..1) -> elapsed lap_time_ms trace for the AC1 delta (#20),
  // using the same boundary rule as the recorder (#16) so a lap that closes
  // for recording also closes for timing.
  LapBoundary lap_boundary_;
  std::vector<std::pair<double, double>> current_lap_trace_, best_lap_trace_;
  double best_lap_trace_duration_ = -1;
  std::uint64_t next_event_ = 1;
  void sectors(Json &frame);

public:
  explicit Runtime(Config config);
  // receipt_monotonic is the time udp_loop's recvfrom returned (#17); process_ms
  // and lock_wait_ms are measured from it. Defaults to monotonic() taken here
  // for callers (tests, other senders) with no better timestamp, which only
  // omits the negligible call-overhead gap.
  bool receive(const std::string &payload, const std::string &host,
               double receipt_monotonic = -1);
  void expire();
  void finish();
  Json snapshot() const;
  void upload(bool enabled);
  Json events(std::uint64_t &cursor, int history = 30) const;
  void power();
  void acc(int type, const Json &packet);
  void upload_state(const std::string &state);
  void replace_paired_keys(std::vector<std::string> keys);
};

void udp_loop(Runtime &runtime, const Config &config);
void acc_loop(Runtime &runtime, const Config &config);
void upload_loop(Runtime &runtime, const Config &config);
struct Response {
  int status = 200;
  std::string body;
  std::string type = "application/json";
  std::vector<std::pair<std::string, std::string>> headers{};
};
struct Request {
  std::string method, target, body;
  std::map<std::string, std::string> headers;
};
using Handler = std::function<Response(const Request &)>;
void serve(const std::string &host, int port, Handler handler,
           Runtime *runtime = nullptr);
void serve_tls(const std::string &host, int port, Handler handler,
               const fs::path &certificate, const fs::path &private_key);
} // namespace rapid::native
