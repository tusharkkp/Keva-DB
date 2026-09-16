# ==============================================================================
# Keva - src/CMakeLists.txt
#
# Purpose:
#   Collects all Keva C++ source files from every subsystem module and builds
#   them into a single static library target: 'keva_core'.
#
#   The static library separates the database engine from the entry point
#   (main.cpp), which is important for:
#   - Unit testing: tests can link against keva_core without a main() conflict.
#   - Docker builds: the library is compiled once in a dedicated build stage.
#
#   Additionally, this file defines the 'keva-server' executable target that
#   links main.cpp against keva_core to produce the runnable database server.
# ==============================================================================

# --------------------------------------------------------------------------
# keva_core Static Library
# Aggregates all subsystem implementations into one linkable archive.
# --------------------------------------------------------------------------
add_library(keva_core STATIC
    # Common utilities
    common/logger.cpp

    # Networking & Event Loop (Phase 1 Milestones 1.2 - 1.4)
    net/socket.cpp
    net/connection.cpp
    net/event_loop.cpp
    net/tcp_server.cpp

    # RESP2 Protocol Engine (Phase 2)
    protocol/resp_parser.cpp
    protocol/resp_encoder.cpp

    # Core Storage Engine (Phase 3)
    core/buffer.cpp
    core/object.cpp
    core/dict.cpp
    core/db.cpp
    core/server_context.cpp

    # Command Dispatcher & Handlers (Phase 4)
    command/command.cpp
    command/handlers/string_cmd.cpp
    command/handlers/key_cmd.cpp
    command/handlers/server_cmd.cpp

    # RDB Persistence Engine (Phase 5)
    persistence/rdb_save.cpp
    persistence/rdb_load.cpp
)

# keva_core can see all public headers via the project include/ directory
target_include_directories(keva_core PUBLIC ${PROJECT_SOURCE_DIR}/include)

# --------------------------------------------------------------------------
# keva-server Executable
# The main database server binary: links main.cpp against the keva_core lib.
# --------------------------------------------------------------------------
add_executable(keva-server main.cpp)
target_link_libraries(keva-server PRIVATE keva_core)
