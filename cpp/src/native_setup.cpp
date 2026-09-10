#include "rapid/setup.hpp"
#include <sys/stat.h>
#include <unistd.h>

namespace rapid::native {
namespace {
fs::path private_database(const fs::path &directory) {
  if (directory.empty())
    throw std::runtime_error("setup directory must not be empty");
  // The parent is an installation concern. Refuse symlinked paths rather than
  // inadvertently creating or changing permissions on somebody else's data.
  auto absolute = fs::absolute(directory);
  for (auto part = absolute; !part.empty(); part = part.parent_path()) {
    if (fs::is_symlink(fs::symlink_status(part)))
      throw std::runtime_error("setup directory must not contain symlinks");
    if (part == part.parent_path())
      break;
  }
  if (fs::create_directory(absolute)) {
    if (::chmod(absolute.c_str(), 0700) != 0)
      throw std::runtime_error("cannot secure setup directory");
    sync_file(absolute.parent_path());
  }
  struct stat info {};
  if (::lstat(absolute.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
      info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0)
    throw std::runtime_error("setup directory must be owned by the service user and mode 0700");
  // SQLite creates sidecars. All must remain beneath the private directory.
  for (const auto *name : {"setup.db", "setup.db-wal", "setup.db-shm", "setup.db-journal"}) {
    auto file = absolute / name;
    auto state = fs::symlink_status(file);
    if (fs::exists(state) &&
        (!fs::is_regular_file(state) || fs::hard_link_count(file) != 1))
      throw std::runtime_error("setup database must be a regular, unlinked file");
  }
  return absolute / "setup.db";
}

void validate_settings(const Json &value) {
  if (!value.is_object() || value.size() != 3 ||
      !value.contains("hostname") || !value["hostname"].is_string() ||
      !value.contains("rotation") || !value["rotation"].is_number_integer() ||
      !value.contains("boot_network") || value["boot_network"] != "home_then_ap")
    throw std::invalid_argument("expected hostname, landscape rotation and home_then_ap boot policy");
  const auto host = value["hostname"].get<std::string>();
  auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
  if (host.empty() || host.size() > 63 || !alnum(host.front()) || !alnum(host.back()))
    throw std::invalid_argument("invalid hostname");
  for (char c : host)
    if (!alnum(c) && c != '-')
      throw std::invalid_argument("invalid hostname");
  if (value["rotation"] != 0 && value["rotation"] != 180)
    throw std::invalid_argument("rotation must be 0 or 180");
}
} // namespace

SetupStore::SetupStore(const fs::path &directory)
    : store_(private_database(directory)) {
  store_.exec("BEGIN IMMEDIATE");
  try {
    const auto version = store_.query("PRAGMA user_version").at(0).at("user_version").get<int>();
    if (version == 0) {
      store_.exec("CREATE TABLE setup_state (id INTEGER PRIMARY KEY CHECK(id=1), "
                  "revision INTEGER NOT NULL CHECK(revision>0), document TEXT NOT NULL)");
      const auto id = unique_id();
      Json document = {{"device_id", id}, {"setup_complete", false},
                       {"settings", {{"hostname", "rapid-" + id.substr(0, 6)},
                                     {"rotation", 0}, {"boot_network", "home_then_ap"}}}};
      store_.exec("INSERT INTO setup_state VALUES(1,1,?)", {document.dump()});
      store_.exec("PRAGMA user_version=1");
    } else if (version != 1 && version != 2) {
      throw std::runtime_error("unsupported setup schema; use a compatible application version");
    }
    if (version < 2) {
      store_.exec("CREATE TABLE setup_owner (id INTEGER PRIMARY KEY CHECK(id=1), password_hash TEXT NOT NULL)");
      store_.exec("PRAGMA user_version=2");
    }
    // Reject corrupt state, preserving it for recovery instead of reinitializing
    // identity or treating the device as unowned.
    const auto state = snapshot_unlocked();
    validate_settings(state.at("settings"));
    const auto id = state.at("device_id").get<std::string>();
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos ||
        !state.at("setup_complete").is_boolean() || state.at("revision").get<std::int64_t>() < 1)
      throw std::runtime_error("invalid setup state");
    store_.exec("COMMIT");
  } catch (...) {
    store_.exec("ROLLBACK");
    throw;
  }
}

Json SetupStore::snapshot_unlocked() {
  const auto row = store_.query("SELECT revision,document FROM setup_state WHERE id=1");
  if (row.size() != 1)
    throw std::runtime_error("missing setup state");
  auto document = Json::parse(row[0].at("document").get<std::string>());
  if (!document.is_object())
    throw std::runtime_error("invalid setup document");
  document["schema_version"] = 2;
  document["revision"] = row[0].at("revision");
  return document;
}

Json SetupStore::snapshot() {
  std::lock_guard lock(mutex_);
  return snapshot_unlocked();
}

std::string SetupStore::owner_hash() {
  std::lock_guard lock(mutex_);
  const auto rows = store_.query("SELECT password_hash FROM setup_owner WHERE id=1");
  return rows.empty() ? "" : rows[0].at("password_hash").get<std::string>();
}

bool SetupStore::claim_owner(const std::string &password_hash) {
  if (!password_hash.starts_with("$argon2id$") || password_hash.size() > 512)
    throw std::invalid_argument("invalid owner password hash");
  std::lock_guard lock(mutex_);
  store_.exec("INSERT OR IGNORE INTO setup_owner VALUES(1,?)", {password_hash});
  return store_.query("SELECT changes() AS count")[0]["count"] == 1;
}

bool SetupStore::update(std::int64_t expected_revision, const Json &settings) {
  validate_settings(settings);
  std::lock_guard lock(mutex_);
  store_.exec("BEGIN IMMEDIATE");
  try {
    auto document = snapshot_unlocked();
    if (document["revision"] != expected_revision) {
      store_.exec("ROLLBACK");
      return false;
    }
    document.erase("revision");
    document.erase("schema_version");
    document["settings"] = settings;
    store_.exec("UPDATE setup_state SET revision=revision+1, document=? WHERE id=1",
                {document.dump()});
    store_.exec("COMMIT");
    return true;
  } catch (...) {
    store_.exec("ROLLBACK");
    throw;
  }
}

Json setup_status(const Json &snapshot) {
  return {{"available", true}, {"schema_version", snapshot.at("schema_version")},
          {"revision", snapshot.at("revision")}, {"device_id", snapshot.at("device_id")},
          {"setup_complete", snapshot.at("setup_complete")},
          {"capabilities", {{"settings_write", false}, {"network_setup", false},
                            {"display_calibration", false}, {"pairing", false}}}};
}
} // namespace rapid::native
