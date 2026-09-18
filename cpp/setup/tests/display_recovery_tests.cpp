#include "rapid/native.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

using namespace rapid::native;

namespace {
using Matrix = std::array<double, 9>;
const Matrix identity{1, 0, 0, 0, 1, 0, 0, 0, 1};

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int run(const fs::path &program, const std::vector<std::string> &args) {
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
int run(const fs::path &program, std::initializer_list<std::string> args) {
  return run(program, std::vector<std::string>(args));
}
void script(const fs::path &path, const std::string &body) {
  { std::ofstream file(path); file << "#!/bin/sh\n" << body; }
  fs::permissions(path, fs::perms::owner_all);
}
Matrix multiply(const Matrix &left, const Matrix &right) {
  Matrix result{};
  for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column)
      for (int k = 0; k < 3; ++k) result[row * 3 + column] += left[row * 3 + k] * right[k * 3 + column];
  return result;
}
std::array<double, 2> apply(const Matrix &m, double x, double y) {
  return {m[0] * x + m[1] * y + m[2], m[3] * x + m[4] * y + m[5]};
}
bool close(double left, double right) { return std::abs(left - right) <= 1e-6; }
bool same_matrix(const Matrix &left, const Matrix &right) {
  for (int i = 0; i < 9; ++i)
    if (!close(left[i], right[i])) return false;
  return true;
}
void require_maps(const Matrix &m, double x, double y, double expected_x, double expected_y,
                  const char *message) {
  const auto mapped = apply(m, x, y);
  require(close(mapped[0], expected_x) && close(mapped[1], expected_y), message);
}
// Physical taps at fixed device positions: X currently reports them through
// `applied`; the operator was aiming at targets produced by the true mapping.
std::vector<std::string> samples(const Matrix &applied, const Matrix &truth, double target_shift = 0) {
  std::vector<std::string> arguments;
  const double points[5][2] = {{0.15, 0.15}, {0.85, 0.15}, {0.15, 0.85}, {0.85, 0.85}, {0.5, 0.5}};
  for (int i = 0; i < 5; ++i) {
    const auto observed = apply(applied, points[i][0], points[i][1]);
    const auto target = apply(truth, points[i][0], points[i][1]);
    char text[128];
    std::snprintf(text, sizeof text, "%.9f,%.9f,%.9f,%.9f", observed[0], observed[1],
                  target[0] + (i == 4 ? target_shift : 0), target[1]);
    arguments.push_back("--sample");
    arguments.push_back(text);
  }
  return arguments;
}
bool stored_matrix(const fs::path &file, const Matrix &expected) {
  const auto matrix = Json::parse(read_file(file))["matrix"];
  for (int i = 0; i < 6; ++i)
    if (std::abs(matrix[i].get<double>() - expected[i]) > 1e-6) return false;
  return true;
}
std::string last_line(const fs::path &file) {
  std::ifstream input(file);
  std::string line, last;
  while (std::getline(input, line)) if (!line.empty()) last = line;
  return last;
}
// The last coordinate transformation the helper applied through xinput, as a
// 3x3 matrix, so a test can assert where a specific tap coordinate lands.
Matrix last_input_matrix(const fs::path &log) {
  const std::string key = "Coordinate Transformation Matrix";
  const auto line = last_line(log);
  const auto at = line.find(key);
  require(at != std::string::npos, "no touch matrix was applied");
  std::istringstream stream(line.substr(at + key.size()));
  Matrix matrix{};
  for (int i = 0; i < 9; ++i)
    require(static_cast<bool>(stream >> matrix[i]), "touch matrix was truncated");
  return matrix;
}
Json load_state(const fs::path &path) { return Json::parse(read_file(path)); }
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "recovery utility required");
    const fs::path utility = argv[1];
    const auto root = fs::temp_directory_path() / ("rapid-display-" + unique_id());
    fs::create_directory(root);
    const auto state = root / "state.json";
    const auto fake = root / "xrandr";
    const auto xrandr_log = root / "xrandr-calls";
    // A deliberately failing xrandr. If any code path ever executes it, the
    // run fails and leaves a log line; the assertions below require both the
    // success and the absence of the log.
    script(fake, "printf '%s\\n' \"$*\" >> \"" + xrandr_log.string() + "\"\nexit 1\n");
    const auto preview = [&](const fs::path &state_file, int rotation) {
      return run(utility, {"--state-file", state_file.string(), "--xrandr", fake.string(),
                           "--output", "default", "--rotation", std::to_string(rotation), "--preview"});
    };

    // #13: preview persists the requested rotation for the Qt panel and never
    // shells out to xrandr, which cannot rotate this fbdev panel.
    require(preview(state, 180) == 0, "preview succeeds without xrandr");
    const auto previewed = load_state(state);
    require(previewed["pending"] == true && previewed["rotation"] == 180 &&
                previewed["previous_rotation"] == 0,
            "preview persists the rotation and the previous one for rollback");
    require(previewed.contains("deadline") && previewed["deadline"].is_number(),
            "preview arms the rollback deadline");
    require(!fs::exists(xrandr_log), "preview never invokes xrandr");

    // #11: an unconfirmed preview rolls back to the previous orientation.
    require(run(utility, {"--state-file", state.string(), "--rollback"}) == 0, "rollback succeeds");
    const auto rolled_back = load_state(state);
    require(rolled_back["pending"] == false && rolled_back["rotation"] == 0 &&
                !rolled_back.contains("previous_rotation") && !rolled_back.contains("deadline"),
            "rollback restores the previous orientation and clears the preview state");
    require(!fs::exists(xrandr_log), "rollback never invokes xrandr");

    // A confirmed preview keeps the new orientation.
    require(preview(state, 180) == 0 &&
                run(utility, {"--state-file", state.string(), "--confirm"}) == 0,
            "preview then confirm succeeds");
    const auto confirmed = load_state(state);
    require(confirmed["rotation"] == 180 && confirmed["pending"] == false &&
                !confirmed.contains("previous_rotation") && !confirmed.contains("deadline"),
            "confirm keeps the previewed orientation and stops the timer");
    require(!fs::exists(xrandr_log), "confirm never invokes xrandr");

    // Boot recovery of an interrupted preview returns to the confirmed value.
    require(preview(state, 0) == 0, "an interrupted preview is written");
    require(run(utility, {"--state-file", state.string(), "--recover"}) == 0, "boot recovery succeeds");
    require(load_state(state)["pending"] == false && load_state(state)["rotation"] == 180,
            "boot recovery restores the last confirmed orientation");
    require(!fs::exists(xrandr_log), "recovery never invokes xrandr");

    const auto calibration = root / "calibration";
    atomic_file(calibration, "stale");
    require(run(utility, {"--state-file", state.string(), "--reset-calibration",
                          "--calibration-file", calibration.string()}) == 0 &&
                !fs::exists(calibration),
            "calibration reset removes stale calibration");

    // Touch input follows calibration and the X server baseline only: the
    // scene rotation is rendered and unwound by Qt, so the X matrix must be
    // identical at 0 and 180 degrees (#13).
    const auto input_log = root / "input-calls";
    const auto xinput = root / "xinput";
    script(xinput, "printf '%s\\n' \"$*\" >> \"" + input_log.string() + "\"\n"
                   "[ \"$1\" = list ] && printf 'Virtual core pointer\\nADS7846 Touchscreen\\n'\n"
                   "[ \"$1\" = list-props ] && printf 'Device:\\n\\tCoordinate Transformation Matrix (152):\\t"
                   "0.000000, 1.000000, 0.000000, 1.000000, 0.000000, 0.000000, 0.000000, 0.000000, 1.000000\\n'\n"
                   "exit 0\n");
    const std::vector<std::string> input{"--state-file", state.string(), "--xinput", xinput.string(),
                                         "--calibration-file", calibration.string()};
    const auto with = [&](std::vector<std::string> extra) {
      auto arguments = input;
      arguments.insert(arguments.end(), extra.begin(), extra.end());
      return run(utility, arguments);
    };
    // The X server's own matrix from list-props: an axis swap with no rotation.
    const Matrix session_baseline{0, 1, 0, 1, 0, 0, 0, 0, 1};

    const Matrix skewed{0.9, 0.02, 0.04, -0.01, 1.1, -0.05, 0, 0, 1};
    atomic_file(calibration,
                "{\"version\":1,\"matrix\":[0.9,0.02,0.04,-0.01,1.1,-0.05],\"pending\":false}\n");
    atomic_file(state, "{\"rotation\":0,\"pending\":false}\n");
    require(with({"--apply-input"}) == 0, "apply touch at 0 degrees");
    const auto at_zero = last_input_matrix(input_log);
    require(same_matrix(at_zero, skewed), "0 degrees applies the calibration matrix unchanged");
    require_maps(at_zero, 0.5, 0.5, 0.5, 0.495, "0 degrees calibration maps a tap to its target");
    require_maps(at_zero, 0.2, 0.3, 0.226, 0.278, "0 degrees calibration maps a second tap");

    require(preview(state, 180) == 0 && run(utility, {"--state-file", state.string(), "--confirm"}) == 0,
            "preview 180 then confirm for the touch matrix check");
    require(with({"--apply-input"}) == 0, "apply touch at 180 degrees");
    const auto at_half = last_input_matrix(input_log);
    require(same_matrix(at_half, at_zero),
            "the X input matrix is identical at 0 and 180 degrees (no rotation composed)");
    require_maps(at_half, 0.5, 0.5, 0.5, 0.495, "180 degrees maps the same tap coordinate identically");
    require(!fs::exists(xrandr_log), "touch updates never invoke xrandr");

    // Calibration fit recovers the true mapping. The panel submits taps and
    // targets in the unrotated screen frame, so the stored matrix is
    // orientation-independent.
    atomic_file(state, "{\"rotation\":180,\"pending\":false}\n");
    fs::remove(calibration);
    auto calibrate = std::vector<std::string>{"--calibrate"};
    auto taps = samples(identity, skewed);
    calibrate.insert(calibrate.end(), taps.begin(), taps.end());
    require(with(calibrate) == 0 && stored_matrix(calibration, skewed) &&
                load_state(calibration)["pending"] == true,
            "calibration fit recovers the rotation-independent matrix");
    const auto fitted = last_input_matrix(input_log);
    require(same_matrix(fitted, skewed),
            "calibration applies the fitted matrix with no session baseline recorded");
    require_maps(fitted, 0.2, 0.3, 0.226, 0.278,
                 "calibration output maps a tap coordinate to the true target");
    require(with({"--rollback-calibration"}) == 0 && !fs::exists(calibration),
            "unconfirmed first calibration rolls back to no calibration");
    require(same_matrix(last_input_matrix(input_log), identity),
            "rollback to no calibration leaves only the identity session baseline");

    require(with(calibrate) == 0 && with({"--confirm-calibration"}) == 0 &&
                load_state(calibration)["pending"] == false,
            "confirmed calibration is retained");
    const Matrix corrected{1.05, 0, -0.02, 0, 0.95, 0.03, 0, 0, 1};
    calibrate = {"--calibrate"};
    taps = samples(skewed, corrected);
    calibrate.insert(calibrate.end(), taps.begin(), taps.end());
    require(with(calibrate) == 0 && stored_matrix(calibration, corrected),
            "recalibration composes with the applied matrix");
    require(with({"--rollback-calibration"}) == 0 && stored_matrix(calibration, skewed) &&
                load_state(calibration)["pending"] == false,
            "panel restart restores the last confirmed calibration");

    const auto before = read_file(calibration);
    auto collinear = std::vector<std::string>{"--calibrate"};
    for (const char *sample : {"0.1,0.1,0.1,0.1", "0.3,0.3,0.3,0.3", "0.6,0.6,0.6,0.6", "0.9,0.9,0.9,0.9"}) {
      collinear.push_back("--sample");
      collinear.push_back(sample);
    }
    require(with(collinear) != 0 && read_file(calibration) == before,
            "collinear taps are rejected without changing calibration");
    auto inconsistent = std::vector<std::string>{"--calibrate"};
    taps = samples(skewed, skewed, 0.2);
    inconsistent.insert(inconsistent.end(), taps.begin(), taps.end());
    require(with(inconsistent) != 0 && read_file(calibration) == before,
            "an inconsistent tap is rejected without changing calibration");
    atomic_file(calibration, "not json");
    require(with({"--apply-input"}) == 0 && same_matrix(last_input_matrix(input_log), identity),
            "unreadable calibration falls back to the identity session baseline");

    // A vendor display setup may already swap axes through the same X property;
    // it is composed with, not replaced by, the calibration.
    auto vendor = input;
    vendor[5] = (root / "vendor-calibration").string();
    const auto with_vendor = [&](std::vector<std::string> extra) {
      auto arguments = vendor;
      arguments.insert(arguments.end(), extra.begin(), extra.end());
      return run(utility, arguments);
    };
    require(with_vendor({"--rollback-calibration", "--record-input-baseline"}) == 0 &&
                fs::exists(root / "vendor-calibration.baseline") &&
                same_matrix(last_input_matrix(input_log), session_baseline),
            "the X server's configured touch matrix is recorded and composed, not replaced");
    require_maps(last_input_matrix(input_log), 0.2, 0.3, 0.3, 0.2,
                 "the session baseline alone maps a tap coordinate through the axis swap");
    atomic_file(root / "vendor-calibration",
                "{\"version\":1,\"matrix\":[0.9,0.02,0.04,-0.01,1.1,-0.05],\"pending\":false}\n");
    require(with_vendor({"--apply-input"}) == 0 &&
                same_matrix(last_input_matrix(input_log), multiply(skewed, session_baseline)),
            "later touch updates compose the calibration with the recorded session baseline");
    require_maps(last_input_matrix(input_log), 0.2, 0.3, 0.314, 0.167,
                 "calibration over the session baseline maps a tap to the true target");

    script(xinput, "[ \"$1\" = list ] && printf 'Virtual core pointer\\n'\nexit 0\n");
    fs::remove(calibration);
    atomic_file(state, "{\"rotation\":0,\"pending\":false}\n");
    require(preview(state, 0) == 0, "rotation still succeeds without a touchscreen");
    calibrate = {"--calibrate"};
    taps = samples(identity, skewed);
    calibrate.insert(calibrate.end(), taps.begin(), taps.end());
    require(with(calibrate) != 0 && !fs::exists(calibration),
            "calibration fails closed when no touchscreen can receive it");
    require(!fs::exists(xrandr_log), "no recovery action ever invoked xrandr");

    std::error_code error; fs::remove_all(root, error);
    std::cout << "Display preview, rollback, rotation-free touch matrix and calibration passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
