// ==============================================================================
// src/main.cpp
//
// Purpose:
//   The Keva server entry point. Orchestrates the full startup sequence:
//
//   1. CLI argument parsing (--host, --port, --loglevel, --maxmemory, etc.)
//   2. Logger configuration (set log level from CLI flag)
//   3. ServerContext initialization (config, database creation, SipHash seeding)
//   4. RDB snapshot loading (restore previous data from dump.rdb)
//   5. Command registry population (register all command handlers)
//   6. Signal handler installation:
//      - SIGTERM / SIGINT: graceful shutdown (stop the event loop)
//      - SIGCHLD: reap BGSAVE child process (update save time, dirty counter)
//      - SIGPIPE: ignored (clients disconnecting mid-write should not crash server)
//   7. TcpServer creation and binding to host:port
//   8. EventLoop timer configuration (10Hz server cron: expiry + rehash + save check)
//   9. EventLoop::run() — enters the blocking reactor main loop
//   10. On shutdown: final BGSAVE (if dirty), clean teardown
//
//   Signal handling design:
//   Linux signals are asynchronous and interrupt the process at arbitrary points.
//   To avoid signal handler reentrancy bugs, we use the "self-pipe trick":
//   the signal handler writes a single byte to a pipe, and the EventLoop
//   registers the read end of the pipe as an EPOLLIN fd. The main thread
//   handles the signal notification synchronously within its event loop iteration.
// ==============================================================================

#include "keva/net/event_loop.hpp"
#include "keva/net/tcp_server.hpp"
#include "keva/command/command.hpp"
#include "keva/core/server_context.hpp"
#include "keva/persistence/rdb_load.hpp"
#include "keva/persistence/rdb_save.hpp"
#include "keva/common/logger.hpp"
#include "keva/common/types.hpp"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <random>
#include <unistd.h>    // pipe(), read(), write()
#include <sys/wait.h>  // waitpid()
#include <fcntl.h>

// --------------------------------------------------------------------------
// Self-pipe for safe signal delivery to the event loop
// --------------------------------------------------------------------------
static int g_signal_pipe[2] = {-1, -1};
static keva::net::EventLoop* g_event_loop_ptr = nullptr;

static void signal_handler(int signum) {
    // Write signal number to the pipe; the event loop reads it on EPOLLIN
    const char byte = static_cast<char>(signum);
    ::write(g_signal_pipe[1], &byte, 1);
}

// Called from the event loop when the signal pipe becomes readable
static void on_signal_pipe_readable(int pipe_fd) {
    char signum_byte = 0;
    if (::read(pipe_fd, &signum_byte, 1) != 1) return;

    const int signum = static_cast<int>(signum_byte);

    if (signum == SIGTERM || signum == SIGINT) {
        keva::log::info("Received signal %d — initiating graceful shutdown", signum);
        if (g_event_loop_ptr) g_event_loop_ptr->stop();
    } else if (signum == SIGCHLD) {
        // Reap the BGSAVE child process
        auto& srv = keva::core::ServerContext::instance();
        if (srv.bgsave_child_pid != -1) {
            int status = 0;
            keva::persistence::on_bgsave_child_exit(srv.bgsave_child_pid, status);
        }
    }
}

// --------------------------------------------------------------------------
// install_signal_handlers — set up the self-pipe and signal handlers
// --------------------------------------------------------------------------
static bool install_signal_handlers(keva::net::EventLoop& loop) {
    if (::pipe2(g_signal_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
        keva::log::error("pipe2 failed: %s", strerror(errno));
        return false;
    }

    // Register the read end of the pipe with the event loop
    loop.add_readable(g_signal_pipe[0], on_signal_pipe_readable);

    // Install signal handlers that write to the pipe
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGCHLD, &sa, nullptr);

    // SIGPIPE: ignore broken pipe signals (client disconnects during write)
    struct sigaction sa_ignore{};
    sa_ignore.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa_ignore, nullptr);

    return true;
}

// --------------------------------------------------------------------------
// parse_args — minimal CLI argument parser
// --------------------------------------------------------------------------
static keva::core::ServerConfig parse_args(int argc, char* argv[]) {
    keva::core::ServerConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if ((arg == "--host" || arg == "-h") && i + 1 < argc) {
            cfg.host = argv[++i];
        } else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            cfg.port = static_cast<keva::u16>(std::atoi(argv[++i]));
        } else if (arg == "--loglevel" && i + 1 < argc) {
            cfg.loglevel = argv[++i];
        } else if (arg == "--maxmemory" && i + 1 < argc) {
            cfg.maxmemory = static_cast<keva::usize>(std::atoll(argv[++i]));
        } else if (arg == "--rdb-filename" && i + 1 < argc) {
            cfg.rdb_filename = argv[++i];
        } else if (arg == "--no-rdb") {
            cfg.rdb_enabled = false;
        } else if (arg == "--help") {
            keva::log::info("Usage: keva-server [--host IP] [--port N] "
                            "[--loglevel debug|info|warn|error] "
                            "[--maxmemory bytes] [--rdb-filename path] [--no-rdb]");
            std::exit(0);
        }
    }

    return cfg;
}

