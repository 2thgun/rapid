#include "rapid/native.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace rapid::native;

namespace {
using Matrix = std::array<double, 9>;

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
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "recovery utility required");
    const fs::path utility = argv[1];
    const auto root = fs::temp_directory_path() / ("rapid-display-" + unique_id());
    fs::create_directory(root);
    const auto state = root / "state.json";
    const auto fake = root / "xrandr";
    const auto log = root / "calls";
    script(fake, "printf '%s\\n' \"$*\" >> \"" + log.string() + "\"\n");
    require(run(utility, {"--state-file", state.string(), "--xrandr", fake.string(),
                          "--output", "default", "--rotation", "180", "--preview"}) == 0,
            "preview succeeds");
    auto preview = Json::parse(read_file(state));
    require(preview["pending"] == true && preview["rotation"] == 180 &&
                read_file(log).find("--rotate inverted") != std::string::npos,
            "preview persists rotation and uses inverted output");
    require(run(utility, {"--state-file", state.string(), "--xrandr", fake.string(),
                          "--output", "default", "--rollback"}) == 0,
            "rollback succeeds");
    auto rolled_back = Json::parse(read_file(state));
    require(rolled_back["pending"] == false && rolled_back["rotation"] == 0 &&
                read_file(log).find("--rotate normal") != std::string::npos,
            "rollback restores the previous orientation");
    require(run(utility, {"--state-file", state.string(), "--xrandr", fake.string(),
                          "--output", "default", "--rotation", "180", "--preview"}) == 0,
            "second preview succeeds");
    require(run(utility, {"--state-file", state.string(), "--xrandr", fake.string(),
                          "--output", "default", "--recover"}) == 0,
            "boot recovery succeeds");
    require(Json::parse(read_file(state))["pending"] == false &&
                Json::parse(read_file(state))["rotation"] == 0,
            "boot recovery clears an interrupted preview");
    const auto calibration = root / "calibration";
    atomic_file(calibration, "stale");
    require(run(utility, {"--state-file", state.string(), "--reset-calibration",
                          "--calibration-file", calibration.string()}) == 0 &&
                !fs::exists(calibration),
            "calibration reset removes stale calibration");

    // Touch input follows rotation and calibration through the X input matrix.
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
    const Matrix inverted{-1, 0, 1, 0, -1, 1, 0, 0, 1};
    require(with({"--xrandr", fake.string(), "--rotation", "180", "--preview"}) == 0 &&
                with({"--confirm"}) == 0 &&
                last_line(input_log) == "set-prop ADS7846 Touchscreen Coordinate Transformation Matrix "
                                        "-1.000000 0.000000 1.000000 0.000000 -1.000000 1.000000 "
                                        "0.000000 0.000000 1.000000",
            "inverted rotation also inverts touch coordinates");

    const Matrix skewed{0.9, 0.02, 0.04, -0.01, 1.1, -0.05, 0, 0, 1};
    auto calibrate = std::vector<std::string>{"--calibrate"};
    auto taps = samples(inverted, multiply(inverted, skewed));
    calibrate.insert(calibrate.end(), taps.begin(), taps.end());
    require(with(calibrate) == 0 && stored_matrix(calibration, skewed) &&
                Json::parse(read_file(calibration))["pending"] == true &&
                last_line(input_log).find("-0.900000 -0.020000 0.960000 0.010000 -1.100000 1.050000") !=
                    std::string::npos,
            "calibration fit recovers the rotation-independent matrix and applies it");
    require(with({"--rollback-calibration"}) == 0 && !fs::exists(calibration) &&
                last_line(input_log).find("-1.000000 0.000000 1.000000 0.000000 -1.000000") != std::string::npos,
            "unconfirmed first calibration rolls back to rotation only");

    require(with(calibrate) == 0 && with({"--confirm-calibration"}) == 0 &&
                Json::parse(read_file(calibration))["pending"] == false,
            "confirmed calibration is retained");
    const Matrix corrected{1.05, 0, -0.02, 0, 0.95, 0.03, 0, 0, 1};
    calibrate = {"--calibrate"};
    taps = samples(multiply(inverted, skewed), multiply(inverted, corrected));
    calibrate.insert(calibrate.end(), taps.begin(), taps.end());
    require(with(calibrate) == 0 && stored_matrix(calibration, corrected),
            "recalibration composes with the applied matrix");
    require(with({"--rollback-calibration"}) == 0 && stored_matrix(calibration, skewed) &&
                Json::parse(read_file(calibration))["pending"] == false,
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
    taps = samples(multiply(inverted, skewed), multiply(inverted, skewed), 0.2);
    inconsistent.insert(inconsistent.end(), taps.begin(), taps.end());
    require(with(inconsistent) != 0 && read_file(calibration) == before,
            "an inconsistent tap is rejected without changing calibration");
    atomic_file(calibration, "not json");
    require(with({"--apply-input"}) == 0 &&
                last_line(input_log).find("-1.000000 0.000000 1.000000 0.000000 -1.000000") != std::string::npos,
            "unreadable calibration falls back to rotation-only touch input");

    // A vendor display setup may already swap axes through the same X property.
    auto vendor = input;
    vendor[5] = (root / "vendor-calibration").string();
    const auto with_vendor = [&](std::vector<std::string> extra) {
      auto arguments = vendor;
      arguments.insert(arguments.end(), extra.begin(), extra.end());
      return run(utility, arguments);
    };
    const std::string inverted_swap =
        "0.000000 -1.000000 1.000000 -1.000000 0.000000 1.000000 0.000000 0.000000 1.000000";
    require(with_vendor({"--rollback-calibration", "--record-input-baseline"}) == 0 &&
                fs::exists(root / "vendor-calibration.baseline") &&
                last_line(input_log).find(inverted_swap) != std::string::npos,
            "the X server's configured touch matrix is recorded and composed, not replaced");
    require(with_vendor({"--apply-input"}) == 0 && last_line(input_log).find(inverted_swap) != std::string::npos,
            "later touch updates keep the recorded session baseline");

    script(xinput, "[ \"$1\" = list ] && printf 'Virtual core pointer\\n'\nexit 0\n");
    fs::remove(calibration);
    require(with({"--xrandr", fake.string(), "--rotation", "0", "--preview"}) == 0,
            "rotation still succeeds without a touchscreen");
    calibrate = {"--calibrate"};
    taps = samples(Matrix{1, 0, 0, 0, 1, 0, 0, 0, 1}, skewed);
    calibrate.insert(calibrate.end(), taps.begin(), taps.end());
    require(with(calibrate) != 0 && !fs::exists(calibration),
            "calibration fails closed when no touchscreen can receive it");

    std::error_code error; fs::remove_all(root, error);
    std::cout << "Display preview, rollback, touch transform and calibration passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
