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
    "./usr/lib/tmpfiles.d/rapid.conf"
    "./usr/lib/sysusers.d/rapid.conf"
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
read_service("./usr/lib/tmpfiles.d/rapid.conf" tmpfiles_file)
file(REMOVE "${data_tar}")
set(nl "\n")
# #22: the setup AP is the fixed, open network "rapid" (or "rapid-NNNN" only
# on an SSID collision); its NetworkManager profile has no wifi-security.
# rapid-provision is the only unit that creates/modifies that profile as root,
# so it must never be handed a passphrase or key-management setting either.
if(provision_service MATCHES "[Pp][Ss][Kk]|wifi-sec|wireless-security|password")
  message(FATAL_ERROR "rapid-provision.service must not carry an AP passphrase or security setting; the setup AP is open")
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
  # #32: systemd accumulates repeated ReadWritePaths=/RuntimeDirectory=/
  # StateDirectory= directives, so a unit may legitimately grant the same
  # access over several lines. Read every occurrence, not just the first.
  string(REGEX MATCHALL "${nl}ReadWritePaths=[^${nl}]*" rw_matches "${service_content}")
  foreach(rw_match IN LISTS rw_matches)
    string(REGEX REPLACE "^${nl}ReadWritePaths=" "" rw_value "${rw_match}")
    string(REPLACE " " ";" rw_list "${rw_value}")
    foreach(rw IN LISTS rw_list)
      string(REGEX REPLACE "^-" "" rw "${rw}")
      if(NOT rw STREQUAL "" AND required_path MATCHES "^${rw}(/|$)")
        set(covered TRUE)
      endif()
    endforeach()
  endforeach()
  string(REGEX MATCHALL "${nl}RuntimeDirectory=[^${nl}]*" rd_matches "${service_content}")
  foreach(rd_match IN LISTS rd_matches)
    string(REGEX REPLACE "^${nl}RuntimeDirectory=" "" rd_value "${rd_match}")
    string(REPLACE " " ";" rd_list "${rd_value}")
    foreach(name IN LISTS rd_list)
      if(NOT name STREQUAL "" AND required_path MATCHES "^/run/${name}(/|$)")
        set(covered TRUE)
      endif()
    endforeach()
  endforeach()
  string(REGEX MATCHALL "${nl}StateDirectory=[^${nl}]*" sd_matches "${service_content}")
  foreach(sd_match IN LISTS sd_matches)
    string(REGEX REPLACE "^${nl}StateDirectory=" "" sd_value "${sd_match}")
    string(REPLACE " " ";" sd_list "${sd_value}")
    foreach(name IN LISTS sd_list)
      if(NOT name STREQUAL "" AND required_path MATCHES "^/var/lib/${name}(/|$)")
        set(covered TRUE)
      endif()
    endforeach()
  endforeach()
  if(NOT covered)
    message(FATAL_ERROR "${service_name} is sandboxed with ProtectSystem=strict but does not grant write access to ${required_path}, which its binary is configured to write")
  endif()
endfunction()
# #40: /run/rapid is shared by rapid-firstboot.service (owner), the root
# rapid-provision.service and rapid-setup.service. A shared RuntimeDirectory is
# not reference counted: systemd removes it when ANY declaring unit stops, so a
# consumer that also declared RuntimeDirectory=rapid deleted the directory out
# from under the others (same class as #25). rapid-firstboot.service is the
# single lifecycle owner; the consumers are ordered after it and granted
# ReadWritePaths=/run/rapid. Group=rapid and mode 0770 keep it usable by the
# unprivileged setup server and the root helpers. Checked before the generic
# writable-path cross-check below so a missing owner is reported as the
# ownership defect it is, not as a missing grant.
string(REGEX MATCH "${nl}RuntimeDirectoryMode=([^${nl}]*)${nl}" firstboot_mode_match "${firstboot_service}")
set(firstboot_runtime_mode "${CMAKE_MATCH_1}")
if(NOT firstboot_service MATCHES "User=rapid" OR
   NOT firstboot_service MATCHES "Group=rapid" OR
   NOT firstboot_service MATCHES "StateDirectory=rapid-setup" OR
   NOT firstboot_service MATCHES "${nl}RuntimeDirectory=rapid${nl}" OR
   NOT firstboot_runtime_mode STREQUAL "0770")
  message(FATAL_ERROR "First boot must create private setup state as the rapid service user and own /run/rapid at mode 0770")
