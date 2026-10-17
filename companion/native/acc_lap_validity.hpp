#pragma once
#include "../../cpp/include/rapid/acc_lap_transition.hpp"
namespace rapid {
// The source and receivers use exactly the same physical reducer. Only raw
// versus contractually completed validity observation adapters differ.
// Feed this only after the shared-memory integrity check succeeds.
struct AccLapValidity {
  bool present = false, valid = false;
  native::AccLapTransition transition;
  void update(int lap, double timer_ms, double position, bool sim_valid) {
    native::AccLapTransition::Observation o;
    o.counter = lap; o.timer = timer_ms; o.position = position;
    // An integrity-accepted shared-memory read supplies all three fields;
    // unlike sparse receiver frames, none is an omitted numeric default.
    o.has_counter = o.has_timer = o.has_position = true;
    o.current_valid = sim_valid;
    const auto r = transition.step(o);
    if (r.restarted) present = false;
    if (r.closed) {
      const auto &field = transition.find(r.id)->raw_validity;
      present = field.status != native::AccLapTransition::Status::unknown;
      valid = field.value != 0;
    }
  }
};
} // namespace rapid
