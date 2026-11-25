cmake_minimum_required(VERSION 3.24)
# Sub0Fabric: the family's append-only, per-writer, crash-safe record log with a streaming merging reader (docs/DESIGN.md). Consumers fetch it through
# CPM (CPMAddPackage NAME Sub0Fabric) and link sub0fabric::pure (the codecs, order, cursor and segment format) or sub0fabric::io (the directory store).
project(Sub0Fabric VERSION 0.0.0 LANGUAGES CXX)

set(SUB0FABRIC_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(SUB0FABRIC_TOP_LEVEL ON)
endif()
option(SUB0FABRIC_BUILD_TESTING "Build Sub0Fabric's tests" ${SUB0FABRIC_TOP_LEVEL})
option(SUB0FABRIC_BUILD_BENCHMARKS "Build the budget benchmarks (bench/): Sub0Fabric_Bench" ${SUB0FABRIC_TOP_LEVEL})

if(SUB0FABRIC_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

include(cmake/dependencies.cmake)
include(cmake/Layers.cmake)

if(MSVC)
  set(SUB0FABRIC_STRICT /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
else()
  set(SUB0FABRIC_STRICT -Wall -Wextra -Werror)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # As in Sub0Squad and Sub0Canvas: GCC's flow-based diagnostics fire inside simdjson after inlining; keep them visible but not fatal.
    list(APPEND SUB0FABRIC_STRICT -Wno-error=array-bounds -Wno-error=format-truncation)
  endif()
endif()

# One static library per layer (cmake/Layers.cmake), each linking the layer below it, so an upward call is an unresolved symbol exactly as an upward
# include fails the layering test.
set(lower "")
foreach(layer ${SUB0FABRIC_LAYERS})
  file(GLOB layerSources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${layer}/*.cpp)
  if(layerSources)
    add_library(sub0fabric_${layer} STATIC ${layerSources})
    target_compile_features(sub0fabric_${layer} PUBLIC cxx_std_23)
    target_include_directories(sub0fabric_${layer} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/${layer})
    target_compile_options(sub0fabric_${layer} PRIVATE ${SUB0FABRIC_STRICT})
    if(lower)
      target_link_libraries(sub0fabric_${layer} PUBLIC ${lower})
    endif()
  else()   # a layer with no sources yet is an interface library
    add_library(sub0fabric_${layer} INTERFACE)
    target_include_directories(sub0fabric_${layer} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(lower)
      target_link_libraries(sub0fabric_${layer} INTERFACE ${lower})
    endif()
  endif()
  add_library(sub0fabric::${layer} ALIAS sub0fabric_${layer})
  set(lower sub0fabric_${layer})
endforeach()
target_link_libraries(sub0fabric_pure PRIVATE simdjson::simdjson version::version)
target_link_libraries(sub0fabric_io PUBLIC Sub0Platform::sub0platform)

# The budget benchmarks (bench/bench_fabric.cpp) run through Sub0Perf's tooling (docs/DESIGN.md, "Budgets"): Sub0Fabric_Bench, plus the _json, _runs, _check,
# _baseline, _compare and _metrics targets. Timing is measured locally only (never in CI); the deterministic metrics (perf/metrics.json) and the smoke run are ctest
# tests labelled smoke, so they run in every tier. Sub0Perf is the family's performance tooling, moved out of Sub0Math (ADR 0004); DOWNLOAD_ONLY.
if(SUB0FABRIC_BUILD_BENCHMARKS)
  if(SUB0FABRIC_BUILD_TESTING)
    enable_testing()   # before the first add_test below (the testing block calls it again, harmlessly)
  endif()
  CPMAddPackage(NAME Sub0Perf
    GIT_REPOSITORY https://github.com/CraigHutchinson/Sub0Perf.git
    GIT_TAG 09ddbd42f509f45395718bead3e8305c95dc8ea9
    DOWNLOAD_ONLY YES)
  # No benchmark run after every build (run the targets deliberately). The pre-push gate follows Sub0Perf's default (SUB0PERF_GIT_HOOKS=AUTO): installed
  # only from the main checkout, where this machine has a committed baseline (perf/baselines/<host>.json; SUB0PERF_HOST names another host), never on CI.
  set(SUB0PERF_ON_BUILD "OFF" CACHE STRING "")
  option(SUB0FABRIC_PERF_GATE "Install the pre-push performance gate where this machine has a committed baseline" ON)
  if(NOT SUB0FABRIC_PERF_GATE)
    set(SUB0PERF_GIT_HOOKS OFF)
  endif()
  include("${Sub0Perf_SOURCE_DIR}/cmake/Sub0Perf.cmake")

  sub0perf_add_benchmark(Sub0Fabric_Bench bench/bench_fabric.cpp METRICS perf/metrics.json SMOKE_TEST)
  target_compile_options(Sub0Fabric_Bench PRIVATE ${SUB0FABRIC_STRICT})
  target_link_libraries(Sub0Fabric_Bench PRIVATE sub0fabric_io)
  if(MINGW)
    # The bench replaces operator new to count allocations and live bytes; MinGW's libstdc++ DLL would allocate strings with its own operator new and the
    # executable free them with ours (heap corruption, 0xC0000374). Linking the runtime statically keeps one allocator.
    target_link_options(Sub0Fabric_Bench PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  # The timing run as a test, for `ctest --preset timing` (local only: label timing, which the quick and full presets exclude): interleaved rounds, then the
  # judgement against perf/baselines/<host>.json. `_smoke` and `_metrics` (label smoke) are registered by sub0perf_add_benchmark.
  if(SUB0FABRIC_BUILD_TESTING AND TARGET Sub0Fabric_Bench_check)
    add_test(NAME Sub0Fabric_Bench_timing COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target Sub0Fabric_Bench_check)
    set_tests_properties(Sub0Fabric_Bench_timing PROPERTIES LABELS timing TIMEOUT 3600 RUN_SERIAL TRUE)
  endif()
endif()

if(SUB0FABRIC_BUILD_TESTING)
  enable_testing()
  add_test(NAME layering COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/CheckLayers.cmake)
  add_test(NAME test_reporting COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/CheckTests.cmake)

  function(sub0fabric_add_test name layer)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE sub0fabric_${layer} Sub0Platform::sub0platform)
    target_include_directories(${name} PRIVATE tests)
    target_compile_options(${name} PRIVATE ${SUB0FABRIC_STRICT})
    target_compile_definitions(${name} PRIVATE SUB0FABRIC_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  include(cmake/TestTiers.cmake)
  sub0fabric_label_tests(AREA structure TIER smoke TARGETS layering test_reporting)
  sub0fabric_label_tests(AREA bench TIER smoke TARGETS Sub0Fabric_Bench_smoke Sub0Fabric_Bench_metrics)
  include(cmake/Tests.cmake OPTIONAL)   # the test list, one place: area and tier per test
  sub0fabric_check_labels()
endif()
