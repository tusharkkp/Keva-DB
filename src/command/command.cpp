// ==============================================================================
// src/command/command.cpp
//
// Purpose:
//   Implements the CommandRegistry singleton and the main dispatch() function —
//   the central router that bridges raw TCP bytes and RESP2 parsing to the
//   appropriate command handler.
//
//   dispatch() flow (called by TcpServer's RequestHandler for every readable fd):
//   1. Acquire the connection's read buffer as a string_view.
//   2. Call RespParser::parse() to attempt parsing one RESP2 command.
//   3. If Incomplete: return (wait for more bytes).
//   4. If Error: write a RESP error and close the connection.
//   5. If Complete: consume the parsed bytes from the connection buffer.
//   6. Validate: the parsed value must be an Array with at least one element.
//   7. Extract the command name (first array element, must be a BulkString).
//   8. Uppercase the command name (RESP2 commands are case-insensitive).
//   9. Look up the command in the registry table.
//   10. Validate arity (argument count).
//   11. Call the handler function.
//   12. Record the command in server statistics.
//   13. Loop back to step 1 (one read() may have delivered multiple commands
//       in the buffer — pipeline support).
// ==============================================================================

#include "keva/command/command.hpp"
#include "keva/command/handlers/string_cmd.hpp"
#include "keva/command/handlers/key_cmd.hpp"
#include "keva/command/handlers/server_cmd.hpp"
#include "keva/protocol/resp_parser.hpp"
#include "keva/protocol/resp_encoder.hpp"
#include "keva/core/server_context.hpp"
#include "keva/common/logger.hpp"

#include <algorithm>
#include <cctype>

namespace keva::command {

using protocol::RespParser;
using protocol::RespEncoder;
using protocol::ParseResult;

// --------------------------------------------------------------------------
// Singleton
// --------------------------------------------------------------------------
CommandRegistry& CommandRegistry::instance() {
    static CommandRegistry reg;
    return reg;
}

// --------------------------------------------------------------------------
// register_command
// --------------------------------------------------------------------------
void CommandRegistry::register_command(CommandEntry entry) {
    // Store with uppercase name for case-insensitive matching
    std::string upper_name = entry.name;
    std::transform(upper_name.begin(), upper_name.end(), upper_name.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    table_.emplace(std::move(upper_name), std::move(entry));
}

// --------------------------------------------------------------------------
// find — case-insensitive command lookup
// --------------------------------------------------------------------------
const CommandEntry* CommandRegistry::find(std::string_view name) const {
    std::string upper(name);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    auto it = table_.find(upper);
    return it != table_.end() ? &it->second : nullptr;
}

// --------------------------------------------------------------------------
// dispatch — parse commands from connection buffer and execute them
// --------------------------------------------------------------------------
void CommandRegistry::dispatch(net::Connection& conn, core::KevaDatabase& db) {
    static thread_local RespParser parser;

    // Pipeline loop: consume as many complete commands as are in the buffer
    while (true) {
        std::string_view buf = conn.read_view();
        if (buf.empty()) break;

        protocol::RespValue cmd_value;
        usize consumed = 0;

        ParseResult result = parser.parse(buf, cmd_value, consumed);

        if (result == ParseResult::Incomplete) {
            break;  // Wait for more data from the next read() event
        }

        if (result == ParseResult::Error) {
            RespEncoder::error(conn, "Protocol error: invalid RESP2 format");
            conn.set_state(net::ConnectionState::WriteOnly);
            break;
        }

        // Consume the parsed bytes from the connection read buffer
        conn.consume(consumed);

        // Validate: must be an Array
        if (!cmd_value.is_array() || cmd_value.elements.empty()) {
            RespEncoder::error(conn, "Protocol error: expected array");
            continue;
        }

        // Extract command name (first element must be a Bulk String)
        const auto& name_value = cmd_value.elements[0];
        if (!name_value.is_bulk_string()) {
            RespEncoder::error(conn, "Protocol error: command name must be bulk string");
            continue;
        }

        // Uppercase for case-insensitive dispatch
        std::string cmd_name = name_value.str;
        std::transform(cmd_name.begin(), cmd_name.end(), cmd_name.begin(),
                       [](unsigned char c) { return std::toupper(c); });

        // Look up the command in the registry
        const CommandEntry* entry = find(cmd_name);
        if (!entry) {
            RespEncoder::error(conn,
                std::string("ERR unknown command `") + cmd_name + "`");
            core::ServerContext::instance().record_command();
            continue;
        }

        // Arity check: cmd_value.elements includes the command name itself
        const int argc = static_cast<int>(cmd_value.elements.size());
        if (argc < entry->arity_min ||
            (entry->arity_max != -1 && argc > entry->arity_max)) {
            RespEncoder::error(conn,
                std::string("ERR wrong number of arguments for '") + cmd_name + "' command");
            core::ServerContext::instance().record_command();
            continue;
        }

        // Execute the command handler
        CommandContext ctx{cmd_value, conn, db};
        entry->handler(ctx);

        // Record statistics
        core::ServerContext::instance().record_command();

        // Mark writes for write commands
        if (entry->flags & CMD_FLAG_WRITE) {
            core::ServerContext::instance().dirty_keys.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// --------------------------------------------------------------------------
// register_all_commands — populate the registry at startup
// --------------------------------------------------------------------------
void register_all_commands() {
    auto& reg = CommandRegistry::instance();

    // String commands
    handlers::register_string_commands(reg);

    // Key commands
    handlers::register_key_commands(reg);

    // Server commands
    handlers::register_server_commands(reg);

    log::info("Command registry initialized");
}

} // namespace keva::command
