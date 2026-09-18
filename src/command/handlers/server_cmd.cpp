// ==============================================================================
// src/command/handlers/server_cmd.cpp
//
// Purpose:
//   Implements server administration commands: PING, ECHO, FLUSHDB, INFO,
//   SAVE (synchronous RDB), BGSAVE (async fork-based RDB), SELECT, and QUIT.
//
//   INFO command:
//   Redis's INFO command returns a rich multi-section text blob that monitoring
//   tools (Redis Insight, Prometheus exporters, redis-cli INFO) parse to display
//   server health metrics. We implement the key sections:
//   - server:  version, uptime, port, hz
//   - clients: connected_clients
//   - memory:  (placeholder — full memory tracking in Phase 4)
//   - stats:   total_commands_processed, hits, misses, expired_keys
//   - keyspace: db0:keys=N,expires=M
//
//   BGSAVE:
//   Triggers the RDB persistence engine (implemented in Phase 5 in rdb_save.cpp).
//   In Phase 1, returns "+Background saving started" immediately. The actual
//   fork() will be wired in Phase 5.
// ==============================================================================

#include "keva/command/handlers/server_cmd.hpp"
#include "keva/protocol/resp_encoder.hpp"
#include "keva/core/server_context.hpp"
#include "keva/persistence/rdb_save.hpp"
#include "keva/common/logger.hpp"

#include <sstream>
#include <ctime>
#include <charconv>

