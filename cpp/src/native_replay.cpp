// Batched, crash-safe v4 replay watermarks (#17 step 8).
//
// Before this, every accepted v4 packet committed its watermark to SQLite with
// synchronous=FULL on the receive path. Committing the exact watermark in
// batches instead would open a replay window after a crash: packets between
// the last durable watermark and the last accepted one would be accepted a
// second time.
//
// The guard therefore persists a *floor* rather than a watermark. A packet
// with sequence s is admitted only once the durable row already holds a floor
// >= s. The writer reserves floors `margin` packets ahead (at least every
// period, and immediately when the admitted sequence gets within margin/2 of
// the durable floor), so the receive path waits only if storage falls more
// than about margin packets behind. Every admitted packet is rejected by
// durable state at the moment it is admitted, whatever the crash point.
// A clean shutdown writes exact floors, so a planned restart loses no
// legitimate packets; a crash costs the legitimate sender at most `margin`.
#include "rapid/native.hpp"
#include <algorithm>
#include <chrono>

namespace rapid::native {
namespace {
using Seconds = std::chrono::duration<double>;
}

ReplayGuard::ReplayGuard(const fs::path &database, std::uint64_t margin,
                         double period_s, double wait_s)
    : db_(database), margin_(std::max<std::uint64_t>(margin, 2)),
      period_s_(period_s), wait_s_(wait_s) {
  db_.exec("CREATE TABLE IF NOT EXISTS v4_runs(id TEXT PRIMARY KEY,sequence "
           "INTEGER NOT NULL,time INTEGER NOT NULL,simulator INTEGER NOT "
           "NULL,metadata TEXT NOT NULL,closed INTEGER NOT NULL,active INTEGER "
           "NOT NULL)");
  for (const auto &row : db_.query("SELECT id,sequence,time,simulator,metadata,"
                                   "closed,active FROM v4_runs")) {
    Entry entry;
    entry.run = {row["sequence"].get<std::uint64_t>(),
                 row["time"].get<std::uint64_t>(),
                 int(row["simulator"].get<std::int64_t>()),
                 row["metadata"].get<std::string>(), row["closed"] == 1,
                 row["active"] == 1};
    entry.admitted = entry.persisted = true;
    entry.durable = entry.target = entry.run.sequence;
    entries_.emplace(row["id"].get<std::string>(), std::move(entry));
  }
  writer_ = std::thread([this] { loop(); });
}

ReplayGuard::~ReplayGuard() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  if (writer_.joinable())
    writer_.join();
}

std::optional<ReplayGuard::Run>
ReplayGuard::find(const std::string &id) const {
  std::lock_guard lock(mutex_);
  auto it = entries_.find(id);
  if (it == entries_.end() || !it->second.admitted)
    return std::nullopt;
  return it->second.run;
}

void ReplayGuard::admit(const std::string &id, const Run &run, bool new_run) {
  std::unique_lock lock(mutex_);
  if (stop_)
    throw ReplayDeferred("replay guard stopped");
  auto &entry = entries_[id];
  const auto sequence = run.sequence;
  if (new_run) {
    // Only one run is live at a time: a new run retires every open run, in
    // the same transaction that makes the new run's floor durable.
    for (auto &[other, candidate] : entries_)
      if (other != id && candidate.admitted && !candidate.run.closed) {
        candidate.run.closed = true;
        candidate.dirty = true;
        ++candidate.version;
      }
    entry.staged = run;
    entry.dirty = true;
    ++entry.version;
  }
  const bool behind = !entry.persisted || entry.durable < sequence;
  if (behind || sequence + margin_ / 2 > entry.durable) {
    entry.target = std::max(entry.target, sequence + margin_);
    entry.dirty = true;
    ++entry.version;
    urgent_ = true;
    wake_.notify_all();
  }
  // While storage is failing, reject at once instead of stalling every packet
  // for the full wait; the writer keeps retrying and clears the error.
  if (behind &&
      (!error_.empty() ||
       !durable_.wait_for(lock, Seconds(wait_s_), [&] {
         return stop_ || !error_.empty() ||
                (entry.persisted && entry.durable >= sequence);
       }))) {
    ++deferred_;
    throw ReplayDeferred("v4 replay floor not durable: " +
                         (error_.empty() ? std::string("storage slow")
                                         : error_));
  }
  if (!entry.persisted || entry.durable < sequence) {
    ++deferred_;
    throw ReplayDeferred(error_.empty() ? "replay guard stopped" : error_);
  }
  entry.run = run;
  entry.staged.reset();
  entry.admitted = true;
  entry.dirty = true;
  ++entry.version;
}

