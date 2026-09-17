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
                bootstrap["bootstrap"]["setup_url"] == "https://192.168.50.1:8002/setup",
            "first boot publishes the configured local AP address");
    const auto certificate_file = firstboot_directory / "device.crt";
    const auto private_key_file = firstboot_directory / "device.key";
    require(fs::file_size(certificate_file) > 0 && fs::file_size(private_key_file) > 0 &&
                (fs::status(private_key_file).permissions() & fs::perms::group_all) == fs::perms::none &&
                (fs::status(private_key_file).permissions() & fs::perms::others_all) == fs::perms::none,
            "first boot creates a persistent private TLS identity");
    const auto activation = bootstrap["bootstrap"]["activation_token"].get<std::string>();
    const auto certificate_bytes = read_file(certificate_file);
    require(activation.size() == 64 &&
                activation.find_first_not_of("0123456789abcdef") == std::string::npos,
            "first boot creates a 256-bit activation token");
    require(bootstrap["bootstrap"]["certificate_fingerprint"].get<std::string>().size() == 64 &&
                bootstrap["bootstrap"]["certificate_fingerprint"].get<std::string>().find_first_not_of("0123456789abcdef") == std::string::npos,
            "first boot publishes the TLS certificate fingerprint");
    // #22: the open setup AP has no per-device passphrase, and its SSID is
    // resolved later by the privileged provisioner (it alone can scan for a
    // collision), not by first boot.
    require(bootstrap["bootstrap"].size() == 5 && !bootstrap["bootstrap"].contains("ssid") &&
                !bootstrap["bootstrap"].contains("access_point_password"),
            "first boot no longer generates a device-specific SSID or AP passphrase");
    require((fs::status(token_file).permissions() & fs::perms::group_all) == fs::perms::none &&
                (fs::status(token_file).permissions() & fs::perms::others_all) == fs::perms::none,
            "activation token stays private");
    const auto legacy_ap_password = firstboot_directory / "ap-password";
    atomic_file(legacy_ap_password, "0123456789abcdef\n");
    require(fs::exists(legacy_ap_password), "legacy AP password fixture is in place before re-running first boot");
    run_firstboot();
    require(!fs::exists(legacy_ap_password),
            "first boot removes a leftover passphrase from the old device-specific scheme");
    const auto resumed = Json::parse(read_file(firstboot_status))["bootstrap"];
    require(resumed["activation_token"] == activation &&
                read_file(certificate_file) == certificate_bytes &&
                resumed["certificate_fingerprint"] == bootstrap["bootstrap"]["certificate_fingerprint"],
            "interrupted onboarding preserves the activation token and device identity");
    const auto fake_nmcli = root.path / "nmcli";
    const auto nmcli_log = root.path / "nmcli.log";
    {
      std::ofstream script(fake_nmcli);
      script << "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >> \"$RAPID_TEST_NMCLI_LOG\"\n"
                "if [ \"$1 $2 $3\" = \"connection show rapid-setup\" ]; then exit 1; fi\n"
                "if [ \"$1 $2 $3\" = \"-t -f SSID\" ]; then printf 'some-other-network\\n'; exit 0; fi\n"
                "exit 0\n";
    }
    fs::permissions(fake_nmcli, fs::perms::owner_all);
    setenv("RAPID_TEST_NMCLI_LOG", nmcli_log.c_str(), 1);
    const auto ssid_file = root.path / "network-ssid";
    const auto provision = fork();
    require(provision >= 0, "fork AP provisioner");
    if (provision == 0) {
      execl(argv[2], argv[2], "--status-file", firstboot_status.c_str(), "--ssid-file",
            ssid_file.c_str(), "--nmcli", fake_nmcli.c_str(), nullptr);
      _exit(127);
    }
    int provision_status = 0;
    require(waitpid(provision, &provision_status, 0) == provision &&
                WIFEXITED(provision_status) && WEXITSTATUS(provision_status) == 0,
            "AP provisioner configures NetworkManager");
    unsetenv("RAPID_TEST_NMCLI_LOG");
    const auto nmcli_calls = read_file(nmcli_log);
    require(nmcli_calls.find("connection add type wifi ifname wlan0 con-name rapid-setup autoconnect yes ssid rapid") !=
                std::string::npos &&
                nmcli_calls.find("wifi-sec") == std::string::npos &&
                nmcli_calls.find("psk") == std::string::npos &&
                nmcli_calls.find("ipv4.addresses 192.168.50.1/24") != std::string::npos &&
                nmcli_calls.find("connection up rapid-setup ifname wlan0") != std::string::npos,
            "AP provisioner creates an open profile (no key management or PSK) with no collision seen");
    require(read_file(ssid_file) == "rapid\n",
            "AP provisioner publishes the resolved SSID for the panel/card to display");
    // #22 edge case: two Pis in range would both try "rapid". A one-shot scan
    // picks a disambiguating suffix only when a collision is actually seen,
    // and holds that choice for the rest of the boot/setup session instead of
    // re-scanning (and possibly landing on a different answer) each time the
    // provisioner runs again within the same boot.
    const auto collision_ssid_file = root.path / "network-ssid-collision";
    const auto fake_nmcli_collision = root.path / "nmcli-collision";
    const auto nmcli_collision_log = root.path / "nmcli-collision.log";
    {
      std::ofstream script(fake_nmcli_collision);
      script << "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >> \"$RAPID_TEST_NMCLI_LOG\"\n"
                "if [ \"$1 $2 $3\" = \"connection show rapid-setup\" ]; then exit 1; fi\n"
                "if [ \"$1 $2 $3\" = \"-t -f SSID\" ]; then printf 'some-other-network\\nrapid\\n'; exit 0; fi\n"
                "exit 0\n";
    }
    fs::permissions(fake_nmcli_collision, fs::perms::owner_all);
    setenv("RAPID_TEST_NMCLI_LOG", nmcli_collision_log.c_str(), 1);
    const auto run_provision = [&](const fs::path &ssid_path, const fs::path &nmcli_path) {
      const auto child = fork();
      require(child >= 0, "fork AP provisioner");
      if (child == 0) {
        execl(argv[2], argv[2], "--status-file", firstboot_status.c_str(), "--ssid-file",
              ssid_path.c_str(), "--nmcli", nmcli_path.c_str(), nullptr);
        _exit(127);
      }
      int status = 0;
      return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    };
    require(run_provision(collision_ssid_file, fake_nmcli_collision),
            "AP provisioner still succeeds when a collision is seen");
    unsetenv("RAPID_TEST_NMCLI_LOG");
    const auto collision_ssid = read_file(collision_ssid_file);
    require(collision_ssid.size() == 11 && collision_ssid.rfind("rapid-", 0) == 0 &&
                collision_ssid.substr(6, 4).find_first_not_of("0123456789") == std::string::npos &&
                collision_ssid.back() == '\n',
            "a seen collision picks a 4-digit disambiguated SSID");
    require(read_file(nmcli_collision_log).find("ssid " + collision_ssid.substr(0, 10)) != std::string::npos,
            "the disambiguated SSID is the one actually configured");
    const auto fake_nmcli_no_rescan = root.path / "nmcli-no-rescan";
    const auto nmcli_no_rescan_log = root.path / "nmcli-no-rescan.log";
    {
      std::ofstream script(fake_nmcli_no_rescan);
      script << "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >> \"$RAPID_TEST_NMCLI_LOG\"\n"
                "if [ \"$1 $2 $3\" = \"connection show rapid-setup\" ]; then exit 0; fi\n"
                "if [ \"$1 $2 $3\" = \"-t -f SSID\" ]; then echo 'scan must not run again this session' >&2; exit 1; fi\n"
                "exit 0\n";
    }
    fs::permissions(fake_nmcli_no_rescan, fs::perms::owner_all);
    setenv("RAPID_TEST_NMCLI_LOG", nmcli_no_rescan_log.c_str(), 1);
    require(run_provision(collision_ssid_file, fake_nmcli_no_rescan),
            "a later provisioner run this session succeeds without re-scanning");
    unsetenv("RAPID_TEST_NMCLI_LOG");
    require(read_file(collision_ssid_file) == collision_ssid,
            "the disambiguated SSID stays stable across the rest of the session");
    require(read_file(nmcli_no_rescan_log).find("-t -f SSID") == std::string::npos &&
                read_file(nmcli_no_rescan_log).find("connection add") == std::string::npos,
            "an already-provisioned session neither re-scans nor re-creates the connection");
    const auto apply_request = root.path / "apply-request.json";
    const auto apply_result = root.path / "apply-result.json";
    const auto fake_hostnamectl = root.path / "hostnamectl";
    const auto hostname_log = root.path / "hostnamectl.log";
    const auto fake_display = root.path / "display-recovery";
    const auto display_log = root.path / "display-recovery.log";
    const auto display_state = root.path / "display-state.json";
    const auto display_confirm = root.path / "display-confirm.json";
    const auto touch_calibration = root.path / "touch-calibration.conf";
    atomic_file(apply_request, Json{{"revision", 2}, {"settings",
        {{"hostname", "rapid-applied"}, {"rotation", 180},
         {"boot_network", "home_then_ap"}}}}.dump());
    {
      std::ofstream script(fake_hostnamectl);
      script << "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >> \"$RAPID_TEST_HOSTNAME_LOG\"\n"
                "exit 0\n";
    }
    fs::permissions(fake_hostnamectl, fs::perms::owner_all);
    setenv("RAPID_TEST_HOSTNAME_LOG", hostname_log.c_str(), 1);
    {
      std::ofstream script(fake_display);
      script << "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >> \"$RAPID_TEST_DISPLAY_LOG\"\n"
                "exit 0\n";
    }
    fs::permissions(fake_display, fs::perms::owner_all);
    setenv("RAPID_TEST_DISPLAY_LOG", display_log.c_str(), 1);
    const auto launch_apply = [&](int timeout) {
      const auto child = fork();
      require(child >= 0, "fork settings applicator");
      if (child == 0) {
        const auto seconds = std::to_string(timeout);
        execl(argv[3], argv[3], "--request-file", apply_request.c_str(), "--result-file",
              apply_result.c_str(), "--hostnamectl", fake_hostnamectl.c_str(),
              "--display-recovery", fake_display.c_str(), "--display-state-file", display_state.c_str(),
              "--display-output", "default", "--display-calibration-file", touch_calibration.c_str(),
              "--display-confirm-file", display_confirm.c_str(), "--display-confirm-timeout",
              seconds.c_str(), nullptr);
        _exit(127);
      }
      return child;
    };
    const auto succeeded = [](pid_t child) {
      int status = 0;
      return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    };
    const auto result_rotation = [&] {
      try {
        return Json::parse(read_file(apply_result)).value("rotation", std::string{});
      } catch (const std::exception &) {
        return std::string{};
      }
    };
    auto apply = launch_apply(10);
    for (int i = 0; i < 100 && result_rotation() != "awaiting_confirmation"; ++i) usleep(50000);
    require(result_rotation() == "awaiting_confirmation" && !fs::exists(apply_request),
            "a changed orientation is previewed and waits for the owner after consuming its request");
    atomic_file(display_confirm, Json{{"revision", 2}}.dump());
    require(succeeded(apply), "settings applicator finishes after owner confirmation");
    require(read_file(hostname_log) == "set-hostname rapid-applied\n",
            "settings applicator invokes only fixed hostnamectl arguments");
    const auto applied = Json::parse(read_file(apply_result));
    require(applied["hostname_applied"] == true && applied["rotation"] == "confirmed" &&
                applied["pending"] == Json::array({"wifi"}) && !fs::exists(display_confirm) &&
                read_file(display_log).find("--rotation 180 --preview --calibration-file " +
                                            touch_calibration.string()) != std::string::npos &&
                read_file(display_log).find("--confirm") != std::string::npos,
            "owner confirmation keeps the previewed orientation in a secret-free result");
    atomic_file(apply_request, Json{{"revision", 4}, {"settings",
        {{"hostname", "rapid-applied"}, {"rotation", 180}, {"boot_network", "home_then_ap"}}}}.dump());
    atomic_file(display_confirm, Json{{"revision", 2}}.dump());
    apply = launch_apply(1);
    require(succeeded(apply) && result_rotation() == "rolled_back" &&
                Json::parse(read_file(apply_result))["pending"] == Json::array({"rotation", "wifi"}) &&
                read_file(display_log).find("--rollback --calibration-file") != std::string::npos,
            "an unconfirmed orientation rolls back after its timeout and ignores a stale confirmation");
    atomic_file(display_state, Json{{"rotation", 180}, {"pending", false}}.dump());
    atomic_file(apply_request, Json{{"revision", 5}, {"settings",
        {{"hostname", "rapid-applied"}, {"rotation", 180}, {"boot_network", "home_then_ap"}}}}.dump());
    const auto display_calls = read_file(display_log);
    apply = launch_apply(1);
    require(succeeded(apply) && result_rotation() == "unchanged" && read_file(display_log) == display_calls &&
                Json::parse(read_file(apply_result))["pending"] == Json::array({"wifi"}),
            "saving an unchanged orientation neither previews nor asks for confirmation");
    unsetenv("RAPID_TEST_HOSTNAME_LOG");
    const auto wifi_request = root.path / "wifi-request.json";
    const auto wifi_result = root.path / "wifi-result.json";
    const auto wifi_log = root.path / "wifi.log";
    const auto wifi_connections = root.path / "nm-connections";
    const std::string wifi_password(64, 'a');
    atomic_file(wifi_request, Json{{"revision", 2}, {"ssid", "test-network"}, {"password", wifi_password}}.dump());
    setenv("RAPID_TEST_HOSTNAME_LOG", wifi_log.c_str(), 1);
    const auto wifi = fork();
    require(wifi >= 0, "fork Wi-Fi applicator");
    if (wifi == 0) {
      execl(argv[4], argv[4], "--request-file", wifi_request.c_str(), "--result-file", wifi_result.c_str(),
            "--nmcli", fake_hostnamectl.c_str(), "--connection-directory", wifi_connections.c_str(), nullptr);
      _exit(127);
    }
    int wifi_status = 0;
    require(waitpid(wifi, &wifi_status, 0) == wifi && WIFEXITED(wifi_status) && WEXITSTATUS(wifi_status) == 0 && !fs::exists(wifi_request),
            "Wi-Fi applicator consumes a valid request");
    unsetenv("RAPID_TEST_HOSTNAME_LOG");
    const auto wifi_calls = read_file(wifi_log);
    const auto wifi_connection_file = wifi_connections / "rapid-home.nmconnection";
    require(Json::parse(read_file(wifi_result))["connected"] == true &&
                wifi_calls.find("connection delete rapid-home") != std::string::npos &&
                wifi_calls.find("connection load " + wifi_connection_file.string()) != std::string::npos &&
                wifi_calls.find("connection up rapid-home ifname wlan0") != std::string::npos,
            "Wi-Fi applicator uses the fixed Home profile and no browser command");
    // #26: the passphrase must never appear on nmcli's command line -- the
    // fake nmcli above logs argv only, so any occurrence here would be one.
    require(wifi_calls.find(wifi_password) == std::string::npos && wifi_calls.find("psk") == std::string::npos &&
                wifi_calls.find("wifi-sec") == std::string::npos,
            "Wi-Fi applicator never passes the passphrase to nmcli");
    require(fs::is_regular_file(wifi_connection_file) &&
                (fs::status(wifi_connection_file).permissions() &
                 (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none,
            "the installed NetworkManager keyfile is private");
    require(read_file(wifi_connection_file).find("psk=" + wifi_password) != std::string::npos,
            "the installed NetworkManager keyfile carries the passphrase NetworkManager needs");
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
      require(store.remember_peer(std::string(32, 'a'), "Driver laptop", std::string(64, 'b'), 1.0),
              "first paired PC is persisted");
      require(store.peer_keys().size() == 1 && store.peer_keys()[0].size() == 32,
              "private paired telemetry key is available to the runtime only");
      const auto peers = store.peers();
      require(peers.size() == 1 && peers[0]["label"] == "Driver laptop" &&
                  !peers[0].contains("key"),
              "paired-PC listing excludes telemetry key material");
      require(store.revoke_peer(std::string(32, 'a')) && store.peers().empty(),
              "individual paired-PC revocation removes its key record");
      for (int i = 0; i < 16; ++i) {
        std::string id(32, '0'); id.back() = "0123456789abcdef"[i];
        require(store.remember_peer(id, "PC" + std::to_string(i), std::string(64, 'c'), 2.0 + i),
                "paired PC fits within limit");
      }
      require(!store.remember_peer(std::string(31, 'd') + "e", "overflow", std::string(64, 'c'), 99.0),
              "seventeenth paired PC is rejected");
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
      database.exec("PRAGMA user_version=3");
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
    unsetenv("RAPID_COMPANION_KEY");
    setenv("RAPID_COMPANION_KEYS", (std::string(64, '2') + "," + std::string(64, '3')).c_str(), 1);
    require(Config::load(config).companion_keys.size() == 2,
            "packaged runtime accepts a bounded paired-key set");
    unsetenv("RAPID_COMPANION_KEYS");
    unsetenv("RAPID_REQUIRE_V4");
    atomic_file(config, "[app]\nrequire_v4 = true\n[setup]\nstate_directory = \"/var/lib/rapid-setup\"\n[pairing]\nenabled = true\nhost = \"0.0.0.0\"\nport = 8003\n");
    const auto pairing_config = Config::load(config);
    require(pairing_config.pairing_enabled && pairing_config.pairing_host == "0.0.0.0" &&
                pairing_config.pairing_port == 8003,
            "pairing-only fresh-device runtime has an AP-reachable listener");
    const auto broken_identity = root.path / "broken-identity";
    const auto broken_status = root.path / "broken-identity.json";
    const auto broken_cert = broken_identity / "device.crt";
    const auto broken_key = broken_identity / "device.key";
    fs::create_directory(broken_identity);
    fs::permissions(broken_identity, fs::perms::owner_all,
                    fs::perm_options::replace);
    auto run_broken_firstboot = [&] {
      const auto process = fork();
      require(process >= 0, "fork broken identity first boot");
      if (process == 0) {
        execl(argv[1], argv[1], "--state-directory", broken_identity.c_str(),
              "--status-file", broken_status.c_str(), "--setup-address", "192.168.50.1", nullptr);
        _exit(127);
      }
      int result = 0;
      require(waitpid(process, &result, 0) == process && WIFEXITED(result),
              "broken identity first boot exits");
      return WEXITSTATUS(result);
    };
    require(run_broken_firstboot() == 0 && fs::exists(broken_cert) && fs::exists(broken_key),
            "baseline identity for missing-half test created");
    fs::remove(broken_key);
    require(run_broken_firstboot() != 0 && !fs::exists(broken_key),
            "missing private certificate half fails closed without regeneration");
    const auto missing_certificate = root.path / "missing-certificate";
    const auto missing_certificate_status = root.path / "missing-certificate.json";
    fs::create_directory(missing_certificate);
    fs::permissions(missing_certificate, fs::perms::owner_all,
                    fs::perm_options::replace);
    auto run_missing_certificate = [&] {
      const auto process = fork();
      require(process >= 0, "fork missing certificate first boot");
      if (process == 0) {
        execl(argv[1], argv[1], "--state-directory", missing_certificate.c_str(),
              "--status-file", missing_certificate_status.c_str(), "--setup-address",
              "192.168.50.1", nullptr);
        _exit(127);
      }
      int result = 0;
      require(waitpid(process, &result, 0) == process && WIFEXITED(result),
              "missing certificate first boot exits");
      return WEXITSTATUS(result);
    };
    require(run_missing_certificate() == 0,
            "baseline identity for missing certificate test created");
    fs::remove(missing_certificate / "device.crt");
    require(run_missing_certificate() != 0 &&
                !fs::exists(missing_certificate / "device.crt"),
            "missing certificate half fails closed without regeneration");
    std::cout << "setup persistence, validation, isolation and recovery tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
