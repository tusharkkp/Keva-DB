// ==============================================================================
// keva/net/tcp_server.hpp
//
// Purpose:
//   Declares the TcpServer class — the top-level network coordinator that
//   brings together the Socket, Connection, and EventLoop subsystems into a
//   fully functioning TCP server for Keva.
//
//   Responsibilities:
//   1. Owns and manages the master listening socket (bound to host:port).
//   2. Registers the listening socket with the EventLoop for EPOLLIN events.
//      When a new client connects, Linux delivers an EPOLLIN event on the
//      listen fd, and TcpServer calls accept() to receive it.
//   3. For each accepted client, creates a Connection object, registers the
//      client fd with EventLoop for EPOLLIN events, and stores the Connection
//      in the client registry.
//   4. When a client fd becomes readable, calls fill_read_buffer() and then
//      invokes the registered RequestHandler callback with the connection.
//      The RequestHandler is the interface between networking and the RESP
//      parser + command dispatcher.
//   5. When a client disconnects or errors, destroys the Connection and
//      removes the fd from the event loop.
//
//   The TcpServer intentionally does NOT know anything about RESP parsing or
//   command execution — it only knows about bytes and connections. This
//   separation of concerns keeps each subsystem independently testable.
// ==============================================================================

#pragma once

#include "keva/net/socket.hpp"
#include "keva/net/connection.hpp"
#include "keva/net/event_loop.hpp"
#include "keva/common/types.hpp"
#include "keva/common/status.hpp"

#include <functional>
#include <unordered_map>
#include <memory>
#include <string_view>
#include <atomic>

namespace keva::net {

// --------------------------------------------------------------------------
// RequestHandler — callback invoked by TcpServer when a client's read buffer
// has new data. The handler is responsible for parsing RESP2 from the
// connection's read buffer and writing the encoded response back.
// --------------------------------------------------------------------------
using RequestHandler = std::function<void(Connection& conn)>;

// --------------------------------------------------------------------------
// TcpServer — accept and manage TCP client connections via epoll
// --------------------------------------------------------------------------
class TcpServer {
public:
    TcpServer(std::string_view host, u16 port, EventLoop& loop);

    // Non-copyable, non-movable
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Start listening: creates the socket, binds, listens, registers with epoll
    Status start();

    // Stop the server: closes the listen socket and all client connections
    void stop();

    // Register the application-layer request handler callback
    void set_request_handler(RequestHandler handler);

    // Returns the number of currently connected clients
    [[nodiscard]] usize client_count() const noexcept;

    // Returns the next unique client ID (monotonically increasing)
    [[nodiscard]] u64 next_client_id() noexcept;

private:
    // Called by EventLoop when the listen fd receives an EPOLLIN event (new client)
    void on_new_connection(int listen_fd);

    // Called by EventLoop when a client fd receives an EPOLLIN event (data ready)
    void on_client_readable(int client_fd);

    // Called by EventLoop when a client fd receives an EPOLLOUT event (space available)
    void on_client_writable(int client_fd);

    // Close and remove a client connection
    void close_connection(int client_fd);

    std::string host_;
    u16         port_;
    EventLoop&  loop_;           // Reference: TcpServer does not own the EventLoop
    Socket      listen_socket_;

    // client_fd -> Connection (unique_ptr for stable addresses even during rehash)
    std::unordered_map<int, std::unique_ptr<Connection>> clients_;

    RequestHandler request_handler_;

    std::atomic<u64> next_client_id_{1};
};

} // namespace keva::net
