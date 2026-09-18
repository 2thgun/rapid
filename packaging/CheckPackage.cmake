if(NOT EXISTS "${PACKAGE}")
  message(FATAL_ERROR "Pass -DPACKAGE=/absolute/path/to/rapid.deb")
endif()
if(NOT DEFINED DPKG_DEB)
  set(DPKG_DEB dpkg-deb)
endif()
execute_process(COMMAND "${DPKG_DEB}" --contents "${PACKAGE}" OUTPUT_VARIABLE contents
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot inspect package")
endif()
foreach(path IN ITEMS
    "./etc/rapid/config.toml"
    "./usr/lib/rapid/rapid-pi"
    "./usr/lib/rapid/rapid-qt-display"
    "./usr/lib/rapid/rapid-log-status"
    "./usr/lib/rapid/rapid-setup-server"
    "./usr/lib/rapid/rapid-firstboot"
    "./usr/lib/rapid/rapid-provision"
    "./usr/lib/rapid/rapid-apply"
    "./usr/lib/rapid/rapid-display-recovery"
    "./usr/lib/rapid/rapid-wifi"
    "./usr/lib/rapid/rapid-account"
    "./usr/lib/rapid/rapid-network-mode"
    "./usr/share/rapid/setup.html"
    "./usr/lib/systemd/system/rapid.service"
    "./usr/lib/systemd/system/rapid-display.service"
    "./usr/lib/systemd/system/rapid-display-recovery.service"
    "./usr/lib/systemd/system/rapid-setup.service"
    "./usr/lib/systemd/system/rapid-firstboot.service"
    "./usr/lib/systemd/system/rapid-provision.service"
    "./usr/lib/systemd/system/rapid-apply.service"
    "./usr/lib/systemd/system/rapid-apply.path"
    "./usr/lib/systemd/system/rapid-wifi.service"
    "./usr/lib/systemd/system/rapid-wifi.path"
    "./usr/lib/systemd/system/rapid-account.service"
    "./usr/lib/systemd/system/rapid-account.path"
    "./usr/lib/systemd/system/rapid-network-mode.service")
  string(FIND "${contents}" " ${path}\n" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Package is missing ${path}")
  endif()
endforeach()
string(TOLOWER "${contents}" normalized_contents)
if(normalized_contents MATCHES "[.]key\n|[.]dpapi\n|[.]pem\n|[.]crt\n|[.]p12\n|[.]db\n|runtime[.]env\n|pairing[.]json\n|pairing-approval[.]json\n|/home/rapid/|/usr/etc/")
  message(FATAL_ERROR "Package contains private state or invalid installation paths")
endif()
# #23: no credential, sudo rule or SSH configuration ships in the package; the
# owner chooses them during setup and rapid-account applies them.
if(normalized_contents MATCHES "[.]/etc/sudoers|[.]/etc/ssh/|[.]/etc/shadow|[.]/etc/passwd|authorized_keys")
  message(FATAL_ERROR "Package must not contain account credentials, sudo rules or SSH configuration")
endif()
set(data_tar "${PACKAGE}.data.tar")
execute_process(COMMAND "${DPKG_DEB}" --fsys-tarfile "${PACKAGE}" OUTPUT_FILE "${data_tar}"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot inspect packaged service definitions")
endif()
function(read_service path output)
  execute_process(COMMAND tar -xOf "${data_tar}" "${path}"
                  OUTPUT_VARIABLE value RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot read packaged ${path}")
  endif()
  set(${output} "${value}" PARENT_SCOPE)
endfunction()
read_service("./usr/lib/systemd/system/rapid-firstboot.service" firstboot_service)
read_service("./usr/lib/systemd/system/rapid-provision.service" provision_service)
read_service("./usr/lib/systemd/system/rapid-setup.service" setup_service)
read_service("./usr/lib/systemd/system/rapid.service" runtime_service)
read_service("./usr/lib/systemd/system/rapid-apply.service" apply_service)
read_service("./usr/lib/systemd/system/rapid-account.service" account_service)
read_service("./usr/lib/systemd/system/rapid-account.path" account_path)
read_service("./usr/lib/systemd/system/rapid-wifi.service" wifi_service)
read_service("./usr/lib/systemd/system/rapid-display-recovery.service" display_recovery_service)
read_service("./usr/lib/rapid/rapid-panel" panel_script)
file(REMOVE "${data_tar}")
set(nl "\n")
# #25: rapid-firstboot.service and rapid-provision.service both declare
# RuntimeDirectory=rapid, so whichever one starts first fixes /run/rapid's
# owning group and mode for the other (systemd re-applies a unit's own
# RuntimeDirectory ownership recursively on every start that declares it).
# Group=rapid must appear on both (directly, or via User=rapid's own primary
# group for first boot) and RuntimeDirectoryMode must match exactly, or the
# directory can end up unreadable, or unwritable, for whichever unit starts
# second. 0770 (not just 0750) is required because rapid.service and
# rapid-setup.service both create new files under /run/rapid (pairing
# coordinator files, the calibration request) as group "rapid", not as owner.
string(REGEX MATCH "${nl}RuntimeDirectoryMode=([^${nl}]*)${nl}" firstboot_mode_match "${firstboot_service}")
set(firstboot_runtime_mode "${CMAKE_MATCH_1}")
string(REGEX MATCH "${nl}RuntimeDirectoryMode=([^${nl}]*)${nl}" provision_mode_match "${provision_service}")
set(provision_runtime_mode "${CMAKE_MATCH_1}")
if(NOT firstboot_service MATCHES "User=rapid" OR
   NOT firstboot_service MATCHES "Group=rapid" OR
   NOT firstboot_service MATCHES "StateDirectory=rapid-setup" OR
   NOT firstboot_service MATCHES "RuntimeDirectory=rapid" OR
   NOT firstboot_runtime_mode STREQUAL "0770")
  message(FATAL_ERROR "First boot must create private setup state as the rapid service user and share /run/rapid at mode 0770")
endif()
if(NOT provision_service MATCHES "User=root" OR
   NOT provision_service MATCHES "Group=rapid" OR
   NOT provision_service MATCHES "Requires=rapid-firstboot[.]service" OR
   NOT provision_service MATCHES "--status-file /run/rapid/firstboot[.]json" OR
   NOT provision_service MATCHES "--ssid-file /run/rapid/network-ssid" OR
   NOT provision_service MATCHES "RuntimeDirectory=rapid")
  message(FATAL_ERROR "Only the provisioner may run as root for the generated setup-AP state, and it must share /run/rapid with the rapid group")
endif()
# Checked against firstboot's own required value (rather than hard-coding 0770
# again here) so this rule stays meaningful even if that value ever changes:
# the two units must agree, whatever the value is.
if(NOT provision_runtime_mode STREQUAL firstboot_runtime_mode)
  message(FATAL_ERROR "rapid-firstboot.service and rapid-provision.service must declare the exact same RuntimeDirectoryMode for the /run/rapid they share")
endif()
# Generic cross-check (#25): every unit below runs with ProtectSystem=strict,
# which makes its whole filesystem view read-only except paths it explicitly
# grants through ReadWritePaths, or its own RuntimeDirectory/StateDirectory.
# For each path a unit's binary is actually configured (by flag) to write,
# assert some grant on that same unit covers it - so a narrowed ReadWritePaths
# or a path moved to a new flag value cannot silently reintroduce this bug.
function(assert_writable_path service_content service_name required_path)
  if(NOT service_content MATCHES "${nl}ProtectSystem=strict${nl}")
    return()
  endif()
  set(covered FALSE)
  string(REGEX MATCH "${nl}ReadWritePaths=([^${nl}]*)${nl}" rw_match "${service_content}")
  if(CMAKE_MATCH_1)
    string(REPLACE " " ";" rw_list "${CMAKE_MATCH_1}")
    foreach(rw IN LISTS rw_list)
      string(REGEX REPLACE "^-" "" rw "${rw}")
      if(NOT rw STREQUAL "" AND required_path MATCHES "^${rw}(/|$)")
        set(covered TRUE)
      endif()
    endforeach()
  endif()
  string(REGEX MATCH "${nl}RuntimeDirectory=([^${nl}]*)${nl}" rd_match "${service_content}")
  if(CMAKE_MATCH_1)
    string(REPLACE " " ";" rd_list "${CMAKE_MATCH_1}")
    foreach(name IN LISTS rd_list)
      if(NOT name STREQUAL "" AND required_path MATCHES "^/run/${name}(/|$)")
        set(covered TRUE)
      endif()
    endforeach()
  endif()
  string(REGEX MATCH "${nl}StateDirectory=([^${nl}]*)${nl}" sd_match "${service_content}")
  if(CMAKE_MATCH_1)
    string(REPLACE " " ";" sd_list "${CMAKE_MATCH_1}")
    foreach(name IN LISTS sd_list)
      if(NOT name STREQUAL "" AND required_path MATCHES "^/var/lib/${name}(/|$)")
        set(covered TRUE)
      endif()
    endforeach()
  endif()
  if(NOT covered)
    message(FATAL_ERROR "${service_name} is sandboxed with ProtectSystem=strict but does not grant write access to ${required_path}, which its binary is configured to write")
  endif()
endfunction()
assert_writable_path("${firstboot_service}" "rapid-firstboot.service" "/run/rapid/firstboot.json")
assert_writable_path("${provision_service}" "rapid-provision.service" "/run/rapid/network-ssid")
assert_writable_path("${setup_service}" "rapid-setup.service" "/run/rapid/calibration-request.json")
assert_writable_path("${wifi_service}" "rapid-wifi.service" "/run/rapid-apply/wifi-result.json")
assert_writable_path("${display_recovery_service}" "rapid-display-recovery.service" "/var/lib/rapid/display-recovery.json")
if(NOT setup_service MATCHES "User=rapid" OR
   NOT setup_service MATCHES "Requires=rapid-firstboot[.]service rapid-provision[.]service" OR
   NOT setup_service MATCHES "--listen 192[.]168[.]1[.]64" OR
   NOT setup_service MATCHES "--enrollment-token-file /var/lib/rapid-setup/enrollment[.]token" OR
   NOT setup_service MATCHES "--tls-certificate /var/lib/rapid-setup/device[.]crt" OR
   NOT setup_service MATCHES "--tls-private-key /var/lib/rapid-setup/device[.]key" OR
   NOT setup_service MATCHES "--calibration-file /var/lib/rapid-setup/touch-calibration[.]conf" OR
   NOT setup_service MATCHES "--calibration-request-file /run/rapid/calibration-request[.]json" OR
   NOT setup_service MATCHES "--display-confirm-file /run/rapid-apply/display-confirm[.]json")
  message(FATAL_ERROR "Setup service must use generated rapid-owned AP/TLS state and fixed listener")
endif()
if(NOT firstboot_service MATCHES "--tls-certificate /var/lib/rapid-setup/device[.]crt" OR
   NOT firstboot_service MATCHES "--tls-private-key /var/lib/rapid-setup/device[.]key")
  message(FATAL_ERROR "First boot must provision the setup TLS identity")
endif()
if(NOT runtime_service MATCHES "Requires=rapid-firstboot[.]service rapid-provision[.]service" OR
   runtime_service MATCHES "ConditionPathExists=/etc/rapid/runtime[.]env" OR
   NOT runtime_service MATCHES "EnvironmentFile=-/etc/rapid/runtime[.]env")
  message(FATAL_ERROR "Runtime must start after fresh-device provisioning and permit pairing-only startup")
endif()
if(NOT apply_service MATCHES "--display-confirm-file /run/rapid-apply/display-confirm[.]json" OR
   NOT apply_service MATCHES "TimeoutStartSec=" OR
   NOT apply_service MATCHES "--display-calibration-file /var/lib/rapid-setup/touch-calibration[.]conf" OR
   NOT panel_script MATCHES "--calibration-file /var/lib/rapid-setup/touch-calibration[.]conf --rollback-calibration --record-input-baseline")
  message(FATAL_ERROR "Orientation must await bounded owner confirmation; touch must follow rotation and roll back unconfirmed calibration")
endif()
# #23: the device password reaches root only as a hash in a request file that
# the setup service queues and a sandboxed, path-activated helper consumes.
string(REGEX MATCH "ExecStart=[^${nl}]*" account_exec "${account_service}")
string(REGEX MATCH "ExecStart=[^${nl}]*" setup_exec "${setup_service}")
foreach(line IN ITEMS "User=root" "NoNewPrivileges=true" "ProtectSystem=strict" "ProtectHome=read-only"
                      "LimitCORE=0" "ReadWritePaths=/etc /run/rapid-apply -/home/rapid")
  string(FIND "${account_service}" "${nl}${line}${nl}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "rapid-account.service must keep '${line}'")
  endif()
endforeach()
if(NOT account_exec STREQUAL "ExecStart=/usr/lib/rapid/rapid-account --request-file /run/rapid-apply/account-request.json --result-file /run/rapid-apply/account-result.json --user rapid" OR
   account_service MATCHES "${nl}User=rapid" OR
   NOT account_path MATCHES "${nl}PathExists=/run/rapid-apply/account-request[.]json${nl}" OR
   NOT account_path MATCHES "${nl}Unit=rapid-account[.]service${nl}")
  message(FATAL_ERROR "Device access must be applied by the sandboxed root rapid-account helper from its fixed request file")
endif()
if(NOT setup_exec MATCHES " --account-request-file /run/rapid-apply/account-request[.]json --account-result-file /run/rapid-apply/account-result[.]json" OR
   setup_exec MATCHES "password" OR account_exec MATCHES "password" OR
   NOT setup_service MATCHES "${nl}LimitCORE=0${nl}" OR
   NOT setup_service MATCHES "${nl}ReadWritePaths=/run/rapid-apply /run/rapid${nl}")
  message(FATAL_ERROR "The setup service must queue device access through rapid-account without secrets on a command line or in core dumps")
endif()
if(NOT setup_exec MATCHES " --ssid-file /run/rapid/network-ssid( |$)")
  message(FATAL_ERROR "The setup service must publish the setup AP name chosen by rapid-provision")
endif()
# #25: rapid-wifi.service and rapid-display-recovery.service each write a
# result/state file back under a ProtectSystem=strict mount namespace; without
# their own ReadWritePaths grant, that write silently fails on a real device
# (WSL has no systemd, so the gate cannot exercise this).
if(NOT wifi_service MATCHES "${nl}ProtectSystem=strict${nl}" OR
   NOT wifi_service MATCHES "${nl}ReadWritePaths=/run/rapid-apply${nl}")
  message(FATAL_ERROR "rapid-wifi.service must keep ProtectSystem=strict and grant ReadWritePaths=/run/rapid-apply for its result file")
endif()
if(NOT display_recovery_service MATCHES "${nl}ProtectSystem=strict${nl}" OR
   NOT display_recovery_service MATCHES "${nl}ReadWritePaths=/var/lib/rapid${nl}")
  message(FATAL_ERROR "rapid-display-recovery.service must keep ProtectSystem=strict and grant ReadWritePaths=/var/lib/rapid for its recovery state file")
endif()
# #26: the Home Wi-Fi passphrase reaches NetworkManager as a private keyfile
# rapid-wifi writes itself, never as an nmcli argument; under
# ProtectSystem=strict it needs its own explicit write access to
# NetworkManager's connection directory for that (a separate, additive grant;
# see g2-sandbox's own #25 check below for this service's request/result
# queue and its ProtectSystem=strict/User=root wiring).
if(NOT wifi_service MATCHES "${nl}ReadWritePaths=[^${nl}]*/etc/NetworkManager/system-connections")
  message(FATAL_ERROR "rapid-wifi.service must grant write access to NetworkManager's connection directory for its own keyfile")
endif()
execute_process(COMMAND "${DPKG_DEB}" --field "${PACKAGE}" Depends OUTPUT_VARIABLE dependencies
                RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT dependencies MATCHES "libargon2" OR NOT dependencies MATCHES "libqt6core" OR
   NOT dependencies MATCHES "xinput" OR
   NOT dependencies MATCHES "openssh-server" OR NOT dependencies MATCHES "sudo" OR
   NOT dependencies MATCHES "libcrypt")
  message(FATAL_ERROR "Missing generated runtime library dependencies")
endif()
execute_process(COMMAND "${DPKG_DEB}" --ctrl-tarfile "${PACKAGE}" OUTPUT_FILE "${PACKAGE}.control.tar"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Cannot inspect package control files")
endif()
execute_process(COMMAND tar -xOf "${PACKAGE}.control.tar" ./conffiles OUTPUT_VARIABLE conffiles
                RESULT_VARIABLE result)
file(REMOVE "${PACKAGE}.control.tar")
if(NOT result EQUAL 0 OR NOT conffiles STREQUAL "/etc/rapid/config.toml\n")
  message(FATAL_ERROR "Packaged configuration must be preserved as a Debian conffile")
endif()
message(STATUS "Package contents, dependencies and conffile checks passed")