Json ReplayGuard::status() const {
  std::lock_guard lock(mutex_);
  return {{"replay_commits", commits_},
          {"packets_deferred", deferred_},
          {"replay_write_error", error_.empty() ? Json() : Json(error_)}};
}

void ReplayGuard::loop() {
  std::unique_lock lock(mutex_);
  while (!stop_) {
    wake_.wait_for(lock, Seconds(period_s_), [&] { return stop_ || urgent_; });
    if (stop_)
      break;
    urgent_ = false;
    bool dirty = false;
    for (const auto &[id, entry] : entries_)
      dirty = dirty || entry.dirty;
    if (!dirty)
      continue;
    lock.unlock();
    const bool ok = commit(false);
    lock.lock();
    if (!ok)
      wake_.wait_for(lock, Seconds(0.5), [&] { return stop_; });
  }
  durable_.notify_all();
  lock.unlock();
  // Nothing can be admitted any more: exact floors lose no legitimate packet
  // on a planned restart and stay >= every admitted sequence.
  commit(true);
}

bool ReplayGuard::commit(bool exact) {
  struct Row {
    std::string id;
    Run run;
    std::uint64_t floor, version;
    bool lowered;
  };
  std::vector<Row> rows;
  {
    std::lock_guard lock(mutex_);
    for (const auto &[id, entry] : entries_) {
      const bool exact_row = exact && entry.admitted && !entry.staged;
      if (!entry.dirty && !(exact_row && entry.durable != entry.run.sequence))
        continue;
      const Run &run = entry.staged ? *entry.staged : entry.run;
      auto floor = std::max({entry.durable, entry.target, run.sequence});
      if (exact_row)
        floor = entry.run.sequence;
      rows.push_back({id, run, floor, entry.version, exact_row});
    }
  }
  if (rows.empty())
    return true;
  try {
    db_.exec("BEGIN IMMEDIATE");
    try {
      for (const auto &row : rows)
        db_.exec("INSERT INTO v4_runs VALUES(?,?,?,?,?,?,?) ON CONFLICT(id) DO "
                 "UPDATE SET sequence=excluded.sequence,time=excluded.time,"
                 "metadata=excluded.metadata,closed=excluded.closed",
                 {row.id, row.floor, row.run.time, row.run.simulator,
                  row.run.metadata, int(row.run.closed), int(row.run.active)});
      db_.exec("COMMIT");
    } catch (...) {
      try {
        db_.exec("ROLLBACK");
      } catch (...) {
      }
      throw;
    }
  } catch (const std::exception &e) {
    std::lock_guard lock(mutex_);
    if (error_.empty())
      log(std::string("ERROR v4 replay state not persisted: ") + e.what());
    error_ = e.what();
    durable_.notify_all();
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    if (!error_.empty())
      log("v4 replay state persistence recovered");
    error_.clear();
    ++commits_;
    for (const auto &row : rows) {
      auto &entry = entries_[row.id];
      entry.persisted = true;
      entry.durable = row.lowered ? row.floor : std::max(entry.durable, row.floor);
      if (entry.version == row.version)
        entry.dirty = false;
    }
  }
  durable_.notify_all();
  return true;
}
} // namespace rapid::native
