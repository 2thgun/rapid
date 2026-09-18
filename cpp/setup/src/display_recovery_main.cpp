#include "rapid/native.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sys/wait.h>
#include <unistd.h>

using namespace rapid::native;

namespace {
// X input "Coordinate Transformation Matrix": row-major 3x3 affine transform from
// normalized touch-device coordinates to normalized screen coordinates.
using Matrix = std::array<double, 9>;
const Matrix identity{1, 0, 0, 0, 1, 0, 0, 0, 1};

void require(bool ok, const char *message) {
  if (!ok) throw std::invalid_argument(message);
}

int wait_for(pid_t child) {
  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) return 127;
  return WEXITSTATUS(status);
}

std::vector<char *> arguments_for(const fs::path &program, const std::vector<std::string> &arguments) {
  std::vector<char *> argv{const_cast<char *>(program.c_str())};
  for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
  argv.push_back(nullptr);
  return argv;
}

void run(const fs::path &program, const std::vector<std::string> &arguments, const char *failure) {
  auto argv = arguments_for(program, arguments);
  const auto child = fork();
  require(child >= 0, failure);
  if (child == 0) {
    execv(program.c_str(), argv.data());
    _exit(127);
  }
  require(wait_for(child) == 0, failure);
}

std::string output_of(const fs::path &program, const std::vector<std::string> &arguments) {
  int descriptors[2];
  require(pipe(descriptors) == 0, "cannot list input devices");
  auto argv = arguments_for(program, arguments);
  const auto child = fork();
  if (child < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    throw std::runtime_error("cannot list input devices");
  }
  if (child == 0) {
    close(descriptors[0]);
    dup2(descriptors[1], STDOUT_FILENO);
    close(descriptors[1]);
    execv(program.c_str(), argv.data());
    _exit(127);
  }
  close(descriptors[1]);
  std::string output;
  char buffer[4096];
  for (ssize_t length; (length = read(descriptors[0], buffer, sizeof buffer)) > 0;)
    if (output.size() < 65536) output.append(buffer, static_cast<std::size_t>(length));
  close(descriptors[0]);
  require(wait_for(child) == 0, "input device listing failed");
  return output;
}

Json load(const fs::path &path) {
  if (!fs::exists(path)) return Json{{"rotation", 0}, {"pending", false}};
  const auto state = Json::parse(read_file(path));
  require(state.is_object() && (state.value("rotation", -1) == 0 ||
                                    state.value("rotation", -1) == 180),
          "invalid display recovery state");
  return state;
}

Matrix multiply(const Matrix &left, const Matrix &right) {
  Matrix result{};
  for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column)
      for (int k = 0; k < 3; ++k) result[row * 3 + column] += left[row * 3 + k] * right[k * 3 + column];
  return result;
}

void require_plausible(const Matrix &matrix) {
  for (int i = 0; i < 6; ++i)
    require(std::isfinite(matrix[i]) && std::abs(matrix[i]) <= 10, "invalid calibration matrix");
  const double scale = matrix[0] * matrix[4] - matrix[1] * matrix[3];
  require(std::abs(scale) >= 0.25 && std::abs(scale) <= 4, "calibration result is implausible");
}

struct Calibration {
  bool present = false;
  Matrix matrix = identity;
  bool pending = false;
  std::optional<Matrix> previous;
};

Matrix matrix_from(const Json &value) {
  require(value.is_array() && value.size() == 6, "invalid calibration matrix");
  Matrix matrix = identity;
  for (std::size_t i = 0; i < 6; ++i) {
    require(value[i].is_number(), "invalid calibration matrix");
    matrix[i] = value[i].get<double>();
  }
  require_plausible(matrix);
  return matrix;
}

Json matrix_json(const Matrix &matrix) {
  return Json::array({matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]});
}

Calibration load_calibration(const fs::path &path) {
  Calibration calibration;
  if (!fs::exists(path)) return calibration;
  try {
    const auto document = Json::parse(read_file(path));
    require(document.is_object() && document.value("version", 0) == 1, "invalid calibration file");
    calibration.matrix = matrix_from(document.at("matrix"));
    calibration.pending = document.value("pending", false);
    if (calibration.pending && document.contains("previous") && !document["previous"].is_null())
      calibration.previous = matrix_from(document["previous"]);
    calibration.present = true;
  } catch (const std::exception &error) {
    // Unusable calibration must never lock out touch input: fall back to identity.
    log(std::string("WARNING display-recovery: ignoring calibration file: ") + error.what());
    return Calibration{};
  }
  return calibration;
}

