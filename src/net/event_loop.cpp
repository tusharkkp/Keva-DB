// ==============================================================================
// src/net/event_loop.cpp
//
// Purpose:
//   Implements the EventLoop reactor using Linux epoll syscalls.
//
//   The core of the run() method is:
//
//     while (running_) {
//         int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);
//         for each ready event:
//             if (EPOLLIN)  call handlers_[fd].on_readable(fd)
//             if (EPOLLOUT) call handlers_[fd].on_writable(fd)
//             if (error)    close the connection
//         maybe_run_timer();
//     }
//
//   The timeout passed to epoll_wait() is set to the timer interval so
//   the cron tick fires on schedule even when no clients are active.
//
//   Key learning points demonstrated here:
//   - epoll_create1(EPOLL_CLOEXEC): creates epoll fd marked close-on-exec
//     so child processes spawned for RDB BGSAVE don't inherit it.
//   - epoll_ctl(EPOLL_CTL_ADD / MOD / DEL): modifying the interest set.
//   - epoll_wait(): the blocking dispatcher. Timeout=0 means poll (non-block),
//     Timeout=-1 means block forever.
//   - EPOLLET edge-triggered flag intentionally NOT set here (we use LT).
// ==============================================================================

#include "keva/net/event_loop.hpp"
#include "keva/common/logger.hpp"

#include <sys/epoll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <chrono>

namespace keva::net {

// --------------------------------------------------------------------------
// Helpers — get current monotonic time in milliseconds
// --------------------------------------------------------------------------
static long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

// --------------------------------------------------------------------------
// Constructor — create the epoll instance
// --------------------------------------------------------------------------
EventLoop::EventLoop() {
    // epoll_create1(EPOLL_CLOEXEC):
    // EPOLL_CLOEXEC: The epoll fd is automatically closed when fork() spawns
    // a child process (used for RDB BGSAVE). Without this, the child would
    // inherit our epoll fd and interfere with the parent's event loop.
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        log::error("epoll_create1 failed: %s", strerror(errno));
    } else {
        log::info("EventLoop created (epoll fd=%d)", epoll_fd_);
    }
}

// --------------------------------------------------------------------------
// Destructor — close the epoll fd
// --------------------------------------------------------------------------
EventLoop::~EventLoop() {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

// --------------------------------------------------------------------------
// Internal epoll_ctl wrapper
// --------------------------------------------------------------------------
Status EventLoop::epoll_ctl_op(int op, int fd, u32 events) {
    struct epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;    // Store fd in the event data so we can look it up on wakeup

    if (::epoll_ctl(epoll_fd_, op, fd, &ev) < 0) {
        return Status::io_error(strerror(errno));
    }
    return Status::success();
}

// --------------------------------------------------------------------------
// add_readable — register EPOLLIN interest for fd
// --------------------------------------------------------------------------
Status EventLoop::add_readable(int fd, IoCallback callback) {
    auto& h = handlers_[fd];
    h.on_readable = std::move(callback);

    // Determine if we need ADD (new fd) or MOD (fd already registered for writes)
    u32 events = EPOLLIN;
    if (h.on_writable) events |= EPOLLOUT;

    // Try ADD first; if fd is already registered, fall back to MOD
    if (auto s = epoll_ctl_op(EPOLL_CTL_ADD, fd, events); !s.ok()) {
        // fd might already be registered (e.g., adding EPOLLIN after EPOLLOUT)
        return epoll_ctl_op(EPOLL_CTL_MOD, fd, events);
    }
    return Status::success();
}

// --------------------------------------------------------------------------
// add_writable — register EPOLLOUT interest for fd
// --------------------------------------------------------------------------
Status EventLoop::add_writable(int fd, IoCallback callback) {
    auto& h = handlers_[fd];
    h.on_writable = std::move(callback);

    u32 events = EPOLLOUT;
    if (h.on_readable) events |= EPOLLIN;

    if (auto s = epoll_ctl_op(EPOLL_CTL_ADD, fd, events); !s.ok()) {
        return epoll_ctl_op(EPOLL_CTL_MOD, fd, events);
    }
    return Status::success();
}

// --------------------------------------------------------------------------
// remove_writable — stop asking for EPOLLOUT notifications (write buf empty)
// --------------------------------------------------------------------------
Status EventLoop::remove_writable(int fd) {
    auto it = handlers_.find(fd);
    if (it == handlers_.end()) return Status::success();

    it->second.on_writable = nullptr;

    if (it->second.on_readable) {
        // Still reading: downgrade to EPOLLIN only
        return epoll_ctl_op(EPOLL_CTL_MOD, fd, EPOLLIN);
    }
    // Nothing left: remove entirely
    return remove(fd);
}

// --------------------------------------------------------------------------
// remove — fully deregister an fd from epoll and the handlers map
// --------------------------------------------------------------------------
Status EventLoop::remove(int fd) {
    handlers_.erase(fd);
    // epoll automatically removes fds when they are close()d, but explicit
    // DEL ensures we don't receive stale events if fd is reused quickly.
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        if (errno != ENOENT && errno != EBADF) {
            return Status::io_error(strerror(errno));
        }
    }
    return Status::success();
}

