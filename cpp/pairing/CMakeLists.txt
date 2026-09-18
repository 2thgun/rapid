# Device pairing: the sealed-key exchange, the pairing window/coordinator and
# its transport. pairing.cpp and pairing_transport.cpp used to be compiled into
# rapid_native; they are their own library now so that the binaries which
# actually pair (rapid-pi, the setup server) say so in their link line.
#
# pairing.hpp includes "rapid/native.hpp" and "rapid/setup.hpp", so this library
# links rapid_native PUBLIC: the dependency runs pairing -> runtime, and
# anything that links rapid_pairing gets the runtime headers with it.
add_library(rapid_pairing STATIC
  src/pairing.cpp
  src/pairing_transport.cpp)
target_include_directories(rapid_pairing PUBLIC include)
target_link_libraries(rapid_pairing PUBLIC rapid_native)
target_compile_options(rapid_pairing PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
  add_executable(rapid-pairing-tests tests/pairing_tests.cpp)
  target_link_libraries(rapid-pairing-tests PRIVATE rapid_pairing)
  add_test(NAME rapid-pairing-tests COMMAND rapid-pairing-tests)
  add_executable(rapid-pairing-interop-tests tests/pairing_interop_tests.cpp)
  target_link_libraries(rapid-pairing-interop-tests PRIVATE rapid_pairing)
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
  target_link_libraries(rapid-pairing-pi-seals-fixture-tests PRIVATE rapid_pairing)
  set(RAPID_PAIRING_PI_SEALS_FIXTURES_OUT "${CMAKE_BINARY_DIR}/pairing-fixtures-pi-seals" CACHE PATH
      "Where each test run writes a fresh Pi-sealed pairing fixture for CI to pass to the Windows job")
  add_test(NAME rapid-pairing-pi-seals-fixture-tests COMMAND rapid-pairing-pi-seals-fixture-tests
           ${RAPID_PAIRING_PI_SEALS_FIXTURES_OUT})
  set(RAPID_PAIRING_PI_SEALS_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/pairing-fixtures-pi-seals" CACHE PATH
      "Committed snapshot of a Pi-sealed pairing fixture, for the Windows self-test")
endif()