namespace keva::command::handlers {

using protocol::RespEncoder;

// --------------------------------------------------------------------------
// register_server_commands
// --------------------------------------------------------------------------
void register_server_commands(CommandRegistry& reg) {
    reg.register_command({"PING",    handle_ping,    1,  2, CMD_FLAG_FAST});
    reg.register_command({"ECHO",    handle_echo,    2,  2, CMD_FLAG_FAST});
    reg.register_command({"FLUSHDB", handle_flushdb, 1,  1, CMD_FLAG_WRITE | CMD_FLAG_ADMIN});
    reg.register_command({"INFO",    handle_info,    1,  2, CMD_FLAG_READONLY});
    reg.register_command({"SAVE",    handle_save,    1,  1, CMD_FLAG_ADMIN});
    reg.register_command({"BGSAVE",  handle_bgsave,  1,  1, CMD_FLAG_ADMIN});
    reg.register_command({"SELECT",  handle_select,  2,  2, CMD_FLAG_FAST});
    reg.register_command({"QUIT",    handle_quit,    1,  1, CMD_FLAG_FAST});
}

// --------------------------------------------------------------------------
// handle_ping — PING [message]
// --------------------------------------------------------------------------
void handle_ping(CommandContext& ctx) {
    if (ctx.cmd.elements.size() == 1) {
        RespEncoder::simple_string(ctx.conn, "PONG");
    } else {
        // PING with message echoes the message as a bulk string
        RespEncoder::bulk_string(ctx.conn, ctx.cmd.elements[1].str);
    }
}

// --------------------------------------------------------------------------
// handle_echo — ECHO message
// --------------------------------------------------------------------------
void handle_echo(CommandContext& ctx) {
    RespEncoder::bulk_string(ctx.conn, ctx.cmd.elements[1].str);
}

// --------------------------------------------------------------------------
// handle_flushdb — FLUSHDB (remove all keys from the current database)
// --------------------------------------------------------------------------
void handle_flushdb(CommandContext& ctx) {
    ctx.db.flush();
    RespEncoder::ok(ctx.conn);
}

// --------------------------------------------------------------------------
// handle_info — INFO [section]
// --------------------------------------------------------------------------
void handle_info(CommandContext& ctx) {
    auto& srv = core::ServerContext::instance();
    const i64 uptime = static_cast<i64>(::time(nullptr)) - srv.start_time_seconds;

    std::ostringstream ss;

    // --- server section ---
    ss << "# Server\r\n";
    ss << "keva_version:0.1.0\r\n";
    ss << "os:Linux\r\n";
    ss << "arch_bits:64\r\n";
    ss << "hz:" << srv.config().hz << "\r\n";
    ss << "uptime_in_seconds:" << uptime << "\r\n";
    ss << "tcp_port:" << srv.config().port << "\r\n";
    ss << "\r\n";

    // --- clients section ---
    ss << "# Clients\r\n";
    ss << "connected_clients:1\r\n";  // Phase 1 stub; full tracking in Phase 2
    ss << "\r\n";

    // --- stats section ---
    ss << "# Stats\r\n";
    ss << "total_commands_processed:"
       << srv.stats().total_commands_processed.load() << "\r\n";
    ss << "total_connections_received:"
       << srv.stats().total_connections_received.load() << "\r\n";
    ss << "keyspace_hits:"   << srv.stats().keyspace_hits.load()   << "\r\n";
    ss << "keyspace_misses:" << srv.stats().keyspace_misses.load() << "\r\n";
    ss << "expired_keys:"    << srv.stats().expired_keys.load()    << "\r\n";
    ss << "evicted_keys:"    << srv.stats().evicted_keys.load()    << "\r\n";
    ss << "\r\n";

    // --- persistence section ---
    ss << "# Persistence\r\n";
    ss << "rdb_enabled:" << (srv.config().rdb_enabled ? "1" : "0") << "\r\n";
    ss << "rdb_last_save_time:" << srv.last_save_time.load() << "\r\n";
    ss << "rdb_changes_since_last_save:" << srv.dirty_keys.load() << "\r\n";
    ss << "\r\n";

    // --- keyspace section ---
    ss << "# Keyspace\r\n";
    ss << "db0:keys=" << ctx.db.dbsize() << ",expires=0\r\n";  // expires count in Phase 4

    RespEncoder::bulk_string(ctx.conn, ss.str());
}

// --------------------------------------------------------------------------
// handle_save — SAVE (synchronous RDB; blocks the server)
// --------------------------------------------------------------------------
void handle_save(CommandContext& ctx) {
    log::info("SAVE: starting synchronous RDB save");
    const auto& cfg = core::ServerContext::instance().config();

    Status s = persistence::rdb_save_sync(
        ctx.db, cfg.rdb_filename);

    if (s.ok()) {
        core::ServerContext::instance().last_save_time.store(
            static_cast<i64>(::time(nullptr)));
        core::ServerContext::instance().dirty_keys.store(0);
        RespEncoder::ok(ctx.conn);
    } else {
        RespEncoder::error(ctx.conn, s.message());
    }
}

// --------------------------------------------------------------------------
// handle_bgsave — BGSAVE (fork and save in background)
// --------------------------------------------------------------------------
void handle_bgsave(CommandContext& ctx) {
    auto& srv = core::ServerContext::instance();

    if (srv.bgsave_child_pid != -1) {
        RespEncoder::error(ctx.conn,
            "Background save already in progress");
        return;
    }

    log::info("BGSAVE: triggering background RDB save");
    Status s = persistence::rdb_save_background(
        ctx.db,
        srv.config().rdb_filename,
        srv.bgsave_child_pid);

    if (s.ok()) {
        RespEncoder::simple_string(ctx.conn, "Background saving started");
    } else {
        RespEncoder::error(ctx.conn, s.message());
    }
}

// --------------------------------------------------------------------------
// handle_select — SELECT index (Phase 1: only db 0 supported)
// --------------------------------------------------------------------------
void handle_select(CommandContext& ctx) {
    i64 idx = 0;
    if (auto [p, ec] = std::from_chars(ctx.cmd.elements[1].str.data(),
                                        ctx.cmd.elements[1].str.data() + ctx.cmd.elements[1].str.size(),
                                        idx); ec != std::errc{} || idx != 0) {
        RespEncoder::error(ctx.conn, "DB index is out of range");
        return;
    }
    RespEncoder::ok(ctx.conn);
}

// --------------------------------------------------------------------------
// handle_quit — QUIT
// --------------------------------------------------------------------------
void handle_quit(CommandContext& ctx) {
    RespEncoder::ok(ctx.conn);
    ctx.conn.set_state(net::ConnectionState::WriteOnly);
}

} // namespace keva::command::handlers
