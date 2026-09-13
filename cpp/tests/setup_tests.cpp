#include "rapid/setup.hpp"
#include <fstream>
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

int main(int argc, char **argv) {
  try {
    require(argc == 5, "first-boot, provision, apply and Wi-Fi binaries required");
    TemporaryDirectory root;
    const auto firstboot_directory = root.path / "firstboot";
    const auto firstboot_status = root.path / "firstboot.json";
    const auto token_file = firstboot_directory / "enrollment.token";
    auto run_firstboot = [&] {
      const auto child = fork();
      require(child >= 0, "fork first boot");
      if (child == 0) {
        execl(argv[1], argv[1], "--state-directory", firstboot_directory.c_str(),
              "--status-file", firstboot_status.c_str(), "--setup-address",
              "192.168.50.1", nullptr);
        _exit(127);
      }
      int result = 0;
      require(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
                  WEXITSTATUS(result) == 0,
              "first boot succeeds");
    };
    run_firstboot();
    const auto bootstrap = Json::parse(read_file(firstboot_status));
    require(bootstrap["bootstrap"]["setup_address"] == "192.168.50.1" &&
                bootstrap["bootstrap"]["setup_url"] == "http://192.168.50.1:8002/setup",
            "first boot publishes the configured local AP address");
    const auto activation = bootstrap["bootstrap"]["activation_token"].get<std::string>();
    require(activation.size() == 64 &&
                activation.find_first_not_of("0123456789abcdef") == std::string::npos,
            "first boot creates a 256-bit activation token");
    const auto ap_password = bootstrap["bootstrap"]["access_point_password"].get<std::string>();
    require(bootstrap["bootstrap"]["ssid"].get<std::string>().starts_with("rapid-") &&
                ap_password.size() == 16 &&
                ap_password.find_first_not_of("0123456789abcdef") == std::string::npos,
            "first boot creates device-specific AP credentials");
    require((fs::status(token_file).permissions() & fs::perms::group_all) == fs::perms::none &&
                (fs::status(token_file).permissions() & fs::perms::others_all) == fs::perms::none,
            "activation token stays private");
    run_firstboot();
    const auto resumed = Json::parse(read_file(firstboot_status))["bootstrap"];
    require(resumed["activation_token"] == activation &&
                resumed["access_point_password"] == ap_password,
            "interrupted onboarding preserves AP credentials and activation token");
    const auto fake_nmcli = root.path / "nmcli";
    const auto nmcli_log = root.path / "nmcli.log";
    {
      std::ofstream script(fake_nmcli);
      script << "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >> \"$RAPID_TEST_NMCLI_LOG\"\n"
                "if [ \"$1 $2 $3\" = \"connection show rapid-setup\" ]; then exit 1; fi\n"
                "exit 0\n";
    }
    fs::permissions(fake_nmcli, fs::perms::owner_all);
    setenv("RAPID_TEST_NMCLI_LOG", nmcli_log.c_str(), 1);
    const auto provision = fork();
    require(provision >= 0, "fork AP provisioner");
    if (provision == 0) {
      execl(argv[2], argv[2], "--status-file", firstboot_status.c_str(), "--nmcli",
            fake_nmcli.c_str(), nullptr);
      _exit(127);
    }
    int provision_status = 0;
    require(waitpid(provision, &provision_status, 0) == provision &&
                WIFEXITED(provision_status) && WEXITSTATUS(provision_status) == 0,
            "AP provisioner configures NetworkManager");
    unsetenv("RAPID_TEST_NMCLI_LOG");
    const auto nmcli_calls = read_file(nmcli_log);
    require(nmcli_calls.find("connection add type wifi ifname wlan0 con-name rapid-setup") !=
                std::string::npos &&
                nmcli_calls.find("wifi-sec.psk " + ap_password) != std::string::npos &&
                nmcli_calls.find("ipv4.addresses 192.168.50.1/24") != std::string::npos &&
                nmcli_calls.find("connection up rapid-setup ifname wlan0") != std::string::npos,
            "AP provisioner passes only the generated profile values to NetworkManager");
    const auto apply_request = root.path / "apply-request.json";
    const auto apply_result = root.path / "apply-result.json";
    const auto fake_hostnamectl = root.path / "hostnamectl";
    const auto hostname_log = root.path / "hostnamectl.log";
    atomic_file(apply_request, Json{{"revision", 2}, {"settings",
        {{"hostname", "rapid-applied"}, {"rotation", 0},
         {"boot_network", "home_then_ap"}}}}.dump());
    {
      std::ofstream script(fake_hostnamectl);
      script << "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >> \"$RAPID_TEST_HOSTNAME_LOG\"\n"
                "exit 0\n";
    }
    fs::permissions(fake_hostnamectl, fs::perms::owner_all);
    setenv("RAPID_TEST_HOSTNAME_LOG", hostname_log.c_str(), 1);
    const auto apply = fork();
    require(apply >= 0, "fork settings applicator");
    if (apply == 0) {
      execl(argv[3], argv[3], "--request-file", apply_request.c_str(), "--result-file",
            apply_result.c_str(), "--hostnamectl", fake_hostnamectl.c_str(), nullptr);
      _exit(127);
    }
    int apply_status = 0;
    require(waitpid(apply, &apply_status, 0) == apply && WIFEXITED(apply_status) &&
                WEXITSTATUS(apply_status) == 0 && !fs::exists(apply_request),
            "settings applicator applies and consumes a valid hostname request");
    unsetenv("RAPID_TEST_HOSTNAME_LOG");
    require(read_file(hostname_log) == "set-hostname rapid-applied\n",
            "settings applicator invokes only fixed hostnamectl arguments");
    require(Json::parse(read_file(apply_result))["hostname_applied"] == true,
            "settings applicator records a secret-free completion result");
    const auto wifi_request = root.path / "wifi-request.json";
    const auto wifi_result = root.path / "wifi-result.json";
    const auto wifi_log = root.path / "wifi.log";
    atomic_file(wifi_request, Json{{"revision", 2}, {"ssid", "test-network"}, {"password", std::string(64, 'a')}}.dump());
    setenv("RAPID_TEST_HOSTNAME_LOG", wifi_log.c_str(), 1);
    const auto wifi = fork();
    require(wifi >= 0, "fork Wi-Fi applicator");
    if (wifi == 0) {
      execl(argv[4], argv[4], "--request-file", wifi_request.c_str(), "--result-file", wifi_result.c_str(), "--nmcli", fake_hostnamectl.c_str(), nullptr);
      _exit(127);
    }
    int wifi_status = 0;
    require(waitpid(wifi, &wifi_status, 0) == wifi && WIFEXITED(wifi_status) && WEXITSTATUS(wifi_status) == 0 && !fs::exists(wifi_request),
            "Wi-Fi applicator consumes a valid request");
    unsetenv("RAPID_TEST_HOSTNAME_LOG");
    require(Json::parse(read_file(wifi_result))["connected"] == true && read_file(wifi_log).find("connection add type wifi ifname wlan0 con-name rapid-home ssid test-network") != std::string::npos,
            "Wi-Fi applicator uses the fixed Home profile and no browser command");
    atomic_file(apply_request, Json{{"revision", 3}, {"settings",
        {{"hostname", "rapid.bad"}, {"rotation", 0},
         {"boot_network", "home_then_ap"}}}}.dump());
    const auto rejected_apply = fork();
    require(rejected_apply >= 0, "fork invalid settings applicator");
    if (rejected_apply == 0) {
      execl(argv[3], argv[3], "--request-file", apply_request.c_str(), "--result-file",
            apply_result.c_str(), "--hostnamectl", fake_hostnamectl.c_str(), nullptr);
      _exit(127);
    }
    int rejected_status = 0;
    require(waitpid(rejected_apply, &rejected_status, 0) == rejected_apply &&
                WIFEXITED(rejected_status) && WEXITSTATUS(rejected_status) != 0 &&
                !fs::exists(apply_request) &&
                Json::parse(read_file(apply_result))["hostname_applied"] == false,
            "invalid settings request is consumed with a terminal application result");
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
      const auto initial_provisioning = provisioning_status(initial, false);
      require(initial_provisioning["state"] == "owner_enrollment_required" &&
                  initial_provisioning["capabilities"]["secure_ap_bootstrap"] == false &&
                  initial_provisioning.dump().find("do-not-expose") == std::string::npos,
              "first-boot status identifies the missing secure bootstrap without leaking settings");
      const auto owner_provisioning = provisioning_status(state, true);
      require(owner_provisioning["state"] == "settings_application_required" &&
                  owner_provisioning["setup_complete"] == false,
              "owner enrollment alone does not report first boot complete");
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