void save_calibration(const fs::path &path, const Calibration &calibration) {
  Json document{{"version", 1}, {"matrix", matrix_json(calibration.matrix)},
                {"pending", calibration.pending}};
  if (calibration.pending)
    document["previous"] = calibration.previous ? matrix_json(*calibration.previous) : Json(nullptr);
  atomic_file(path, document.dump() + "\n");
}

void roll_back_calibration(const fs::path &path) {
  const auto calibration = load_calibration(path);
  if (!calibration.pending) return;
  if (!calibration.previous) {
    fs::remove(path);
    return;
  }
  save_calibration(path, Calibration{true, *calibration.previous, false, std::nullopt});
}

std::string lower(std::string value) {
  for (auto &character : value)
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  return value;
}

bool affine(const Matrix &matrix) {
  for (const double value : matrix)
    if (!std::isfinite(value)) return false;
  return std::abs(matrix[6]) <= 1e-6 && std::abs(matrix[7]) <= 1e-6 && std::abs(matrix[8] - 1) <= 1e-6;
}

fs::path baseline_file(const fs::path &calibration_file) {
  auto path = calibration_file;
  path += ".baseline";
  return path;
}

// Reads the matrix X applies from its own configuration (for example a vendor
// display setup) so raPId composes with it instead of replacing it.
Matrix parse_baseline(const std::string &properties) {
  try {
    const auto at = properties.find("Coordinate Transformation Matrix (");
    const auto colon = at == std::string::npos ? at : properties.find(':', at);
    if (colon == std::string::npos) return identity;
    const auto end = properties.find('\n', colon);
    const auto values = properties.substr(colon + 1, end == std::string::npos ? end : end - colon - 1);
    Matrix matrix{};
    std::size_t start = 0;
    for (int i = 0; i < 9; ++i) {
      const auto comma = values.find(',', start);
      if (comma == std::string::npos && i < 8) return identity;
      matrix[i] = std::stod(values.substr(start, comma == std::string::npos ? comma : comma - start));
      start = comma + 1;
    }
    return affine(matrix) ? matrix : identity;
  } catch (const std::exception &) {
    return identity;
  }
}

Matrix stored_baseline(const Json &devices, const std::string &device) {
  if (!devices.is_object() || !devices.contains(device) || !devices[device].is_array() ||
      devices[device].size() != 9)
    return identity;
  Matrix matrix{};
  for (std::size_t i = 0; i < 9; ++i) {
    if (!devices[device][i].is_number()) return identity;
    matrix[i] = devices[device][i].get<double>();
  }
  return affine(matrix) ? matrix : identity;
}

void apply_input(const fs::path &xinput, const std::string &touch_device,
                 const fs::path &calibration_file, bool required, bool record_baseline = false) {
  std::vector<std::string> devices;
  if (!touch_device.empty()) {
    devices.push_back(touch_device);
  } else if (fs::is_regular_file(xinput)) {
    std::string names = output_of(xinput, {"list", "--name-only"});
    for (std::size_t start = 0; start < names.size();) {
      auto end = names.find('\n', start);
      if (end == std::string::npos) end = names.size();
      const auto name = names.substr(start, end - start);
      const auto key = lower(name);
      if (key.find("touch") != std::string::npos && key.find("virtual core") == std::string::npos)
        devices.push_back(name);
      start = end + 1;
    }
  }
  if (devices.empty()) {
    require(!required, "no touchscreen input device found");
    log("WARNING display-recovery: no touchscreen input device found; touch transform not applied");
    return;
  }
  const auto baselines = baseline_file(calibration_file);
  if (record_baseline) {
    // Only valid before raPId changes the property, i.e. at X session start.
    Json recorded{{"version", 1}, {"devices", Json::object()}};
    for (const auto &device : devices) {
      Matrix baseline = identity;
      try {
        baseline = parse_baseline(output_of(xinput, {"list-props", device}));
      } catch (const std::exception &) {}
      recorded["devices"][device] = Json(std::vector<double>(baseline.begin(), baseline.end()));
    }
    try {
      atomic_file(baselines, recorded.dump() + "\n");
    } catch (const std::exception &error) {
      log(std::string("WARNING display-recovery: cannot record touch baseline: ") + error.what());
    }
  }
  Json stored = Json::object();
  try {
    if (fs::exists(baselines)) stored = Json::parse(read_file(baselines)).at("devices");
  } catch (const std::exception &) {}
  // #13: rotation is a Qt scene transform now, so it is deliberately absent
  // here. The X matrix is the touch calibration over the session baseline and
  // is identical at 0 and 180 degrees; Qt's own hit-testing unwinds the scene
  // rotation before the panel sees a tap.
  const auto calibrated = load_calibration(calibration_file).matrix;
  for (const auto &device : devices) {
    std::vector<std::string> arguments{"set-prop", device, "Coordinate Transformation Matrix"};
    for (const double value : multiply(calibrated, stored_baseline(stored, device))) {
      char text[32];
      std::snprintf(text, sizeof text, "%.6f", std::abs(value) < 5e-7 ? 0.0 : value);
      arguments.push_back(text);
    }
    run(xinput, arguments, "cannot apply touchscreen transformation");
  }
}

