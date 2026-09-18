# The receiver/recorder runtime: the shared library every non-Qt binary links,
# and the rapid-pi daemon itself.
#
# rapid_native owns two include roots: ../include, where the two genuinely
# universal headers (native.hpp, lap_boundary.hpp) live, and its own include/,
# where setup.hpp and channels.inc live because native_setup.cpp and
# native_runtime.cpp implement them. Both are PUBLIC, so every component that
# links rapid_native resolves "rapid/native.hpp" and "rapid/setup.hpp" without
# reaching into another component's directory by hand.
add_library(rapid_native STATIC
  src/native_common.cpp
  src/native_v4.cpp
  src/native_replay.cpp
  src/native_recorder.cpp
  src/native_runtime.cpp
  src/native_http.cpp
  src/native_acc.cpp
  src/native_upload.cpp
  src/native_setup.cpp)
target_include_directories(rapid_native PUBLIC "${CMAKE_SOURCE_DIR}/include" include)
target_link_libraries(rapid_native PUBLIC nlohmann_json::nlohmann_json OpenSSL::Crypto OpenSSL::SSL
  SQLite::SQLite3 CURL::libcurl Boost::system tomlplusplus::tomlplusplus Threads::Threads)
target_compile_options(rapid_native PRIVATE -Wall -Wextra -Wpedantic)

# pi_main.cpp includes "rapid/pairing.hpp" and drives the pairing coordinator,
# so it links rapid_pairing as well; rapid_pairing brings rapid_native with it.
add_executable(rapid-pi src/pi_main.cpp)
target_link_libraries(rapid-pi PRIVATE rapid_native rapid_pairing)

if(BUILD_TESTING)
  add_executable(rapid-v4-tests tests/v4_tests.cpp)
  target_link_libraries(rapid-v4-tests PRIVATE rapid_native)
  set(RAPID_V4_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/v4-fixtures" CACHE PATH "Windows encoder fixtures")
  add_test(NAME rapid-v4-tests COMMAND rapid-v4-tests "${CMAKE_SOURCE_DIR}/assets" ${RAPID_V4_FIXTURES})
  # #17 fix round: sender_lag_ms/process_ms/lock_wait_ms measurement points
  # (native_runtime.cpp, native_v4.cpp), with injected sender/receipt clocks.
  add_executable(rapid-latency-tests tests/latency_tests.cpp)
  target_link_libraries(rapid-latency-tests PRIVATE rapid_native)
  add_test(NAME rapid-latency-tests COMMAND rapid-latency-tests "${CMAKE_SOURCE_DIR}/assets")
  set_tests_properties(rapid-latency-tests PROPERTIES TIMEOUT 60)
  # The HTTP surface test drives the real binaries end to end, so it reaches
  # across components for the setup server it starts; rapid_setup_auth is
  # defined in setup/, which CMake resolves once every subdirectory is read.
  add_executable(rapid-network-tests tests/native_network_tests.cpp)
  target_link_libraries(rapid-network-tests PRIVATE rapid_setup_auth)
  add_test(NAME rapid-network-tests COMMAND rapid-network-tests $<TARGET_FILE:rapid-pi> "${CMAKE_SOURCE_DIR}/assets" $<TARGET_FILE:rapid-archive> $<TARGET_FILE:rapid-setup-server>)
  set_tests_properties(rapid-network-tests PROPERTIES TIMEOUT 45)
  add_executable(rapid-native-tests tests/native_runtime_tests.cpp)
  target_link_libraries(rapid-native-tests PRIVATE rapid_native)
  add_test(NAME rapid-native-tests COMMAND rapid-native-tests "${CMAKE_SOURCE_DIR}/assets")
  add_executable(rapid-recorder-tests tests/native_recorder_tests.cpp)
  target_link_libraries(rapid-recorder-tests PRIVATE rapid_native)
  add_test(NAME rapid-recorder-tests COMMAND rapid-recorder-tests)
  add_executable(rapid-lap-boundary-tests tests/lap_boundary_tests.cpp)
  target_link_libraries(rapid-lap-boundary-tests PRIVATE rapid_native)
  set(RAPID_LAP_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/lap-fixtures" CACHE PATH "Decimated real-recording lap boundary fixtures (#16, #20)")
  add_test(NAME rapid-lap-boundary-tests COMMAND rapid-lap-boundary-tests "${CMAKE_SOURCE_DIR}/assets" ${RAPID_LAP_FIXTURES})
  # #17 step 8: receive-path durability and latency. The executable defines
  # fsync/fdatasync to simulate slow or failing storage, so its symbols must be
  # visible to libsqlite3.
  add_executable(rapid-io-path-tests tests/io_path_tests.cpp)
  target_link_libraries(rapid-io-path-tests PRIVATE rapid_native)
  set_target_properties(rapid-io-path-tests PROPERTIES ENABLE_EXPORTS ON)
  foreach(io_case replay-crash recording-crash finalize-race write-failure latency)
    add_test(NAME rapid-io-${io_case} COMMAND rapid-io-path-tests
             "${CMAKE_SOURCE_DIR}/assets" ${io_case})
    set_tests_properties(rapid-io-${io_case} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
  endforeach()
endif()
