# Whole-package checks that belong to no single component: the Home/AP/Off
# network-mode worker shipped from packaging/, and the package verifier that
# reads the repository's packaging tree.
#
# Both take a path into the repository, above cpp/. ${CMAKE_SOURCE_DIR} is cpp/
# whichever subdirectory is being processed, so these stay correct regardless of
# where the test is registered from.
if(BUILD_TESTING)
  add_test(NAME rapid-network-mode-tests COMMAND sh
    "${CMAKE_CURRENT_SOURCE_DIR}/network_mode_tests.sh"
    "${CMAKE_SOURCE_DIR}/../packaging/rapid-network-mode")
  # #50: the nmcli connection load + GLib keyfile-escaping path against real
  # NetworkManager (a fake nmcli covers it in rapid-wifi-secret-tests). Writes
  # a Home profile with an SSID containing spaces, special characters and a
  # backslash, and checks NetworkManager parses it back unchanged. Activation
  # runs only where a wifi device exists (a real Pi); elsewhere the script
  # still passes so it can run in the local gate.
  add_test(NAME rapid-wifi-real-nm-tests COMMAND sh
    "${CMAKE_CURRENT_SOURCE_DIR}/wifi_real_nm_tests.sh"
    "$<TARGET_FILE:rapid-wifi>")
  add_test(NAME rapid-package-verifier-tests COMMAND sh
    "${CMAKE_CURRENT_SOURCE_DIR}/package_verifier_tests.sh"
    "${CMAKE_SOURCE_DIR}/..")
  add_test(NAME rapid-version-tests COMMAND sh
    "${CMAKE_CURRENT_SOURCE_DIR}/rapid_version_tests.sh"
    "${CMAKE_SOURCE_DIR}/..")
endif()
