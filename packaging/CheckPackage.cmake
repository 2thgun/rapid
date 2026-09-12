if(NOT EXISTS "${PACKAGE}")
  message(FATAL_ERROR "Pass -DPACKAGE=/absolute/path/to/rapid.deb")
endif()
execute_process(COMMAND dpkg-deb --contents "${PACKAGE}" OUTPUT_VARIABLE contents
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
    "./usr/share/rapid/setup.html"
    "./usr/lib/systemd/system/rapid.service"
    "./usr/lib/systemd/system/rapid-display.service"
    "./usr/lib/systemd/system/rapid-setup.service"
    "./usr/lib/systemd/system/rapid-firstboot.service")
  string(FIND "${contents}" " ${path}\n" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Package is missing ${path}")
  endif()
endforeach()
if(contents MATCHES "[.]key\n|[.]db\n|runtime[.]env\n|/home/rapid/|/usr/etc/")
  message(FATAL_ERROR "Package contains private state or invalid installation paths")
endif()
execute_process(COMMAND dpkg-deb --field "${PACKAGE}" Depends OUTPUT_VARIABLE dependencies
                RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT dependencies MATCHES "libargon2" OR NOT dependencies MATCHES "libqt6core")
  message(FATAL_ERROR "Missing generated runtime library dependencies")
endif()
execute_process(COMMAND dpkg-deb --ctrl-tarfile "${PACKAGE}" OUTPUT_FILE "${PACKAGE}.control.tar"
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
