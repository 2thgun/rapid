#pragma once
#include <atomic>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <string>
#include <stdexcept>
#include <vector>

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
Json receive_v4(Database &store, const std::string &payload,
                const std::string &key);

struct Config {
  std::string host = "0.0.0.0", companion_host, acc_host = "192.168.1.89",
              acc_password, companion_key;
  std::string display_name = "raPId", upload_url, upload_token,
              upload_policy = "races";
  int port = 8000, companion_port = 9001, acc_port = 9000,
      acc_local_port = 9000, protocol_version = 4, interval_ms = 100;
  bool acc_enabled = false, upload_enabled = false;
  fs::path database = "data/rapid.db", telemetry = "data/telemetry",
           queue = "data/upload-queue.db", assets = "cpp/assets";
  static Config load(const fs::path &path);
};

class Recorder {
  fs::path root_, spool_;
  std::vector<FILE *> streams_;
  Json checkpoint_, last_frame_, last_bundle_;
  std::function<void(const fs::path &)> callback_;
  std::string policy_;
  int last_lap_ = -1;
  std::size_t lap_start_ = 0;
  bool upload_ = false;
  void start(const Json &message);
  void write_frame(const Json &frame, bool substituted);
  void checkpoint();
  void close();
  fs::path publish(const fs::path &spool, Json state,
                   const std::string &reason);

public:
  Recorder(fs::path root, std::string policy,
           std::function<void(const fs::path &)> callback = {});
  ~Recorder();
  void record(const Json &message);
  void finish(const std::string &reason = "ended");
  void set_upload(bool enabled);
  Json status() const;
};

class Runtime {
  Config config_;
  mutable std::mutex mutex_;
  Json state_;
  Database store_;
  Recorder recorder_;
  std::string source_, session_;
  double last_packet_ = 0;
  std::int64_t sequence_ = -1;
  int timing_lap_ = -1, best_lap_ = 0;
  std::vector<int> splits_;
  int best_sectors_[3]{};
  std::deque<std::pair<std::uint64_t, Json>> events_;
  std::uint64_t next_event_ = 1;
  void sectors(Json &frame);

public:
  explicit Runtime(Config config);
  bool receive(const std::string &payload, const std::string &host);
  void expire();
  void finish();
  Json snapshot() const;
  void upload(bool enabled);
  Json events(std::uint64_t &cursor, int history = 30) const;
  void power();
  void acc(int type, const Json &packet);
  void upload_state(const std::string &state);
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
} // namespace rapid::native
