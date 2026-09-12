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
add_library(rapid_native STATIC src/native_common.cpp src/native_v4.cpp src/native_recorder.cpp
  src/native_runtime.cpp src/native_http.cpp src/native_acc.cpp src/native_upload.cpp src/native_setup.cpp)
target_include_directories(rapid_native PUBLIC include)
target_link_libraries(rapid_native PUBLIC nlohmann_json::nlohmann_json OpenSSL::Crypto
  SQLite::SQLite3 CURL::libcurl Boost::system tomlplusplus::tomlplusplus Threads::Threads)
target_compile_options(rapid_native PRIVATE -Wall -Wextra -Wpedantic)
add_executable(rapid-pi src/pi_main.cpp)
target_link_libraries(rapid-pi PRIVATE rapid_native)

# Optional for server-only installations.
# The Qt target consumes the local HTTP API; it does not link to or alter the
# receiver/recorder runtime.
if(RAPID_BUILD_QT_DISPLAY)
  find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Network Qml Quick)
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
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick)
  target_compile_options(rapid-qt-display PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(BUILD_TESTING)
    qt_add_executable(rapid-qt-tests tests/qt_dashboard_tests.cpp
      src/qt_dashboard_model.cpp src/qt_dashboard_model.hpp)
    target_link_libraries(rapid-qt-tests PRIVATE Qt6::Core Qt6::Network)
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
  add_test(NAME rapid-setup-tests COMMAND rapid-setup-tests)
  add_executable(rapid-v4-tests tests/v4_tests.cpp)
  target_link_libraries(rapid-v4-tests PRIVATE rapid_native)
  set(RAPID_V4_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/v4-fixtures" CACHE PATH "Windows encoder fixtures")
  add_test(NAME rapid-v4-tests COMMAND rapid-v4-tests ${CMAKE_CURRENT_SOURCE_DIR}/assets ${RAPID_V4_FIXTURES})
  add_executable(rapid-network-tests tests/native_network_tests.cpp)
  target_link_libraries(rapid-network-tests PRIVATE rapid_setup_auth)
  add_test(NAME rapid-network-tests COMMAND rapid-network-tests $<TARGET_FILE:rapid-pi> ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE:rapid-archive> $<TARGET_FILE:rapid-setup-server>)
  set_tests_properties(rapid-network-tests PROPERTIES TIMEOUT 45)
  add_test(NAME rapid-archive-tests COMMAND rapid-archive --self-test ${CMAKE_CURRENT_SOURCE_DIR}/assets)
  add_executable(rapid-native-tests tests/native_runtime_tests.cpp)
  target_link_libraries(rapid-native-tests PRIVATE rapid_native)
  add_test(NAME rapid-native-tests COMMAND rapid-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/assets)
  add_executable(rapid-log-status-tests tests/log_status_tests.cpp)
  target_include_directories(rapid-log-status-tests PRIVATE include)
  target_compile_options(rapid-log-status-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME rapid-log-status-tests COMMAND rapid-log-status-tests)
endif()

option(RAPID_BUILD_PI_PACKAGE "Assemble the experimental Pi application package" OFF)
if(RAPID_BUILD_PI_PACKAGE)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../packaging/PiPackage.cmake)
endif()