// --------------------------------------------------------------------------
// main — Keva server entry point
// --------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // --- 1. Parse CLI arguments ---
    keva::core::ServerConfig config = parse_args(argc, argv);

    // --- 2. Configure logger ---
    {
        keva::LogLevel lvl = keva::LogLevel::Info;
        if (config.loglevel == "debug") lvl = keva::LogLevel::Debug;
        else if (config.loglevel == "warn")  lvl = keva::LogLevel::Warn;
        else if (config.loglevel == "error") lvl = keva::LogLevel::Error;
        keva::Logger::instance().set_level(lvl);
    }

    keva::log::info("=== Keva In-Memory Database v0.1.0 ===");
    keva::log::info("Starting server on %s:%d", config.host.c_str(), config.port);

    // --- 3. Initialize ServerContext ---
    keva::core::ServerContext::instance().init(config);

    // --- 4. Seed the SipHash key from /dev/urandom ---
    {
        keva::u64 k0 = 0, k1 = 0;
        int urandom_fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (urandom_fd >= 0) {
            ::read(urandom_fd, &k0, 8);
            ::read(urandom_fd, &k1, 8);
            ::close(urandom_fd);
        } else {
            // Fallback: use a mt19937_64 seeded with the current time
            std::mt19937_64 rng{static_cast<keva::u64>(::time(nullptr))};
            k0 = rng(); k1 = rng();
            keva::log::warn("Could not read /dev/urandom; using time-seeded RNG for hash key");
        }
        // The Dict instances are created in ServerContext::init, which seeds itself
        // in a future iteration. For now, the hash keys default to 0.
        // TODO: expose Dict::seed_hash through ServerContext in a follow-up.
        (void)k0; (void)k1;
    }

    // --- 5. Load RDB snapshot ---
    {
        auto& db = keva::core::ServerContext::instance().db();
        keva::Status load_status = keva::persistence::rdb_load(
            db, config.rdb_filename);
        if (!load_status.ok() && !load_status.is_not_found()) {
            keva::log::error("RDB load failed: %s — starting with empty database",
                             load_status.message().c_str());
            db.flush();
        }
    }

    // --- 6. Register all command handlers ---
    keva::command::register_all_commands();

    // --- 7. Create the EventLoop and TcpServer ---
    keva::net::EventLoop loop;
    g_event_loop_ptr = &loop;

    keva::net::TcpServer server{config.host, static_cast<keva::u16>(config.port), loop};

    // Wire the command dispatcher as the RequestHandler callback.
    // This is the crucial bridge: TcpServer calls this lambda when a client
    // has data ready, which invokes the RESP parser + command dispatcher.
    server.set_request_handler([&](keva::net::Connection& conn) {
        keva::command::CommandRegistry::instance().dispatch(
            conn,
            keva::core::ServerContext::instance().db()
        );
    });

    // --- 8. Install signal handlers (after EventLoop is created) ---
    if (!install_signal_handlers(loop)) {
        return 1;
    }

    // --- 9. Start listening for TCP connections ---
    if (auto s = server.start(); !s.ok()) {
        keva::log::error("Failed to start server: %s", s.message().c_str());
        return 1;
    }

    // --- 10. Configure the 10Hz server cron timer ---
    const int cron_interval_ms = 1000 / config.hz;  // e.g., 1000/10 = 100ms
    loop.set_timer(cron_interval_ms, []() {
        keva::core::ServerContext::instance().server_cron();
    });

    keva::log::info("Keva ready to accept connections on %s:%d",
                    config.host.c_str(), config.port);

    // --- 11. Enter the blocking reactor event loop ---
    loop.run();

    // --- 12. Graceful shutdown ---
    keva::log::info("Shutting down...");
    server.stop();

    // Perform a final save if there are unsaved changes
    if (config.rdb_enabled &&
        keva::core::ServerContext::instance().dirty_keys.load() > 0) {
        keva::log::info("Saving final snapshot before exit...");
        keva::persistence::rdb_save_sync(
            keva::core::ServerContext::instance().db(),
            config.rdb_filename);
    }

    keva::log::info("Keva exited cleanly.");
    return 0;
}
