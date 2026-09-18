# Recording archive maintenance: one binary, whose --self-test is its CTest
# target. The schema it reads lives in cpp/assets, shared with the runtime.
add_executable(rapid-archive src/archive_main.cpp)
target_link_libraries(rapid-archive PRIVATE rapid_native ${ARGON2_LIBRARY})
target_compile_options(rapid-archive PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
  add_test(NAME rapid-archive-tests COMMAND rapid-archive --self-test "${CMAKE_SOURCE_DIR}/assets")
endif()
