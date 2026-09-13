#pragma once
#include "rapid/setup.hpp"

namespace rapid::native {
class SetupAuth {
  struct Session { double expires; std::string csrf; };
  SetupStore &store_;
  std::function<double()> clock_;
  std::mutex mutex_;
  std::map<std::string, Session> sessions_;
  std::deque<double> attempts_;
  std::string origin_;
  std::string authority_;
  std::string enrollment_token_;
  fs::path apply_request_file_;
  fs::path apply_result_file_;
  fs::path wifi_request_file_;
  fs::path wifi_result_file_;
  fs::path firstboot_status_file_;
  void expire(double time);

public:
  SetupAuth(SetupStore &store, int port, std::function<double()> clock = monotonic,
            std::string enrollment_token = {}, std::string host = "127.0.0.1",
            fs::path apply_request_file = {}, fs::path apply_result_file = {},
            fs::path firstboot_status_file = {}, fs::path wifi_request_file = {},
            fs::path wifi_result_file = {});
  // The caller supplies an exact loopback or AP authority. It is never inferred
  // from an untrusted Host header.
  bool enroll(const std::string &password);
  Response handle(const Request &request);
};
} // namespace rapid::native
