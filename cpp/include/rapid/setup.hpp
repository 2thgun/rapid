#pragma once
#include "rapid/native.hpp"

namespace rapid::native {
// Desired configuration only. Applying network/display changes belongs to the
// privileged device service, not the telemetry HTTP process.
class SetupStore {
  Database store_;
  mutable std::mutex mutex_;
  Json snapshot_unlocked();

public:
  explicit SetupStore(const fs::path &directory);
  Json snapshot();
  // Compare-and-swap prevents a delayed browser/service request from replacing
  // newer configuration. No unauthenticated HTTP write route exposes this.
  bool update(std::int64_t expected_revision, const Json &settings);
};

// Only public setup information; never serialize the persistent document into
// an HTTP response. Future credentials must stay behind this allowlist.
Json setup_status(const Json &snapshot);
} // namespace rapid::native
