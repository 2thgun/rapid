#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>

namespace rapid::native {
// Accepted observations only. This reducer has no clocks, IO or wire effects.
// IDs identify local evidence, NOT simulator completion generations.
struct AccLapTransition {
  enum class Phase { uninitialized, driving, transition, discontinuity };
  enum class Timer { established, wait_reset };
  enum class Status { unknown, provisional, inferred, authoritative, conflict };
  struct Observation {
    int counter = 0;
    double timer = 0, position = 0;
    bool has_position = true, gap = false, has_timer = true, has_counter = true;
    int completed = 0;
    std::optional<bool> completed_valid;
    std::optional<bool> current_valid;
    // Optional validated source attribution, never synthesized from wire IDs.
    std::optional<std::uint64_t> current_valid_lap_id;
  };
  struct Field {
    double value = 0;
    Status status = Status::unknown;
    const char *source = "absent";
    std::uint64_t observation = 0;
  };
  struct Event {
    std::uint64_t epoch = 0, id = 0, first = 0, last = 0;
    bool position_wrap = false, timer_reset = false, counter_seen = false;
    bool estimated_boundary = false, mapping_ambiguous = false;
    int counter = -1;
    double timer_peak = 0;
    Field time, validity, raw_validity, time_candidate;
    int preceding_completed = 0;
    bool time_changed = false;
  };
  struct Result { bool closed = false, restarted = false; std::uint64_t id = 0; };
  static constexpr std::size_t retention = 64;
  Phase phase = Phase::uninitialized;
  Timer timer_phase = Timer::established;
  std::uint64_t epoch = 0, lap_id = 1, sequence = 0, missing_completions = 0;
  double peak = 0;
  bool progress = false, end_seen = false;
  std::deque<Event> events;
  std::deque<std::uint64_t> counter_debt;

