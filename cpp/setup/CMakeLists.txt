# First-boot, enrollment and device administration: the local setup server and
# the small root helpers it queues work for (firstboot, provision, apply, wifi,
# account) plus the display recovery tool.
#
# This component owns setup_auth.hpp and account.hpp. It does not own setup.hpp,
# which is implemented by runtime/src/native_setup.cpp and therefore lives in
# runtime/include; the binaries here reach it by linking rapid_native.
add_library(rapid_account_policy STATIC src/account_policy.cpp)
target_include_directories(rapid_account_policy PUBLIC include)
target_link_libraries(rapid_account_policy PUBLIC OpenSSL::Crypto)
target_compile_options(rapid_account_policy PRIVATE -Wall -Wextra -Wpedantic -Werror)

# setup_auth.hpp includes "rapid/pairing.hpp" and "rapid/setup.hpp": rapid_pairing
# supplies the first and brings rapid_native, which supplies the second.
add_library(rapid_setup_auth STATIC src/setup_auth.cpp src/setup_account.cpp)
target_include_directories(rapid_setup_auth PUBLIC include)
target_link_libraries(rapid_setup_auth PUBLIC rapid_native rapid_pairing rapid_account_policy ${ARGON2_LIBRARY} ${CRYPT_LIBRARY})
target_compile_options(rapid_setup_auth PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(rapid-setup-server src/setup_main.cpp)
target_link_libraries(rapid-setup-server PRIVATE rapid_setup_auth)
target_compile_options(rapid-setup-server PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(rapid-firstboot src/firstboot_main.cpp)
target_link_libraries(rapid-firstboot PRIVATE rapid_native)
target_compile_options(rapid-firstboot PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(rapid-provision src/provision_main.cpp)
target_link_libraries(rapid-provision PRIVATE rapid_native)
target_compile_options(rapid-provision PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(rapid-apply src/apply_main.cpp)
target_link_libraries(rapid-apply PRIVATE rapid_native)
target_compile_options(rapid-apply PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(rapid-wifi src/wifi_main.cpp)
target_link_libraries(rapid-wifi PRIVATE rapid_native)
target_compile_options(rapid-wifi PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(rapid-account src/account_main.cpp)
target_link_libraries(rapid-account PRIVATE rapid_native rapid_account_policy)
target_compile_options(rapid-account PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(rapid-display-recovery src/display_recovery_main.cpp)
target_link_libraries(rapid-display-recovery PRIVATE rapid_native)
target_compile_options(rapid-display-recovery PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
  add_executable(rapid-setup-auth-tests tests/setup_auth_tests.cpp)
  target_link_libraries(rapid-setup-auth-tests PRIVATE rapid_setup_auth)
  add_test(NAME rapid-setup-auth-tests COMMAND rapid-setup-auth-tests)
  # #23: password/key validation, API auth/CSRF/rate limit, the root helper against a
  # fake root, and secret non-persistence across every produced artifact.
  add_executable(rapid-account-tests tests/account_tests.cpp)
  target_link_libraries(rapid-account-tests PRIVATE rapid_setup_auth)
  target_compile_options(rapid-account-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME rapid-account-tests COMMAND rapid-account-tests $<TARGET_FILE:rapid-account>)
  # #28: the setup page's OpenSSH key encoding, loaded straight out of
  # assets/setup.html and checked against fixed Ed25519/ECDSA vectors with node.
  find_program(NODE_EXECUTABLE node REQUIRED)
  add_test(NAME rapid-openssh-key-tests
           COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/openssh_key_tests.mjs
                   ${CMAKE_SOURCE_DIR}/assets/setup.html)
  # Setup-page UX rework: separated categories with a Save action and status
  # line each, the Wi-Fi wording, required vs recommended, and the shortened
  # TLS fingerprint, all loaded/checked out of the real setup.html.
  add_test(NAME rapid-setup-page-tests
           COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/setup_page_tests.mjs
                   ${CMAKE_SOURCE_DIR}/assets/setup.html)
  add_executable(rapid-setup-tests tests/setup_tests.cpp)
  target_link_libraries(rapid-setup-tests PRIVATE rapid_native)
  add_test(NAME rapid-setup-tests COMMAND rapid-setup-tests $<TARGET_FILE:rapid-firstboot> $<TARGET_FILE:rapid-provision> $<TARGET_FILE:rapid-apply> $<TARGET_FILE:rapid-wifi>)
  # #26: the Home Wi-Fi passphrase must never become an nmcli argument; a fake
  # nmcli records its own argv and environment, and every artifact the flow
  # produces is grepped for the plaintext password (rapid-account-tests'
  # secret_tests()/files_containing() pattern).
  add_executable(rapid-wifi-secret-tests tests/wifi_secret_tests.cpp)
  target_link_libraries(rapid-wifi-secret-tests PRIVATE rapid_native)
  target_compile_options(rapid-wifi-secret-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME rapid-wifi-secret-tests COMMAND rapid-wifi-secret-tests $<TARGET_FILE:rapid-wifi>)
  add_executable(rapid-display-recovery-tests tests/display_recovery_tests.cpp)
  target_link_libraries(rapid-display-recovery-tests PRIVATE rapid_native)
  target_compile_options(rapid-display-recovery-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME rapid-display-recovery-tests COMMAND rapid-display-recovery-tests
           $<TARGET_FILE:rapid-display-recovery>)
endif()
