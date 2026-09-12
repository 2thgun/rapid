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
  void expire(double time);

public:
  SetupAuth(SetupStore &store, int port, std::function<double()> clock = monotonic,
            std::string enrollment_token = {});
  // Local enrollment only until physical/AP bootstrap authorization is built.
  bool enroll(const std::string &password);
  Response handle(const Request &request);
};
} // namespace rapid::native
