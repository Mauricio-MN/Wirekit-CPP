cmake_minimum_required(VERSION 3.16)
project(wirekit VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(WIREKIT_IS_TOP_LEVEL ON)
else()
  set(WIREKIT_IS_TOP_LEVEL OFF)
endif()

if(WIREKIT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Tests, benchmarks and examples are only built when Wirekit is the top-level project.
option(WIREKIT_BUILD_TESTS "Build the tests" ${WIREKIT_IS_TOP_LEVEL})
option(WIREKIT_BUILD_BENCHMARKS "Build the benchmarks" ${WIREKIT_IS_TOP_LEVEL})
option(WIREKIT_BUILD_EXAMPLES "Build the examples" ${WIREKIT_IS_TOP_LEVEL})
option(WIREKIT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(WIREKIT_IS_TOP_LEVEL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

find_package(Threads REQUIRED)

# Header-only core with no threading decision attached.
add_library(wirekit_headers INTERFACE)
target_include_directories(wirekit_headers INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>)
target_compile_features(wirekit_headers INTERFACE cxx_std_17)

# Default (multithreaded): the registry mutex and the collector thread need Threads.
add_library(wirekit INTERFACE)
add_library(wirekit::wirekit ALIAS wirekit)
target_link_libraries(wirekit INTERFACE wirekit_headers Threads::Threads)

# Single-threaded: every consumer gets the macro, so configurations cannot be mixed by accident.
add_library(wirekit_single_thread INTERFACE)
add_library(wirekit::single_thread ALIAS wirekit_single_thread)
target_link_libraries(wirekit_single_thread INTERFACE wirekit_headers)
target_compile_definitions(wirekit_single_thread INTERFACE WIREKIT_SINGLE_THREAD)

# Warnings and sanitizers for this repository's own targets only.
add_library(wirekit_dev_options INTERFACE)
if(MSVC)
  target_compile_options(wirekit_dev_options INTERFACE /W4)
else()
  target_compile_options(wirekit_dev_options INTERFACE -Wall -Wextra -Wpedantic)
  if(WIREKIT_SANITIZE)
    target_compile_options(wirekit_dev_options INTERFACE
      -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(wirekit_dev_options INTERFACE -fsanitize=address,undefined)
  endif()
endif()

# The SINGLE_THREAD sources define WIREKIT_SINGLE_THREAD themselves and must not link Threads.
function(wirekit_executable name)
  cmake_parse_arguments(ARG "SINGLE_THREAD;KEEP_ASSERTS" "" "SOURCES;DEFINITIONS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  if(ARG_SINGLE_THREAD)
    target_link_libraries(${name} PRIVATE wirekit_headers wirekit_dev_options)
  else()
    target_link_libraries(${name} PRIVATE wirekit wirekit_dev_options)
  endif()
  if(ARG_DEFINITIONS)
    target_compile_definitions(${name} PRIVATE ${ARG_DEFINITIONS})
  endif()
  if(ARG_KEEP_ASSERTS)
    # Release adds -DNDEBUG, which would turn every assert in the tests into a no-op.
    if(MSVC)
      target_compile_options(${name} PRIVATE /UNDEBUG)
    else()
      target_compile_options(${name} PRIVATE -UNDEBUG)
    endif()
  endif()
endfunction()

if(WIREKIT_BUILD_TESTS)
  enable_testing()

  wirekit_executable(hub_test SOURCES src/tests/hub_test.cpp KEEP_ASSERTS)
  wirekit_executable(container_test SOURCES src/tests/container_test.cpp KEEP_ASSERTS)
  wirekit_executable(hub_test_st SOURCES src/tests/hub_test_st.cpp SINGLE_THREAD KEEP_ASSERTS)
  wirekit_executable(container_test_st SOURCES src/tests/container_test_st.cpp SINGLE_THREAD KEEP_ASSERTS)

  foreach(test_name hub_test container_test hub_test_st container_test_st)
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 120 LABELS "test")
  endforeach()
endif()

if(WIREKIT_BUILD_EXAMPLES)
  set(HUB_EXAMPLE_DIR src/examples/hub)
  wirekit_executable(hub_example SOURCES
    ${HUB_EXAMPLE_DIR}/hub_example.cpp
    ${HUB_EXAMPLE_DIR}/user_service.cpp
    ${HUB_EXAMPLE_DIR}/user_staff.cpp
    ${HUB_EXAMPLE_DIR}/user_normal.cpp)
  wirekit_executable(registration_modes SOURCES ${HUB_EXAMPLE_DIR}/registration_modes.cpp)
  wirekit_executable(di_example SOURCES src/examples/container/di_example.cpp)

  add_custom_target(run_examples
    COMMAND hub_example
    COMMAND registration_modes
    COMMAND di_example
    USES_TERMINAL)

  if(WIREKIT_BUILD_TESTS)
    foreach(example_name hub_example registration_modes di_example)
      add_test(NAME ${example_name} COMMAND ${example_name})
      set_tests_properties(${example_name} PROPERTIES TIMEOUT 60 LABELS "example")
    endforeach()
  endif()
endif()

if(WIREKIT_BUILD_BENCHMARKS)
  wirekit_executable(dispatch_benchmark SOURCES src/benchmarks/dispatch_benchmark.cpp)
  wirekit_executable(dispatch_benchmark_st
    SOURCES src/benchmarks/dispatch_benchmark.cpp
    DEFINITIONS WIREKIT_SINGLE_THREAD
    SINGLE_THREAD)
  wirekit_executable(collector_benchmark SOURCES src/benchmarks/collector_benchmark.cpp)

  # collector_benchmark takes the collection interval in ms: frequent vs practically idle.
  add_custom_target(run_benchmarks
    COMMAND dispatch_benchmark
    COMMAND dispatch_benchmark_st
    COMMAND collector_benchmark 1
    COMMAND collector_benchmark 2147483647
    USES_TERMINAL)
endif()
