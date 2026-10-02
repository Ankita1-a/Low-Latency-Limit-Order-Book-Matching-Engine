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