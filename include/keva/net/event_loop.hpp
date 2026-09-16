// ==============================================================================
// keva/net/event_loop.hpp
//
// Purpose:
//   Declares the EventLoop class — the central reactor engine of Keva's
//   network layer in Phase 1 (Single-Threaded Reactor model).
//
//   This is the most important file to understand in the networking subsystem.
//   It wraps Linux's epoll API — the kernel mechanism that allows a single
//   thread to efficiently monitor thousands of TCP socket file descriptors
//   simultaneously without blocking.
//
//   How epoll works (OS-level explanation):
//   1. epoll_create1(): Asks the kernel to create an epoll "interest list".
//      Returns an fd that refers to this kernel-managed list.
//   2. epoll_ctl(EPOLL_CTL_ADD/MOD/DEL): Register a socket fd and tell the
//      kernel "wake me up when this fd becomes READABLE (EPOLLIN) or
//      WRITABLE (EPOLLOUT)".
//   3. epoll_wait(): Puts the calling thread to sleep. The kernel wakes it
//      up ONLY when at least one registered fd is ready for I/O. Returns a
//      list of ready events. Zero CPU consumption while idle.
//
//   Level-Triggered (LT) vs. Edge-Triggered (ET):
//   - LT (default): epoll_wait() keeps reporting an fd as ready until all
//     data is consumed. Simpler to implement correctly.
//   - ET: epoll_wait() reports an fd ready only once per state change.
//     Requires reading in a tight loop until EAGAIN on every notification.
//     We use LT for Phase 1 (simplicity) and can switch to ET for Phase 2.
//
//   The EventLoop exposes a callback registration model:
//   - on_readable(fd, callback): register a handler for EPOLLIN events
//   - on_writable(fd, callback): register a handler for EPOLLOUT events
//   - remove(fd): deregister an fd from the epoll interest set
//   - run(): the blocking main loop — blocks in epoll_wait, dispatches events
//   - stop(): signals the loop to exit gracefully
// ==============================================================================

#pragma once

#include "keva/common/types.hpp"
#include "keva/common/status.hpp"

#include <functional>
#include <unordered_map>
#include <sys/epoll.h>   // epoll_event, EPOLLIN, EPOLLOUT, etc.
#include <atomic>

namespace keva::net {

// --------------------------------------------------------------------------
// EventType — bitmask of I/O events of interest
// --------------------------------------------------------------------------
enum class EventType : u32 {
    None     = 0,
    Readable = EPOLLIN,               // Data available to read (client sent data)
    Writable = EPOLLOUT,              // Socket buffer has space (can send response)
    Error    = EPOLLERR | EPOLLHUP,   // Socket error or hang-up (client disconnected)
};

inline EventType operator|(EventType a, EventType b) {
    return static_cast<EventType>(static_cast<u32>(a) | static_cast<u32>(b));
}
inline u32 to_epoll_events(EventType t) { return static_cast<u32>(t); }

// --------------------------------------------------------------------------
// IoCallback — the function signature for all event handlers
// fd: the file descriptor that became ready
// --------------------------------------------------------------------------
using IoCallback = std::function<void(int fd)>;

// --------------------------------------------------------------------------
// FdHandlers — read and write callbacks registered for one fd
// --------------------------------------------------------------------------
struct FdHandlers {
    IoCallback on_readable;
    IoCallback on_writable;
};

// --------------------------------------------------------------------------
// EventLoop — Linux epoll-based single-threaded reactor
// --------------------------------------------------------------------------
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    // Non-copyable, non-movable (owns the epoll fd and the handlers map)
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // --------------------------------------------------------------------------
    // fd registration (calls epoll_ctl internally)
    // --------------------------------------------------------------------------

    // Register 'fd' for EPOLLIN events; 'callback' is called when fd is readable
    Status add_readable(int fd, IoCallback callback);

    // Register 'fd' for EPOLLOUT events (call when write buffer fills up)
    Status add_writable(int fd, IoCallback callback);

    // Remove EPOLLOUT interest (stop asking for writable notifications)
    Status remove_writable(int fd);

    // Remove 'fd' entirely from the epoll interest set (client disconnected)
    Status remove(int fd);

    // --------------------------------------------------------------------------
    // Timer support — runs a callback every 'interval_ms' milliseconds
    // Used for the 10Hz server cron (expiry scanning, incremental rehash)
    // --------------------------------------------------------------------------
    void set_timer(int interval_ms, std::function<void()> callback);

    // --------------------------------------------------------------------------
    // Reactor main loop
    // --------------------------------------------------------------------------

    // Blocking: calls epoll_wait in a loop, dispatches events to callbacks.
    // Returns when stop() is called.
    void run();

    // Signal the run() loop to exit cleanly on the next iteration
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return running_.load(); }

private:
    // Modify the epoll interest set for an fd (add / modify / delete)
    Status epoll_ctl_op(int op, int fd, u32 events);

    // Run the periodic timer callback if the interval has elapsed
    void maybe_run_timer();

    int epoll_fd_ = -1;   // The epoll instance file descriptor

    // Per-fd handler registry (maps fd -> {on_readable, on_writable})
    std::unordered_map<int, FdHandlers> handlers_;

    // Maximum number of events to process per epoll_wait() call
    static constexpr int MAX_EVENTS = 1024;

    std::atomic<bool> running_{false};

    // Timer state (simple single-timer for server cron)
    std::function<void()> timer_callback_;
    int  timer_interval_ms_ = 0;
    long last_timer_fire_ms_ = 0;
};

} // namespace keva::net
