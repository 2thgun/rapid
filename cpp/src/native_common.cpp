#include "rapid/native.hpp"
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sstream>
#include <toml++/toml.h>
#include <unistd.h>

namespace rapid::native {
std::atomic_bool stopping = false;
std::string now() {
  const auto t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char out[32];
  std::strftime(out, sizeof out, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return out;
}
double monotonic() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
std::string unique_id() {
  unsigned char bytes[16];
  if (RAND_bytes(bytes, sizeof bytes) != 1)
    throw std::runtime_error("random generator failed");
  std::ostringstream out;
  for (auto b : bytes)
    out << std::hex << std::setw(2) << std::setfill('0') << int(b);
  return out.str();
}
std::string env(const std::string &name, const std::string &fallback) {
  const char *p = std::getenv(name.c_str());
  return p ? p : fallback;
}
void log(const std::string &message) {
  static std::mutex m;
  std::lock_guard lock(m);
  std::cerr << now() << " " << message << std::endl;
}
std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot read " + path.string());
  return {std::istreambuf_iterator<char>(in), {}};
}
void sync_file(const fs::path &path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    throw std::runtime_error("cannot sync " + path.string());
  int result = fsync(fd);
  ::close(fd);
  if (result)
    throw std::runtime_error("fsync failed");
}
void atomic_file(const fs::path &path, const std::string &data) {
  fs::create_directories(path.parent_path());
  auto temp = path;
  temp += "." + unique_id() + ".part";
  std::ofstream out(temp, std::ios::binary);
  out.exceptions(std::ios::badbit | std::ios::failbit);
  out.write(data.data(), data.size());
  out.close();
  sync_file(temp);
  fs::rename(temp, path);
  sync_file(path.parent_path());
}
static std::string digest(EVP_MD_CTX *ctx) {
  unsigned char out[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  EVP_DigestFinal_ex(ctx, out, &length);
  EVP_MD_CTX_free(ctx);
  std::ostringstream text;
  for (unsigned int i = 0; i < length; ++i)
    text << std::hex << std::setw(2) << std::setfill('0') << int(out[i]);
  return text.str();
}
std::string hash_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot hash file");
  auto ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  char data[65536];
  while (in) {
    in.read(data, sizeof data);
    EVP_DigestUpdate(ctx, data, in.gcount());
  }
  return digest(ctx);
}
std::string hash_text(const std::string &data) {
  auto ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  EVP_DigestUpdate(ctx, data.data(), data.size());
  return digest(ctx);
}
std::string safe_name(std::string value) {
  for (auto &c : value)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
      c = '_';
  if (value.empty())
    value = "unknown";
  return value.substr(0, 100);
}
double number(const Json &j, const std::string &key, double fallback) {
  if (!j.contains(key) || !j[key].is_number())
    return fallback;
  double v = j[key].get<double>();
  return std::isfinite(v) ? v : fallback;
}
std::string string(const Json &j, const std::string &key,
                   const std::string &fallback) {
  return j.contains(key) && j[key].is_string() ? j[key].get<std::string>()
                                               : fallback;
}
Database::Database(const fs::path &path) {
  if (!path.parent_path().empty())
    fs::create_directories(path.parent_path());
  if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
    throw std::runtime_error("database open failed");
  sqlite3_busy_timeout(db_, 5000);
  exec("PRAGMA journal_mode=WAL");
  exec("PRAGMA synchronous=FULL");
  exec("PRAGMA foreign_keys=ON");
}
Database::~Database() { sqlite3_close(db_); }
Json Database::query(const std::string &sql, const Json &args) {
  sqlite3_stmt *raw = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db_));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(
      raw, sqlite3_finalize);
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto &a = args[i];
    int k = int(i + 1);
    if (a.is_null())
      sqlite3_bind_null(raw, k);
    else if (a.is_boolean())
      sqlite3_bind_int(raw, k, a.get<bool>());
    else if (a.is_number_integer())
      sqlite3_bind_int64(raw, k, a.get<sqlite3_int64>());
    else if (a.is_number())
      sqlite3_bind_double(raw, k, a.get<double>());
    else {
      auto s = a.is_string() ? a.get<std::string>() : a.dump();
      sqlite3_bind_text(raw, k, s.c_str(), int(s.size()), SQLITE_TRANSIENT);
    }
  }
  Json rows = Json::array();
  int result;
  while ((result = sqlite3_step(raw)) == SQLITE_ROW) {
    Json row = Json::object();
    for (int i = 0; i < sqlite3_column_count(raw); ++i) {
      auto name = sqlite3_column_name(raw, i);
      switch (sqlite3_column_type(raw, i)) {
      case SQLITE_INTEGER:
        row[name] = sqlite3_column_int64(raw, i);
        break;
      case SQLITE_FLOAT:
        row[name] = sqlite3_column_double(raw, i);
        break;
      case SQLITE_NULL:
        row[name] = nullptr;
        break;
      default:
        row[name] = reinterpret_cast<const char *>(sqlite3_column_text(raw, i));
        break;
      }
    }
    rows.push_back(row);
  }
  if (result != SQLITE_DONE)
    throw std::runtime_error(sqlite3_errmsg(db_));
  return rows;
}
void Database::exec(const std::string &sql, const Json &args) {
  query(sql, args);
}
Config Config::load(const fs::path &path) {
  Config c;
  toml::table t;
  if (fs::exists(path))
    t = toml::parse_file(path.string());
  auto text = [&](const char *section, const char *key, const char *variable,
                  std::string fallback) {
    return env(variable, t[section][key].value_or(fallback));
  };
  auto integer = [&](const char *section, const char *key, const char *variable,
                     int fallback) {
    return std::stoi(
        env(variable, std::to_string(t[section][key].value_or(fallback))));
  };
  auto boolean = [&](const char *section, const char *key, const char *variable,
                     bool fallback) {
    auto s =
        env(variable, t[section][key].value_or(fallback) ? "true" : "false");
    for (auto &ch : s)
      ch = std::tolower(static_cast<unsigned char>(ch));
    if (s == "true" || s == "1" || s == "yes" || s == "on")
      return true;
    if (s == "false" || s == "0" || s == "no" || s == "off")
      return false;
    throw std::runtime_error("invalid boolean configuration");
  };
  c.host = text("app", "host", "RAPID_APP_HOST", c.host);
  c.port = integer("app", "port", "RAPID_APP_PORT", c.port);
  c.companion_port = integer("app", "companion_port", "RAPID_COMPANION_PORT",
                             c.companion_port);
  c.companion_host = text("app", "companion_host", "RAPID_COMPANION_HOST", "");
  c.companion_key = telemetry_key(text("app", "companion_key", "RAPID_COMPANION_KEY", ""));
  if (boolean("app", "require_v4", "RAPID_REQUIRE_V4", false) && c.companion_key.empty())
    throw std::runtime_error("authenticated v4 key required before runtime activation");
  c.database =
      text("app", "database_path", "RAPID_DATABASE_PATH", c.database.string());
  c.telemetry = text("app", "telemetry_directory", "RAPID_TELEMETRY_DIRECTORY",
                     c.telemetry.string());
  c.assets = text("app", "assets_directory", "RAPID_ASSETS_DIRECTORY",
                  c.assets.string());
  c.network_control = text("app", "network_control_directory",
                           "RAPID_NETWORK_CONTROL_DIRECTORY",
                           c.network_control.string());
  c.setup_directory = text("setup", "state_directory", "RAPID_SETUP_STATE_DIRECTORY", "");
  c.acc_enabled = boolean("acc", "enabled", "RAPID_ACC_ENABLED", false);
  c.acc_host = text("acc", "host", "RAPID_ACC_HOST", c.acc_host);
  c.acc_port = integer("acc", "port", "RAPID_ACC_PORT", c.acc_port);
  c.acc_local_port =
      integer("acc", "local_port", "RAPID_ACC_LOCAL_PORT", c.acc_local_port);
  c.acc_password = text("acc", "password", "RAPID_ACC_PASSWORD", "");
  c.display_name =
      text("acc", "display_name", "RAPID_DISPLAY_NAME", c.display_name);
  c.protocol_version =
      integer("acc", "protocol_version", "RAPID_PROTOCOL_VERSION", 4);
  c.interval_ms =
      integer("acc", "update_interval_ms", "RAPID_UPDATE_INTERVAL_MS", 100);
  c.upload_enabled =
      boolean("upload", "enabled", "RAPID_UPLOAD_ENABLED", false);
  c.upload_url = text("upload", "url", "RAPID_UPLOAD_URL", "");
  c.upload_token = text("upload", "token", "RAPID_UPLOAD_TOKEN", "");
  while (!c.upload_url.empty() && c.upload_url.back() == '/')
    c.upload_url.pop_back();
  c.upload_policy = text("upload", "policy", "RAPID_UPLOAD_POLICY", "races");
  c.queue =
      text("upload", "queue_path", "RAPID_UPLOAD_QUEUE_PATH", c.queue.string());
  for (auto port : {c.port, c.companion_port, c.acc_port, c.acc_local_port})
    if (port < 1 || port > 65535)
      throw std::runtime_error("port out of range");
  if (c.upload_policy != "races" && c.upload_policy != "all" &&
      c.upload_policy != "manual")
    throw std::runtime_error("invalid upload policy");
  if (c.upload_enabled && (c.upload_url.empty() || c.upload_token.empty()))
    throw std::runtime_error("upload enabled without URL/token");
  return c;
}
} // namespace rapid::native