endif()
if(NOT provision_service MATCHES "User=root" OR
   NOT provision_service MATCHES "${nl}Group=rapid${nl}" OR
   NOT provision_service MATCHES "Requires=rapid-firstboot[.]service" OR
   NOT provision_service MATCHES "--status-file /run/rapid/firstboot[.]json" OR
   NOT provision_service MATCHES "--ssid-file /run/rapid/network-ssid" OR
   NOT provision_service MATCHES "${nl}ReadWritePaths=/run/rapid${nl}")
  message(FATAL_ERROR "Only the provisioner may run as root for the generated setup-AP state, and it must consume /run/rapid through ReadWritePaths=")
endif()
if(provision_service MATCHES "${nl}RuntimeDirectory=rapid${nl}")
  message(FATAL_ERROR "rapid-provision.service must not declare RuntimeDirectory=rapid; rapid-firstboot.service is the single lifecycle owner of /run/rapid")
endif()
if(setup_service MATCHES "${nl}RuntimeDirectory=rapid${nl}")
  message(FATAL_ERROR "rapid-setup.service must not declare RuntimeDirectory=rapid; rapid-firstboot.service is the single lifecycle owner of /run/rapid")
endif()
assert_writable_path("${firstboot_service}" "rapid-firstboot.service" "/run/rapid/firstboot.json")
assert_writable_path("${provision_service}" "rapid-provision.service" "/run/rapid/network-ssid")
assert_writable_path("${setup_service}" "rapid-setup.service" "/run/rapid/calibration-request.json")
assert_writable_path("${apply_service}" "rapid-apply.service" "/run/rapid-apply/request.json")
assert_writable_path("${account_service}" "rapid-account.service" "/run/rapid-apply/account-request.json")
assert_writable_path("${wifi_service}" "rapid-wifi.service" "/run/rapid-apply/wifi-result.json")
assert_writable_path("${display_recovery_service}" "rapid-display-recovery.service" "/var/lib/rapid/display-recovery.json")
# /run/rapid-apply is the shared setup request/result queue. It used to be
# created only by a tmpfiles.d line, which a live upgrade or a service restart
# does not reliably apply; ReadWritePaths= requires the path to exist, so
# rapid-setup failed with 226/NAMESPACE when it was absent. rapid-setup.service
# is now the single lifecycle owner through RuntimeDirectory=, so systemd
# creates it (and adds it to the sandbox writable set) before its mount
# namespace and, crucially, removes it only when the server itself stops.
# A shared RuntimeDirectory is not reference counted: systemd removes it when
# ANY declaring unit stops, even while others still use it. The transient
# consumers (rapid-apply/wifi/account) therefore must NOT declare it, or they
# delete the queue out from under the still-active server after the first
# request (#25). They are ordered after the owner (Requires=/After=) and get the
# queue through ReadWritePaths=/run/rapid-apply instead. Group=rapid and mode
# 0770 keep it usable by the unprivileged server and the root helpers. The
# tmpfiles rule must NOT own this path, or a future edit could quietly restore
# the old, unreliable lifecycle.
if(tmpfiles_file MATCHES "${nl}d /run/rapid-apply")
  message(FATAL_ERROR "rapid.tmpfiles must not create /run/rapid-apply; rapid-setup.service owns it with RuntimeDirectory=")
endif()
if(NOT tmpfiles_file MATCHES "d /etc/rapid 0750 root rapid")
  message(FATAL_ERROR "rapid.tmpfiles must keep the /etc/rapid rule")
endif()
if(NOT setup_service MATCHES "${nl}RuntimeDirectory=rapid-apply${nl}")
  message(FATAL_ERROR "rapid-setup.service must declare RuntimeDirectory=rapid-apply as the single lifecycle owner of the shared queue")
