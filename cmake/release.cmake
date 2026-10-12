# Cut a release: set the version in CMakeLists.txt, commit, push main, then
# create and push the annotated tag vX.Y.Z, which starts the GitHub release
# workflow (.github/workflows/release.yml).
#
# Through the build directory's "release" target:
#   RELEASE_VERSION=1.2.0 RELEASE_NOTES="Short description" \
#     cmake --build build --target release
#
# Or directly, without a build directory:
#   cmake -DRELEASE_VERSION=1.2.0 -DRELEASE_NOTES="Short description" \
#     -P cmake/release.cmake
#
# Set RELEASE_DRY_RUN=1 to check everything and show what would be done
# without changing or pushing anything.
cmake_minimum_required(VERSION 3.24)

# Inputs: -D variables win over environment variables.
foreach(var RELEASE_VERSION RELEASE_NOTES RELEASE_DRY_RUN)
  if(NOT DEFINED ${var} AND DEFINED ENV{${var}})
    set(${var} "$ENV{${var}}")
  endif()
endforeach()
if(NOT SOURCE_DIR)
  get_filename_component(SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

if(NOT RELEASE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR
    "Set RELEASE_VERSION to the new version as X.Y.Z (got \"${RELEASE_VERSION}\"), e.g.\n"
    "  RELEASE_VERSION=1.2.0 RELEASE_NOTES=\"Short description\" "
    "cmake --build build --target release")
endif()
string(STRIP "${RELEASE_NOTES}" RELEASE_NOTES)
if(RELEASE_NOTES STREQUAL "")
  message(FATAL_ERROR "Set RELEASE_NOTES to a short description of the release.")
endif()
set(tag "v${RELEASE_VERSION}")

find_package(Git REQUIRED QUIET)

# Run git in the source directory; stop with its output if it fails.
function(git out)
  execute_process(COMMAND "${GIT_EXECUTABLE}" ${ARGN}
                  WORKING_DIRECTORY "${SOURCE_DIR}"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT rc EQUAL 0)
    string(REPLACE ";" " " cmd "${ARGN}")
    message(FATAL_ERROR "git ${cmd} failed:\n${stderr}${stdout}")
  endif()
  set(${out} "${stdout}" PARENT_SCOPE)
endfunction()

# --- Checks; nothing is changed until all of them pass -------------------

git(status status --porcelain --untracked-files=no)
if(NOT status STREQUAL "")
  message(FATAL_ERROR
    "The working tree has uncommitted changes; commit or stash them first:\n${status}")
endif()

git(branch rev-parse --abbrev-ref HEAD)
if(NOT branch STREQUAL "main")
  message(FATAL_ERROR "Releases are cut from main, but you're on \"${branch}\".")
endif()

git(dummy fetch --quiet origin main)
git(behind rev-list --count HEAD..origin/main)
if(NOT behind STREQUAL "0")
  message(FATAL_ERROR
    "main is ${behind} commit(s) behind origin/main; run `git pull` first.")
endif()

git(local_tag tag --list "${tag}")
git(remote_tag ls-remote --tags origin "refs/tags/${tag}")
if(NOT local_tag STREQUAL "" OR NOT remote_tag STREQUAL "")
  message(FATAL_ERROR
    "Tag ${tag} already exists. Pick a new version, or delete the old tag "
    "first if it was never released.")
endif()

set(cmakelists "${SOURCE_DIR}/CMakeLists.txt")
file(READ "${cmakelists}" contents)
if(NOT contents MATCHES "project\\(imgview VERSION ([0-9.]+)")
  message(FATAL_ERROR "Couldn't find `project(imgview VERSION ...)` in ${cmakelists}")
endif()
set(old_version "${CMAKE_MATCH_1}")
if(NOT RELEASE_VERSION VERSION_GREATER old_version)
  message(FATAL_ERROR
    "The new version must be higher than the current ${old_version} "
    "(got ${RELEASE_VERSION}).")
endif()

message(STATUS "Releasing imgview ${old_version} -> ${RELEASE_VERSION}")
message(STATUS "  commit:  \"Release ${RELEASE_VERSION}: ${RELEASE_NOTES}\" on main")
message(STATUS "  tag:     ${tag} \"${RELEASE_NOTES}\"")
if(RELEASE_DRY_RUN)
  message(STATUS "Dry run: nothing changed or pushed.")
  return()
endif()

# --- Release ----------------------------------------------------------------

string(REGEX REPLACE "project\\(imgview VERSION [0-9.]+"
       "project(imgview VERSION ${RELEASE_VERSION}" contents "${contents}")
file(WRITE "${cmakelists}" "${contents}")

git(dummy commit --quiet -m "Release ${RELEASE_VERSION}: ${RELEASE_NOTES}" -- CMakeLists.txt)
git(dummy push --quiet origin main)
git(dummy tag -a "${tag}" -m "${RELEASE_NOTES}")
git(dummy push --quiet origin "${tag}")
git(url remote get-url origin)

message(STATUS "Pushed main and ${tag}. The release workflow is now building it;")
message(STATUS "follow it on the repository's Actions tab (${url}).")
