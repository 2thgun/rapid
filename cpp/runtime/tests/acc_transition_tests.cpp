#include "rapid/acc_lap_transition.hpp"
#include "../../../companion/native/acc_lap_validity.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <functional>
#include <vector>
using R = rapid::native::AccLapTransition;
void check(bool ok, const char *reason) { if (!ok) throw std::runtime_error(reason); }
void drive(rapid::AccLapValidity &v, int counter, bool valid) {
  for (int i=0; i<13; ++i) v.update(counter, 99760+i*20, .35+i*.05, valid);
}
void timer_first_validity_ambiguity() {
  for (bool previously_invalid : {false,true}) {
    rapid::AccLapValidity v;
    v.update(3,100,.01,true);
    v.update(3,30000,.4,!previously_invalid);
    v.update(3,90000,.95,true);
    v.update(3,5,.95,false);
    v.update(4,25,.01,false);
    const auto &e=v.transition.events.back();
    check(previously_invalid ? (v.present && !v.valid) : !v.present,
          "timer-first untagged false is ambiguous, not finishing invalidity; confirmed earlier invalid stays invalid");
    check(previously_invalid || e.raw_validity.status==R::Status::unknown,
          "reset-pending flag cannot certify a finishing generation");
  }
}
void partial_join_validity() {
  rapid::AccLapValidity v;
  v.update(3,50000,.4,true); v.update(3,90000,.95,true); v.update(4,5,.01,true);
  check(!v.present, "partial join lacks earlier validity coverage and must stay unknown");
}
void n2() {
  rapid::AccLapValidity v;
  drive(v,3,false);
  v.update(4,5,.01,false);
  v.update(4,25,.02,true);
  drive(v,4,true);
  v.update(5,5,.01,true);
  check(!v.present, "N2 boundary false then true must be unknown, not sticky invalid or forced clean");
}
R::Result send(R &r, int c, double t, double p, bool gap=false) {
  R::Observation o; o.counter=c; o.timer=t; o.position=p; o.gap=gap;
  return r.step(o);
}
void drive(R &r, int c, double final=100000) {
  send(r,c,100,.01);
  for(int i=0;i<13;++i) send(r,c,final-240+i*20,.35+i*.05);
}
void timer_position_orders() {
  for (bool timer_first : {false,true}) {
    R r; drive(r,3);
    if(timer_first) {send(r,3,5,.95); send(r,3,25,.01);}
    else {send(r,3,100020,.01); send(r,3,5,.02);}
    check(r.events.size()==1, "separated timer/position evidence must close exactly one event in either order");
    check(r.events.back().timer_peak==100000 || r.events.back().timer_peak==100020,
          "timer peak retains old epoch until separated evidence joins");
    auto id=r.events.back().id;
    send(r,4,45,.02);
    check(r.events.size()==1 && r.events.back().id==id, "counter joins the same immutable event identity");
  }
}
void n3() {
  R r; drive(r,3); send(r,3,5,.01);
  drive(r,3,98000);
  auto second=send(r,3,20000,.20); // missing interval inferred from end-to-early progress
  send(r,4,20020,.201);
  check(second.closed && r.events.size()==2, "N3: independent gap reset must survive old counter debt");
  check(r.events[0].counter_seen && !r.events[1].counter_seen && r.counter_debt.size()==1,
        "N3: counter acknowledges oldest crossing, not new physical crossing");
  check(r.events[1].estimated_boundary && r.events[1].timer_peak==98000,
        "N3: gap close carries uncertainty and correct timer ownership");
}
void o4() {
  R r; drive(r,3); send(r,3,5,.01);
  send(r,4,30000,.35,true);
  check(r.events.size()==1 && r.counter_debt.empty(), "O4: first counter catchup mid-lap after gap is not another crossing");
}
void two_debts() {
  R r; drive(r,3); send(r,3,5,.01); drive(r,3,98000); send(r,3,5,.01);
  check(r.events.size()==2 && r.counter_debt.size()==2, "two physical laps retain two unresolved records");
  send(r,4,25,.02); send(r,5,45,.03);
  check(r.events.size()==2 && r.counter_debt.empty() && r.events[0].counter==4 && r.events[1].counter==5,
        "ordered counter debt cannot overwrite or manufacture events");
}
void equal_times() {
  R r; R::Observation o; o.counter=3; o.timer=100; o.position=.01; o.completed=100000; r.step(o);
  for(int i=0;i<13;++i) {o.timer=99760+i*20; o.position=.35+i*.05; r.step(o);}
  o.counter=4; o.timer=5; o.position=.01; o.completed_valid=true; r.step(o);
  check(r.events.size()==1 && r.events[0].time.value==100000 && r.events[0].time.status==R::Status::provisional,
        "equal completion candidate preserved, never certified by numeric equality");
  check(r.events[0].validity.value==1 && r.events[0].validity.status==R::Status::authoritative,
        "validity contract is independent of equal time freshness");
  check(!R::rankable(r.events[0]), "equal time uncertainty is excluded from exact ranking");
  o.timer=25; o.completed=99520; r.step(o);
  check(r.events[0].time.value==99520 && r.events[0].time.status==R::Status::inferred,
        "O1: near-equal held time remains repairable but not generation-certified");
}
void counter_jump() {
  R r; drive(r,3); send(r,6,5,.01);
  check(r.events.size()==1 && r.missing_completions==2 && r.events[0].mapping_ambiguous,
        "counter jump records missing completions, not invented ranges");
}
void startup_flicker() {
  R r; send(r,3,5,.01);
  for(int i=0;i<30;++i) send(r,3,i%2 ? 5 : 100000,i%2 ? .01 : .95);
  check(r.events.empty(), "startup line reversal without progress cannot fabricate a lap");
}
void pause() {
  R r; drive(r,3);
  for(int i=0;i<500;++i) send(r,3,100000,.95,i==250);
  send(r,3,5,.01);
  check(r.events.size()==1 && r.events[0].time.value==100000, "pause and gap observation count cannot inflate simulator timer estimate");
}
void correction() {
  R r; send(r,3,100,.01); send(r,3,50000,.4); send(r,3,60000,.5);
  send(r,3,20000,.51,true);
  check(r.events.empty() && r.phase==R::Phase::discontinuity,
        "pit/teleport/correction timer reset without end/wrap evidence is a discontinuity");
}
void restart() {
  R r; drive(r,3); send(r,3,5,.01); send(r,1,0,.01);
  check(r.epoch==1 && r.events.empty() && r.counter_debt.empty() && r.lap_id==1,
        "counter regression clears provenance and advances local epoch");
}
void source_tagged() {
  for(bool valid:{false,true}) {
    R r; R::Observation o; o.counter=3; o.timer=100; o.position=.01; o.current_valid=false; r.step(o);
    for(int i=0;i<13;++i) {o.timer=99760+i*20; o.position=.35+i*.05; r.step(o);}
    o.counter=4; o.timer=5; o.position=.01; r.step(o); // ambiguous early false
    o.current_valid=valid; o.current_valid_lap_id=r.lap_id; o.timer=25; r.step(o);
    for(int i=0;i<13;++i) {o.timer=99760+i*20; o.position=.35+i*.05; r.step(o);}
    o.counter=5; o.timer=5; o.position=.01; r.step(o);
    check(r.events[1].raw_validity.value==double(valid) && r.events[1].raw_validity.status==R::Status::authoritative,
          "validated phase tag resolves boundary ambiguity without forcing clean or invalid");
  }
}
void rejected_observation() {
  R r; drive(r,3); const auto seq=r.sequence;
  // Rejected reads are not passed to this accepted-observation API.
  check(r.sequence==seq && r.events.empty(), "a missing/rejected read supplies no event evidence");
  send(r,3,5,.01); check(r.events.size()==1, "next accepted read still supplies physical evidence");
}
void counter_only() {
  R r; R::Observation o; o.has_position=false; o.counter=3; o.timer=100; r.step(o);
  o.timer=90000; r.step(o); o.counter=4; o.timer=5; r.step(o);
  o.timer=100000; r.step(o); o.counter=5; o.timer=5; r.step(o);
  check(r.events.size()==2 && r.events[1].timer_peak==100000,
        "counter-only telemetry must reopen after timer epoch progress");
}
void expired_debt() {
  R r;
  for(int i=0;i<65;++i) {drive(r,3); send(r,3,5,.01);}
  check(r.events.size()==R::retention && r.expired_records==1, "pending record retention is bounded");
  send(r,3,30000,.4); const auto id=r.lap_id;
  send(r,68,30020,.401);
  check(r.lap_id==id && r.counter_debt.empty(),
        "expired crossing debt must still consume old acknowledgments, not fabricate a new event");
}
void wrap_index() {
  R r; drive(r,3); const auto index=r.sequence;
  auto wrap=send(r,3,100020,.01);
  check(wrap.closed && r.events.back().last==index,
        "observed physical wrap must retain its original segmentation index when timer resets later");
  send(r,3,5,.02); send(r,4,25,.03);
  check(r.events.size()==1 && r.events[0].timer_reset && r.events[0].last==index,
        "later timer/counter evidence joins without moving physical boundary");
}
void permutations() {
  std::vector<int> order={0,1,2}; int count=0;
  do {
    R r; drive(r,3,180000);
    R::Observation o; o.counter=3; o.timer=180000; o.position=.95;
    for(int field:order) {
      if(field==0) o.counter=4;
      if(field==1) o.timer=5;
      if(field==2) o.position=.01;
      r.step(o);
    }
    check(r.events.size()==1 && r.events[0].timer_reset && r.events[0].counter_seen && r.events[0].position_wrap,
          "all six counter/timer/position permutations join one evidence-carrying event");
    drive(r,4); send(r,5,5,.01);
    check(r.events.size()==2 && r.events[1].timer_peak==100000,
          "no ordering can contaminate the next timer epoch"); ++count;
  } while(std::next_permutation(order.begin(),order.end()));
  check(count==6,"all six transition signal permutations exercised");
}
void consecutive_equal_candidates() {
  R r; R::Observation o; o.counter=3; o.timer=100; o.position=.01; o.completed=100000; r.step(o);
  for(int lap=3;lap<5;++lap) {
    o.counter=lap;
    for(int i=0;i<13;++i) {o.timer=99760+i*20; o.position=.35+i*.05; r.step(o);}
    o.counter=lap+1; o.timer=5; o.position=.01; o.completed_valid=true; r.step(o);
  }
  check(r.events.size()==2 && r.events[1].time_candidate.value==100000 &&
        r.events[1].time.status==R::Status::provisional && !R::rankable(r.events[1]),
        "genuine identical consecutive times preserve numeric candidates without fabricated generations");
}
void sparse_counter_presence() {
  R r; send(r,3,100,.01); send(r,3,30000,.4); send(r,3,60000,.7); send(r,3,90000,.95);
  R::Observation o; o.counter=0; o.has_counter=false; o.timer=90020; o.position=.96;
  auto absent=r.step(o);
  check(!absent.closed && !absent.restarted && r.epoch==0 && r.missing_completions==0 && r.peak==90020,
        "absent counter cannot restart/regress/increment and must preserve physical timer evidence");
  // A garbage high value is equally absent; it cannot overwrite the baseline.
  o.counter=100; o.timer=90030; o.position=.965; r.step(o);
  auto resumed=send(r,3,90040,.97);
  check(!resumed.closed && !resumed.restarted && r.events.empty(),
        "resumed counter compares with last observed counter, not absent placeholder");
  auto closed=send(r,4,5,.01);
  check(closed.closed && r.events.size()==1 && r.events[0].last==7 && r.events[0].counter==4 &&
        r.events[0].timer_peak==90040 && r.epoch==0 && r.missing_completions==0,
        "resumed counter must close only the real physical boundary");
  auto regression=send(r,2,25,.02);
  check(regression.restarted && r.epoch==1,"real observed counter regression remains restart evidence");
}
void sparse_counter_initial_and_wrap() {
  R r; R::Observation o; o.counter=100; o.has_counter=false; o.timer=100; o.position=.01;
  r.step(o);
  auto first=send(r,3,30000,.4);
  check(!first.restarted && !first.closed && r.epoch==0 && r.missing_completions==0,
        "first observed counter initializes baseline after absent first sample");
  send(r,3,60000,.7);send(r,3,90000,.95);
  o.counter=0;o.timer=5;o.position=.01;auto physical=r.step(o);
  check(physical.closed && !physical.restarted && r.events.size()==1 && r.counter_debt.size()==1 &&
        r.events[0].position_wrap && r.events[0].timer_reset && !r.events[0].counter_seen,
        "missing counter cannot suppress physical timer/position close");
  auto ack=send(r,4,25,.02);
  check(!ack.closed && !ack.restarted && r.counter_debt.empty() && r.events[0].counter==4 && r.missing_completions==0,
        "resumed counter acknowledges physical event rather than fabricating another");
  R no_counter;
  o.counter=100;o.timer=100;o.position=.01;no_counter.step(o);
  o.timer=30000;o.position=.4;no_counter.step(o);o.timer=90000;o.position=.95;no_counter.step(o);
  o.timer=5;o.position=.01;no_counter.step(o);
  auto baseline=send(no_counter,4,25,.02);
  check(!baseline.closed && !baseline.restarted && no_counter.events.size()==1 && no_counter.counter_debt.size()==1 && no_counter.missing_completions==0,
        "first counter after entirely counterless crossing establishes baseline, not invented acknowledgment");
  send(no_counter,5,45,.03);
  check(no_counter.counter_debt.empty() && no_counter.events.size()==1,
        "subsequent observed increment consumes retained physical debt");
}
int main(int argc,char**argv) {
  std::vector<std::pair<std::string,std::function<void()>>> cases={{"timer-first-validity-ambiguity",timer_first_validity_ambiguity},{"partial-join-validity-coverage",partial_join_validity},{"N2-ambiguous-boundary-false",n2},{"timer-position-orders",timer_position_orders},
    {"N3-gap-reset-before-counter",n3},{"O4-gap-counter-catchup",o4},
    {"two-unacknowledged-crossings",two_debts},{"O1-equal-near-equal-repair",equal_times},
    {"counter-jump-missing-completions",counter_jump},{"startup-flicker",startup_flicker},
    {"pause-gap-timer-ownership",pause},{"timer-correction-discontinuity",correction},
    {"restart-epoch",restart},{"tagged-validity-resolution",source_tagged},
    { "accepted-observations-only",rejected_observation},{"counter-only-epochs",counter_only},{"bounded-expired-counter-debt",expired_debt},{"physical-wrap-index",wrap_index},{"six-transition-permutations",permutations},{"consecutive-equal-candidates",consecutive_equal_candidates},{"sparse-counter-presence",sparse_counter_presence},{"sparse-counter-first-and-wrap",sparse_counter_initial_and_wrap}};
  int failures=0, count=0;
  for (auto &[name,run]:cases) {
    if (argc>1 && name!=argv[1]) continue;
    ++count;
    try {run(); std::cout<<"PASS "<<name<<'\n';}
    catch(const std::exception &e) {++failures; std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
  }
  std::cout<<"cases="<<count<<" failures="<<failures<<'\n';
  return failures || !count;
}
