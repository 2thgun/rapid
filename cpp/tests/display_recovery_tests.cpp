#include "rapid/native.hpp"
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace rapid::native;

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int run(const fs::path &program, std::initializer_list<std::string> args) {
  std::vector<std::string> values(args);
  std::vector<char *> argv{const_cast<char *>(program.c_str())};
  for (auto &value : values) argv.push_back(value.data());
  argv.push_back(nullptr);
  const auto child = fork();
  require(child >= 0, "fork recovery utility");
  if (child == 0) { execv(program.c_str(), argv.data()); _exit(127); }
  int status = 0;
  require(waitpid(child, &status, 0) == child, "wait recovery utility");
  return WIFEXITED(status) ? WEXITSTATUS(status) : 127;
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "recovery utility required");
    const auto root = fs::temp_directory_path() / ("rapid-display-" + unique_id());
    fs::create_directory(root);
    const auto state = root / "state.json";
    const auto fake = root / "xrandr";
    const auto log = root / "calls";
    { std::ofstream script(fake); script << "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"" << log.string() << "\"\n"; }
    fs::permissions(fake, fs::perms::owner_all);
    require(run(argv[1], {"--state-file", state.string(), "--xrandr", fake.string(),
                          "--output", "default", "--rotation", "180", "--preview"}) == 0,
            "preview succeeds");
    auto preview = Json::parse(read_file(state));
    require(preview["pending"] == true && preview["rotation"] == 180 &&
                read_file(log).find("--rotate inverted") != std::string::npos,
            "preview persists rotation and uses inverted output");
    require(run(argv[1], {"--state-file", state.string(), "--xrandr", fake.string(),
                          "--output", "default", "--rollback"}) == 0,
            "rollback succeeds");
    auto rolled_back = Json::parse(read_file(state));
    require(rolled_back["pending"] == false && rolled_back["rotation"] == 0 &&
                read_file(log).find("--rotate normal") != std::string::npos,
            "rollback restores the previous orientation");
    require(run(argv[1], {"--state-file", state.string(), "--xrandr", fake.string(),
                          "--output", "default", "--rotation", "180", "--preview"}) == 0,
            "second preview succeeds");
    require(run(argv[1], {"--state-file", state.string(), "--xrandr", fake.string(),
                          "--output", "default", "--recover"}) == 0,
            "boot recovery succeeds");
    require(Json::parse(read_file(state))["pending"] == false &&
                Json::parse(read_file(state))["rotation"] == 0,
            "boot recovery clears an interrupted preview");
    const auto calibration = root / "calibration";
    atomic_file(calibration, "stale");
    require(run(argv[1], {"--state-file", state.string(), "--reset-calibration",
                          "--calibration-file", calibration.string()}) == 0 &&
                !fs::exists(calibration),
            "calibration reset removes stale calibration");
    std::error_code error; fs::remove_all(root, error);
    std::cout << "Display preview, rollback and calibration reset passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
