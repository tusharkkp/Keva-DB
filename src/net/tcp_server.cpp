// ==============================================================================
// src/net/tcp_server.cpp
//
// Purpose:
//   Implements the TcpServer class — the accept loop, connection lifecycle
//   manager, and bridge between raw TCP bytes and the RESP2 request handler.
//
//   Critical implementation details:
//
//   accept() in a loop:
//     When the listen socket gets EPOLLIN, there may be MULTIPLE pending client
//     connections queued in the kernel accept backlog. We loop calling accept()
//     until it returns EAGAIN, accepting all of them in one event cycle.
//     This prevents a thundering herd: if we only accepted one connection per
//     epoll event, a burst of 100 simultaneous connections would require 100
//     separate epoll_wait() round trips.
//
//   Connection cleanup:
//     When fill_read_buffer() returns 0 (EOF) or -1 (error), we call
//     close_connection() which: removes the fd from epoll, destructs the
//     Connection object (which closes the socket via RAII), and removes the
//     entry from the clients_ map.
//
//   Write buffer + EPOLLOUT flow:
//     After each request_handler invocation, if the connection has pending
//     writes, we immediately try flush_write_buffer(). If not all bytes are
//     sent (kernel send buffer full), we register EPOLLOUT interest on the fd.
//     When epoll signals the fd is writable, on_client_writable() drains the
//     rest and removes EPOLLOUT registration.
// ==============================================================================

#include "keva/net/tcp_server.hpp"
#include "keva/common/logger.hpp"

#include <sys/socket.h>   // accept()
#include <netinet/in.h>   // sockaddr_in
#include <arpa/inet.h>    // inet_ntop() for logging client IP
#include <cerrno>
#include <cstring>

namespace keva::net {

// --------------------------------------------------------------------------
// Constructor
// --------------------------------------------------------------------------
TcpServer::TcpServer(std::string_view host, u16 port, EventLoop& loop)
    : host_(host), port_(port), loop_(loop)
{}

// --------------------------------------------------------------------------
// start — create socket, bind, listen, register with EventLoop
// --------------------------------------------------------------------------
Status TcpServer::start() {
    auto [sock, status] = Socket::create_server(host_, port_);
    if (!status.ok()) {
        log::error("Failed to create server socket: %s", status.message().c_str());
        return status;
    }
    listen_socket_ = std::move(sock);

    // Register the listening socket with the EventLoop.
    // When a new TCP client completes its 3-way handshake, the kernel delivers
    // EPOLLIN on the listen fd. We handle it by calling accept().
    const int listen_fd = listen_socket_.fd();
    auto s = loop_.add_readable(listen_fd, [this](int fd) {
        on_new_connection(fd);
    });

    if (!s.ok()) {
        log::error("Failed to register listen socket with event loop: %s",
                   s.message().c_str());
        return s;
    }

    log::info("Keva listening on %s:%d", host_.c_str(), port_);
    return Status::ok();
}

// --------------------------------------------------------------------------
// stop — close all connections and the listen socket
// --------------------------------------------------------------------------
void TcpServer::stop() {
    loop_.remove(listen_socket_.fd());
    // Close all client connections
    for (auto& [fd, conn] : clients_) {
        loop_.remove(fd);
    }
    clients_.clear();
    log::info("TcpServer stopped");
}

// --------------------------------------------------------------------------
// set_request_handler
// --------------------------------------------------------------------------
void TcpServer::set_request_handler(RequestHandler handler) {
    request_handler_ = std::move(handler);
}

// --------------------------------------------------------------------------
// client_count / next_client_id
// --------------------------------------------------------------------------
usize TcpServer::client_count() const noexcept {
    return clients_.size();
}

u64 TcpServer::next_client_id() noexcept {
    return next_client_id_.fetch_add(1, std::memory_order_relaxed);
}

// --------------------------------------------------------------------------
// on_new_connection — accept loop triggered by EPOLLIN on listen fd
// --------------------------------------------------------------------------
void TcpServer::on_new_connection(int /*listen_fd*/) {
    // Loop to accept ALL pending connections in one epoll notification
    while (true) {
        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        // accept4(): Like accept() but atomically sets O_NONBLOCK and SOCK_CLOEXEC.
        // SOCK_CLOEXEC: client fds are not inherited by the RDB BGSAVE child.
        int client_fd = ::accept4(
            listen_socket_.fd(),
            reinterpret_cast<struct sockaddr*>(&client_addr),
            &addr_len,
            SOCK_NONBLOCK | SOCK_CLOEXEC
        );

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No more pending connections; done for this event cycle
                break;
            }
            log::error("accept4 failed: %s", strerror(errno));
            break;
        }

