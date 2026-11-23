cmake_minimum_required(VERSION 3.20)
project(ultralob LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ULTRALOB_BUILD_TESTS "Build the GoogleTest suites" ON)
option(ULTRALOB_BUILD_BENCHMARKS "Build the native benchmark harness" ON)

set(ULTRALOB_ENGINE_SOURCES
    engine/src/book_image.cpp
    engine/src/chaos.cpp
    engine/src/event.cpp
    engine/src/event_processor.cpp
    engine/src/feed_generator.cpp
    engine/src/matching_engine.cpp
    engine/src/order_book.cpp
    engine/src/order_index.cpp
    engine/src/price_level.cpp
    engine/src/replay.cpp
    engine/src/snapshot.cpp)

find_package(Threads REQUIRED)

# --- WebAssembly build (configure with `emcmake cmake ...`) -----------------
if(EMSCRIPTEN)
  add_executable(ultralob_wasm wasm/bindings.cpp ${ULTRALOB_ENGINE_SOURCES})
  target_include_directories(ultralob_wasm PRIVATE engine/include)
  target_link_options(ultralob_wasm PRIVATE
      --no-entry
      -sMODULARIZE=1
      -sEXPORT_ES6=1
      -sENVIRONMENT=web,worker
      -sALLOW_MEMORY_GROWTH=0
      -sINITIAL_MEMORY=134217728
      # The chaos scenarios use stack buffers; the default 64 KiB WASM stack
      # overflows and silently corrupts static data. Native has an 8 MiB stack.
      -sTOTAL_STACK=2097152
      -sSTACK_OVERFLOW_CHECK=2
      -sFILESYSTEM=0
      -sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_SOURCE_DIR}/wasm/exports.json
      -sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString,HEAPU8,HEAPU32,HEAPU64,HEAPF64)
  set_target_properties(ultralob_wasm PROPERTIES OUTPUT_NAME ultralob SUFFIX ".mjs")
  return()
endif()

add_library(ultralob_engine STATIC ${ULTRALOB_ENGINE_SOURCES})
target_include_directories(ultralob_engine PUBLIC engine/include)
target_compile_options(ultralob_engine PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ultralob_engine PUBLIC Threads::Threads)

# --- CLI -------------------------------------------------------------------
add_executable(ultralob_cli tools/ultralob_cli.cpp)
target_link_libraries(ultralob_cli PRIVATE ultralob_engine)

# --- Benchmarks ------------------------------------------------------------
if(ULTRALOB_BUILD_BENCHMARKS)
  add_executable(ultralob_bench benchmarks/native_benchmark.cpp benchmarks/allocation_tracker.cpp)
  target_link_libraries(ultralob_bench PRIVATE ultralob_engine)

  # Baseline vs optimized A/B harness (same workloads, same correctness gate).
  add_executable(ultralob_compare benchmarks/compare_benchmarks.cpp benchmarks/allocation_tracker.cpp)
  target_include_directories(ultralob_compare PRIVATE benchmarks)
  target_link_libraries(ultralob_compare PRIVATE ultralob_engine)
endif()

# --- Tests -----------------------------------------------------------------
if(ULTRALOB_BUILD_TESTS)
  include(FetchContent)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(ultralob_tests
      tests/alloc_counter.cpp
      tests/backpressure_test.cpp
      tests/chaos_test.cpp
      tests/determinism_test.cpp
      tests/matching_test.cpp
      tests/memory_pool_test.cpp
      tests/order_book_limits_test.cpp
      tests/order_index_test.cpp
      tests/order_book_test.cpp
      tests/sequence_gap_test.cpp
      tests/wasm_bindings_test.cpp
      wasm/bindings.cpp)
  target_include_directories(ultralob_tests PRIVATE tests)
  target_link_libraries(ultralob_tests PRIVATE ultralob_engine GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(ultralob_tests)
endif()
