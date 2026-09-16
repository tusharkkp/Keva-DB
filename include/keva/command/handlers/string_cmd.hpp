// ==============================================================================
// keva/command/handlers/string_cmd.hpp
//
// Purpose:
//   Declares the handler functions for Redis-compatible String commands.
//   String commands operate on keys whose value is a KevaObject of type String
//   (ObjectType::String), stored in any of our three encodings:
//   ENCODING_INT, ENCODING_EMBSTR, or ENCODING_RAW.
//
//   Commands implemented in Phase 1:
//   - SET key value [EX seconds] [PX ms] [NX|XX]
//   - GET key
//   - GETSET key value
//   - MSET key1 val1 key2 val2 ...
//   - MGET key1 key2 ...
//   - APPEND key value
//   - STRLEN key
//   - INCR key
//   - INCRBY key increment
//   - DECR key
//   - DECRBY key decrement
//   - SETEX key seconds value
//   - PSETEX key milliseconds value
//   - SETNX key value
// ==============================================================================

#pragma once

#include "keva/command/command.hpp"

namespace keva::command::handlers {

// Register all string commands into the provided registry
void register_string_commands(CommandRegistry& reg);

// Individual handler functions
void handle_set    (CommandContext& ctx);
void handle_get    (CommandContext& ctx);
void handle_getset (CommandContext& ctx);
void handle_mset   (CommandContext& ctx);
void handle_mget   (CommandContext& ctx);
void handle_append (CommandContext& ctx);
void handle_strlen (CommandContext& ctx);
void handle_incr   (CommandContext& ctx);
void handle_incrby (CommandContext& ctx);
void handle_decr   (CommandContext& ctx);
void handle_decrby (CommandContext& ctx);
void handle_setex  (CommandContext& ctx);
void handle_psetex (CommandContext& ctx);
void handle_setnx  (CommandContext& ctx);

} // namespace keva::command::handlers
