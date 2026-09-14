#include "rapid/native.hpp"
#include <cstdlib>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace rapid::native;

namespace {
void require(bool ok, const char *message) {
  if (!ok) throw std::invalid_argument(message);
}

void rotate(const fs::path &xrandr, const std::string &output, int degrees) {
  const char *value = degrees == 180 ? "inverted" : "normal";
  const auto child = fork();
  require(child >= 0, "cannot start xrandr");
  if (child == 0) {
    execl(xrandr.c_str(), xrandr.c_str(), "--output", output.c_str(),
          "--rotate", value, nullptr);
    _exit(127);
  }
  int status = 0;
  require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "display rotation failed");
}

Json load(const fs::path &path) {
  if (!fs::exists(path)) return Json{{"rotation", 0}, {"pending", false}};
  const auto state = Json::parse(read_file(path));
  require(state.is_object() && (state.value("rotation", -1) == 0 ||
                                    state.value("rotation", -1) == 180),
          "invalid display recovery state");
  return state;
}
} // namespace

int main(int argc, char **argv) {
  fs::path state_file, xrandr = "/usr/bin/xrandr", calibration_file;
  std::string output = "default", action;
  int rotation = -1;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--state-file" && i + 1 < argc) state_file = argv[++i];
      else if (option == "--xrandr" && i + 1 < argc) xrandr = argv[++i];
      else if (option == "--output" && i + 1 < argc) output = argv[++i];
      else if (option == "--rotation" && i + 1 < argc) rotation = std::stoi(argv[++i]);
      else if (option == "--preview") action = "preview";
      else if (option == "--confirm") action = "confirm";
      else if (option == "--rollback") action = "rollback";
      else if (option == "--reset-calibration") action = "reset-calibration";
      else if (option == "--calibration-file" && i + 1 < argc) calibration_file = argv[++i];
      else if (option == "--help") {
        std::cout << "rapid-display-recovery --state-file PATH [--preview --rotation 0|180 | "
                     "--confirm | --rollback | --reset-calibration --calibration-file PATH]\n";
        return 0;
      } else throw std::invalid_argument("unknown or incomplete option");
    }
    require(!state_file.empty() && !action.empty(), "state file and action are required");
    auto state = load(state_file);
    if (action == "preview") {
      require(rotation == 0 || rotation == 180, "preview requires rotation 0 or 180");
      require(fs::is_regular_file(xrandr), "xrandr is unavailable");
      const int previous = state.value("rotation", 0);
      rotate(xrandr, output, rotation);
      atomic_file(state_file, Json{{"rotation", rotation}, {"previous_rotation", previous},
                                   {"pending", true},
                                   {"deadline", monotonic() + 30}}.dump() + "\n");
    } else if (action == "confirm") {
      state["pending"] = false;
      state.erase("deadline");
      state.erase("previous_rotation");
      atomic_file(state_file, state.dump() + "\n");
    } else if (action == "rollback") {
      if (state.value("pending", false)) {
        const int previous = state.value("previous_rotation", 0);
        require(fs::is_regular_file(xrandr), "xrandr is unavailable");
        rotate(xrandr, output, previous);
        state["rotation"] = previous;
      }
      state["pending"] = false;
      state.erase("deadline");
      state.erase("previous_rotation");
      atomic_file(state_file, state.dump() + "\n");
    } else {
      require(!calibration_file.empty(), "calibration file is required");
      std::error_code error;
      fs::remove(calibration_file, error);
      atomic_file(state_file, Json{{"rotation", state.value("rotation", 0)},
                                   {"pending", false},
                                   {"calibration_reset", true}}.dump() + "\n");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
