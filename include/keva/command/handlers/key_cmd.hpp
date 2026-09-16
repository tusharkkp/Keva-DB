// ==============================================================================
// keva/command/handlers/key_cmd.hpp
// Purpose: Declares key-space commands (DEL, EXISTS, TYPE, EXPIRE, TTL, KEYS, RENAME)
// ==============================================================================
#pragma once
#include "keva/command/command.hpp"
namespace keva::command::handlers {
void register_key_commands(CommandRegistry& reg);
void handle_del    (CommandContext& ctx);
void handle_exists (CommandContext& ctx);
void handle_type   (CommandContext& ctx);
void handle_expire (CommandContext& ctx);
void handle_pexpire(CommandContext& ctx);
void handle_expireat(CommandContext& ctx);
void handle_ttl    (CommandContext& ctx);
void handle_pttl   (CommandContext& ctx);
void handle_persist(CommandContext& ctx);
void handle_keys   (CommandContext& ctx);
void handle_rename (CommandContext& ctx);
void handle_dbsize (CommandContext& ctx);
} // namespace keva::command::handlers