endif()
if(NOT setup_service MATCHES "${nl}RuntimeDirectoryMode=0770${nl}")
  message(FATAL_ERROR "rapid-setup.service must declare RuntimeDirectoryMode=0770 for the shared /run/rapid-apply queue")
endif()
if(NOT setup_service MATCHES "${nl}Group=rapid${nl}")
  message(FATAL_ERROR "rapid-setup.service must run in the rapid group so shared /run/rapid-apply ownership stays usable")
endif()
foreach(name IN ITEMS apply wifi account)
  set(service_var "${name}_service")
  set(service "${${service_var}}")
  if(service MATCHES "${nl}RuntimeDirectory=rapid-apply${nl}")
    message(FATAL_ERROR "rapid-${name}.service must not declare RuntimeDirectory=rapid-apply; rapid-setup.service is the single lifecycle owner of the shared queue")
  endif()
  if(NOT service MATCHES "${nl}Requires=rapid-setup[.]service${nl}")
    message(FATAL_ERROR "rapid-${name}.service must require rapid-setup.service so the shared queue exists before its mount namespace")
  endif()
  if(NOT service MATCHES "${nl}After=rapid-setup[.]service${nl}")
    message(FATAL_ERROR "rapid-${name}.service must be ordered After=rapid-setup.service, the owner of the shared queue")
  endif()
  if(NOT service MATCHES "${nl}Group=rapid${nl}")
    message(FATAL_ERROR "rapid-${name}.service must run in the rapid group so shared /run/rapid-apply ownership stays usable")
  endif()
endforeach()
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
# #51: /var/lib/rapid-setup holds the device TLS identity, the enrollment token
# and the owner's setup database. It must stay private to the rapid service
# user, so both units that create it must declare StateDirectoryMode=0700; a
# future edit that widened it would otherwise pass verification.
if(NOT firstboot_service MATCHES "${nl}StateDirectory=rapid-setup${nl}" OR
   NOT firstboot_service MATCHES "${nl}StateDirectoryMode=0700${nl}")
  message(FATAL_ERROR "rapid-firstboot.service must create /var/lib/rapid-setup with StateDirectoryMode=0700")
