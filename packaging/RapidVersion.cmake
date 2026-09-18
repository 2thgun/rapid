# Single source of truth for the Debian package version.
# Release tags vX.Y.Z become X.Y.Z. Any other commit is 0.9.9~dev+<shortsha>.
# Override with -DRAPID_PACKAGE_VERSION=... when a workflow already knows the
# version (a v* tag checkout).
set(RAPID_VERSION_BASE "0.9.9")
if(NOT RAPID_PACKAGE_VERSION)
  set(_rapid_git_sha "unknown")
  execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}/.." rev-parse --short=7 HEAD
    OUTPUT_VARIABLE _rapid_git_sha
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_rapid_git_sha STREQUAL "")
    set(_rapid_git_sha "unknown")
  endif()
  set(_rapid_git_tag "")
  execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}/.." describe --exact-match --tags HEAD
    OUTPUT_VARIABLE _rapid_git_tag
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_rapid_git_tag MATCHES "^v([0-9][0-9A-Za-z.+~-]*)$")
    set(RAPID_PACKAGE_VERSION "${CMAKE_MATCH_1}")
  else()
    set(RAPID_PACKAGE_VERSION "${RAPID_VERSION_BASE}~dev+${_rapid_git_sha}")
  endif()
endif()
message(STATUS "raPId package version ${RAPID_PACKAGE_VERSION}")
