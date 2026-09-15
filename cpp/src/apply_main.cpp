#include "rapid/native.hpp"
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <sys/wait.h>
#include <thread>
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

void run_display_recovery(const fs::path &program, const std::vector<std::string> &arguments) {
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
}

// The confirmed orientation, or -1 when unknown or mid-preview (always preview).
int current_rotation(const fs::path &state_file) {
  try {
    if (!fs::exists(state_file)) return 0;
    const auto state = Json::parse(read_file(state_file));
    if (state.is_object() && !state.value("pending", false) &&
        (state.value("rotation", -1) == 0 || state.value("rotation", -1) == 180))
      return state.value("rotation", 0);
  } catch (const std::exception &) {}
  return -1;
}

// Waits for the owner to keep a previewed orientation. Only a confirmation naming
// this exact revision counts; anything else is discarded.
bool owner_confirmed(const fs::path &confirm_file, std::int64_t revision, int timeout_seconds) {
  const auto deadline = monotonic() + timeout_seconds;
  while (monotonic() < deadline) {
    if (fs::exists(confirm_file)) {
      bool match = false;
      try {
        const auto confirmation = Json::parse(read_file(confirm_file));
        match = confirmation.is_object() && confirmation.size() == 1 &&
                confirmation.value("revision", std::int64_t{-1}) == revision;
      } catch (const std::exception &) {}
      std::error_code ignored;
      fs::remove(confirm_file, ignored);
      if (match) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}
} // namespace

int main(int argc, char **argv) {
  fs::path request_file, result_file;
  std::optional<std::int64_t> revision;
  bool consumed = false, hostname_applied = false;
  try {
    fs::path hostnamectl = "/usr/bin/hostnamectl";
    fs::path display_recovery, display_state, display_calibration, display_confirm;
    std::string display_output = "default";
    int confirm_timeout = 30;
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--help") {
        std::cout << "rapid-apply --request-file PATH --result-file PATH [--hostnamectl PATH] "
                     "[--display-recovery PATH --display-state-file PATH --display-confirm-file PATH "
                     "[--display-output NAME] [--display-calibration-file PATH] "
                     "[--display-confirm-timeout SECONDS]]\n"
                     "Applies validated setup settings through constrained services. A changed "
                     "orientation is kept only if the owner confirms it before the timeout.\n";
        return 0;
      } else if (option == "--request-file" && i + 1 < argc) request_file = argv[++i];
      else if (option == "--result-file" && i + 1 < argc) result_file = argv[++i];
      else if (option == "--hostnamectl" && i + 1 < argc) hostnamectl = argv[++i];
      else if (option == "--display-recovery" && i + 1 < argc) display_recovery = argv[++i];
      else if (option == "--display-state-file" && i + 1 < argc) display_state = argv[++i];
      else if (option == "--display-output" && i + 1 < argc) display_output = argv[++i];
      else if (option == "--display-calibration-file" && i + 1 < argc) display_calibration = argv[++i];
      else if (option == "--display-confirm-file" && i + 1 < argc) display_confirm = argv[++i];
      else if (option == "--display-confirm-timeout" && i + 1 < argc) confirm_timeout = std::stoi(argv[++i]);
      else throw std::invalid_argument("unknown or incomplete argument; use --help");
    }
    require(!request_file.empty() && !result_file.empty(), "request and result files are required");
    require(confirm_timeout >= 1 && confirm_timeout <= 120, "confirmation timeout must be 1 to 120 seconds");
    const auto contents = read_file(request_file);
    // Consume before applying: a newer save made while waiting for the owner
    // must survive and trigger its own application afterwards.
    fs::remove(request_file);
    consumed = true;
    const auto request = Json::parse(contents);
    require(request.is_object() && request.size() == 2 &&
                request.contains("revision") && request["revision"].is_number_integer() &&
                request.contains("settings"),
            "invalid settings request");
    revision = request["revision"].get<std::int64_t>();
    valid_settings(request["settings"]);
    require(fs::is_regular_file(hostnamectl) && ::access(hostnamectl.c_str(), X_OK) == 0,
            "hostnamectl is unavailable");
    set_hostname(hostnamectl, request["settings"]["hostname"].get<std::string>());
    hostname_applied = true;
    Json result{{"revision", *revision}, {"hostname_applied", true}, {"pending", Json::array({"wifi"})}};
    if (display_recovery.empty() && display_state.empty() && display_confirm.empty()) {
      result["pending"] = Json::array({"rotation", "wifi"});
    } else {
      require(!display_recovery.empty() && !display_state.empty() && !display_confirm.empty(),
              "display recovery, state file and confirmation file must be supplied together");
      require(fs::is_regular_file(display_recovery) && ::access(display_recovery.c_str(), X_OK) == 0,
              "display recovery command is unavailable");
      const int rotation = request["settings"]["rotation"].get<int>();
      if (current_rotation(display_state) == rotation) {
        result["rotation"] = "unchanged";
      } else {
        std::vector<std::string> preview{"--state-file", display_state.string(), "--output", display_output,
                                         "--rotation", std::to_string(rotation), "--preview"};
        std::vector<std::string> rollback{"--state-file", display_state.string(), "--output", display_output,
                                          "--rollback"};
        // Touch coordinates must follow the picture, including any saved calibration.
        if (!display_calibration.empty())
          for (auto *arguments : {&preview, &rollback}) {
            arguments->push_back("--calibration-file");
            arguments->push_back(display_calibration.string());
          }
        std::error_code ignored;
        fs::remove(display_confirm, ignored);
        run_display_recovery(display_recovery, preview);
        result["rotation"] = "awaiting_confirmation";
        result["confirm_timeout_seconds"] = confirm_timeout;
        result["pending"] = Json::array({"rotation", "wifi"});
        atomic_file(result_file, result.dump() + "\n");
        log("INFO apply: orientation preview is waiting for owner confirmation");
        if (owner_confirmed(display_confirm, *revision, confirm_timeout)) {
          run_display_recovery(display_recovery, {"--state-file", display_state.string(),
                                                  "--output", display_output, "--confirm"});
          result["rotation"] = "confirmed";
          result["pending"] = Json::array({"wifi"});
        } else {
          run_display_recovery(display_recovery, rollback);
          result["rotation"] = "rolled_back";
        }
        result.erase("confirm_timeout_seconds");
      }
    }
    atomic_file(result_file, result.dump() + "\n");
    log("INFO apply: hostname applied; orientation " + result.value("rotation", std::string("not configured")));
    return 0;
  } catch (const std::exception &error) {
    // A path unit observes the request's existence. Consume failures too, so a
    // malformed or unavailable application cannot spin forever. A subsequent
    // authenticated save creates a fresh request and is the explicit retry.
    if (!consumed && !request_file.empty()) {
      std::error_code ignored;
      fs::remove(request_file, ignored);
    }
    if (revision && !result_file.empty()) {
      try {
        Json failure{{"revision", *revision}, {"hostname_applied", hostname_applied},
                     {"error", hostname_applied ? "display orientation application failed"
                                                : "hostname application failed"},
                     {"pending", Json::array({"rotation", "wifi"})}};
        if (hostname_applied) failure["rotation"] = "failed";
        atomic_file(result_file, failure.dump() + "\n");
      } catch (const std::exception &) {}
    }
    log(std::string("ERROR apply: ") + error.what());
    return 1;
  }
}
