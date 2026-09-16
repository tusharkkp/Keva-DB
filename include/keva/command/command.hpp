// ==============================================================================
// keva/command/command.hpp
//
// Purpose:
//   Declares the command dispatch infrastructure — the table-driven router
//   that maps RESP2 command names (e.g., "SET", "GET") to their C++ handler
//   functions.
//
//   Design: Table-Driven Command Dispatch
//   Redis dispatches commands using a hash table lookup on the command name.
//   We replicate this with a std::unordered_map<string, CommandEntry> where
//   CommandEntry stores:
//   - The handler function (CommandHandler callback)
//   - The minimum and maximum arity (number of arguments)
//   - Flags (read-only vs. write, etc.) for future ACL / replication use
//
//   Why table-driven instead of a big if-else chain?
//   - O(1) lookup regardless of how many commands are registered.
//   - New commands are added by inserting one row in the registry table,
//     not by modifying a dispatch function.
//   - Arity validation is centralized: the dispatcher checks argument counts
//     before dispatching to any handler, so handlers never receive malformed
//     argument lists.
//
//   CommandContext bundles everything a handler needs:
//   - The parsed RESP2 command array (command name + arguments)
//   - The Connection (to write the response)
//   - The KevaDatabase (to read/write keys)
// ==============================================================================

#pragma once

#include "keva/protocol/resp_types.hpp"
#include "keva/net/connection.hpp"
#include "keva/core/db.hpp"
#include "keva/common/types.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <string_view>

namespace keva::command {

// --------------------------------------------------------------------------
// CommandContext — all inputs a handler needs to execute a command
// --------------------------------------------------------------------------
struct CommandContext {
    const protocol::RespValue&       cmd;    // The full parsed RESP array
    net::Connection&                 conn;   // For writing the response
    core::KevaDatabase&              db;     // The active database
};

// --------------------------------------------------------------------------
// CommandHandler — function signature for all command implementations
// --------------------------------------------------------------------------
using CommandHandler = std::function<void(CommandContext&)>;

// --------------------------------------------------------------------------
// CommandFlags — bitfield for command categorization
// --------------------------------------------------------------------------
enum CommandFlags : u32 {
    CMD_FLAG_NONE     = 0,
    CMD_FLAG_WRITE    = 1 << 0,  // Modifies data (excluded from read-only replicas)
    CMD_FLAG_READONLY = 1 << 1,  // Does not modify data
    CMD_FLAG_FAST     = 1 << 2,  // O(1) complexity
    CMD_FLAG_ADMIN    = 1 << 3,  // Administrative command (SAVE, FLUSHDB, etc.)
};

// --------------------------------------------------------------------------
// CommandEntry — one row in the command dispatch table
// --------------------------------------------------------------------------
struct CommandEntry {
    std::string    name;
    CommandHandler handler;
    int            arity_min;   // Minimum number of tokens in the command array
    int            arity_max;   // -1 means variadic (any number >= arity_min)
    u32            flags;
};

// --------------------------------------------------------------------------
// CommandRegistry — the global command dispatch table
// --------------------------------------------------------------------------
class CommandRegistry {
public:
    // Retrieve the singleton registry instance
    static CommandRegistry& instance();

    // Register a command entry
    void register_command(CommandEntry entry);

    // Look up a command by name (case-insensitive).
    // Returns nullptr if the command is not registered.
    [[nodiscard]] const CommandEntry* find(std::string_view name) const;

    // --------------------------------------------------------------------------
    // Main dispatch: parse a connection's read buffer, dispatch the command,
    // write the response, and consume the bytes.
    // --------------------------------------------------------------------------
    void dispatch(net::Connection& conn, core::KevaDatabase& db);

private:
    CommandRegistry() = default;
    std::unordered_map<std::string, CommandEntry> table_;
};

// --------------------------------------------------------------------------
// Global registration: called once at startup to populate the registry
// --------------------------------------------------------------------------
void register_all_commands();

} // namespace keva::command
