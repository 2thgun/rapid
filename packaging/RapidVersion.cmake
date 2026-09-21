# Single source of truth for the Debian package version.
# Release tags vX.Y.Z become X.Y.Z. Any other commit is
# 0.9.9~dev.<commit-count>+<short-sha>.
#
# The commit count is monotonic along the branch, so installing a newer commit
# is seen by dpkg/apt as an upgrade instead of a downgrade (#58); the short SHA
# keeps the build revision-traceable. The separator is "." rather than "+"
# before the count so every count-form dev version also sorts above the old
# SHA-only "0.9.9~dev+<sha>" packages (no --allow-downgrades needed on upgrade).
#
# Override with -DRAPID_PACKAGE_VERSION=... when the caller already knows the
# version (a v* tag checkout, a release workflow, or a source archive with no
# .git). When neither an explicit value nor usable git metadata is available
# this is a hard error (#46): a package that installs as "0.9.9~dev+unknown" is
# not revision-traceable and previously failed silently.
set(RAPID_VERSION_BASE "0.9.9")
if(NOT RAPID_PACKAGE_VERSION)
  execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}/.." rev-parse --short=7 HEAD
    OUTPUT_VARIABLE _rapid_git_sha
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _rapid_git_error
    RESULT_VARIABLE _rapid_git_result)
  if(NOT _rapid_git_result EQUAL 0 OR _rapid_git_sha STREQUAL "")
    string(STRIP "${_rapid_git_error}" _rapid_git_error)
    message(FATAL_ERROR
      "Cannot derive the raPId package version: 'git rev-parse' failed in "
      "'${CMAKE_CURRENT_LIST_DIR}/..' (${_rapid_git_error}). Pass "
      "-DRAPID_PACKAGE_VERSION=<version> explicitly (for example for a source "
      "archive, a checkout git refuses to trust, or a workflow that already "
      "knows the version) instead of building an untraceable "
      "'${RAPID_VERSION_BASE}~dev+unknown' package.")
  endif()
  execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}/.." describe --exact-match --tags HEAD
    OUTPUT_VARIABLE _rapid_git_tag
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_rapid_git_tag MATCHES "^v([0-9][0-9A-Za-z.+~-]*)$")
    set(RAPID_PACKAGE_VERSION "${CMAKE_MATCH_1}")
  else()
    execute_process(
      COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}/.." rev-list --count HEAD
      OUTPUT_VARIABLE _rapid_git_count
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_VARIABLE _rapid_count_error
      RESULT_VARIABLE _rapid_count_result)
    if(NOT _rapid_count_result EQUAL 0 OR NOT _rapid_git_count MATCHES "^[0-9]+$")
      string(STRIP "${_rapid_count_error}" _rapid_count_error)
      message(FATAL_ERROR
        "Cannot derive the raPId package version: 'git rev-list --count HEAD' "
        "failed in '${CMAKE_CURRENT_LIST_DIR}/..' (${_rapid_count_error}). Pass "
        "-DRAPID_PACKAGE_VERSION=<version> explicitly.")
    endif()
    set(RAPID_PACKAGE_VERSION
        "${RAPID_VERSION_BASE}~dev.${_rapid_git_count}+${_rapid_git_sha}")
  endif()
endif()
message(STATUS "raPId package version ${RAPID_PACKAGE_VERSION}")