std::array<double, 4> parse_sample(const std::string &text) {
  std::array<double, 4> sample{};
  std::size_t start = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto end = i == 3 ? text.size() : text.find(',', start);
    require(end != std::string::npos, "sample must be observed_x,observed_y,target_x,target_y");
    const auto field = text.substr(start, end - start);
    std::size_t consumed = 0;
    sample[i] = std::stod(field, &consumed);
    require(consumed == field.size() && std::isfinite(sample[i]),
            "sample must be observed_x,observed_y,target_x,target_y");
    start = end + 1;
  }
  return sample;
}

double determinant(const Matrix &m) {
  return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
         m[2] * (m[3] * m[7] - m[4] * m[6]);
}

// Least-squares affine correction from normalized observed tap positions (as
// currently delivered by X) to the normalized targets that were displayed.
Matrix fit_correction(const std::vector<std::array<double, 4>> &samples) {
  require(samples.size() >= 4 && samples.size() <= 16, "calibration requires 4 to 16 samples");
  Matrix normal{};
  std::array<double, 3> x_terms{}, y_terms{};
  for (const auto &sample : samples) {
    require(sample[0] >= -0.25 && sample[0] <= 1.25 && sample[1] >= -0.25 && sample[1] <= 1.25,
            "calibration tap is outside the display");
    require(sample[2] >= 0 && sample[2] <= 1 && sample[3] >= 0 && sample[3] <= 1,
            "calibration target is outside the display");
    const std::array<double, 3> point{sample[0], sample[1], 1};
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 3; ++column) normal[row * 3 + column] += point[row] * point[column];
      x_terms[row] += point[row] * sample[2];
      y_terms[row] += point[row] * sample[3];
    }
  }
  const double scale = determinant(normal);
  require(std::abs(scale) > 1e-6, "calibration taps must not lie on one line");
  const auto solve = [&](const std::array<double, 3> &terms) {
    std::array<double, 3> solution{};
    for (int column = 0; column < 3; ++column) {
      auto replaced = normal;
      for (int row = 0; row < 3; ++row) replaced[row * 3 + column] = terms[row];
      solution[column] = determinant(replaced) / scale;
    }
    return solution;
  };
  const auto x = solve(x_terms), y = solve(y_terms);
  const Matrix correction{x[0], x[1], x[2], y[0], y[1], y[2], 0, 0, 1};
  require_plausible(correction);
  for (const auto &sample : samples) {
    const double dx = correction[0] * sample[0] + correction[1] * sample[1] + correction[2] - sample[2];
    const double dy = correction[3] * sample[0] + correction[4] * sample[1] + correction[5] - sample[3];
    require(std::abs(dx) <= 0.05 && std::abs(dy) <= 0.05,
            "calibration taps are inconsistent; retry the calibration");
  }
  return correction;
}
} // namespace

