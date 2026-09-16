# ==============================================================================
# Keva - High-Performance In-Memory Key-Value Database
# Root CMakeLists.txt
#
# Purpose:
#   Configures the top-level CMake build system for the Keva project.
#   - Enforces C++20 standard across all targets
#   - Enables strict compiler warnings (-Wall -Wextra -Wpedantic) to catch
#     memory safety and UB issues at compile time
#   - Provides two build profiles:
#       Debug:   -O0 + AddressSanitizer (ASan) + UndefinedBehaviorSanitizer (UBsan)
#                for development and memory safety verification
#       Release: -O3 + Link-Time Optimization (LTO) for production performance
#   - Wires together the three sub-build trees: src/, include/, tests/
# ==============================================================================

cmake_minimum_required(VERSION 3.20)

project(
    keva
    VERSION 0.1.0
    DESCRIPTION "A Redis-inspired high-performance in-memory key-value database"
    LANGUAGES CXX
)

# --------------------------------------------------------------------------
# Global C++ Standard
# --------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # Disable GNU extensions: enforce strict ISO C++20

# --------------------------------------------------------------------------
# Compiler Warning Flags (applied to all targets)
# --------------------------------------------------------------------------
add_compile_options(
    -Wall           # Enable most warning classes
    -Wextra         # Enable extra warnings beyond -Wall
    -Wpedantic      # Enforce strict ISO C++ compliance
    -Wshadow        # Warn when a local variable shadows an outer variable
    -Wno-unused-parameter  # Suppress unused parameter warnings in early dev stubs
)

# --------------------------------------------------------------------------
# Build Type Configuration
# --------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

message(STATUS "=======================================================")
message(STATUS " Keva Build Configuration")
message(STATUS " Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS " C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "=======================================================")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS " Sanitizers : AddressSanitizer + UBSan ENABLED")
    add_compile_options(
        -O0             # No optimization: accurate debug symbols
        -g3             # Maximum debug information
        -fsanitize=address          # ASan: detect heap/stack/global buffer overflows
        -fsanitize=undefined        # UBSan: detect signed overflow, null deref, etc.
        -fno-omit-frame-pointer     # Preserve frame pointers for clean stack traces
    )
    add_link_options(
        -fsanitize=address
        -fsanitize=undefined
    )
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS " Optimizations: -O3 + LTO ENABLED")
    add_compile_options(
        -O3             # Maximum compiler optimization
        -DNDEBUG        # Disable debug assertions in release builds
    )
    # Enable Link-Time Optimization for cross-translation-unit inlining
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# --------------------------------------------------------------------------
# Include Paths
# --------------------------------------------------------------------------
# The include/ directory is the public API surface for all Keva headers.
# All targets include via: #include "keva/module/file.hpp"
include_directories(${PROJECT_SOURCE_DIR}/include)

# --------------------------------------------------------------------------
# Sub-Trees
# --------------------------------------------------------------------------
add_subdirectory(src)
enable_testing()          # Must be in root CMakeLists for CTest to discover tests
add_subdirectory(tests)
