# Journal/HTTP log monitor. The executable needs libsystemd, so it stays behind
# RAPID_BUILD_LOG_STATUS; its tests exercise header-only logic and build
# unconditionally, exactly as they did before the split.
#
# log_status.hpp is consumed only from this folder, so there is no library to
# export it: both targets take the include root directly.
if(RAPID_BUILD_LOG_STATUS)
  add_executable(rapid-log-status src/log_status_main.cpp)
  target_include_directories(rapid-log-status PRIVATE include)
  target_link_libraries(rapid-log-status PRIVATE PkgConfig::SYSTEMD)
  target_compile_options(rapid-log-status PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING)
  add_executable(rapid-log-status-tests tests/log_status_tests.cpp)
  target_include_directories(rapid-log-status-tests PRIVATE include)
  target_compile_options(rapid-log-status-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME rapid-log-status-tests COMMAND rapid-log-status-tests)
endif()
