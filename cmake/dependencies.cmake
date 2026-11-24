# Dependencies through CPM, each pinned to an exact revision. For local development CPM honours CPM_<NAME>_SOURCE (the Sub0 dev machine points
# these at the checkouts under the GitHub folder); CPM prints "Using local source" when one is in effect.
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/get_cpm.cmake)

# Sub0Platform: files (append, truncate, sync, atomic replace, locks) for the I/O layer, and the test helper (testing/check.hpp). The pure layer
# links nothing from it (cmake/CheckLayers.cmake). Same pin as Sub0Canvas.
CPMAddPackage(NAME Sub0Platform
  GIT_REPOSITORY https://github.com/CraigHutchinson/Sub0Platform.git
  GIT_TAG 5feccdf226046cc790214bc30793832a1888ce4b
  SOURCE_SUBDIR cpp
  OPTIONS "SUB0PLATFORM_BUILD_TESTS OFF" "SUB0PLATFORM_INSTALL OFF")

# simdjson: the one JSON library family-wide. Private to the sources: no simdjson type reaches a public header.
CPMAddPackage(NAME simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG 40a686ab89dc5ee58ec10d0514dd5c99abbc472b   # 5.1.1, the pin Sub0Squad and Sub0Canvas use
  OPTIONS "SIMDJSON_DEVELOPER_MODE OFF")

# Version.cmake: git describe -> version fields and a generated header, refreshed on every configure. The wrapper keeps `describe` to v<major>.<minor>
# tags; the package is fetched with plain git and only then added, so FetchContent's own git calls do not go through the wrapper.
set(VERSION_OUT_DIR "${CMAKE_BINARY_DIR}/generated/sub0fabric")
set(VERSION_FALLBACK "0.0.0")
set(VERSION_H_FILENAME "Version.hpp")
set(VERSION_NAMESPACE "sub0fabric::version")
CPMAddPackage(NAME Version.cmake
  GIT_REPOSITORY https://github.com/BareCpper/Version.cmake.git
  GIT_TAG c9e67af0aec3588912b760b45dca3ff217a62f59
  DOWNLOAD_ONLY YES)
if(CMAKE_HOST_WIN32)
  set(GIT_EXECUTABLE "${CMAKE_CURRENT_LIST_DIR}/git-semver.cmd")
else()
  set(GIT_EXECUTABLE "${CMAKE_CURRENT_LIST_DIR}/git-semver.sh")
endif()
add_subdirectory(${Version.cmake_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_deps/version.cmake-build)