int main(int argc, char **argv) {
  fs::path state_file, xinput = "/usr/bin/xinput", calibration_file;
  std::string action, touch_device;
  std::vector<std::array<double, 4>> samples;
  bool record_baseline = false;
  int rotation = -1;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--state-file" && i + 1 < argc) state_file = argv[++i];
      // Accepted and ignored: rotation is rendered by the Qt panel from the
      // state file, so no X output is touched. Kept so existing callers
      // (rapid-apply) do not have to change their argument list.
      else if (option == "--xrandr" && i + 1 < argc) ++i;
      else if (option == "--xinput" && i + 1 < argc) xinput = argv[++i];
      else if (option == "--touch-device" && i + 1 < argc) touch_device = argv[++i];
      else if (option == "--output" && i + 1 < argc) ++i;
      else if (option == "--rotation" && i + 1 < argc) rotation = std::stoi(argv[++i]);
      else if (option == "--preview") action = "preview";
      else if (option == "--confirm") action = "confirm";
      else if (option == "--rollback" || option == "--recover") action = "rollback";
      else if (option == "--reset-calibration") action = "reset-calibration";
      else if (option == "--calibrate") action = "calibrate";
      else if (option == "--confirm-calibration") action = "confirm-calibration";
      else if (option == "--rollback-calibration") action = "rollback-calibration";
      else if (option == "--apply-input") action = "apply-input";
      else if (option == "--record-input-baseline") record_baseline = true;
      else if (option == "--sample" && i + 1 < argc) samples.push_back(parse_sample(argv[++i]));
      else if (option == "--calibration-file" && i + 1 < argc) calibration_file = argv[++i];
      else if (option == "--help") {
        std::cout << "rapid-display-recovery --state-file PATH [--calibration-file PATH]\n"
                     "  [--xinput PATH --touch-device NAME]\n"
                     "  --preview --rotation 0|180 | --confirm | --rollback | --recover\n"
                     "  | --calibrate --sample OX,OY,TX,TY... | --confirm-calibration\n"
                     "  | --rollback-calibration | --apply-input | --reset-calibration\n"
                     "  [--record-input-baseline]  keep the X server's own touch matrix (session start)\n"
                     "Rotation is rendered by the Qt panel from the state file, not by xrandr\n"
                     "(--xrandr/--output are still accepted and ignored for that reason).\n"
                     "Samples and targets are normalized screen coordinates (0 to 1).\n";
        return 0;
      } else throw std::invalid_argument("unknown or incomplete option");
    }
    require(!state_file.empty() && !action.empty(), "state file and action are required");
    auto state = load(state_file);
    const bool calibration_action = action == "calibrate" || action == "confirm-calibration" ||
                                    action == "rollback-calibration" || action == "apply-input";
    require(!calibration_action || !calibration_file.empty(), "calibration file is required");
    if (action == "preview") {
      require(rotation == 0 || rotation == 180, "preview requires rotation 0 or 180");
      const int previous = state.value("rotation", 0);
      // #13: persist the requested rotation for the Qt panel to render instead
      // of touching the X output. xrandr cannot rotate this fbdev panel (step 1
      // probe: --rotate inverted exits 1), and the panel picks the value up on
      // its next state-file poll, so preview and rollback stay instant.
      atomic_file(state_file, Json{{"rotation", rotation}, {"previous_rotation", previous},
                                   {"pending", true},
                                   {"deadline", monotonic() + 30}}.dump() + "\n");
      if (!calibration_file.empty()) apply_input(xinput, touch_device, calibration_file, false);
    } else if (action == "confirm") {
      state["pending"] = false;
      state.erase("deadline");
      state.erase("previous_rotation");
      atomic_file(state_file, state.dump() + "\n");
    } else if (action == "rollback") {
      if (state.value("pending", false)) {
        const int previous = state.value("previous_rotation", 0);
        state["rotation"] = previous;
      }
      state["pending"] = false;
      state.erase("deadline");
      state.erase("previous_rotation");
      atomic_file(state_file, state.dump() + "\n");
      if (!calibration_file.empty())
        apply_input(xinput, touch_device, calibration_file, false);
    } else if (action == "calibrate") {
      const auto current = load_calibration(calibration_file);
      // #13: the panel submits observed and target taps in the unrotated screen
      // frame, so the stored correction never contains the scene rotation and
      // one calibration is reused at both orientations.
      const auto applied = current.matrix;
      const auto correction = fit_correction(samples);
      const auto next = multiply(correction, applied);
      require_plausible(next);
      const auto previous = current.pending ? current.previous
                            : current.present ? std::optional<Matrix>(current.matrix)
                                              : std::nullopt;
      save_calibration(calibration_file, Calibration{true, next, true, previous});
      try {
        apply_input(xinput, touch_device, calibration_file, true);
      } catch (...) {
        roll_back_calibration(calibration_file);
        throw;
      }
      std::cout << Json{{"pending", true}, {"matrix", matrix_json(next)}}.dump() << '\n';
    } else if (action == "confirm-calibration") {
      auto calibration = load_calibration(calibration_file);
      require(calibration.pending, "no calibration is awaiting confirmation");
      calibration.pending = false;
      calibration.previous.reset();
      save_calibration(calibration_file, calibration);
    } else if (action == "rollback-calibration") {
      roll_back_calibration(calibration_file);
      apply_input(xinput, touch_device, calibration_file, false, record_baseline);
    } else if (action == "apply-input") {
      apply_input(xinput, touch_device, calibration_file, false, record_baseline);
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
