#include "rapid/native.hpp"
#ifdef BASELINE
#include "baseline_validity.hpp"
#else
#include "../../../companion/native/acc_lap_validity.hpp"
#endif
#include <iostream>
using namespace rapid::native;
#ifndef BASELINE
struct ReviewBoundary {
 AccLapTransition r;
 AccLapTransition::Result step(int c,double t,double p,bool has) {
  AccLapTransition::Observation o; o.counter=c;o.timer=t;o.position=p;o.has_position=has;
  return r.step(o);
 }
};
#else
using ReviewBoundary=LapBoundary;
#endif
Json message(int lap,int timer,double position,int completed,bool valid=true){return {{"version",4},{"simulator","ACC"},{"session_id","design-repro"},{"sample_rate_hz",50},{"telemetry",{{"lap_number",lap},{"current_lap_ms",timer},{"lap_position",position},{"completed_lap_ms",completed},{"lap_valid",valid}}}};}
Json finish(Recorder& r){r.finish();r.wait_idle();auto p=fs::path(r.status().at("last_bundle_path").get<std::string>());return Json::parse(read_file(p/"manifest.json"))["laps"];}
void update(rapid::AccLapValidity& v,int lap,int timer,double pos,bool valid){
#ifdef BASELINE
v.update(lap,valid);
#else
v.update(lap,timer,pos,valid);
#endif
}
int main(int argc,char**argv){
 if(argc!=2) {std::cerr<<"evidence directory required\n";return 2;}
 const fs::path root=fs::path(argv[1])/unique_id(); int failures=0;
 auto check=[&](const char*name,const Json&actual,bool ok){std::cout<<Json{{"case",name},{"actual",actual},{"pass",ok}}.dump()<<"\n";failures+=!ok;};
 {Recorder r(root/"old-held","all");for(int i=0;i<13;++i)r.record(message(3,99260+i*20,.35+i*.05,100000));r.record(message(4,5,.01,100000,false));r.record(message(4,25,.02,99520,false));auto x=finish(r);check("O1_stale_within_tolerance",x,x.size()==1&&x[0]["time_ms"]==99520&&x[0]["valid"]==false);}
 {Recorder r(root/"old-timer","all");for(int i=0;i<13;++i)r.record(message(3,179760+i*20,.35+i*.05,120000));r.record(message(4,180000,.99,180010));r.record(message(4,5,.01,180010));for(int i=0;i<13;++i)r.record(message(4,99760+i*20,.35+i*.05,180010));r.record(message(4,5,.01,180010));auto x=finish(r);check("O2_counter_first_timer",x,x.size()==2&&x[1]["time_ms"]==100000);}
 {rapid::AccLapValidity v;for(int i=0;i<13;++i)update(v,3,179760+i*20,.35+i*.05,false);update(v,4,180000,.99,false);update(v,4,5,.01,true);for(int i=0;i<13;++i)update(v,4,99760+i*20,.35+i*.05,true);update(v,5,5,.01,true);check("O3_counter_first_validity",Json{{"present",v.present},{"valid",v.valid}},!v.present);}
 {ReviewBoundary b;b.step(3,50000,.35,true);for(int i=0;i<12;++i)b.step(3,60000+i*4000,.4+i*.05,true);auto first=b.step(3,5,.01,true);for(int i=0;i<12;++i)b.step(3,25+i*20,.02,true);auto second=b.step(4,30000,.35,true);check("O4_gap_counter_catchup",Json{{"first_closed",first.closed},{"catchup_closed",second.closed}},first.closed&&!second.closed);}
 {Recorder r(root/"new-position-first","all");for(int i=0;i<13;++i)r.record(message(3,179760+i*20,.35+i*.05,120000));r.record(message(4,180000,.01,180010));r.record(message(4,5,.02,180010));for(int i=0;i<13;++i)r.record(message(4,99760+i*20,.35+i*.05,180010));r.record(message(5,5,.01,180010));auto x=finish(r);check("N1_counter_position_before_timer",x,x.size()==2&&x[1]["time_ms"]==100000);}
 {rapid::AccLapValidity v;for(int i=0;i<13;++i)update(v,3,179760+i*20,.35+i*.05,false);update(v,4,5,.01,false);update(v,4,25,.02,true);for(int i=0;i<13;++i)update(v,4,99760+i*20,.35+i*.05,true);update(v,5,5,.01,true);check("N2_false_resets_one_sample_late",Json{{"present",v.present},{"valid",v.valid}},!v.present);}
 {ReviewBoundary b;b.step(3,50000,.35,true);for(int i=0;i<12;++i)b.step(3,60000+i*4000,.4+i*.05,true);auto first=b.step(3,5,.01,true);for(int i=0;i<13;++i)b.step(3,50000+i*4000,.35+i*.05,true);auto physical=b.step(3,20000,.20,true);auto ack=b.step(4,20020,.201,true);check("N3_rearmed_gap_before_increment",Json{{"first_closed",first.closed},{"gap_closed",physical.closed},{"increment_closed",ack.closed}},first.closed&&physical.closed&&!ack.closed);}
 std::cout<<Json{{"failures",failures},{"cases",7}}.dump()<<"\n";return failures?1:0;
}
