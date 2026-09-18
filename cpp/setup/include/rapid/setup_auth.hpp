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
  // Setup AP name chosen by rapid-provision for this boot (#22).
  fs::path network_ssid_file_;
  // #10: the companion artifact served by the unauthenticated
  // /companion/download route. Empty when nothing suitable was installed, so
  // the setup page never offers a download that cannot be served.
  fs::path companion_artifact_;
  std::string companion_sha256_;
  std::string companion_filename_;
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
  // Publishes the setup AP SSID in use ("rapid" or "rapid-NNNN") as
  // network_ssid in GET /api/v1/setup when the file holds a valid name.
  void set_network_ssid_file(fs::path ssid_file);
  // #10: publishes the companion artifact served, unauthenticated, by
  // /companion/download (it holds no secret). `artifact` may be the file
  // itself or the packaged companion directory holding exactly one artifact;
  // anything missing, unreadable or ambiguous clears it, so a stale or
  // half-installed package can never advertise a download. Its SHA-256 is
  // computed once here and republished in GET /api/v1/setup.
  void set_companion_artifact(fs::path artifact);
  Response handle(const Request &request);
};
} // namespace rapid::native
