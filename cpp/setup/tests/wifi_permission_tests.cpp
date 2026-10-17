#include "rapid/setup_auth.hpp"
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace rapid::native;
namespace {
// Interpose real libc calls, not atomic_file: observe the inode immediately
// after creation and on both sides of publication, without timing/polling.
std::string watched;
struct Observation { const char *stage; struct stat info; };
std::vector<Observation> observations;
bool fail_rename = false;
fs::path helper_result;
void observe(const char *stage, const char *path) {
  if (watched.empty() || !std::string_view(path).starts_with(watched)) return;
  struct stat info {};
  if (::lstat(path, &info) == 0) observations.push_back({stage, info});
}
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path() / ("rapid-wifi-permissions-" + unique_id());
  TemporaryDirectory() { fs::create_directory(path); }
  ~TemporaryDirectory() { std::error_code error; fs::remove_all(path, error); }
};
struct Umask {
  mode_t previous = ::umask(0000);
  ~Umask() { ::umask(previous); }
};
Request request(const std::string &path, const Json &body) {
  return {"POST", path, body.dump(), {{"host", "127.0.0.1:8002"},
    {"origin", "http://127.0.0.1:8002"}, {"content-type", "application/json"}}};
}
void check_observations(const std::string &label, bool published) {
  require(observations.size() == (published ? 3u : 2u), "creation/rename observation coverage");
  require(std::string_view(observations[0].stage) == "creation" &&
          std::string_view(observations[1].stage) == "before-publish",
          "observe creation before writing and before publication");
  require(observations[0].info.st_size == 0 && observations[1].info.st_size > 0,
          "observe empty creation and completed secret write");
  bool private_at_every_stage = true;
  for (const auto &entry : observations) {
    std::cout << label << ": " << entry.stage << " mode=" << std::oct
              << (entry.info.st_mode & 07777) << std::dec
              << " uid=" << entry.info.st_uid << " gid=" << entry.info.st_gid << '\n';
    private_at_every_stage &= (entry.info.st_mode & 07777) == 0600;
    require(entry.info.st_uid == ::geteuid() && entry.info.st_gid == ::getegid(),
            "request retains creator ownership");
    require(entry.info.st_ino == observations.front().info.st_ino,
            "publication renames the same completed inode");
  }
  require(private_at_every_stage, "Wi-Fi request/temp must be 0600 from creation through publication");
}
} // namespace

extern "C" FILE *fopen64(const char *path, const char *mode) {
  static auto real = reinterpret_cast<FILE *(*)(const char *, const char *)>(dlsym(RTLD_NEXT, "fopen64"));
  FILE *result = real(path, mode);
  if (result && mode[0] == 'w') observe("creation", path);
  return result;
}
extern "C" FILE *fopen(const char *path, const char *mode) {
  static auto real = reinterpret_cast<FILE *(*)(const char *, const char *)>(dlsym(RTLD_NEXT, "fopen"));
  FILE *result = real(path, mode);
  if (result && mode[0] == 'w') observe("creation", path);
  return result;
}
extern "C" int open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
  }
  static auto real = reinterpret_cast<int (*)(const char *, int, ...)>(dlsym(RTLD_NEXT, "open"));
  int result = real(path, flags, mode);
  if (result >= 0 && (flags & O_CREAT)) observe("creation", path);
  return result;
}
extern "C" int rename(const char *from, const char *to) {
  static auto real = reinterpret_cast<int (*)(const char *, const char *)>(dlsym(RTLD_NEXT, "rename"));
  const bool target = !watched.empty() && watched == to;
  if (target) observe("before-publish", from);
  if (target && fail_rename) { errno = EIO; return -1; }
  int result = real(from, to);
  if (target && result == 0) {
    observe("published", to);
    // Synchronous helper response keeps the setup-AP wait test deterministic.
    if (!helper_result.empty()) atomic_file(helper_result, "{\"secured\":true}\n");
  }
  return result;
}

int main(int argc, char **argv) {
  try {
    require(argc == 2, "usage: rapid-wifi-permission-tests save|setup_ap");
    const std::string action = argv[1];
    require(action == "save" || action == "setup_ap", "known action");
    TemporaryDirectory root;
    const auto queue = root.path / "queue";
    fs::create_directory(queue);
    const auto file = queue / "wifi-request.json";
    const auto result = queue / "wifi-result.json";
    SetupStore store(root.path / "state");
    SetupAuth auth(store, 8002, monotonic, {}, "127.0.0.1", {}, {}, {}, file, result);
    require(auth.enroll("fixture-owner-password"), "enroll test owner");
    const auto login = auth.handle(request("/api/v1/auth/login", {{"password", "fixture-owner-password"}}));
    require(login.status == 200, "test owner login");
    auto body = Json{{"action", action}, {"password", "fixture-wifi-password"}};
    if (action == "save") {
      body["revision"] = store.snapshot()["revision"];
      body["ssid"] = "Fixture Network";
    }
    auto post = request("/api/v1/wifi", body);
    post.headers["x-csrf-token"] = Json::parse(login.body)["csrf_token"];
    for (const auto &[name, value] : login.headers)
      if (name == "Set-Cookie") post.headers["cookie"] = value.substr(0, value.find(';'));
    Umask permissive;
    if (action == "setup_ap") helper_result = result;
    watched = file.string();
    auto response = auth.handle(post);
    require(response.status == (action == "save" ? 202 : 200), "request accepted with unchanged status");
    check_observations(action + " first", true);
    require(Json::parse(read_file(file)) == body, "exact helper request payload preserved");
    require(response.body.find("fixture-wifi-password") == std::string::npos, "response omits secret");
    const auto old_inode = observations.back().info.st_ino;
    observations.clear();
    const int old = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    require(old >= 0, "hold old request inode across replacement");
    response = auth.handle(post);
    check_observations(action + " replacement", true);
    struct stat old_info {};
    require(::fstat(old, &old_info) == 0 && old_info.st_ino == old_inode &&
            old_info.st_nlink == 0 && observations.back().info.st_ino != old_inode,
            "atomic replacement preserves old open inode until unlinked");
    ::close(old);
    const auto previous = read_file(file);
    const auto previous_inode = observations.back().info.st_ino;
    observations.clear();
    fail_rename = true;
    response = auth.handle(post);
    fail_rename = false;
    require(response.status == 503, "failed publication returns generic queue error");
    check_observations(action + " failed rename", false);
    struct stat kept {};
    require(::lstat(file.c_str(), &kept) == 0 && kept.st_ino == previous_inode && read_file(file) == previous,
            "failed replacement preserves existing request");
    require(response.body.find("fixture-wifi-password") == std::string::npos, "failure omits secret");
    for (const auto &entry : fs::directory_iterator(queue))
      require(entry.path().extension() != ".part", "failed publication cleans secret temporary");
    watched.clear();
    std::cout << "ok: " << action << " creation, publication, replacement and failure cleanup\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
