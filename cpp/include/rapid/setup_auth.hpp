#pragma once
#include "rapid/setup.hpp"
#include "rapid/pairing.hpp"
#include <memory>

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
  fs::path calibration_file_;
  fs::path calibration_request_file_;
  fs::path display_confirm_file_;
  PairingCoordinator *pairing_ = nullptr;
  std::unique_ptr<PairingTransport> pairing_transport_;
  bool secure_transport_ = false;
  std::string certificate_fingerprint_;
  // Device access (#23): owner-chosen account password and SSH key.
  fs::path account_request_file_;
  fs::path account_result_file_;
  std::deque<double> account_attempts_;
  void expire(double time);
  Response handle_account(const Request &request, const std::string &path,
                          const std::string &csrf, double time);

public:
  SetupAuth(SetupStore &store, int port, std::function<double()> clock = monotonic,
            std::string enrollment_token = {}, std::string host = "127.0.0.1",
            fs::path apply_request_file = {}, fs::path apply_result_file = {},
            fs::path firstboot_status_file = {}, fs::path wifi_request_file = {},
            fs::path wifi_result_file = {}, PairingCoordinator *pairing = nullptr,
            bool secure_transport = false, std::string certificate_fingerprint = {},
            bool pairing_transport_enabled = true, fs::path calibration_file = {},
            fs::path calibration_request_file = {}, fs::path display_confirm_file = {});
  // The caller supplies an exact loopback or AP authority. It is never inferred
  // from an untrusted Host header.
  bool enroll(const std::string &password);
  // Enables /api/v1/account. The request carries only a password hash and is
  // consumed by the root rapid-account helper; the result carries only status.
  void set_account_files(fs::path request_file, fs::path result_file);
  Response handle(const Request &request);
};
} // namespace rapid::native
