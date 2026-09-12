# Application package first; image generation consumes this artifact later.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT RAPID_BUILD_QT_DISPLAY OR NOT RAPID_BUILD_LOG_STATUS)
  message(FATAL_ERROR "Pi packaging requires Linux, RAPID_BUILD_QT_DISPLAY=ON and RAPID_BUILD_LOG_STATUS=ON")
endif()

install(TARGETS rapid-pi rapid-qt-display rapid-log-status rapid-setup-server rapid-firstboot
        RUNTIME DESTINATION lib/rapid)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/" DESTINATION share/rapid)
install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/../systemd/rapid-network-mode"
        DESTINATION lib/rapid)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/config.toml" DESTINATION /etc/rapid)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/rapid.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-display.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-setup.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-firstboot.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-network-mode.service"
              "${CMAKE_CURRENT_LIST_DIR}/rapid-log-status.service"
        DESTINATION lib/systemd/system)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/rapid.sysusers" DESTINATION lib/sysusers.d RENAME rapid.conf)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/rapid.tmpfiles" DESTINATION lib/tmpfiles.d RENAME rapid.conf)
install(PROGRAMS "${CMAKE_CURRENT_LIST_DIR}/rapid-panel" DESTINATION lib/rapid)

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME rapid)
set(CPACK_PACKAGE_VERSION "0.1.0~preview1")
set(CPACK_PACKAGE_CONTACT "raPId maintainers (github.com/2thgun/rapid)")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "raPId Pi runtime, Qt panel and local setup preview")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
# Configuration belongs in /etc, not /usr/etc.
set(CPACK_SET_DESTDIR ON)
set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Installation prefix" FORCE)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "systemd, network-manager, xinit, xserver-xorg-core, xserver-xorg-video-fbdev, xserver-xorg-input-evdev, x11-xserver-utils, qml6-module-qtquick, qml6-module-qtquick-window, qml6-module-qtqml-workerscript")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_LIST_DIR}/conffiles")
include(CPack)
