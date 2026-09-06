#include "rapid/native.hpp"
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <unistd.h>

namespace rapid::native {
namespace {
struct Channel {
  const char *name, *short_name, *unit, *key;
  double scale;
};
const Channel channels[] = {
#include "rapid/channels.inc"
};
constexpr std::size_t channel_count = std::size(channels), header_size = 1762,
                      event_size = 1154, channel_size = 124;
std::string channel_file(std::size_t i) {
  std::ostringstream s;
  s << std::setw(2) << std::setfill('0') << i << ".bin";
  return s.str();
}
void little(std::ostream &out, std::uint64_t value, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i)
    out.put(char((value >> (i * 8)) & 255));
}
void fixed(std::ostream &out, const std::string &value, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i)
    out.put(i < value.size()
                ? (static_cast<unsigned char>(value[i]) < 128 ? value[i] : '?')
                : 0);
}
void float_value(std::ostream &out, float value) {
  little(out, std::bit_cast<std::uint32_t>(value), 4);
}
void write_ld(const fs::path &spool, const fs::path &target, const Json &state,
              std::size_t start, std::size_t end) {
  if (end < start ||
      (end - start) > (std::numeric_limits<std::uint32_t>::max() - 10000) /
                          (channel_count * 4))
    throw std::runtime_error("LD size exceeds format limits");
  const auto count = end - start, metadata_pointer = header_size + event_size,
             data_pointer = metadata_pointer + channel_count * channel_size;
  const auto &m = state.at("metadata");
  int rate = state.at("sample_rate_hz");
  auto part = target;
  part += ".part";
  fs::create_directories(target.parent_path());
  std::ofstream out(part, std::ios::binary);
  out.exceptions(std::ios::failbit | std::ios::badbit);
  little(out, 0x40, 4);
  little(out, 0, 4);
  little(out, metadata_pointer, 4);
  little(out, data_pointer, 4);
  fixed(out, "", 20);
  little(out, header_size, 4);
  fixed(out, "", 24);
  little(out, 1, 2);
  little(out, 0x4240, 2);
  little(out, 0xf, 2);
  little(out, 0x1f44, 4);
  fixed(out, "ADL", 8);
  little(out, 420, 2);
  little(out, 0xadb0, 2);
  little(out, channel_count, 4);
  fixed(out, "", 4);
  auto started = string(m, "started_at", now());
  std::tm tm{};
  std::istringstream time(started);
  time >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  char date[20]{}, clock[20]{};
  std::strftime(date, sizeof date, "%d/%m/%Y", &tm);
  std::strftime(clock, sizeof clock, "%H:%M:%S", &tm);
  fixed(out, date, 16);
  fixed(out, "", 16);
  fixed(out, clock, 16);
  fixed(out, "", 16);
  fixed(out, string(m, "driver"), 64);
  fixed(out, string(m, "vehicle"), 64);
  fixed(out, "", 64);
  fixed(out, string(m, "venue"), 64);
  fixed(out, "", 1088);
  little(out, 0xc81a4, 4);
  fixed(out, "", 66);
  fixed(out, "raPId " + string(m, "simulator") + " telemetry", 64);
  fixed(out, "", 126);
  if (std::size_t(out.tellp()) != header_size)
    throw std::runtime_error("LD header mismatch");
  fixed(out, string(m, "simulator"), 64);
  fixed(out, string(m, "session"), 64);
  fixed(out, "Recorded by raPId", 1024);
  little(out, 0, 2);
  for (std::size_t i = 0; i < channel_count; ++i) {
    const auto &c = channels[i];
    little(out, i ? metadata_pointer + (i - 1) * channel_size : 0, 4);
    little(out,
           i + 1 < channel_count ? metadata_pointer + (i + 1) * channel_size
                                 : 0,
           4);
    little(out, data_pointer + i * count * 4, 4);
    little(out, count, 4);
    little(out, 0x2ee1 + i, 2);
    little(out, 7, 2);
    little(out, 4, 2);
    little(out, rate, 2);
    little(out, 0, 2);
    little(out, 1, 2);
    little(out, 1, 2);
    little(out, 0, 2);
    fixed(out, c.name, 32);
    fixed(out, c.short_name, 8);
    fixed(out, c.unit, 12);
    fixed(out, "", 40);
  }
  if (std::size_t(out.tellp()) != data_pointer)
    throw std::runtime_error("LD channel mismatch");
  for (std::size_t i = 0; i < channel_count; ++i) {
    if (i == 0 && start) {
      for (std::size_t n = 0; n < count; ++n)
        float_value(out, float(n) / rate);
      continue;
    }
    std::ifstream in(spool / channel_file(i), std::ios::binary);
    in.seekg(start * 4);
    std::size_t remaining = count * 4;
    char block[65536];
    while (remaining) {
      auto size = std::min(remaining, sizeof block);
      in.read(block, size);
      if (std::size_t(in.gcount()) != size)
        throw std::runtime_error("truncated spool");
      out.write(block, size);
      remaining -= size;
    }
  }
  out.close();
  sync_file(part);
  fs::rename(part, target);
  sync_file(target.parent_path());
}
std::string lap_name(const Json &lap) {
  std::ostringstream out;
  out << "lap-" << std::setw(3) << std::setfill('0')
      << lap.at("number").get<int>() << "-"
      << (lap["valid"].is_boolean()
              ? (lap["valid"].get<bool>() ? "valid" : "invalid")
              : "unknown")
      << ".ld";
  return out.str();
}
void write_ldx(const fs::path &path, const Json &laps, int rate) {
  std::set<std::size_t> beacons;
  Json fastest;
  for (const auto &lap : laps) {
    beacons.insert(lap.at("start_sample").get<std::size_t>());
    beacons.insert(lap.at("end_sample").get<std::size_t>());
    if (fastest.is_null() ||
        number(lap, "time_ms") < number(fastest, "time_ms"))
      fastest = lap;
  }
  std::ostringstream out;
  out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<LDXFile Version=\"1.6\" "
         "Locale=\"English\" "
         "DefaultLocale=\"C\"><Layers><Layer><MarkerBlock><MarkerGroup "
         "Name=\"Beacons\" Index=\"3\">\n";
  int index = 0;
  for (auto sample : beacons)
    out << "<Marker Version=\"100\" ClassName=\"BCN\" Name=\"raPId." << ++index
        << "\" Flags=\"77\" Time=\"" << std::fixed << std::setprecision(6)
        << double(sample) * 1000000 / rate << "\"/>\n";
  out << "</MarkerGroup></MarkerBlock><RangeBlock/></Layer><Details><String "
         "Id=\"Total Laps\" Value=\""
      << laps.size() << "\"/>";
  if (!fastest.is_null()) {
    int ms = number(fastest, "time_ms");
    out << "<String Id=\"Fastest Lap\" Value=\"" << fastest["number"].get<int>()
        << "\"/><String Id=\"Fastest Time\" Value=\"" << ms / 60000 << ":"
        << std::setw(6) << std::setfill('0') << std::setprecision(3)
        << double(ms % 60000) / 1000 << "\"/>";
  }
  out << "</Details></Layers></LDXFile>\n";
  atomic_file(path, out.str());
}
} // namespace
Recorder::Recorder(fs::path root, std::string policy,
                   std::function<void(const fs::path &)> callback)
    : root_(std::move(root)), callback_(std::move(callback)),
      policy_(std::move(policy)) {
  fs::create_directories(root_);
  std::vector<fs::path> spools;
  for (const auto &entry : fs::directory_iterator(root_))
    if (entry.is_directory() &&
        entry.path().filename().string().starts_with(".rapid-spool-") &&
        !entry.path().filename().string().ends_with(".old"))
      spools.push_back(entry.path());
  for (const auto &spool : spools)
    try {
      auto state = Json::parse(read_file(spool / "spool.json"));
      std::size_t count = state.at("sample_count");
      for (std::size_t i = 0; i < channel_count; ++i)
        count = std::min(count, fs::file_size(spool / channel_file(i)) / 4);
      if (!count)
        continue;
      state["sample_count"] = count;
      Json laps = Json::array();
      for (const auto &lap : state["laps"])
        if (number(lap, "end_sample") <= count)
          laps.push_back(lap);
      state["laps"] = laps;
      state["quality"]["recovered_after_restart"] = 1;
      auto path = publish(spool, state, "recovered_after_restart");
      fs::rename(spool, spool.string() + ".old");
      if (callback_)
        callback_(path);
      log("Recovered telemetry spool");
    } catch (const std::exception &e) {
      log(std::string("ERROR spool recovery preserved original: ") + e.what());
    }
}
Recorder::~Recorder() { close(); }
void Recorder::close() {
  for (auto *stream : streams_)
    std::fclose(stream);
  streams_.clear();
}
void Recorder::start(const Json &message) {
  spool_ = root_ / (".rapid-spool-" + unique_id());
  fs::create_directory(spool_);
  checkpoint_ = {
      {"metadata",
       {{"session_id",
         string(message, "session_id", string(message, "run_id", unique_id()))},
        {"started_at", string(message, "started_at", now())},
        {"simulator", string(message, "simulator")},
        {"driver", string(message, "driver_name")},
        {"vehicle", string(message, "car_model")},
        {"venue", string(message, "track_name")},
        {"session", string(message, "session_name")},
        {"schema_version", int(number(message, "schema_version", 1))},
        {"run_id", string(message, "run_id")}}},
      {"sample_rate_hz",
       std::clamp(int(number(message, "sample_rate_hz", 10)), 1, 100)},
      {"sample_count", 0},
      {"received_count", 0},
      {"laps", Json::array()},
      {"quality",
       {{"gap_events", 0},
        {"missing_packets", 0},
        {"substituted_samples", 0},
        {"unfilled_missing_packets", 0},
        {"invalid_samples", 0}}},
      {"availability", Json::object()}};
  for (const auto &c : channels)
    if (std::string(c.key) != "elapsed")
      checkpoint_["availability"][c.key] = 0;
  try {
    for (std::size_t i = 0; i < channel_count; ++i) {
      auto *f = std::fopen((spool_ / channel_file(i)).c_str(), "wb");
      if (!f)
        throw std::runtime_error("cannot create spool channel");
      streams_.push_back(f);
    }
  } catch (...) {
    close();
    throw;
  }
  auto session = string(message, "session_name");
  for (auto &ch : session)
    ch = std::tolower(static_cast<unsigned char>(ch));
  upload_ = message.contains("upload_after_session") &&
                    message["upload_after_session"].is_boolean()
                ? message["upload_after_session"].get<bool>()
                : policy_ == "all" || (policy_ == "races" && session == "race");
  last_lap_ = -1;
  lap_start_ = 0;
  last_frame_ = nullptr;
  checkpoint();
  log("Recording native telemetry session");
}
void Recorder::checkpoint() {
  for (auto *f : streams_)
    if (std::fflush(f) || fsync(fileno(f)))
      throw std::runtime_error("spool sync failed");
  checkpoint_["upload_after_session"] = upload_;
  atomic_file(spool_ / "spool.json", checkpoint_.dump());
}
void Recorder::write_frame(const Json &frame, bool substituted) {
  for (std::size_t i = 0; i < channel_count; ++i) {
    const auto &c = channels[i];
    double value = 0;
    if (i == 0)
      value = number(checkpoint_, "sample_count") /
              number(checkpoint_, "sample_rate_hz");
    else {
      auto key = std::string(c.key);
      if (key == "abs" && !frame.contains(key))
        key = "abs_activity";
      if (frame.contains(key) &&
          (frame[key].is_number() || frame[key].is_boolean())) {
        value = frame[key].is_boolean() ? double(frame[key].get<bool>())
                                        : frame[key].get<double>();
        checkpoint_["availability"][c.key] =
            number(checkpoint_["availability"], c.key) + 1;
      }
      value *= c.scale;
    }
    if (!std::isfinite(value) ||
        std::abs(value) > std::numeric_limits<float>::max()) {
      value = 0;
      checkpoint_["quality"]["invalid_samples"] =
          number(checkpoint_["quality"], "invalid_samples") + 1;
    }
    auto bits = std::bit_cast<std::uint32_t>(float(value));
    unsigned char bytes[4];
    for (int b = 0; b < 4; ++b)
      bytes[b] = (bits >> (8 * b)) & 255;
    if (std::fwrite(bytes, 1, 4, streams_[i]) != 4)
      throw std::runtime_error("spool write failed");
  }
  checkpoint_["sample_count"] =
      checkpoint_["sample_count"].get<std::size_t>() + 1;
  if (substituted)
    checkpoint_["quality"]["substituted_samples"] =
        number(checkpoint_["quality"], "substituted_samples") + 1;
}
void Recorder::record(const Json &message) {
  auto session = string(message, "session_id", string(message, "run_id"));
  int rate = std::clamp(int(number(message, "sample_rate_hz", 10)), 1, 100);
  if (!streams_.empty() &&
      (string(message, "simulator") !=
           string(checkpoint_["metadata"], "simulator") ||
       rate != number(checkpoint_, "sample_rate_hz") ||
       (!session.empty() &&
        session != string(checkpoint_["metadata"], "session_id"))))
    finish("session_changed");
  if (streams_.empty())
    start(message);
  for (const auto &[source, target] :
       std::vector<std::pair<std::string, std::string>>{
           {"driver_name", "driver"},
           {"car_model", "vehicle"},
           {"track_name", "venue"},
           {"session_name", "session"}}) {
    auto v = string(message, source);
    if (!v.empty())
      checkpoint_["metadata"][target] = v;
  }
  checkpoint_["received_count"] =
      checkpoint_["received_count"].get<std::size_t>() + 1;
  auto gap = std::max(0, int(number(message, "_packet_gap")));
  if (gap) {
    auto substitute = last_frame_.is_object() ? std::min(gap, rate * 5) : 0;
    checkpoint_["quality"]["gap_events"] =
        number(checkpoint_["quality"], "gap_events") + 1;
    checkpoint_["quality"]["missing_packets"] =
        number(checkpoint_["quality"], "missing_packets") + gap;
    for (int i = 0; i < substitute; ++i)
      write_frame(last_frame_, true);
    checkpoint_["quality"]["unfilled_missing_packets"] =
        number(checkpoint_["quality"], "unfilled_missing_packets") + gap -
        substitute;
  }
  const auto &frame = message.at("telemetry");
  int lap = int(number(frame, "lap_number"));
  auto count = checkpoint_["sample_count"].get<std::size_t>();
  if (last_lap_ < 0) {
    last_lap_ = lap;
    lap_start_ = count;
  }
  if (lap > last_lap_) {
    int completed = int(
        number(message, "completed_lap_ms", number(frame, "completed_lap_ms")));
    if (completed > 0 && count > lap_start_) {
      Json valid = message.value("lap_valid", frame.value("lap_valid", Json()));
      if (!valid.is_boolean())
        valid = nullptr;
      Json segment = {{"number", std::max(0, lap - 1)},
                      {"start_sample", lap_start_},
                      {"end_sample", count},
                      {"time_ms", completed},
                      {"valid", valid}};
      checkpoint_["laps"].push_back(segment);
      checkpoint();
    }
    last_lap_ = lap;
    lap_start_ = count;
  }
  write_frame(frame, false);
  last_frame_ = frame;
  if (checkpoint_["sample_count"].get<std::size_t>() % rate == 0)
    checkpoint();
}
fs::path Recorder::publish(const fs::path &spool, Json state,
                           const std::string &reason) {
  auto destination =
      root_ / ("session-" + spool.filename().string().substr(13));
  if (fs::exists(destination / "manifest.json"))
    return destination;
  auto staging = root_ / (".rapid-publish-" + unique_id());
  fs::create_directory(staging);
  fs::create_directory(staging / "laps");
  auto count = state.at("sample_count").get<std::size_t>();
  int rate = state.at("sample_rate_hz");
  const auto &m = state.at("metadata");
  write_ld(spool, staging / "full-session.ld", state, 0, count);
  write_ldx(staging / "full-session.ldx", state.at("laps"), rate);
  Json artifacts = Json::array();
  auto add = [&](std::string id, std::string role, const fs::path &rel,
                 Json lap = nullptr) {
    Json item = {{"id", id},
                 {"role", role},
                 {"relative_path", rel.generic_string()},
                 {"size", fs::file_size(staging / rel)},
                 {"sha256", hash_file(staging / rel)}};
    if (!lap.is_null())
      item["lap_number"] = lap;
    artifacts.push_back(item);
  };
  add("full-ld", "full_ld", "full-session.ld");
  add("full-ldx", "ldx", "full-session.ldx");
  for (const auto &lap : state.at("laps")) {
    auto rel = fs::path("laps") / lap_name(lap);
    write_ld(spool, staging / rel, state, lap.at("start_sample"),
             lap.at("end_sample"));
    add("lap-" + std::to_string(lap.at("number").get<int>()), "lap_ld", rel,
        lap.at("number"));
  }
  auto metadata = m;
  metadata.erase("session_id");
  metadata.erase("started_at");
  metadata.erase("simulator");
  metadata["sample_rate_hz"] = rate;
  metadata["upload_after_session"] = state.value("upload_after_session", false);
  metadata["finish_reason"] = reason;
  auto quality = state.at("quality");
  quality["received_samples"] = state.at("received_count");
  quality["recorded_samples"] = count;
  quality["channel_available_samples"] = state.at("availability");
  Json manifest = {{"format_version", 1},
                   {"session_id", m.at("session_id")},
                   {"simulator", m.at("simulator")},
                   {"track", string(m, "venue")},
                   {"started_at", m.at("started_at")},
                   {"ended_at", now()},
                   {"metadata", metadata},
                   {"quality", quality},
                   {"laps", state.at("laps")},
                   {"artifacts", artifacts}};
  atomic_file(staging / "manifest.json", manifest.dump(2) + "\n");
  sync_file(staging);
  fs::rename(staging, destination);
  sync_file(root_);
  return destination;
}
void Recorder::finish(const std::string &reason) {
  if (streams_.empty())
    return;
  checkpoint();
  // Do not close or discard the spool before publication succeeds. A failed
  // write can be retried, and a crash can recover the last durable checkpoint.
  if (number(checkpoint_, "sample_count") > 0) {
    auto path = publish(spool_, checkpoint_, reason);
    close();
    fs::rename(spool_, spool_.string() + ".old");
    last_bundle_ = {{"last_bundle_path", path.string()},
                    {"last_manifest_path", (path / "manifest.json").string()}};
    if (callback_)
      callback_(path);
    log("Published native telemetry bundle");
  } else
    close();
  checkpoint_ = nullptr;
  last_frame_ = nullptr;
  spool_.clear();
}
void Recorder::set_upload(bool enabled) {
  upload_ = enabled;
  if (!streams_.empty())
    checkpoint();
}
Json Recorder::status() const {
  Json result = {{"recording", !streams_.empty()},
                 {"session_id", checkpoint_.is_object()
                                    ? checkpoint_["metadata"]["session_id"]
                                    : Json()},
                 {"sample_rate_hz", checkpoint_.is_object()
                                        ? checkpoint_["sample_rate_hz"]
                                        : Json()},
                 {"received_samples", number(checkpoint_, "received_count")},
                 {"recorded_samples", number(checkpoint_, "sample_count")},
                 {"completed_laps",
                  checkpoint_.is_object() ? checkpoint_["laps"].size() : 0},
                 {"upload_after_session", upload_}};
  if (last_bundle_.is_object())
    result.update(last_bundle_);
  return result;
}
} // namespace rapid::native
