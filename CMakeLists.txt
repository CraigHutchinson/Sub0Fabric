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
target_link_libraries(sub0fabric_io INTERFACE Sub0Platform::sub0platform)   # becomes PUBLIC once the layer has sources

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
  include(cmake/Tests.cmake OPTIONAL)   # the test list, one place: area and tier per test
  sub0fabric_check_labels()
endif()
