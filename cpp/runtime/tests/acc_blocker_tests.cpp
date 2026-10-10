#include "rapid/native.hpp"
#include "v4_stream.hpp"
#include <iostream>
#include <functional>
using namespace rapid::native;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void sparse(const fs::path &assets,const fs::path &root) {
 Config c;c.assets=assets;c.database=root/"sparse.db";c.telemetry=root/"sparse";c.queue=root/"queue.db";c.companion_key=std::string(32,'\x11');
 Runtime runtime(c);rapid::test::V4Stream st(rapid::test::v4_run_id());st.simulator=1;std::uint64_t us=1000000;
 require(runtime.receive(st.metadata(us),"127.0.0.1"),"sparse metadata accepted");
 auto send=[&](int lap,float timer,float pos,bool missing=false){us+=20000;auto mask=rapid::test::v4_primary_mask()|rapid::test::v4_mask({45,47});if(!missing)mask|=rapid::test::v4_mask({46});require(runtime.receive(st.telemetry(us,mask,{{5,1000},{6,0},{11,0},{12,0},{13,0},{45,float(lap)},{46,timer},{47,pos}},0,0,-1),"127.0.0.1"),"sparse signed sample accepted");};
 send(3,100,.01);send(3,30000,.4);send(3,90000,.95);send(3,90000,.95,true);send(4,90020,.01);send(4,5,.02);send(4,30000,.4);
 require(runtime.snapshot()["sector_1_ms"]==30000,"runtime missing timer cannot contaminate next sector epoch");
 send(4,60000,.95);send(5,5,.01);runtime.finish();auto p=fs::path(runtime.snapshot()["last_bundle_path"].get<std::string>());auto laps=Json::parse(read_file(p/"manifest.json"))["laps"];
 require(laps.size()==2,"sparse wire closes exactly two laps");
 require(laps[1]["time_ms"]==60000,"missing timer is not reset: second lap owns 60000, not 90020");
 require(laps[0]["end_sample"]==4 && laps[1]["start_sample"]==4 && laps[1]["end_sample"]==8,"sparse signal omission preserves physical sample ranges");
}
void sparse_counter(const fs::path &assets,const fs::path &root) {
 for (const char* mode : {"control", "missing-middle", "missing-first", "missing-wrap"}) {
  Config c;c.assets=assets;c.database=root/(std::string(mode)+".db");c.telemetry=root/mode;c.queue=root/(std::string(mode)+"-q.db");c.companion_key=std::string(32,'\x11');
  Runtime rt(c);rapid::test::V4Stream st(rapid::test::v4_run_id());st.simulator=1;std::uint64_t us=1000000;
  require(rt.receive(st.metadata(us),"127.0.0.1"),"counter metadata accepted");
  auto send=[&](int lap,float timer,float pos,bool absent=false){us+=20000;auto mask=rapid::test::v4_primary_mask()|rapid::test::v4_mask({46,47});if(!absent)mask|=rapid::test::v4_mask({45});require(rt.receive(st.telemetry(us,mask,{{5,1000},{6,0},{11,0},{12,0},{13,0},{45,float(lap)},{46,timer},{47,pos}},0,0,-1),"127.0.0.1"),"counter signed sample accepted");};
  const std::string m=mode;
  send(3,100,.01,m=="missing-first");send(3,30000,.4);send(3,60000,.7);send(3,90000,.95);
  send(3,90020,.96,m=="missing-middle");
  auto s=rt.snapshot();
  require(s["sector_1_ms"]==30000 && s["sector_2_ms"]==30000,"absent counter preserves live S1/S2 at 30000");
  send(3,90040,.97);send(4,5,.01,m=="missing-wrap");
  s=rt.snapshot();require(s["sector_1_ms"]==30000 && s["sector_2_ms"]==30000,"counter resume/physical boundary preserves displayed S1/S2");
  if(m=="missing-wrap")send(4,25,.02);
  rt.finish();auto p=fs::path(rt.snapshot()["last_bundle_path"].get<std::string>());auto manifest=Json::parse(read_file(p/"manifest.json"));auto laps=manifest["laps"];
  require(laps.size()==1 && laps[0]["start_sample"]==0 && laps[0]["end_sample"]==6,"absent counter must preserve recorder range [0,6)");
  require(manifest["acc_attribution"]["session_epoch"]==0 && manifest["acc_attribution"]["missing_completions"]==0,"absent/first/resumed counter cannot fabricate restart or missing completions");
  require(laps[0]["attribution"]["position_wrap"]==true && laps[0]["attribution"]["timer_reset"]==true && laps[0]["attribution"]["counter_acknowledged"]==true && laps[0]["attribution"]["counter"]==4,"missing counter preserves physical evidence and later acknowledgment");
  require(laps[0]["time_ms"]==90040,"missing counter must preserve owned timer peak");
  std::cout<<"counter-mode="<<mode<<" range=[0,6) epoch=0 missing=0 S1=30000 S2=30000\n";
 }
}
void ldx(const fs::path&,const fs::path &root){
 for(const char* sim:{"ACC","AC"}) {
 Recorder rec(root/sim,"all");auto send=[&](int lap,int timer,double pos){rec.record(Json{{"simulator",sim},{"session_id","ldx-blocker"},{"sample_rate_hz",50},{"telemetry",{{"lap_number",lap},{"current_lap_ms",timer},{"lap_position",pos},{"completed_lap_ms",90000},{"lap_valid",true}}}});};
 send(3,100,.01);for(int i=0;i<13;++i)send(3,30000+i*5000,.35+i*.05);send(4,5,.01);rec.finish();rec.wait_idle();auto p=fs::path(rec.status()["last_bundle_path"].get<std::string>());auto laps=Json::parse(read_file(p/"manifest.json"))["laps"];auto xml=read_file(p/"full-session.ldx");
 require(laps.size()==1 && laps[0]["start_sample"]==0 && laps[0]["end_sample"]==14,"LDX retains lap range");
 require(xml.find("raPId.1")!=std::string::npos && xml.find("raPId.2")!=std::string::npos && xml.find("RangeBlock")!=std::string::npos && xml.find("Id=\"Total Laps\" Value=\"1\"")!=std::string::npos,"LDX preserves beacons, ranges and total laps");
 bool fastest=xml.find("Id=\"Fastest Lap\"")!=std::string::npos || xml.find("Id=\"Fastest Time\"")!=std::string::npos;
 if(std::string(sim)=="ACC") {require(laps[0]["attribution"]["ranking_eligible"]==false,"ACC candidate is not rankable");require(!fastest,"uncertain ACC LDX must omit both Fastest annotations");}
 else require(xml.find("Id=\"Fastest Lap\"")!=std::string::npos && xml.find("Id=\"Fastest Time\"")!=std::string::npos,"legacy other simulator retains both Fastest annotations");
 }
}
void restart(const fs::path &assets,const fs::path &root){
 Config c;c.assets=assets;c.database=root/"restart.db";c.telemetry=root/"restart";c.queue=root/"restart-q.db";c.companion_key=std::string(32,'\x11');Runtime rt(c);rapid::test::V4Stream st(rapid::test::v4_run_id());st.simulator=1;std::uint64_t us=1000000;require(rt.receive(st.metadata(us),"127.0.0.1"),"restart metadata");
 auto send=[&](int lap,float timer,float pos){us+=20000;require(rt.receive(st.telemetry(us,rapid::test::v4_primary_mask()|rapid::test::v4_mask({45,46,47}),{{5,1000},{6,0},{11,0},{12,0},{13,0},{45,float(lap)},{46,timer},{47,pos}},90000,0,1),"127.0.0.1"),"restart sample accepted");};
 send(3,100,.01);send(3,30000,.4);send(3,60000,.7);send(3,90000,.95);send(4,5,.01);
 require(rt.snapshot()["sector_1_ms"]==30000 && rt.snapshot()["sector_2_ms"]==30000,"ordinary lap boundary keeps previous displayed sectors");
 send(4,30000,.4);send(4,60000,.7);send(4,90000,.95);send(1,0,.01);auto s=rt.snapshot();
 for(const char* key:{"sector_1_ms","sector_2_ms","sector_3_ms","sector_1_delta_ms","sector_2_delta_ms","sector_3_delta_ms"})require(s[key].is_null(),"counter regression clears previous session displayed sectors and deltas");rt.finish();
}
int main(int argc,char**argv){if(argc<2)return 2;auto root=fs::temp_directory_path()/("acc-blockers-"+unique_id());fs::create_directories(root);int n=0,f=0;for(auto &[name,run]:std::vector<std::pair<std::string,std::function<void(const fs::path&,const fs::path&)>>>{{"sparse-authenticated-timer",sparse},{"sparse-authenticated-counter",sparse_counter},{"uncertain-ldx-fastest",ldx},{"restart-displayed-sectors",restart}}){if(argc>2 && name!=argv[2])continue;++n;try{run(argv[1],root);std::cout<<"PASS "<<name<<'\n';}catch(const std::exception&e){++f;std::cout<<"FAIL "<<name<<": "<<e.what()<<'\n';}}std::cout<<"cases="<<n<<" failures="<<f<<" evidence="<<root<<'\n';return f||!n;}