  Event *find(std::uint64_t id) {
    for (auto &event : events) if (event.id == id) return &event;
    return nullptr;
  }
  static const char *status_name(Status s) {
    switch (s) {
      case Status::provisional: return "provisional";
      case Status::inferred: return "inferred";
      case Status::authoritative: return "authoritative";
      case Status::conflict: return "conflict";
      default: return "unknown";
    }
  }
  // Exact-time ranking requires field-generation evidence. Existing ACC wire
  // does not carry it, even when a numeric candidate changes.
  static bool rankable(const Event &e) {
    return !e.estimated_boundary && !e.mapping_ambiguous &&
           e.time.status == Status::authoritative &&
           e.validity.status == Status::authoritative && e.validity.value == 1;
  }
  Result step(const Observation &o) {
    Result result;
    if (o.has_counter && last_has_counter_ && o.counter < last_counter_) {
      const auto next_epoch = epoch + 1;
      *this = AccLapTransition{};
      epoch = next_epoch;
      result.restarted = true;
    }
    if (phase == Phase::discontinuity) phase = Phase::driving;
    const auto at = sequence++;
    if (phase == Phase::uninitialized) {
      phase = Phase::driving;
      peak = std::max(0.0, o.timer);
      observe_validity(o, false);
      remember(o);
      return result;
    }
    const bool reset = o.has_timer && last_has_timer_ && o.timer + 1 < last_timer_;
    const bool wrap = o.has_position && last_position_ >= .9 && o.position <= .1;
    const int increment = o.has_counter && last_has_counter_ ? std::max(0, o.counter - last_counter_) : 0;
    // Acknowledgments are consumed BEFORE considering independent new
    // physical evidence. Multiple unresolved crossings are not a boolean.
    int remaining = increment;
    const auto expired_ack = std::min<std::uint64_t>(remaining, expired_counter_debt);
    remaining -= int(expired_ack); expired_counter_debt -= expired_ack;
    std::uint64_t mapped = 0;
    while (remaining && !counter_debt.empty()) {
      auto id = counter_debt.front(); counter_debt.pop_front(); --remaining;
      if (auto *e = find(id)) {
        e->counter_seen = true;
        e->counter = o.counter - remaining;
        e->mapping_ambiguous = increment > 1;
        mapped = id;
      }
    }
    // Position and timer evidence can arrive in either order. Retain them
    // until the same generation transitions; a correction at mid-lap is not
    // a crossing. A gap reset after observed end-of-lap is an estimated one.
    if (progress && wrap) wrap_pending_ = true;
    if (progress && reset && end_seen) {
      reset_pending_ = true;
      reset_seed_ = std::max(0.0, o.timer);
    } else if (reset_pending_ && o.has_timer) reset_seed_ = std::max(reset_seed_, o.timer);
    // With ordinary forward-running timer semantics, end-to-early progress
    // plus a reset can support a crossing hidden by a sampling interval. It
    // remains ESTIMATED: a teleport with identical fields is not identifiable.
    const bool interval_gap = o.gap || (o.has_position && o.position < .3 && o.position < last_position_);
    const bool gap_crossing = progress && end_seen && reset && !wrap_pending_ && interval_gap;
    // An armed observed wrap fixes the physical sample boundary now. Timer
    // reset can join later; its ownership stays quarantined in the meantime.
    const bool physical = progress && (wrap_pending_ || gap_crossing);
    const bool counter_close = remaining > 0 && !joining_transition_;
    if (physical || counter_close) {
      if (o.has_timer && !reset_pending_ && !reset && timer_phase == Timer::established)
        peak = std::max(peak, o.timer);
      Event e;
      e.epoch = epoch; e.id = lap_id++; e.first = last_observation_; e.last = at;
      e.position_wrap = wrap_pending_; e.timer_reset = reset_pending_ || reset;
      e.estimated_boundary = !physical || gap_crossing;
      e.timer_peak = peak; e.preceding_completed = last_completed_;
      e.time = {peak, Status::provisional, "current_lap_timer", at};
      e.raw_validity = driven_validity();
      current_validity_ = Validity{};
      if (remaining) {
        e.counter_seen = true; e.counter = o.counter;
        e.mapping_ambiguous = remaining > 1;
        missing_completions += remaining - 1;
        mapped = e.id;
      } else counter_debt.push_back(e.id);
      events.push_back(e);
      if (events.size() > retention) {
        const auto expired = events.front().id;
        events.pop_front();
        if (std::find(counter_debt.begin(),counter_debt.end(),expired) != counter_debt.end()) {
          counter_debt.erase(std::remove(counter_debt.begin(), counter_debt.end(), expired), counter_debt.end());
          ++expired_counter_debt;
        }
        ++expired_records;
      }
      result.closed = true; result.id = e.id;
      phase = Phase::transition;
      timer_phase = e.timer_reset ? Timer::established : Timer::wait_reset;
      peak = e.timer_reset ? (reset_pending_ ? reset_seed_ : std::max(0.0, o.timer)) : 0;
      progress = end_seen = wrap_pending_ = reset_pending_ = false;
      joining_transition_ = true;
    } else if (joining_transition_) {
      missing_completions += remaining;
      if (!events.empty()) {
        auto &e = events.back();
        e.position_wrap |= wrap;
        e.timer_reset |= reset;
      }
      if (timer_phase == Timer::wait_reset && reset) {
        timer_phase = Timer::established;
        peak = std::max(0.0, o.timer);
      }
    } else if (reset && !reset_pending_) {
      // Timer correction / teleport / restart without crossing support.
      phase = Phase::discontinuity;
      peak = std::max(0.0, o.timer);
      progress = end_seen = wrap_pending_ = reset_pending_ = false;
    }
    if (o.has_timer && timer_phase == Timer::established && !reset_pending_) peak = std::max(peak, o.timer);
    if (((o.has_position && o.position > .3 && o.position < .7) ||
         (!o.has_position && o.has_timer && last_has_timer_ && o.timer > last_timer_)) &&
        timer_phase == Timer::established && phase != Phase::discontinuity) {
      progress = true;
      joining_transition_ = false;
      phase = Phase::driving;
    }
    if (progress && o.has_position && o.position >= .9) end_seen = true;
    // Contractual completed validity is independent of timing freshness.
    // Held bits may update the uniquely mapped latest acknowledgment only.
    if (mapped) acknowledged_ = mapped;
    if (auto *e = find(acknowledged_); e && e->id == lap_id - 1) {
      if (o.completed_valid)
        e->validity = {double(*o.completed_valid), Status::authoritative, "wire_completed_validity", at};
      if (o.completed > 0) {
        e->time_candidate = {double(o.completed), Status::provisional, "held_completed_time", at};
        e->time_changed |= o.completed != e->preceding_completed;
        if (e->time_changed) {
          e->time = {double(o.completed), Status::inferred, "simulator", at};
        } else if (e->id == 1 && o.completed >= e->timer_peak) {
          // Preserve the legacy joined-epoch held numeric presentation only
          // for its first completion. Later held predecessors remain separate
          // candidates; their values never displace the owned timer estimate.
          e->time = {double(o.completed), Status::provisional, "held_completed_time", at};
        }
      }
    } else if (events.size() == 1 && o.completed > 0 &&
               o.completed != events.back().preceding_completed) {
      // Candidate arriving before acknowledgment remains repairable; no
      // value is committed to a segment until counter mapping is available.
      events.back().time_changed = true;
    }
    observe_validity(o, result.closed);
    remember(o);
    return result;
  }
  std::uint64_t expired_records = 0, expired_counter_debt = 0;
private:
  struct Validity {
    bool observed = false, invalid = false, ambiguous = false, phase_known = false, gap = false;
    bool tagged = false, covered = false;
    std::uint64_t observation = 0;
  } current_validity_;
  Field driven_validity() const {
    const auto &v = current_validity_;
    if (v.invalid) return {0, v.tagged ? Status::authoritative : Status::inferred, "same_generation_invalidity", v.observation};
    if (!v.observed || v.ambiguous || v.gap || !v.covered)
      return {0, Status::unknown, v.ambiguous ? "ambiguous_boundary_flag" : "sampling_coverage", v.observation};
    return {1, v.tagged ? Status::authoritative : Status::inferred, "observed_current_validity", v.observation};
  }
  void observe_validity(const Observation &o, bool boundary) {
    auto &v = current_validity_;
    v.gap |= o.gap;
    if (!o.current_valid) return;
    v.observed = true; v.observation = sequence - 1;
    const bool tagged = o.current_valid_lap_id && *o.current_valid_lap_id == lap_id;
    if (tagged) {
      v.tagged = true; v.phase_known = true; v.ambiguous = false; v.covered = true;
      if (!*o.current_valid) v.invalid = true;
    } else if (reset_pending_) {
      // Timer has entered another generation before position/counter join.
      // Neither true nor false here establishes ownership for the old lap.
      v.ambiguous = true;
    } else if (boundary) {
      if (*o.current_valid) { v.phase_known = true; v.covered = true; }
      else v.ambiguous = true;
    } else if (*o.current_valid) {
      // A true may show a reset, but cannot erase an ambiguous earlier false.
      v.phase_known = true;
      if (sequence == 1 && o.has_position && o.position <= .1) v.covered = true;
    } else if (v.phase_known || (o.has_position && o.position > .3 && o.position < .9 && timer_phase == Timer::established)) {
      v.invalid = true;
    } else v.ambiguous = true;
  }
  int last_counter_ = 0, last_completed_ = 0;
  double last_timer_ = 0, last_position_ = -1, reset_seed_ = 0;
  std::uint64_t last_observation_ = 0, acknowledged_ = 0;
  bool wrap_pending_ = false, reset_pending_ = false, joining_transition_ = false, last_has_timer_ = false, last_has_counter_ = false;
  void remember(const Observation &o) {
    // Sparse counter omission supplies no evidence and cannot replace the
    // last observed baseline. A first present counter only establishes it.
    if (o.has_counter) { last_counter_ = o.counter; last_has_counter_ = true; }
    if (o.has_timer) { last_timer_ = o.timer; last_has_timer_ = true; }
    if (o.has_position) last_position_ = o.position;
    last_completed_ = o.completed; last_observation_ = sequence - 1;
  }
};
} // namespace rapid::native
