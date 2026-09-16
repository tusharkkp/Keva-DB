# ==============================================================================
# Keva - tests/CMakeLists.txt
#
# Purpose:
#   Configures the Keva unit test suite using the Catch2 header-only testing
#   framework (fetched automatically via CMake FetchContent — the one allowed
#   external dependency under our Pragmatic dependency philosophy).
#
#   All test executables link against 'keva_core' (the static library built
#   in src/CMakeLists.txt) to test internal subsystems directly without
#   spinning up a full server process.
#
#   Test targets:
#   - keva_test_dict   : Tests for the incremental hash table (keva::core::Dict)
#   - keva_test_buffer : Tests for the binary-safe dynamic buffer
#   - keva_test_resp   : Tests for the RESP2 streaming parser FSM
# ==============================================================================

include(FetchContent)

# --------------------------------------------------------------------------
# Fetch Catch2 (Header-Only Testing Framework)
# This is the only external dependency in Keva under our pragmatic philosophy.
# Catch2 v3 is header-only for single-file usage and has zero transitive deps.
# --------------------------------------------------------------------------
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2
)
FetchContent_MakeAvailable(Catch2)

# --------------------------------------------------------------------------
# Enable CTest integration
# --------------------------------------------------------------------------
enable_testing()

# --------------------------------------------------------------------------
# Helper macro: creates a test executable and registers it with CTest
# --------------------------------------------------------------------------
macro(add_keva_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME} PRIVATE keva_core Catch2::Catch2WithMain)
    target_include_directories(${TEST_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endmacro()

# --------------------------------------------------------------------------
# Unit Test Targets
# --------------------------------------------------------------------------
add_keva_test(keva_test_dict   unit/test_dict.cpp)
add_keva_test(keva_test_buffer unit/test_buffer.cpp)
add_keva_test(keva_test_resp   unit/test_resp.cpp)
