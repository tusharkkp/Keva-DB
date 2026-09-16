// ==============================================================================
// keva/command/handlers/server_cmd.hpp
// Purpose: Declares server administration commands (PING, ECHO, FLUSHDB, INFO, SAVE, BGSAVE, SELECT)
// ==============================================================================
#pragma once
#include "keva/command/command.hpp"
namespace keva::command::handlers {
void register_server_commands(CommandRegistry& reg);
void handle_ping   (CommandContext& ctx);
void handle_echo   (CommandContext& ctx);
void handle_flushdb(CommandContext& ctx);
void handle_info   (CommandContext& ctx);
void handle_save   (CommandContext& ctx);
void handle_bgsave (CommandContext& ctx);
void handle_select (CommandContext& ctx);
void handle_quit   (CommandContext& ctx);
} // namespace keva::command::handlers
