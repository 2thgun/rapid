cmake_minimum_required(VERSION 3.24)
project(raPId LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Every component's binaries and archives land in the top build directory, the
# same place they did when this was one flat CMakeLists.txt. Keeping that
# stable means nothing outside the build (packaging, CI, local scripts) has to
# learn which subdirectory now defines which target.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

option(RAPID_BUILD_LOG_STATUS "Build the Linux journal/HTTP log monitor" OFF)
option(RAPID_BUILD_QT_DISPLAY "Build the Qt touchscreen display" OFF)
option(RAPID_BUILD_PI_PACKAGE "Assemble the experimental Pi application package" OFF)
# #10: absolute path to the Windows companion artifact (portable .exe or .msi)
# to bundle into the Pi package and serve from the setup page. Build it from
# the same revision as the package so the download cannot be a stale protocol.
# Empty means "serve no companion" (the setup page then offers no download),
# never a guessed or mismatched binary.
set(RAPID_COMPANION_ARTIFACT "" CACHE FILEPATH "Windows companion artifact bundled into the Pi package")

# Dependency detection stays here, in one place, so that two components cannot
# each discover the same dependency differently. Components declare what they
# use; they do not go looking for it themselves.
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(tomlplusplus REQUIRED)
find_package(Threads REQUIRED)
# Dependencies are supplied by the OS, not downloaded at build time.
find_library(ARGON2_LIBRARY argon2 REQUIRED)
# libxcrypt: yescrypt hashing of the owner-chosen account password (#23).
find_library(CRYPT_LIBRARY crypt REQUIRED)
if(RAPID_BUILD_LOG_STATUS)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
endif()
if(RAPID_BUILD_QT_DISPLAY)
  # WebSockets: the live dashboard push (#17) subscribes to /api/v1/live over
  # a socket instead of polling; HTTP polling remains the fallback when the
  # module or the socket is unavailable.
  find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Network Qml Quick WebSockets)
endif()

# rapid_native and rapid_pairing first: every other component links one of them.
add_subdirectory(runtime)
add_subdirectory(pairing)
add_subdirectory(setup)
add_subdirectory(archive)
# Unconditional: the rapid-log-status executable inside is guarded by
# RAPID_BUILD_LOG_STATUS, but rapid-log-status-tests always builds, exactly as
# it did before the split.
add_subdirectory(log-status)
# Optional for server-only installations.
# The Qt target consumes the local HTTP API; it does not link to or alter the
# receiver/recorder runtime.
if(RAPID_BUILD_QT_DISPLAY)
  add_subdirectory(display)
endif()
# Whole-package tests that belong to no single component.
add_subdirectory(tests)

if(RAPID_BUILD_PI_PACKAGE)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../packaging/PiPackage.cmake)
endif()