endif()
if(NOT setup_service MATCHES "${nl}StateDirectory=rapid-setup${nl}" OR
   NOT setup_service MATCHES "${nl}StateDirectoryMode=0700${nl}")
  message(FATAL_ERROR "rapid-setup.service must keep /var/lib/rapid-setup at StateDirectoryMode=0700")
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
foreach(line IN ITEMS "User=root" "ProtectSystem=strict" "ProtectHome=read-only"
                      "LimitCORE=0" "ReadWritePaths=/etc -/home/rapid")
  string(FIND "${account_service}" "${nl}${line}${nl}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "rapid-account.service must keep '${line}'")
  endif()
endforeach()
# #56: NoNewPrivileges=true plus the rest of this unit's sandboxing makes the
# helper's privilege-dropping child fail setuid() with EPERM, so SSH-key
# enrollment never worked on the device. It must stay off.
if(account_service MATCHES "${nl}NoNewPrivileges=true${nl}")
  message(FATAL_ERROR "rapid-account.service must not set NoNewPrivileges=true: it breaks the helper's setuid key install (#56)")
endif()
if(NOT account_exec STREQUAL "ExecStart=/usr/lib/rapid/rapid-account --request-file /run/rapid-apply/account-request.json --result-file /run/rapid-apply/account-result.json --user rapid" OR
   account_service MATCHES "${nl}User=rapid" OR
   NOT account_path MATCHES "${nl}PathExists=/run/rapid-apply/account-request[.]json${nl}" OR
   NOT account_path MATCHES "${nl}Unit=rapid-account[.]service${nl}")
  message(FATAL_ERROR "Device access must be applied by the sandboxed root rapid-account helper from its fixed request file")
endif()
if(NOT setup_exec MATCHES " --account-request-file /run/rapid-apply/account-request[.]json --account-result-file /run/rapid-apply/account-result[.]json" OR
   setup_exec MATCHES "password" OR account_exec MATCHES "password" OR
   NOT setup_service MATCHES "${nl}LimitCORE=0${nl}" OR
   NOT setup_service MATCHES "${nl}ReadWritePaths=/run/rapid${nl}")
  message(FATAL_ERROR "The setup service must queue device access through rapid-account without secrets on a command line or in core dumps")
endif()
if(NOT setup_exec MATCHES " --ssid-file /run/rapid/network-ssid( |$)")
  message(FATAL_ERROR "The setup service must publish the setup AP name chosen by rapid-provision")
endif()
# #25: rapid-wifi.service and rapid-display-recovery.service each write a
# result/state file back under a ProtectSystem=strict mount namespace; without
# their own runtime directory (wifi) or ReadWritePaths grant
# (display-recovery), that write silently fails on a real device (WSL has no
# systemd, so the gate cannot exercise this).
if(NOT wifi_service MATCHES "${nl}ProtectSystem=strict${nl}" OR
   NOT wifi_service MATCHES "${nl}ReadWritePaths=/run/rapid-apply${nl}")
  message(FATAL_ERROR "rapid-wifi.service must keep ProtectSystem=strict and grant the shared /run/rapid-apply queue through ReadWritePaths=")
endif()
if(NOT display_recovery_service MATCHES "${nl}ProtectSystem=strict${nl}" OR
   NOT display_recovery_service MATCHES "${nl}ReadWritePaths=/var/lib/rapid${nl}")
  message(FATAL_ERROR "rapid-display-recovery.service must keep ProtectSystem=strict and grant ReadWritePaths=/var/lib/rapid for its recovery state file")
endif()
# #41: /var/lib/rapid is created by rapid.service's StateDirectory=rapid
# (rapid:rapid 0700). rapid-apply.service and rapid-display-recovery.service
# grant ReadWritePaths=/var/lib/rapid, which systemd requires to exist before
# ExecStart, so both must be ordered after rapid.service. They must NOT declare
# StateDirectory=rapid themselves: that would re-own the directory to root and
# lock out rapid.service.
foreach(name IN ITEMS apply display_recovery)
  set(service_var "${name}_service")
  set(service "${${service_var}}")
  if(NOT service MATCHES "${nl}Requires=rapid[.]service${nl}")
    message(FATAL_ERROR "rapid-${name}.service must require rapid.service so /var/lib/rapid exists before its mount namespace")
  endif()
  if(NOT service MATCHES "${nl}After=rapid[.]service${nl}")
    message(FATAL_ERROR "rapid-${name}.service must be ordered After=rapid.service, which creates /var/lib/rapid")
  endif()
  if(service MATCHES "${nl}StateDirectory=rapid${nl}")
    message(FATAL_ERROR "rapid-${name}.service must not declare StateDirectory=rapid; that would re-own /var/lib/rapid to root and lock out rapid.service")
  endif()
endforeach()
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
if(NOT DEFINED EXPECT_IMAGE_READY)
  set(EXPECT_IMAGE_READY OFF)
endif()
string(FIND "${contents}" " ./usr/share/rapid/rapid-image-ready-v1\n" image_ready_pos)
if(EXPECT_IMAGE_READY)
  if(image_ready_pos EQUAL -1)
    message(FATAL_ERROR "Release package must contain the image-ready marker")
  endif()
elseif(NOT image_ready_pos EQUAL -1)
  message(FATAL_ERROR "Ordinary package must not contain the image-ready marker")
endif()
# #48: the setup page serves the bundled Windows companion from
# /usr/share/rapid/companion (rapid-setup.service passes --companion-artifact
# /usr/share/rapid/companion). A release package that ships without it would
# advertise a download it cannot serve, so require at least one artifact there
# for an image-ready/release package. Ordinary main builds may omit it.
string(REGEX MATCH " ./usr/share/rapid/companion/[^ \n]+" companion_artifact_pos "${contents}")
if(EXPECT_IMAGE_READY)
  if(companion_artifact_pos STREQUAL "")
    message(FATAL_ERROR "Release package must contain the bundled companion artifact under ./usr/share/rapid/companion/")
  endif()
endif()
message(STATUS "Package contents, dependencies and conffile checks passed")