// --------------------------------------------------------------------------
// set_timer — configure the periodic server cron callback
// --------------------------------------------------------------------------
void EventLoop::set_timer(int interval_ms, std::function<void()> callback) {
    timer_interval_ms_ = interval_ms;
    timer_callback_    = std::move(callback);
    last_timer_fire_ms_ = now_ms();
}

// --------------------------------------------------------------------------
// maybe_run_timer — fire the timer callback if the interval has elapsed
// --------------------------------------------------------------------------
void EventLoop::maybe_run_timer() {
    if (!timer_callback_ || timer_interval_ms_ <= 0) return;

    const long current = now_ms();
    if (current - last_timer_fire_ms_ >= timer_interval_ms_) {
        last_timer_fire_ms_ = current;
        timer_callback_();
    }
}

// --------------------------------------------------------------------------
// run — the blocking reactor main loop
// --------------------------------------------------------------------------
void EventLoop::run() {
    running_.store(true);
    log::info("EventLoop started");

    struct epoll_event events[MAX_EVENTS];

    while (running_.load()) {
        // Compute epoll_wait timeout:
        // - If a timer is set, wake up at most timer_interval_ms_ ms later
        //   to ensure cron ticks fire on schedule even without client activity.
        // - 100ms default prevents indefinite blocking when no timer is set.
        int timeout_ms = (timer_interval_ms_ > 0) ? timer_interval_ms_ : 100;

        int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);

        if (n < 0) {
            if (errno == EINTR) {
                // Interrupted by a signal (e.g., SIGCHLD from RDB child).
                // Not an error — just loop and check running_.
                continue;
            }
            log::error("epoll_wait error: %s", strerror(errno));
            break;
        }

        // --- Dispatch ready events ---
        for (int i = 0; i < n; ++i) {
            const int  fd     = events[i].data.fd;
            const u32  evmask = events[i].events;

            auto it = handlers_.find(fd);
            if (it == handlers_.end()) continue;  // Stale event; fd already removed

            // Handle errors: treat as a read event so the handler can close cleanly
            if (evmask & (EPOLLERR | EPOLLHUP)) {
                if (it->second.on_readable) {
                    it->second.on_readable(fd);
                }
                continue;
            }

            // EPOLLIN: data available to read (or new connection on listen fd)
            if ((evmask & EPOLLIN) && it->second.on_readable) {
                it->second.on_readable(fd);
            }

            // EPOLLOUT: kernel send buffer has space; flush pending write data
            // Re-check the iterator because on_readable might have removed the fd
            it = handlers_.find(fd);
            if (it != handlers_.end() && (evmask & EPOLLOUT) && it->second.on_writable) {
                it->second.on_writable(fd);
            }
        }

        // --- Server cron tick (10Hz: expiry scan, incremental rehash) ---
        maybe_run_timer();
    }

    log::info("EventLoop stopped");
}

// --------------------------------------------------------------------------
// stop — signal the run() loop to exit
// --------------------------------------------------------------------------
void EventLoop::stop() noexcept {
    running_.store(false);
}

} // namespace keva::net
