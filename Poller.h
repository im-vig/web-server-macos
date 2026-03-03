#ifndef POLLER_H
#define POLLER_H

#include <vector>
#include <unistd.h>
#include <errno.h>

#ifdef __linux__
#include <sys/epoll.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#else
#error "Unsupported platform: Poller only supports Linux (epoll) and macOS (kqueue)."
#endif

struct PollerEvent {
    int fd;
    bool readable;
    bool hangup;
    bool error;
};

class Poller {
public:
    Poller() : pollFd_(-1) {
#ifdef __linux__
        pollFd_ = epoll_create1(0);
#elif defined(__APPLE__)
        pollFd_ = kqueue();
#endif
    }

    ~Poller() {
        if (pollFd_ >= 0) {
            close(pollFd_);
        }
    }

    bool isValid() const { return pollFd_ >= 0; }

    bool addFd(int fd, bool oneshot) {
#ifdef __linux__
        return ctlEpoll_(EPOLL_CTL_ADD, fd, oneshot);
#elif defined(__APPLE__)
        return ctlKqueue_(fd, oneshot, false);
#endif
    }

    bool modFd(int fd, bool oneshot) {
#ifdef __linux__
        return ctlEpoll_(EPOLL_CTL_MOD, fd, oneshot);
#elif defined(__APPLE__)
        return ctlKqueue_(fd, oneshot, true);
#endif
    }

    bool delFd(int fd) {
#ifdef __linux__
        return epoll_ctl(pollFd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
#elif defined(__APPLE__)
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        if (kevent(pollFd_, &ev, 1, nullptr, 0, nullptr) == 0) return true;
        return errno == ENOENT || errno == EBADF;
#endif
    }

    int wait(int timeoutMs, std::vector<PollerEvent>& outEvents) {
        outEvents.clear();

#ifdef __linux__
        epoll_event events[1024];
        int n = epoll_wait(pollFd_, events, 1024, timeoutMs);
        if (n <= 0) return n;

        outEvents.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const uint32_t e = events[i].events;
            outEvents.push_back({
                events[i].data.fd,
                (e & EPOLLIN) != 0,
                (e & (EPOLLRDHUP | EPOLLHUP)) != 0,
                (e & EPOLLERR) != 0
            });
        }
        return n;
#elif defined(__APPLE__)
        struct kevent events[1024];
        struct timespec ts;
        struct timespec* tsp = nullptr;
        if (timeoutMs >= 0) {
            ts.tv_sec = timeoutMs / 1000;
            ts.tv_nsec = (timeoutMs % 1000) * 1000000;
            tsp = &ts;
        }

        int n = kevent(pollFd_, nullptr, 0, events, 1024, tsp);
        if (n <= 0) return n;

        outEvents.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const struct kevent& kev = events[i];
            bool readable = (kev.filter == EVFILT_READ) && ((kev.flags & EV_ERROR) == 0);
            bool hangup = (kev.flags & EV_EOF) != 0;
            bool error = (kev.flags & EV_ERROR) != 0;
            outEvents.push_back({static_cast<int>(kev.ident), readable, hangup, error});
        }
        return n;
#endif
    }

private:
#ifdef __linux__
    bool ctlEpoll_(int op, int fd, bool oneshot) {
        epoll_event ev;
        ev.data.fd = fd;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        if (oneshot) ev.events |= EPOLLONESHOT;
        return epoll_ctl(pollFd_, op, fd, &ev) == 0;
    }
#elif defined(__APPLE__)
    bool ctlKqueue_(int fd, bool oneshot, bool isModify) {
        struct kevent ev;
        uint16_t flags = EV_ADD | EV_ENABLE | EV_CLEAR;
        if (oneshot) flags |= EV_ONESHOT;
        if (isModify) flags |= EV_RECEIPT;
        EV_SET(&ev, fd, EVFILT_READ, flags, 0, 0, nullptr);
        if (kevent(pollFd_, &ev, 1, &ev, isModify ? 1 : 0, nullptr) == -1) return false;
        if (isModify && (ev.flags & EV_ERROR) && ev.data != 0) return false;
        return true;
    }
#endif

    int pollFd_;
};

#endif
