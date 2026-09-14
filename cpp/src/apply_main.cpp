#include "rapid/native.hpp"
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <sys/wait.h>
#include <unistd.h>

using namespace rapid::native;
namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::invalid_argument(message);
}

void valid_settings(const Json &settings) {
  require(settings.is_object() && settings.size() == 3 &&
              settings.contains("hostname") && settings["hostname"].is_string() &&
              settings.contains("rotation") && settings["rotation"].is_number_integer() &&
              settings.contains("boot_network") && settings["boot_network"] == "home_then_ap",
          "invalid settings request");
  const auto hostname = settings["hostname"].get<std::string>();
  const auto alnum = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  require(!hostname.empty() && hostname.size() <= 63 && alnum(hostname.front()) &&
              alnum(hostname.back()),
          "invalid hostname request");
  for (const char c : hostname) require(alnum(c) || c == '-', "invalid hostname request");
  require(settings["rotation"] == 0 || settings["rotation"] == 180,
          "invalid rotation request");
}

void set_hostname(const fs::path &program, const std::string &hostname) {
  const auto child = fork();
  if (child < 0) throw std::runtime_error("cannot start hostname command");
  if (child == 0) {
    const int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null >= 0) {
      dup2(null, STDOUT_FILENO);
      dup2(null, STDERR_FILENO);
      if (null > STDERR_FILENO) ::close(null);
    }
    char *const argv[] = {const_cast<char *>(program.c_str()),
                          const_cast<char *>("set-hostname"),
                          const_cast<char *>(hostname.c_str()), nullptr};
    execv(program.c_str(), argv);
    _exit(127);
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    throw std::runtime_error("hostname command failed");
}

void apply_rotation(const fs::path &program, const fs::path &state_file,
                    const std::string &output, int rotation) {
  const auto run = [&](const std::vector<std::string> &arguments) {
    const auto child = fork();
    if (child < 0) throw std::runtime_error("cannot start display recovery command");
    if (child == 0) {
      std::vector<char *> argv;
      argv.reserve(arguments.size() + 2);
      argv.push_back(const_cast<char *>(program.c_str()));
      for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
      argv.push_back(nullptr);
      execv(program.c_str(), argv.data());
      _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
      throw std::runtime_error("display rotation application failed");
  };
  run({"--state-file", state_file.string(), "--output", output,
       "--rotation", std::to_string(rotation), "--preview"});
  run({"--state-file", state_file.string(), "--output", output, "--confirm"});
}
} // namespace

int main(int argc, char **argv) {
  fs::path request_file, result_file;
  std::optional<std::int64_t> revision;
  try {
    fs::path hostnamectl = "/usr/bin/hostnamectl";
    fs::path display_recovery, display_state;
    std::string display_output = "default";
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "rapid-apply --request-file PATH --result-file PATH [--hostnamectl PATH] "
                     "[--display-recovery PATH --display-state-file PATH --display-output NAME]\n"
                     "Applies validated setup settings through constrained services.\n";
        return 0;
      } else if (option == "--request-file" && i + 1 < argc) request_file = argv[++i];
      else if (option == "--result-file" && i + 1 < argc) result_file = argv[++i];
      else if (option == "--hostnamectl" && i + 1 < argc) hostnamectl = argv[++i];
      else if (option == "--display-recovery" && i + 1 < argc) display_recovery = argv[++i];
      else if (option == "--display-state-file" && i + 1 < argc) display_state = argv[++i];
      else if (option == "--display-output" && i + 1 < argc) display_output = argv[++i];
      else throw std::invalid_argument("unknown or incomplete argument; use --help");
    }
    require(!request_file.empty() && !result_file.empty(), "request and result files are required");
    const auto request = Json::parse(read_file(request_file));
    require(request.is_object() && request.size() == 2 &&
                request.contains("revision") && request["revision"].is_number_integer() &&
                request.contains("settings"),
            "invalid settings request");
    revision = request["revision"].get<std::int64_t>();
    valid_settings(request["settings"]);
    require(fs::is_regular_file(hostnamectl) && ::access(hostnamectl.c_str(), X_OK) == 0,
            "hostnamectl is unavailable");
    set_hostname(hostnamectl, request["settings"]["hostname"].get<std::string>());
    Json pending = Json::array({"wifi", "calibration"});
    if (!display_recovery.empty() || !display_state.empty()) {
      require(!display_recovery.empty() && !display_state.empty(),
              "display recovery and state file must be supplied together");
      require(fs::is_regular_file(display_recovery) && ::access(display_recovery.c_str(), X_OK) == 0,
              "display recovery command is unavailable");
      apply_rotation(display_recovery, display_state, display_output,
                     request["settings"]["rotation"].get<int>());
    } else {
      pending.push_back("rotation");
    }
    atomic_file(result_file, Json{{"revision", *revision},
                                  {"hostname_applied", true},
                                  {"pending", pending}}.dump() + "\n");
    fs::remove(request_file);
    log("INFO apply: hostname and configured display settings applied; Wi-Fi and calibration remain pending");
    return 0;
  } catch (const std::exception &error) {
    // A path unit observes the request's existence. Consume failures too, so a
    // malformed or unavailable application cannot spin forever. A subsequent
    // authenticated save creates a fresh request and is the explicit retry.
    if (!request_file.empty()) {
      std::error_code ignored;
      fs::remove(request_file, ignored);
    }
    if (revision && !result_file.empty()) {
      try {
        atomic_file(result_file, Json{{"revision", *revision},
                                      {"hostname_applied", false},
                                      {"error", "hostname application failed"},
                                      {"pending", Json::array({"rotation", "wifi", "calibration"})}}.dump() + "\n");
      } catch (const std::exception &) {}
    }
    log(std::string("ERROR apply: ") + error.what());
    return 1;
  }
}
