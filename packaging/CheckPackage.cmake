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
read_service("./usr/lib/rapid/rapid-panel" panel_script)
file(REMOVE "${data_tar}")
if(NOT firstboot_service MATCHES "User=rapid" OR
   NOT firstboot_service MATCHES "Group=rapid" OR
   NOT firstboot_service MATCHES "StateDirectory=rapid-setup" OR
   NOT firstboot_service MATCHES "RuntimeDirectory=rapid")
  message(FATAL_ERROR "First boot must create private setup state as the rapid service user")
endif()
if(NOT provision_service MATCHES "User=root" OR
   NOT provision_service MATCHES "Requires=rapid-firstboot[.]service" OR
   NOT provision_service MATCHES "--status-file /run/rapid/firstboot[.]json" OR
   NOT provision_service MATCHES "--ssid-file /run/rapid/network-ssid" OR
   NOT provision_service MATCHES "RuntimeDirectory=rapid")
  message(FATAL_ERROR "Only the provisioner may run as root for the generated setup-AP state")
endif()
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
execute_process(COMMAND "${DPKG_DEB}" --field "${PACKAGE}" Depends OUTPUT_VARIABLE dependencies
                RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT dependencies MATCHES "libargon2" OR NOT dependencies MATCHES "libqt6core" OR
   NOT dependencies MATCHES "xinput")
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