        // Log the new client's IP address for observability
        char ip_str[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        const u64 cid = next_client_id();
        log::debug("New client #%llu from %s:%d (fd=%d)",
                   static_cast<unsigned long long>(cid),
                   ip_str,
                   ntohs(client_addr.sin_port),
                   client_fd);

        // Create a Connection object (owns the client socket fd)
        Socket client_sock{client_fd};
        client_sock.set_nodelay();  // TCP_NODELAY for low latency responses

        auto conn = std::make_unique<Connection>(std::move(client_sock), cid);

        // Register the client fd for EPOLLIN events
        loop_.add_readable(client_fd, [this](int fd) {
            on_client_readable(fd);
        });

        clients_.emplace(client_fd, std::move(conn));
    }
}

// --------------------------------------------------------------------------
// on_client_readable — called when a client socket has data ready to read
// --------------------------------------------------------------------------
void TcpServer::on_client_readable(int client_fd) {
    auto it = clients_.find(client_fd);
    if (it == clients_.end()) return;

    Connection& conn = *it->second;

    // Read all available bytes from the kernel receive buffer into our buffer
    ssize_t n = conn.fill_read_buffer();

    if (n == 0) {
        // EOF: client disconnected gracefully
        log::debug("Client #%llu disconnected (fd=%d)",
                   static_cast<unsigned long long>(conn.client_id()), client_fd);
        close_connection(client_fd);
        return;
    }

    if (n < 0) {
        log::debug("Client #%llu I/O error (fd=%d)",
                   static_cast<unsigned long long>(conn.client_id()), client_fd);
        close_connection(client_fd);
        return;
    }

    // Dispatch to the application layer (RESP parser + command executor)
    if (request_handler_) {
        request_handler_(conn);
    }

    // If the handler produced a response, flush it immediately.
    // On EAGAIN, register for EPOLLOUT to drain the rest asynchronously.
    if (conn.has_pending_writes()) {
        conn.flush_write_buffer();
        if (conn.has_pending_writes()) {
            // Write buffer not fully drained: enable EPOLLOUT
            loop_.add_writable(client_fd, [this](int fd) {
                on_client_writable(fd);
            });
        }
    }

    // Connection may have been closed by the request_handler (e.g., CLIENT KILL)
    if (conn.is_closed()) {
        close_connection(client_fd);
    }
}

// --------------------------------------------------------------------------
// on_client_writable — kernel send buffer has space; flush the write buffer
// --------------------------------------------------------------------------
void TcpServer::on_client_writable(int client_fd) {
    auto it = clients_.find(client_fd);
    if (it == clients_.end()) return;

    Connection& conn = *it->second;
    conn.flush_write_buffer();

    if (!conn.has_pending_writes()) {
        // Write buffer fully drained: stop listening for EPOLLOUT events
        loop_.remove_writable(client_fd);
    }

    if (conn.is_closed()) {
        close_connection(client_fd);
    }
}

// --------------------------------------------------------------------------
// close_connection — remove client from epoll and destroy Connection
// --------------------------------------------------------------------------
void TcpServer::close_connection(int client_fd) {
    loop_.remove(client_fd);
    clients_.erase(client_fd);
    // The Connection destructor will call ~Socket() -> close(fd) automatically
}

} // namespace keva::net
