#include "rapid/native.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cmath>
#include <sys/ioctl.h>
#include <set>

namespace rapid::native {
namespace {
const std::set<std::string>& live_channels() {
  static const auto keys = [] {
    struct Channel { const char *name, *short_name, *unit, *key; double scale; };
    const Channel channels[] = {
#include "rapid/channels.inc"
    };
    std::set<std::string> result;
    for (const auto& channel : channels) result.insert(channel.key);
    for (const auto* key : {"abs_activity", "completed_lap_ms", "delta_ms",
         "best_lap_ms", "sector_1_ms", "sector_2_ms", "sector_3_ms",
         "sector_1_delta_ms", "sector_2_delta_ms", "sector_3_delta_ms",
         "track_name", "car_model", "driver_name"}) result.insert(key);
    return result;
  }();
  return keys;
}
}
Runtime::Runtime(Config config):config_(std::move(config)),state_(Json::parse(read_file(config_.assets/"state-defaults.json"))),store_(config_.database),
  recorder_(config_.telemetry,config_.upload_policy,[this](const fs::path& path){
    if(!config_.upload_enabled)return;auto manifest=Json::parse(read_file(path/"manifest.json"));if(!manifest["metadata"].value("upload_after_session",false))return;
    Database queue(config_.queue);queue.exec("CREATE TABLE IF NOT EXISTS jobs(path TEXT PRIMARY KEY,state TEXT NOT NULL,error TEXT,updated_at REAL NOT NULL)");
    queue.exec("INSERT INTO jobs VALUES(?,'pending',NULL,?) ON CONFLICT(path) DO NOTHING",{path.string(),double(std::time(nullptr))});
  }),source_(config_.companion_host){
  store_.exec("CREATE TABLE IF NOT EXISTS native_acc_packets(id INTEGER PRIMARY KEY,received_at TEXT,packet_type INTEGER,normalized_json TEXT)");
  store_.exec("CREATE TABLE IF NOT EXISTS native_acc_laps(id INTEGER PRIMARY KEY,session_index INTEGER,car_index INTEGER,lap_number INTEGER,lap_time_ms INTEGER,is_valid INTEGER,completed_at TEXT,UNIQUE(session_index,car_index,lap_number))");
  // Reconcile finalized bundles as well as spools after a crash between publish
  // and enqueue. Completed queue entries are never reset.
  if(config_.upload_enabled){Database queue(config_.queue);queue.exec("CREATE TABLE IF NOT EXISTS jobs(path TEXT PRIMARY KEY,state TEXT NOT NULL,error TEXT,updated_at REAL NOT NULL)");
    for(const auto& entry:fs::directory_iterator(config_.telemetry))if(entry.is_directory()&&!entry.path().filename().string().starts_with(".")&&fs::exists(entry.path()/"manifest.json")){
      auto manifest=Json::parse(read_file(entry.path()/"manifest.json"));if(manifest["metadata"].value("upload_after_session",false))queue.exec("INSERT INTO jobs VALUES(?,'pending',NULL,?) ON CONFLICT(path) DO NOTHING",{entry.path().string(),double(std::time(nullptr))});
    }
  }
}
void Runtime::sectors(Json& f){
  int lap=number(f,"lap_number",-1),time=number(f,"current_lap_ms"),complete=number(f,"completed_lap_ms");
  auto record=[&](int index,int duration){if(duration<=0)return;auto key="sector_"+std::to_string(index+1);f[key+"_ms"]=duration;f[key+"_delta_ms"]=best_sectors_[index]?duration-best_sectors_[index]:0;if(!best_sectors_[index]||duration<best_sectors_[index])best_sectors_[index]=duration;};
  if(lap!=timing_lap_){if(timing_lap_>=0&&complete>0){if(splits_.size()==2)record(2,complete-splits_.back());if(!best_lap_||complete<best_lap_)best_lap_=complete;f["best_lap_ms"]=best_lap_;}timing_lap_=lap;splits_.clear();}
  if(f.contains("lap_position")&&splits_.size()<2){double position=number(f,"lap_position");if(position>1)position/=100;if(position>=double(splits_.size()+1)/3){int duration=time-(splits_.empty()?0:splits_.back());if(duration>0){record(splits_.size(),duration);splits_.push_back(time);}}}
}
bool Runtime::receive(const std::string& payload,const std::string& host){
  std::lock_guard lock(mutex_);
  try{
    if(!source_.empty()&&source_!=host)return false;
    in_addr address{};if(inet_pton(AF_INET,host.c_str(),&address)!=1)return false;auto ip=ntohl(address.s_addr);
    if(config_.companion_host.empty()&&!((ip>>24)==10||(ip>>24)==127||(ip>>16)==0xc0a8||(ip>>20)==0xac1||(ip>>16)==0xa9fe))return false;
    auto m=Json::parse(payload);if(!m.is_object())throw std::runtime_error("packet must be an object");m.erase("_packet_gap");
    if(m.contains("sample_rate_hz")&&(!m["sample_rate_hz"].is_number_integer()||number(m,"sample_rate_hz")<1||number(m,"sample_rate_hz")>100))throw std::runtime_error("invalid sample rate");
    if(m.contains("schema_version")&&(!m["schema_version"].is_number_integer()||number(m,"schema_version")<1||number(m,"schema_version")>100))throw std::runtime_error("invalid schema version");
    if(m.contains("monotonic_us")&&(!m["monotonic_us"].is_number_integer()||number(m,"monotonic_us")<0||number(m,"monotonic_us")>9007199254740991.0))throw std::runtime_error("invalid sample timestamp");
    if(!m.contains("version")||!m["version"].is_number_integer())throw std::runtime_error("missing version");
    int version=m["version"];if(version<1||version>3)throw std::runtime_error("unsupported wire version");
    auto type=string(m,"type","telemetry"),sim=string(m,"simulator",version==1?"ACC":"");
    if(!sim.empty()&&sim!="ACC"&&sim!="AC"&&sim!="ACE"&&sim!="iRacing")throw std::runtime_error("invalid simulator");
    auto daemon=string(m,"state");Json frame;
    if(type=="status"){if(version<2||(daemon!="waiting"&&daemon!="ready"&&daemon!="driving"))throw std::runtime_error("invalid heartbeat");}
    else if(type=="telemetry"){
      frame=m.value("telemetry",version==3?Json():m);if(!frame.is_object()||sim.empty())throw std::runtime_error("invalid telemetry object");
      for(const auto* key:{"track_name","car_model","driver_name","completed_lap_ms"})if(!frame.contains(key)&&m.contains(key))frame[key]=m[key];
      if(frame.contains("abs")&&!frame.contains("abs_activity"))frame["abs_activity"]=frame["abs"];
      for(auto it=frame.begin();it!=frame.end();++it)if(live_channels().contains(it.key())&&!it.value().is_null()){
        bool text=it.key()=="track_name"||it.key()=="car_model"||it.key()=="driver_name";
        if(text?!it.value().is_string():(!it.value().is_number()&&!(it.key()=="pit_limiter"&&it.value().is_boolean())))throw std::runtime_error("invalid channel type");
      }
      for(const auto* key:{"rpm","steering_angle","g_x","g_y","g_z"})if(!frame.contains(key)||!frame[key].is_number()||!std::isfinite(frame[key].get<double>()))throw std::runtime_error("missing primary channel");
      if(number(frame,"rpm")<0||number(frame,"rpm")>20000)throw std::runtime_error("invalid RPM");
      for(auto it=frame.begin();it!=frame.end();++it){if(it.value().is_number()&&(!std::isfinite(it.value().get<double>())||std::abs(it.value().get<double>())>1e12))throw std::runtime_error("invalid numeric channel");if(it.value().is_string()&&it.value().get_ref<const std::string&>().size()>512)throw std::runtime_error("oversized channel text");}
      for(const auto* key:{"throttle","brake"})if(frame.contains(key)&&(!frame[key].is_number()||number(frame,key)<0||number(frame,key)>1.001))throw std::runtime_error("invalid pedal");
      for(const auto* key:{"gear","lap_number","current_lap_ms","completed_lap_ms","delta_ms"})if(frame.contains(key)&&!frame[key].is_null()&&(!frame[key].is_number()||std::abs(number(frame,key))>2147483647))throw std::runtime_error("invalid integer channel");
      daemon="driving";
    }else throw std::runtime_error("invalid packet type");
    source_=host;last_packet_=monotonic();state_["companion_connected"]=true;state_["companion_daemon_state"]=daemon;state_["companion_source_host"]=host;state_["companion_received_at"]=now();state_["received_at"]=now();
    state_["simulator"]=sim.empty()?Json():Json(sim);state_["connected"]=state_.value("acc_connected",false)||daemon=="driving";
    if(type=="status"){if(daemon!="driving"){recorder_.finish();state_["session_active"]=false;session_.clear();sequence_=-1;}return true;}
    auto session=string(m,"session_id",string(m,"run_id"));auto identity=sim+"/"+session;
    if(session.empty())identity+="/"+string(m,"track_name")+"/"+string(m,"car_model")+"/"+string(m,"session_name");
    if(identity!=session_&&version==3)recorder_.finish("session_changed");
    if(identity!=session_){session_=identity;sequence_=-1;timing_lap_=-1;best_lap_=0;splits_.clear();std::fill(std::begin(best_sectors_),std::end(best_sectors_),0);for(auto* key:{"best_lap_ms","sector_1_ms","sector_2_ms","sector_3_ms","sector_1_delta_ms","sector_2_delta_ms","sector_3_delta_ms"})state_[key]=nullptr;}
    std::int64_t sequence=-1;if(m.contains("sequence")&&!m["sequence"].is_null()){
      if(!m["sequence"].is_number_integer()||number(m,"sequence")<0||number(m,"sequence")>9007199254740991.0)throw std::runtime_error("invalid sequence");sequence=m["sequence"];
      if(sequence_>=0&&sequence<=sequence_){state_["packets_replayed"]=number(state_,"packets_replayed")+1;return false;}
      if(sequence_>=0){auto gap=sequence-sequence_-1;m["_packet_gap"]=std::min<std::int64_t>(gap,1000000);state_["packets_lost"]=number(state_,"packets_lost")+gap;}sequence_=sequence;
    }
    // The deployed v3 companion does not send session IDs or timestamps. Supply
    // local identity/time for recording and charts without inventing wire sequence
    // numbers or claiming packet-loss measurements for that older sender.
    if(version==3&&session.empty())m["session_id"]=string(recorder_.status(),"session_id",unique_id());
    if(!m.contains("monotonic_us"))m["monotonic_us"]=std::uint64_t(monotonic()*1000000);
    sectors(frame);for(auto it=frame.begin();it!=frame.end();++it)if(live_channels().contains(it.key())&&state_.contains(it.key()))state_[it.key()]=it.value();
    state_["rpm"]=int(number(frame,"rpm"));state_["samples_received"]=number(state_,"samples_received")+1;state_["schema_version"]=version;state_["last_sequence"]=sequence<0?Json():Json(sequence);
    state_["last_monotonic_us"]=m.value("monotonic_us",Json());state_["session_active"]=true;state_["session_name"]=m.value("session_name",Json());
    if(version==3){m["telemetry"]=frame;try{recorder_.record(m);}catch(const std::exception& e){log(std::string("ERROR recording: ")+e.what());}
      Json values=Json::object();for(auto it=frame.begin();it!=frame.end();++it)if(it.value().is_number())values[it.key()]=it.value();
      Json event={{"type","sample"},{"session_id",m.value("session_id",Json())},{"sequence",m.value("sequence",Json())},{"monotonic_us",m.value("monotonic_us",Json())},{"received_monotonic",monotonic()},{"packet_gap",m.value("_packet_gap",0)},{"values",values}};
      events_.emplace_back(next_event_++,std::move(event));if(events_.size()>12256)events_.pop_front();
    }return true;
  }catch(const std::exception&){state_["packets_invalid"]=number(state_,"packets_invalid")+1;return false;}
}
void Runtime::expire(){std::lock_guard lock(mutex_);if(last_packet_&&monotonic()-last_packet_>1.5){
  recorder_.finish("disconnected");for(const auto* key:{"rpm","steering_angle","g_x","g_y","g_z","throttle","brake","companion_daemon_state","companion_source_host"})state_[key]=nullptr;
  state_["companion_connected"]=false;state_["connected"]=state_.value("acc_connected",false);state_["session_active"]=false;state_["session_ended_at"]=now();last_packet_=0;sequence_=-1;session_.clear();
  if(!state_.value("acc_connected",false))state_["simulator"]=nullptr;source_=config_.companion_host;
}}
void Runtime::finish(){std::lock_guard lock(mutex_);recorder_.finish("shutdown");}
Json Runtime::snapshot()const{std::lock_guard lock(mutex_);auto result=state_;result.update(recorder_.status());result["upload_enabled"]=config_.upload_enabled;result["runtime"]="cpp";return result;}
void Runtime::upload(bool enabled){std::lock_guard lock(mutex_);recorder_.set_upload(enabled);state_["upload_after_session"]=enabled;}
void Runtime::upload_state(const std::string& state){std::lock_guard lock(mutex_);state_["upload_state"]=state;}
Json Runtime::events(std::uint64_t& cursor,int history)const{
  std::lock_guard lock(mutex_);Json items=Json::array();std::uint64_t dropped=0;
  if(cursor==0){cursor=next_event_-1;for(const auto& [id,event]:events_)if(number(event,"received_monotonic")>=monotonic()-std::clamp(history,0,120)){cursor=id-1;break;}}
  if(!events_.empty()&&cursor+1<events_.front().first)dropped=events_.front().first-cursor-1;
  for(const auto& [id,event]:events_)if(id>cursor){items.push_back(event);cursor=id;}
  if(items.size()>250){dropped+=items.size()-250;items.erase(items.begin(),items.end()-250);}return {{"events",items},{"dropped",dropped}};
}
void Runtime::power(){
  // Firmware property interface, avoiding a child process every second.
  std::uint32_t message[]={32,0,0x30046,8,0,0,0,0};int fd=::open("/dev/vcio",O_RDWR|O_CLOEXEC);bool ok=false;
  if(fd>=0){ok=ioctl(fd,_IOWR(100,0,char*),message)==0&&(message[1]&0x80000000U);::close(fd);}
  std::lock_guard lock(mutex_);state_["power_status_available"]=ok;state_["throttled_flags"]=ok?Json(message[5]):Json();state_["power_limited"]=ok&&(message[5]&7);state_["power_limited_since_boot"]=ok&&(message[5]&0x70000);
}
void Runtime::acc(int type,const Json& p){
  std::lock_guard lock(mutex_);store_.exec("INSERT INTO native_acc_packets(received_at,packet_type,normalized_json) VALUES(?,?,?)",{now(),type,p.dump()});
  if(type==1){state_["acc_connected"]=p.at("success");state_["connection_id"]=p.at("connection_id");}
  if(type==2){for(const auto* key:{"session_index","session_type"})state_[key]=p.at(key);state_["selected_car_index"]=p.at("focused_car_index");}
  if(type==5)state_["track_name"]=p.at("track_name");
  if(type==6&&p.at("car_index")==state_["selected_car_index"]){state_["car_model"]=std::to_string(p.at("car_model").get<int>());int driver=number(p,"current_driver_index");if(driver>=0&&std::size_t(driver)<p.at("drivers").size())state_["driver_name"]=string(p["drivers"][driver],"first_name")+" "+string(p["drivers"][driver],"last_name");}
  if(type==3&&p.at("car_index")==state_["selected_car_index"]){state_["gear"]=number(p,"gear_raw")-1;state_["speed_kmh"]=p.at("speed_kmh");state_["current_lap_ms"]=p["current_lap"]["lap_time_ms"];state_["delta_ms"]=p.at("delta_ms");state_["lap_number"]=p.at("laps");int completed=number(p["last_lap"],"lap_time_ms");
    if(completed>0&&completed!=2147483647){state_["completed_lap_ms"]=completed;store_.exec("INSERT OR IGNORE INTO native_acc_laps(session_index,car_index,lap_number,lap_time_ms,is_valid,completed_at) VALUES(?,?,?,?,?,?)",{state_["session_index"],p.at("car_index"),p.at("laps"),completed,p["last_lap"]["valid"],now()});}
  }
  if(state_.value("acc_connected",false)){state_["connected"]=true;state_["simulator"]="ACC";}state_["received_at"]=now();
}
void udp_loop(Runtime& runtime,const Config& config){
  int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)throw std::runtime_error("UDP socket failed");
  sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(config.companion_port);addr.sin_addr.s_addr=INADDR_ANY;
  if(bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof addr)){::close(fd);throw std::runtime_error("companion UDP port unavailable");}
  while(!stopping){pollfd item{fd,POLLIN,0};if(poll(&item,1,100)>0){char buffer[65536];sockaddr_in source{};socklen_t length=sizeof source;auto size=recvfrom(fd,buffer,sizeof buffer,0,reinterpret_cast<sockaddr*>(&source),&length);char host[INET_ADDRSTRLEN];inet_ntop(AF_INET,&source.sin_addr,host,sizeof host);if(size>0&&size<=8192)runtime.receive(std::string(buffer,size),host);}
    try{runtime.expire();}catch(const std::exception& e){log(std::string("ERROR finalization retry: ")+e.what());}
  }::close(fd);
}
}
