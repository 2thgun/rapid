// #44: the physical owner reset. Covers the root rapid-owner-reset helper
// against a fixture state directory — owner cleared, token reissued, status
// published, request consumed, identity/paired PCs/settings untouched — the
// rejection cases that must leave the device exactly as it was, and the setup
// server's runtime re-read of the token file that reopens enrollment without
// a restart, including the fail-closed cases.
#include "rapid/setup_auth.hpp"
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace rapid::native;
namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
unsigned mode_of(const fs::path &path) {
  struct stat info {};
  require(::lstat(path.c_str(), &info) == 0, "stat " + path.string());
  return info.st_mode & 07777;
}
uid_t owner_of(const fs::path &path) {
  struct stat info {};
  require(::lstat(path.c_str(), &info) == 0, "stat " + path.string());
  return info.st_uid;
}
struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path() / ("rapid-owner-reset-tests-" + unique_id());
  TemporaryDirectory() { fs::create_directory(path); }
  ~TemporaryDirectory() { std::error_code error; fs::remove_all(path, error); }
};
Request request(const std::string &path, const std::string &method = "GET", const Json &body = {}) {
  return {method, path, method == "GET" ? "" : body.dump(),
          {{"host", "127.0.0.1:8002"}, {"origin", "http://127.0.0.1:8002"}, {"content-type", "application/json"}}};
}
// A well-formed stored hash shape; the helper never sees a password.
const char *kOwnerHash = "$argon2id$fixturehashfixturehashfixturehash";
std::string cookie(const Response &response) {
  for (const auto &[name, value] : response.headers)
    if (name == "Set-Cookie") return value.substr(0, value.find(';'));
  throw std::runtime_error("missing session cookie");
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "owner-reset helper binary required");
    const fs::path helper = argv[1];
    TemporaryDirectory root;
    const auto state_directory = root.path / "state";
    const auto run_directory = root.path / "run";
    const auto apply_directory = root.path / "apply";
    fs::create_directories(run_directory);
    fs::create_directories(apply_directory);
    const auto token_file = state_directory / "enrollment.token";
    const auto status_file = run_directory / "firstboot.json";
    const auto request_file = apply_directory / "owner-reset-request.json";
    const auto result_file = apply_directory / "owner-reset-result.json";

    // A fixture device: enrolled owner, a published activation token, a paired
    // PC and saved settings.
    SetupStore store(state_directory);
    const auto device_id = store.snapshot()["device_id"].get<std::string>();
    const auto hostname = store.snapshot()["settings"]["hostname"].get<std::string>();
    const auto setup_complete = store.snapshot()["setup_complete"].get<bool>();
    require(store.claim_owner(kOwnerHash), "fixture owner is enrolled");
    require(store.remember_peer(std::string(32, 'a'), "Driver PC", std::string(64, 'b'), 1.0),
            "fixture paired PC is stored");
    const std::string first_token = unique_id() + unique_id();
    atomic_file(token_file, first_token + "\n", fs::perms::owner_read | fs::perms::owner_write);
    const auto bootstrap = Json{{"setup_address", "192.168.1.64"},
                                 {"setup_port", 8002},
                                 {"setup_url", "https://192.168.1.64:8002/setup"},
                                 {"certificate_fingerprint", std::string(64, 'c')},
                                 {"activation_token", first_token}};
    const auto write_status = [&] {
      atomic_file(status_file, Json{{"owner_configured", true},
                                     {"setup_complete", true},
                                     {"state", "complete"},
                                     {"bootstrap", bootstrap}}.dump() + "\n",
                  fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    };
    write_status();

    const auto run_helper = [&] {
      const auto child = fork();
      require(child >= 0, "fork owner-reset helper");
      if (child == 0) {
        execl(helper.c_str(), helper.c_str(),
              "--request-file", request_file.c_str(),
              "--result-file", result_file.c_str(),
              "--state-directory", state_directory.c_str(),
              "--status-file", status_file.c_str(), nullptr);
        _exit(127);
      }
      int wait_status = 0;
      require(waitpid(child, &wait_status, 0) == child && WIFEXITED(wait_status),
              "owner-reset helper exited");
      return WEXITSTATUS(wait_status);
    };
    const auto write_request = [&](const Json &body) {
      atomic_file(request_file, body.dump(), fs::perms::owner_read | fs::perms::owner_write);
    };
    const auto result_body = [&] {
      std::error_code error;
      return fs::is_regular_file(result_file, error) ? Json::parse(read_file(result_file)) : Json();
    };

    // A malformed request is rejected without touching the device: the owner
    // stays enrolled and the token is not rotated.
    write_request(Json{{"request_id", "not-a-valid-id"}});
    require(run_helper() == 1, "a malformed request id is rejected");
    require(result_body()["status"] == "failed", "the failure is reported");
    require(store.owner_hash() == kOwnerHash, "a rejected request keeps the owner");
    require(read_file(token_file) == first_token + "\n", "a rejected request keeps the token");
    write_request(Json{{"request_id", std::string(32, '1')}, {"password", "secret"}});
    require(run_helper() == 1, "a request carrying anything but its id is rejected");
    require(store.owner_hash() == kOwnerHash, "a rejected request keeps the owner");
    // A missing first-boot status document is rejected before anything is
    // mutated, so a reset can never leave the new token unpublished.
    fs::remove(status_file);
    write_request(Json{{"request_id", std::string(32, '2')}});
    require(run_helper() == 1, "a missing status document is rejected");
    require(store.owner_hash() == kOwnerHash, "a rejected request keeps the owner");
    require(read_file(token_file) == first_token + "\n", "a rejected request keeps the token");
    write_status();

    // The physical reset: the owner password is cleared, the token is reissued
    // and published, and nothing else changes.
    write_request(Json{{"request_id", std::string(32, '3')}});
    require(run_helper() == 0, "the owner reset applies");
    const auto applied = result_body();
    require(applied["status"] == "applied" && applied["owner_reset"] == true &&
                applied["enrollment"] == "reopened" &&
                applied["request_id"].get<std::string>() == std::string(32, '3'),
            "the result reports the reset without secrets");
    require(applied.dump().find(first_token) == std::string::npos,
            "the result never carries the activation token");
    require(store.owner_hash().empty(), "the owner password is cleared");
    const auto second_token = validated_enrollment_token(token_file);
    require(second_token.size() == 64 && second_token != first_token,
            "the enrollment token is reissued");
    require(mode_of(token_file) == 0600 && owner_of(token_file) == owner_of(state_directory),
            "the new token stays private and owned by the setup service user");
    const auto published = Json::parse(read_file(status_file));
    require(published["owner_configured"] == false &&
                published["state"] == "owner_enrollment_required" &&
                published["bootstrap"]["activation_token"] == second_token &&
                published["bootstrap"]["setup_url"] == "https://192.168.1.64:8002/setup" &&
                published["bootstrap"]["certificate_fingerprint"] == std::string(64, 'c'),
            "the new token is published with the bootstrap address and fingerprint kept");
    require(mode_of(status_file) == 0640, "the status document stays group-readable only");
    require(!fs::exists(request_file), "the request is consumed");
    const auto after = store.snapshot();
    require(after["device_id"] == device_id && after["setup_complete"] == setup_complete &&
                after["settings"]["hostname"] == hostname,
            "device identity and settings are kept");
    require(store.peers().size() == 1 && store.peers()[0]["label"] == "Driver PC",
            "paired PCs are kept");

    // A second reset is idempotent for the owner and rotates the token again.
    write_request(Json{{"request_id", std::string(32, '4')}});
    require(run_helper() == 0, "a second reset applies");
    require(result_body()["owner_reset"] == false, "the second reset reports no owner to clear");
    require(validated_enrollment_token(token_file) != second_token, "the token rotates again");

    // The setup server re-reads the token file while no owner is enrolled, so
    // the reset reopens enrollment without a service restart.
    {
      SetupStore fresh(root.path / "reopen");
      const auto reopen_token = unique_id() + unique_id();
      const auto reopen_token_file = root.path / "reopen" / "enrollment.token";
      atomic_file(reopen_token_file, reopen_token + "\n", fs::perms::owner_read | fs::perms::owner_write);
      const auto reopen_status = root.path / "reopen-status.json";
      atomic_file(reopen_status, Json{{"owner_configured", false},
                                      {"state", "owner_enrollment_required"},
                                      {"bootstrap", bootstrap}}.dump() + "\n",
                  fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
      double time = 0;
      SetupAuth auth(fresh, 8002, [&] { return time; }, {}, "127.0.0.1", {}, {}, reopen_status);
      auth.set_enrollment_token_file(reopen_token_file);
      auto public_setup = Json::parse(auth.handle(request("/api/v1/setup")).body);
      require(public_setup["capabilities"]["browser_owner_enrollment"] == true,
              "the reset reopens enrollment without a restart");
      // Fail closed: an invalid token file is rejected and never becomes the
      // token; the last known good token keeps working.
      atomic_file(reopen_token_file, "too-short\n", fs::perms::owner_read | fs::perms::owner_write);
      public_setup = Json::parse(auth.handle(request("/api/v1/setup")).body);
      require(public_setup["capabilities"]["browser_owner_enrollment"] == true,
              "an invalid token file is rejected without closing enrollment");
      auto invalid = request("/api/v1/auth/enroll", "POST",
          {{"token", "too-short"}, {"password", "browser-owner-password"}});
      require(auth.handle(invalid).status == 403, "the invalid file never becomes the token");
      // An unreadable token file leaves the current token unchanged rather
      // than clearing it, so enrollment cannot be closed by deleting a file.
      fs::remove(reopen_token_file);
      public_setup = Json::parse(auth.handle(request("/api/v1/setup")).body);
      require(public_setup["capabilities"]["browser_owner_enrollment"] == true,
              "a missing token file keeps the last known good token");
      // The file is the live source: a rotated token takes effect at once and
      // the previous token stops working.
      const auto rotated = unique_id() + unique_id();
      atomic_file(reopen_token_file, rotated + "\n", fs::perms::owner_read | fs::perms::owner_write);
      auto stale = request("/api/v1/auth/enroll", "POST",
          {{"token", reopen_token}, {"password", "browser-owner-password"}});
      require(auth.handle(stale).status == 403, "the previous token stops working");
      auto enroll = request("/api/v1/auth/enroll", "POST",
          {{"token", rotated}, {"password", "browser-owner-password"}});
      require(auth.handle(enroll).status == 201, "enrollment reopens with the new token");
      const auto enrolled_status = Json::parse(read_file(reopen_status));
      require(enrolled_status["owner_configured"] == true &&
                  enrolled_status["state"] == "settings_application_required" &&
                  !enrolled_status["bootstrap"].contains("activation_token"),
              "enrollment through the re-read token clears it and records the owner");
      require(fresh.owner_hash().starts_with("$argon2id$"), "the new owner is stored");
    }
    // While an owner is enrolled the token file alone cannot reopen
    // enrollment: only the physical reset, which clears the owner first,
    // reopens it.
    {
      SetupStore enrolled(root.path / "enrolled");
      require(enrolled.claim_owner(kOwnerHash), "fixture owner is enrolled");
      const auto enrolled_token_file = root.path / "enrolled" / "enrollment.token";
      atomic_file(enrolled_token_file, unique_id() + unique_id() + "\n",
                  fs::perms::owner_read | fs::perms::owner_write);
      SetupAuth auth(enrolled, 8002, monotonic, {}, "127.0.0.1");
      auth.set_enrollment_token_file(enrolled_token_file);
      const auto public_setup = Json::parse(auth.handle(request("/api/v1/setup")).body);
      require(public_setup["capabilities"]["browser_owner_enrollment"] == false,
              "an enrolled device cannot reopen enrollment from the token file alone");
    }
    // A session that was signed in before the physical reset must not outlive
    // it. The reset exists to revoke the previous owner's access (a forgotten
    // or leaked password), but the root helper edits the database from another
    // process, so a session held in the setup server's memory has to notice
    // that the credential it was created for is gone -- and must not carry
    // over to whoever enrolls next.
    {
      SetupStore sessions_store(root.path / "sessions");
      double time = 0;
      SetupAuth auth(sessions_store, 8002, [&] { return time; }, {}, "127.0.0.1");
      require(auth.enroll("first-owner-password"), "the first owner enrolls");
      const auto login = auth.handle(request("/api/v1/auth/login", "POST",
                                             {{"password", "first-owner-password"}}));
      require(login.status == 200, "the first owner signs in");
      auto settings = request("/api/v1/settings");
      settings.headers["cookie"] = cookie(login);
      require(auth.handle(settings).status == 200, "the signed-in session works before the reset");

      // What the root helper does (clear_owner in owner_reset_main.cpp).
      {
        Database database(root.path / "sessions" / "setup.db");
        database.exec("DELETE FROM setup_owner WHERE id=1");
      }
      require(auth.handle(settings).status == 401,
              "a session from before the owner reset is rejected");

      require(auth.enroll("second-owner-password"), "a new owner enrolls after the reset");
      require(auth.handle(settings).status == 401,
              "the old session does not carry over to the new owner");
      const auto second = auth.handle(request("/api/v1/auth/login", "POST",
                                              {{"password", "second-owner-password"}}));
      require(second.status == 200, "the new owner signs in");
      auto second_settings = request("/api/v1/settings");
      second_settings.headers["cookie"] = cookie(second);
      require(auth.handle(second_settings).status == 200 && auth.handle(settings).status == 401,
              "the new owner's own session works while the old one stays rejected");
    }
    std::cout << "owner reset helper and enrollment re-read tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
