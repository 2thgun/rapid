#pragma once
// Shared lap-boundary rule for AC1/ACE/iRacing telemetry that does not
// reliably increment its lap counter at the crossing.
//
// Both the recorder (native_recorder.cpp, which persists lap segments) and
// the live timing/delta code (native_runtime.cpp, which drives sector splits
// and the AC1 delta) must treat the same telemetry sample as the same lap
// boundary, or the saved laps and the on-screen timing disagree. This header
// is the one place that decides "a lap just closed" so both call sites stay
// in sync (#16, #20).
//
// A lap closes when either is true:
//   (a) the sim's lap_number increases, or
//   (b) current_lap_ms resets (drops sharply) at the same time lap_position
//       wraps from the end of the lap back toward the start -- the case
//       AC1's own lap_number sometimes misses entirely.
//
// The wrap path only "arms" once the car has been seen away from the line
// (lap_position in the middle third of the lap), and every close -- from
// either path -- starts a short cooldown measured in samples. Together these
// stop a flicker at the line, or the car reversing back over it, from
// manufacturing more than one lap out of a single crossing: real telemetry
// (session-4e6de929..., sample 124782-124783) shows the wrap signal and the
// lap_number increment for one physical crossing arrive a sample apart, and
// without the cooldown both would independently close a lap.
namespace rapid::native {

struct LapBoundary {
  struct Result {
    // True when this sample closed a lap.
    bool closed = false;
    // True when the close was triggered by lap_number increasing (the sim's
    // own signal); false when it came from the current_lap_ms reset +
    // lap_position wrap heuristic (the sim never ticked lap_number).
    bool via_lap_number = false;
  };

  // Feed one telemetry sample. lap_position is a fraction in [0, 1]; pass
  // has_position=false when the sample carries no lap_position field.
  // Thresholds are fractions of the lap; defaults were validated against the
  // 2026-09-13 AC1 recordings named in #16 and #20.
  Result step(int lap_number, double lap_time_ms, double lap_position,
              bool has_position, double wrap_high = 0.9, double wrap_low = 0.1,
              double mid_low = 0.3, double mid_high = 0.7,
              long long min_gap_samples = 10) {
    Result result;
    const long long index = sample_index_++;
    if (!started_) {
      started_ = true;
      last_lap_number_ = lap_number;
      last_lap_time_ms_ = lap_time_ms;
      if (has_position)
        last_lap_position_ = lap_position;
      return result;
    }
    if (has_position && lap_position > mid_low && lap_position < mid_high)
      armed_ = true;
    const bool number_increment = lap_number > last_lap_number_;
    const bool time_reset = lap_time_ms + 1.0 < last_lap_time_ms_;
    const bool position_wrap = has_position && last_lap_position_ >= 0 &&
                                last_lap_position_ >= wrap_high &&
                                lap_position <= wrap_low;
    const bool cooldown_elapsed =
        index - last_close_index_ >= min_gap_samples;
    if (cooldown_elapsed &&
        (number_increment || (armed_ && time_reset && position_wrap))) {
      result.closed = true;
      result.via_lap_number = number_increment;
      armed_ = false;
      last_close_index_ = index;
    }
    last_lap_number_ = lap_number;
    last_lap_time_ms_ = lap_time_ms;
    if (has_position)
      last_lap_position_ = lap_position;
    return result;
  }

  int last_lap_number() const { return last_lap_number_; }

private:
  bool started_ = false;
  // Starts disarmed: a session that begins right at the line and reverses
  // once before actually driving off must not manufacture a lap out of that
  // reversal. The wrap path only arms once the car is actually seen mid-lap.
  bool armed_ = false;
  int last_lap_number_ = -1;
  double last_lap_time_ms_ = -1;
  double last_lap_position_ = -1;
  long long sample_index_ = 0;
  long long last_close_index_ = -1000000;
};

} // namespace rapid::native
