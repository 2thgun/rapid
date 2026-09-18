# Application package first; image generation consumes this artifact later.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT RAPID_BUILD_QT_DISPLAY OR NOT RAPID_BUILD_LOG_STATUS)
  message(FATAL_ERROR "Pi packaging requires Linux, RAPID_BUILD_QT_DISPLAY=ON and RAPID_BUILD_LOG_STATUS=ON")
endif()

install(TARGETS rapid-pi rapid-qt-display rapid-log-status rapid-setup-server rapid-firstboot rapid-provision rapid-apply rapid-display-recovery rapid-wifi rapid-account
        RUNTIME DESTINATION lib/rapid)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/" DESTINATION share/rapid)
install(PROGRAMS "${CMAKE_CURRENT_LIST_DIR}/rapid-network-mode"
        DESTINATION lib/rapid)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/config.toml" DESTINATION /etc/rapid)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/rapid.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-display.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-display-recovery.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-setup.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-firstboot.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-provision.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-apply.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-apply.path"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-wifi.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-wifi.path"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-account.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-account.path"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-network-mode.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-log-status.service"
        DESTINATION lib/systemd/system)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/rapid.sysusers" DESTINATION lib/sysusers.d RENAME rapid.conf)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/rapid.tmpfiles" DESTINATION lib/tmpfiles.d RENAME rapid.conf)
install(PROGRAMS "${CMAKE_CURRENT_LIST_DIR}/rapid-panel" DESTINATION lib/rapid)
# Off by default: the marker means "this package is the complete first-time
# flow" and must not appear on ordinary main builds. Release.yml turns it on.
if(RAPID_IMAGE_READY)
  install(FILES "${CMAKE_CURRENT_LIST_DIR}/rapid-image-ready-v1" DESTINATION share/rapid)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/RapidVersion.cmake")
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME rapid)
set(CPACK_PACKAGE_VERSION "${RAPID_PACKAGE_VERSION}")
set(CPACK_PACKAGE_CONTACT "raPId maintainers (github.com/2thgun/rapid)")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "raPId Pi runtime, Qt panel and local setup preview")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
# Configuration belongs in /etc, not /usr/etc.
set(CPACK_SET_DESTDIR ON)
set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Installation prefix" FORCE)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "systemd, network-manager, openssh-server, sudo, xinit, xserver-xorg-core, xserver-xorg-video-fbdev, xserver-xorg-input-evdev, x11-xserver-utils, xinput,qml6-module-qtquick, qml6-module-qtquick-window, qml6-module-qtqml-workerscript")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_LIST_DIR}/conffiles")
include(CPack)
