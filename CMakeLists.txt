# Oldest CMake version this file works with.
cmake_minimum_required(VERSION 3.24)

# Project name and language (CXX means C++).
project(lob_engine LANGUAGES CXX)

# Use standard C++20, and fail if the compiler can't do it.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)        # plain C++20, no compiler-specific dialect

# Write compile_commands.json so the clangd extension understands our code.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to an optimized build. Benchmarks are meaningless without optimization.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# Keep debug information (-g) in optimized builds so profilers can show
# function names and source lines. It doesn't change the generated code.
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g")


# A reusable bundle of compiler warnings. Every program we build will link to it.
add_library(lob_warnings INTERFACE)
target_compile_options(lob_warnings INTERFACE -Wall -Wextra -Wpedantic)

# ---- Programs ----

# Machine and timer probe.
add_executable(env_probe bench/env_probe.cpp)
target_link_libraries(env_probe PRIVATE lob_warnings)

# ---- The order book library ----
# Header-only, so it's an INTERFACE library: nothing to compile on its own,
# but anything that links to it gets the include/ folder on its search path.
add_library(lob INTERFACE)
target_include_directories(lob INTERFACE include)

# Sanity check for types.hpp
add_executable(check_types tests/check_types.cpp)
target_link_libraries(check_types PRIVATE lob lob_warnings)

# Sanity check for events.hpp
add_executable(check_events tests/check_events.cpp)
target_link_libraries(check_events PRIVATE lob lob_warnings)

# ---- The reference (naive) book ----
add_library(lob_baseline INTERFACE)
target_include_directories(lob_baseline INTERFACE baseline)
target_link_libraries(lob_baseline INTERFACE lob)

# Walkthrough of the naive book
add_executable(check_naive_book tests/check_naive_book.cpp)
target_link_libraries(check_naive_book PRIVATE lob_baseline lob_warnings)

# ---- Unit tests ----

# doctest: single header in third_party/.
# SYSTEM = don't show warnings from inside a library we didn't write.
# DOCTEST_CONFIG_USE_STD_HEADERS = use the real standard headers, which
# avoids compatibility problems with newer compilers.
add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE third_party/doctest)
target_compile_definitions(doctest INTERFACE DOCTEST_CONFIG_USE_STD_HEADERS)

add_executable(unit_tests tests/test_main.cpp tests/test_book.cpp)
target_link_libraries(unit_tests PRIVATE lob_baseline doctest lob_warnings)

# Register with CTest, CMake's test runner.
enable_testing()
add_test(NAME unit_tests COMMAND unit_tests)

# ---- Randomized differential fuzzer ----
add_executable(fuzz_differential tests/fuzz_differential.cpp)
target_link_libraries(fuzz_differential PRIVATE lob_baseline lob_warnings)

# A quick run as part of the test suite: seeds 1..50, 2000 ops each.
add_test(NAME fuzz_quick COMMAND fuzz_differential 1 50 2000)

# ---- Benchmarks ----
add_executable(bench_book bench/bench_main.cpp)
# tests/ holds random_ops.hpp (Op and Rng are shared with the fuzzer)
target_include_directories(bench_book PRIVATE tests)
target_link_libraries(bench_book PRIVATE lob_baseline lob_warnings)