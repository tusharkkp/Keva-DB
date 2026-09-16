// ==============================================================================
// src/net/socket.cpp
//
// Purpose:
//   Implements the Socket RAII class declared in keva/net/socket.hpp.
//
//   This file contains the actual POSIX syscall invocations:
//   - socket()     : Creates the underlying OS socket file descriptor
//   - setsockopt() : Configures SO_REUSEADDR and TCP_NODELAY socket options
//   - fcntl()      : Sets O_NONBLOCK flag for non-blocking I/O
//   - bind()       : Associates the socket with a specific IP:port address
//   - listen()     : Marks the socket as passive (ready to accept connections)
//   - read()       : Reads bytes from a connected client socket
//   - write()      : Writes bytes to a connected client socket
//   - close()      : Releases the file descriptor back to the OS kernel
//
//   All syscall return values are checked and translated into Status objects
//   that callers can inspect without catching exceptions.
// ==============================================================================

#include "keva/net/socket.hpp"
#include "keva/common/logger.hpp"

#include <sys/socket.h>   // socket(), bind(), listen(), setsockopt()
#include <netinet/in.h>   // sockaddr_in, IPPROTO_TCP
#include <netinet/tcp.h>  // TCP_NODELAY
#include <arpa/inet.h>    // inet_pton()
#include <fcntl.h>        // fcntl(), O_NONBLOCK
#include <unistd.h>       // close(), read(), write()
#include <cerrno>
#include <cstring>        // strerror()

namespace keva::net {

// --------------------------------------------------------------------------
// Move semantics — transfer fd ownership, leave source invalid
// --------------------------------------------------------------------------
Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = INVALID_FD;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close_fd();
        fd_ = other.fd_;
        other.fd_ = INVALID_FD;
    }
    return *this;
}

// --------------------------------------------------------------------------
// Destructor — RAII guarantee: always close the fd
// --------------------------------------------------------------------------
Socket::~Socket() {
    close_fd();
}

void Socket::close_fd() noexcept {
    if (fd_ != INVALID_FD) {
        ::close(fd_);
        fd_ = INVALID_FD;
    }
}

// --------------------------------------------------------------------------
// Factory: create_server()
// Creates and configures a non-blocking TCP listening socket.
// --------------------------------------------------------------------------
std::pair<Socket, Status> Socket::create_server(std::string_view host, u16 port) {
    // --- 1. Create a TCP socket ---
    // AF_INET   : IPv4 address family
    // SOCK_STREAM: reliable, connection-oriented byte stream (TCP)
    // 0         : let the OS pick the protocol (TCP for SOCK_STREAM)
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return {Socket{}, Status::io_error(strerror(errno))};
    }

    Socket sock{fd};

    // --- 2. SO_REUSEADDR ---
    // Without this, after a server restart, the old socket lingers in TIME_WAIT
    // state (typically 60-120 seconds) and bind() would return EADDRINUSE.
    int opt = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        return {Socket{}, Status::io_error(strerror(errno))};
    }

    // --- 3. TCP_NODELAY ---
    // Disables Nagle's algorithm. Nagle coalesces small TCP segments to improve
    // bandwidth efficiency, but introduces up to 40-200ms latency for small
    // RESP2 command messages (bad for interactive key-value access patterns).
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        return {Socket{}, Status::io_error(strerror(errno))};
    }

    // --- 4. O_NONBLOCK ---
    // Makes all I/O operations on this socket non-blocking. accept(), read(),
    // write() return immediately with EAGAIN if no data/connection is available,
    // which is the fundamental requirement for our epoll reactor event loop.
    if (auto s = sock.set_nonblocking(); !s.ok()) {
        return {Socket{}, s};
    }

    // --- 5. Bind to the specified address and port ---
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);  // htons: host byte order -> network byte order
    if (host == "0.0.0.0" || host.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        // inet_pton: human-readable dotted-decimal IP -> binary network address
        if (::inet_pton(AF_INET, host.data(), &addr.sin_addr) != 1) {
            return {Socket{}, Status::io_error("Invalid bind address")};
        }
    }

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        return {Socket{}, Status::io_error(strerror(errno))};
    }

    // --- 6. Listen ---
    // TCP_BACKLOG: how many pending connections can queue in the kernel
    // before keva-server calls accept(). If the queue overflows, new
    // connection attempts receive ECONNREFUSED.
    if (::listen(fd, constants::TCP_BACKLOG) < 0) {
        return {Socket{}, Status::io_error(strerror(errno))};
    }

    return {std::move(sock), Status::success()};
}

// --------------------------------------------------------------------------
// set_nonblocking — set O_NONBLOCK on an existing socket fd
// --------------------------------------------------------------------------
Status Socket::set_nonblocking() noexcept {
    // fcntl GET: read existing flags (must not clear other flags during SET)
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        return Status::io_error(strerror(errno));
    }
    // fcntl SET: OR in O_NONBLOCK
    if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        return Status::io_error(strerror(errno));
    }
    return Status::success();
}

// --------------------------------------------------------------------------
// set_nodelay — disable Nagle's algorithm on a connected client socket
// --------------------------------------------------------------------------
Status Socket::set_nodelay() noexcept {
    int opt = 1;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        return Status::io_error(strerror(errno));
    }
    return Status::success();
}

// --------------------------------------------------------------------------
// read — read up to len bytes from the socket
// --------------------------------------------------------------------------
ssize_t Socket::read(void* buf, usize len) const noexcept {
    return ::read(fd_, buf, len);
}

// --------------------------------------------------------------------------
// write — write up to len bytes to the socket
// --------------------------------------------------------------------------
ssize_t Socket::write(const void* buf, usize len) const noexcept {
    return ::write(fd_, buf, len);
}

} // namespace keva::net
