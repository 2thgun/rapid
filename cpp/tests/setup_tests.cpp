#include "rapid/setup.hpp"
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace rapid::native;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F action, const char *message) {
  bool rejected = false;
  try { action(); } catch (const std::exception &) { rejected = true; }
  require(rejected, message);
}
struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path() / ("rapid-setup-tests-" + unique_id());
  TemporaryDirectory() { fs::create_directory(path); }
  ~TemporaryDirectory() { std::error_code error; fs::remove_all(path, error); }
};
} // namespace

int main() {
  try {
    TemporaryDirectory root;
    const auto directory = root.path / "device";
    Json initial;
    {
      SetupStore store(directory);
      initial = store.snapshot();
      require(initial["revision"] == 1 && initial["setup_complete"] == false,
              "new device starts incomplete");
      require(initial["settings"]["rotation"] == 0 &&
                  initial["settings"]["boot_network"] == "home_then_ap",
              "agreed defaults persisted");
      require((fs::status(directory).permissions() & fs::perms::group_all) == fs::perms::none &&
                  (fs::status(directory).permissions() & fs::perms::others_all) == fs::perms::none,
              "state directory is private");
      SetupStore other(root.path / "other");
      require(other.snapshot()["device_id"] != initial["device_id"], "fresh devices have distinct identities");
    }
    {
      SetupStore store(directory), stale_client(directory);
      require(store.snapshot() == initial, "restart preserves identity and state");
      auto desired = initial["settings"];
      desired["rotation"] = 180;
      desired["hostname"] = "rapid-demo";
      require(store.update(1, desired), "valid update succeeds");
      require(!stale_client.update(1, initial["settings"]), "stale independent connection cannot overwrite update");
      auto state = stale_client.snapshot();
      require(state["settings"] == desired && state["revision"] == 2 &&
                  state["device_id"] == initial["device_id"], "update preserves identity and advances revision");
      for (const auto &invalid : {Json(nullptr), Json::array(), Json{{"hostname", "rapid"}}})
        rejects([&] { store.update(2, invalid); }, "invalid settings shape rejected");
      for (const auto &rotation : {Json(90), Json(270), Json("180"), Json(true), Json(180.0)}) {
        auto bad = desired;
        bad["rotation"] = rotation;
        rejects([&] { store.update(2, bad); }, "invalid rotation rejected");
      }
      for (const auto &hostname : {"", "-rapid", "rapid-", "rapid.local", "RAPID", "rapid\nreboot"}) {
        auto bad = desired;
        bad["hostname"] = hostname;
        rejects([&] { store.update(2, bad); }, "invalid hostname rejected");
      }
      auto bad = desired;
      bad["owner_password"] = "never accept arbitrary fields";
      rejects([&] { store.update(2, bad); }, "unknown fields rejected");
      bad = desired;
      bad["boot_network"] = "off";
      rejects([&] { store.update(2, bad); }, "unsupported boot policy rejected");
      require(store.snapshot() == state, "rejected updates leave durable state untouched");
      state["private_key"] = "do-not-expose";
      state["settings"]["wifi_password"] = "do-not-expose";
      const auto public_state = setup_status(state);
      require(public_state.dump().find("do-not-expose") == std::string::npos &&
                  !public_state.contains("settings") && public_state["capabilities"]["pairing"] == false,
              "public status explicitly excludes private configuration and unavailable features");
    }
    // Abrupt exit in a transaction simulates interrupted configuration writes.
    auto child = fork();
    require(child >= 0, "fork interrupted writer");
    if (child == 0) {
      Database database(directory / "setup.db");
      database.exec("BEGIN IMMEDIATE");
      database.exec("DELETE FROM setup_state");
      _exit(0);
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "interrupted writer exited");
    {
      SetupStore store(directory);
      require(store.snapshot()["revision"] == 2 && store.snapshot()["device_id"] == initial["device_id"],
              "uncommitted interrupted write recovered without identity reset");
    }
    {
      Database database(directory / "setup.db");
      database.exec("PRAGMA user_version=99");
    }
    rejects([&] { SetupStore store(directory); }, "future schema rejected");
    {
      Database database(directory / "setup.db");
      require(database.query("PRAGMA user_version")[0]["user_version"] == 99,
              "future schema not downgraded");
      database.exec("PRAGMA user_version=2");
      database.exec("UPDATE setup_state SET document='{}'");
    }
    rejects([&] { SetupStore store(directory); }, "corrupt document rejected without regeneration");
    {
      Database database(directory / "setup.db");
      require(database.query("SELECT document FROM setup_state")[0]["document"] == "{}",
              "corrupt state preserved for recovery");
    }
    fs::create_directory_symlink(directory, root.path / "link");
    rejects([&] { SetupStore store(root.path / "link"); }, "symlink directory rejected");
    fs::create_directory(root.path / "public");
    fs::permissions(root.path / "public", fs::perms::all);
    rejects([&] { SetupStore store(root.path / "public"); }, "public state directory rejected");
    fs::create_directory(root.path / "linked-db");
    fs::permissions(root.path / "linked-db", fs::perms::owner_all);
    fs::create_symlink(directory / "setup.db", root.path / "linked-db" / "setup.db");
    rejects([&] { SetupStore store(root.path / "linked-db"); }, "symlink database rejected");
    const auto config = root.path / "package.toml";
    atomic_file(config, "[app]\nrequire_v4 = true\n");
    setenv("RAPID_REQUIRE_V4", "true", 1);
    setenv("RAPID_COMPANION_KEY", "", 1);
    rejects([&] { (void)Config::load(config); }, "packaged runtime refuses missing authentication key");
    setenv("RAPID_COMPANION_KEY", std::string(64, '1').c_str(), 1);
    require(Config::load(config).companion_key.size() == 32, "packaged runtime accepts configured v4 key");
    unsetenv("RAPID_REQUIRE_V4");
    unsetenv("RAPID_COMPANION_KEY");
    std::cout << "setup persistence, validation, isolation and recovery tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
