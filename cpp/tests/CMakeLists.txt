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
  add_test(NAME rapid-package-verifier-tests COMMAND sh
    "${CMAKE_CURRENT_SOURCE_DIR}/package_verifier_tests.sh"
    "${CMAKE_SOURCE_DIR}/..")
endif()
