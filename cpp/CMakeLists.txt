cmake_minimum_required(VERSION 3.24)
project(raPId LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RAPID_BUILD_LOG_STATUS "Build the Linux journal/HTTP log monitor" OFF)
option(RAPID_BUILD_QT_DISPLAY "Build the Qt touchscreen display" OFF)
if(RAPID_BUILD_LOG_STATUS)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
  add_executable(rapid-log-status src/log_status_main.cpp)
  target_include_directories(rapid-log-status PRIVATE include)
  target_link_libraries(rapid-log-status PRIVATE PkgConfig::SYSTEMD)
  target_compile_options(rapid-log-status PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(tomlplusplus REQUIRED)
find_package(Threads REQUIRED)
add_library(rapid_native STATIC src/native_common.cpp src/native_v4.cpp src/native_replay.cpp src/pairing.cpp src/pairing_transport.cpp src/native_recorder.cpp
  src/native_runtime.cpp src/native_http.cpp src/native_acc.cpp src/native_upload.cpp src/native_setup.cpp)
target_include_directories(rapid_native PUBLIC include)
target_link_libraries(rapid_native PUBLIC nlohmann_json::nlohmann_json OpenSSL::Crypto OpenSSL::SSL
  SQLite::SQLite3 CURL::libcurl Boost::system tomlplusplus::tomlplusplus Threads::Threads)
target_compile_options(rapid_native PRIVATE -Wall -Wextra -Wpedantic)
add_executable(rapid-pi src/pi_main.cpp)
target_link_libraries(rapid-pi PRIVATE rapid_native)
add_executable(rapid-display-recovery src/display_recovery_main.cpp)
target_link_libraries(rapid-display-recovery PRIVATE rapid_native)
target_compile_options(rapid-display-recovery PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Optional for server-only installations.
# The Qt target consumes the local HTTP API; it does not link to or alter the
# receiver/recorder runtime.
if(RAPID_BUILD_QT_DISPLAY)
  # WebSockets: the live dashboard push (#17) subscribes to /api/v1/live over
  # a socket instead of polling; HTTP polling remains the fallback when the
  # module or the socket is unavailable.
  find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Network Qml Quick WebSockets)
  qt_standard_project_setup()
  qt_add_executable(rapid-qt-display
    src/qt_dashboard_main.cpp
    src/qt_dashboard_model.cpp
    src/qt_dashboard_model.hpp
  )
  qt_add_resources(rapid-qt-display rapid_qt_display_assets
    PREFIX "/"
    FILES
      display/Main.qml
      display/HistoryPlot.qml
      assets/steering-wheel-cartoon.png
  )
  target_link_libraries(rapid-qt-display PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::WebSockets)
  target_compile_options(rapid-qt-display PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(BUILD_TESTING)
    qt_add_executable(rapid-qt-tests tests/qt_dashboard_tests.cpp
      src/qt_dashboard_model.cpp src/qt_dashboard_model.hpp)
    target_link_libraries(rapid-qt-tests PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets)
    target_compile_options(rapid-qt-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME rapid-qt-tests COMMAND rapid-qt-tests)
  endif()
endif()

# Dependencies are supplied by the OS, not downloaded at build time.
find_library(ARGON2_LIBRARY argon2 REQUIRED)
add_library(rapid_setup_auth STATIC src/setup_auth.cpp)
target_link_libraries(rapid_setup_auth PUBLIC rapid_native ${ARGON2_LIBRARY})
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
add_executable(rapid-archive src/archive_main.cpp)
target_link_libraries(rapid-archive PRIVATE rapid_native ${ARGON2_LIBRARY})
target_compile_options(rapid-archive PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
  add_test(NAME rapid-network-mode-tests COMMAND sh
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/network_mode_tests.sh
    ${CMAKE_CURRENT_SOURCE_DIR}/../systemd/rapid-network-mode)
  add_executable(rapid-setup-auth-tests tests/setup_auth_tests.cpp)
  target_link_libraries(rapid-setup-auth-tests PRIVATE rapid_setup_auth)
  add_test(NAME rapid-setup-auth-tests COMMAND rapid-setup-auth-tests)
  add_executable(rapid-setup-tests tests/setup_tests.cpp)
  target_link_libraries(rapid-setup-tests PRIVATE rapid_native)
  add_test(NAME rapid-setup-tests COMMAND rapid-setup-tests $<TARGET_FILE:rapid-firstboot> $<TARGET_FILE:rapid-provision> $<TARGET_FILE:rapid-apply> $<TARGET_FILE:rapid-wifi>)
  add_executable(rapid-v4-tests tests/v4_tests.cpp)
  target_link_libraries(rapid-v4-tests PRIVATE rapid_native)
  set(RAPID_V4_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/v4-fixtures" CACHE PATH "Windows encoder fixtures")
  add_test(NAME rapid-v4-tests COMMAND rapid-v4-tests ${CMAKE_CURRENT_SOURCE_DIR}/assets ${RAPID_V4_FIXTURES})
  add_executable(rapid-pairing-tests tests/pairing_tests.cpp)
  target_link_libraries(rapid-pairing-tests PRIVATE rapid_native)
  add_test(NAME rapid-pairing-tests COMMAND rapid-pairing-tests)
  add_executable(rapid-pairing-interop-tests tests/pairing_interop_tests.cpp)
  target_link_libraries(rapid-pairing-interop-tests PRIVATE rapid_native)
  set(RAPID_PAIRING_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/pairing-fixtures" CACHE PATH "Windows pairing interop fixtures")
  add_test(NAME rapid-pairing-interop-tests COMMAND rapid-pairing-interop-tests ${RAPID_PAIRING_FIXTURES})
  # #14 production direction (the reverse of the fixture above): the Pi seals
  # with the real code, and the Windows self-test decrypts with the real
  # decrypt_pairing_envelope(). This target generates and self-checks a fresh
  # fixture on every run, written to the build tree (never the source tree,
  # so a local ctest run never leaves the committed snapshot dirty); a
  # deliberately-committed snapshot for local/offline Windows runs lives at
  # RAPID_PAIRING_PI_SEALS_FIXTURES below.
  add_executable(rapid-pairing-pi-seals-fixture-tests tests/pairing_pi_seals_fixture_tests.cpp)
  target_link_libraries(rapid-pairing-pi-seals-fixture-tests PRIVATE rapid_native)
  set(RAPID_PAIRING_PI_SEALS_FIXTURES_OUT "${CMAKE_BINARY_DIR}/pairing-fixtures-pi-seals" CACHE PATH
      "Where each test run writes a fresh Pi-sealed pairing fixture for CI to pass to the Windows job")
  add_test(NAME rapid-pairing-pi-seals-fixture-tests COMMAND rapid-pairing-pi-seals-fixture-tests
           ${RAPID_PAIRING_PI_SEALS_FIXTURES_OUT})
  set(RAPID_PAIRING_PI_SEALS_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/pairing-fixtures-pi-seals" CACHE PATH
      "Committed snapshot of a Pi-sealed pairing fixture, for the Windows self-test")
  add_executable(rapid-network-tests tests/native_network_tests.cpp)
  target_link_libraries(rapid-network-tests PRIVATE rapid_setup_auth)
  add_test(NAME rapid-network-tests COMMAND rapid-network-tests $<TARGET_FILE:rapid-pi> ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE:rapid-archive> $<TARGET_FILE:rapid-setup-server>)
  set_tests_properties(rapid-network-tests PROPERTIES TIMEOUT 45)
  add_test(NAME rapid-archive-tests COMMAND rapid-archive --self-test ${CMAKE_CURRENT_SOURCE_DIR}/assets)
  add_executable(rapid-native-tests tests/native_runtime_tests.cpp)
  target_link_libraries(rapid-native-tests PRIVATE rapid_native)
  add_test(NAME rapid-native-tests COMMAND rapid-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/assets)
  add_executable(rapid-recorder-tests tests/native_recorder_tests.cpp)
  target_link_libraries(rapid-recorder-tests PRIVATE rapid_native)
  add_test(NAME rapid-recorder-tests COMMAND rapid-recorder-tests)
  add_executable(rapid-lap-boundary-tests tests/lap_boundary_tests.cpp)
  target_link_libraries(rapid-lap-boundary-tests PRIVATE rapid_native)
  set(RAPID_LAP_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/lap-fixtures" CACHE PATH "Decimated real-recording lap boundary fixtures (#16, #20)")
  add_test(NAME rapid-lap-boundary-tests COMMAND rapid-lap-boundary-tests ${CMAKE_CURRENT_SOURCE_DIR}/assets ${RAPID_LAP_FIXTURES})
  # #17 step 8: receive-path durability and latency. The executable defines
  # fsync/fdatasync to simulate slow or failing storage, so its symbols must be
  # visible to libsqlite3.
  add_executable(rapid-io-path-tests tests/io_path_tests.cpp)
  target_link_libraries(rapid-io-path-tests PRIVATE rapid_native)
  set_target_properties(rapid-io-path-tests PROPERTIES ENABLE_EXPORTS ON)
  foreach(io_case replay-crash recording-crash finalize-race write-failure latency)
    add_test(NAME rapid-io-${io_case} COMMAND rapid-io-path-tests
             ${CMAKE_CURRENT_SOURCE_DIR}/assets ${io_case})
    set_tests_properties(rapid-io-${io_case} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
  endforeach()
  add_executable(rapid-display-recovery-tests tests/display_recovery_tests.cpp)
  target_link_libraries(rapid-display-recovery-tests PRIVATE rapid_native)
  target_compile_options(rapid-display-recovery-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME rapid-display-recovery-tests COMMAND rapid-display-recovery-tests
           $<TARGET_FILE:rapid-display-recovery>)
  add_executable(rapid-log-status-tests tests/log_status_tests.cpp)
  target_include_directories(rapid-log-status-tests PRIVATE include)
  target_compile_options(rapid-log-status-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME rapid-log-status-tests COMMAND rapid-log-status-tests)
endif()

option(RAPID_BUILD_PI_PACKAGE "Assemble the experimental Pi application package" OFF)
if(RAPID_BUILD_PI_PACKAGE)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../packaging/PiPackage.cmake)
endif()
