#include "epoll_reactor.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace foundation {
namespace {

std::runtime_error systemError(const char* operation) {
    return std::runtime_error(std::string(operation) + " failed: " + std::strerror(errno));
}

#ifdef __linux__

std::uint32_t toNativeEvents(std::uint32_t events) {
    std::uint32_t native_events = EPOLLRDHUP;

    if ((events & EpollEvents::Read) != 0) {
        native_events |= EPOLLIN;
    }
    if ((events & EpollEvents::Write) != 0) {
        native_events |= EPOLLOUT;
    }
    if ((events & EpollEvents::EdgeTriggered) != 0) {
        native_events |= EPOLLET;
    }
    if ((events & EpollEvents::OneShot) != 0) {
        native_events |= EPOLLONESHOT;
    }

    return native_events;
}

std::uint32_t fromNativeEvents(std::uint32_t events) {
    std::uint32_t translated = EpollEvents::None;

    if ((events & EPOLLIN) != 0) {
        translated |= EpollEvents::Read;
    }
    if ((events & EPOLLOUT) != 0) {
        translated |= EpollEvents::Write;
    }
    if ((events & EPOLLERR) != 0) {
        translated |= EpollEvents::Error;
    }
    if ((events & EPOLLHUP) != 0) {
        translated |= EpollEvents::HangUp;
    }
    if ((events & EPOLLRDHUP) != 0) {
        translated |= EpollEvents::PeerClosed;
    }

    return translated;
}

void fillNativeEvent(epoll_event& event, int fd, std::uint32_t events) {
    event.events = toNativeEvents(events);
    event.data.fd = fd;
}

int timeoutToMilliseconds(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        return -1;
    }

    const auto max_timeout =
        static_cast<long long>(std::numeric_limits<int>::max());
    if (timeout.count() > max_timeout) {
        return std::numeric_limits<int>::max();
    }

    return static_cast<int>(timeout.count());
}

#endif

} // namespace

struct EpollReactor::State {
    struct Watcher {
        std::uint32_t events = EpollEvents::None;
        Callback callback;
    };

    explicit State(std::size_t max_events) {
        if (max_events == 0) {
            throw std::invalid_argument("EpollReactor max_events must be positive");
        }

#ifdef __linux__
        epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0) {
            throw systemError("epoll_create1");
        }

        wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd < 0) {
            const int saved_errno = errno;
            ::close(epoll_fd);
            epoll_fd = -1;
            errno = saved_errno;
            throw systemError("eventfd");
        }

        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = wake_fd;
        if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &event) < 0) {
            const int saved_errno = errno;
            ::close(wake_fd);
            ::close(epoll_fd);
            wake_fd = -1;
            epoll_fd = -1;
            errno = saved_errno;
            throw systemError("epoll_ctl wake_fd add");
        }

        ready.resize(max_events);
#else
        (void)max_events;
        throw std::runtime_error("EpollReactor is only supported on Linux");
#endif
    }

    ~State() {
#ifdef __linux__
        if (wake_fd >= 0) {
            ::close(wake_fd);
        }
        if (epoll_fd >= 0) {
            ::close(epoll_fd);
        }
#endif
    }

    void wake() noexcept {
#ifdef __linux__
        if (wake_fd < 0) {
            return;
        }

        std::uint64_t value = 1;
        const auto ignored = ::write(wake_fd, &value, sizeof(value));
        (void)ignored;
#endif
    }

    void drainWake() noexcept {
#ifdef __linux__
        std::uint64_t value = 0;
        while (::read(wake_fd, &value, sizeof(value)) == sizeof(value)) {
        }
#endif
    }

#ifdef __linux__
    int epoll_fd = -1;
    int wake_fd = -1;
    std::vector<epoll_event> ready;
#endif

    mutable std::mutex mutex;
    std::unordered_map<int, Watcher> watchers;
    std::atomic<bool> stopping{false};
};

EpollReactor::EpollReactor(std::size_t max_events)
    : state_(std::make_shared<State>(max_events)) {}

EpollReactor::~EpollReactor() = default;

bool EpollReactor::isSupported() noexcept {
#ifdef __linux__
    return true;
#else
    return false;
#endif
}

void EpollReactor::add(int fd, std::uint32_t events, Callback callback) {
    if (fd < 0) {
        throw std::invalid_argument("EpollReactor cannot add a negative fd");
    }
    if (!callback) {
        throw std::invalid_argument("EpollReactor callback cannot be empty");
    }

#ifdef __linux__
    if (fd == state_->wake_fd) {
        throw std::invalid_argument("EpollReactor wake fd is reserved");
    }

    epoll_event event{};
    fillNativeEvent(event, fd, events);

    std::unique_lock<std::mutex> lock(state_->mutex);
    auto [it, inserted] = state_->watchers.emplace(fd, State::Watcher{events, std::move(callback)});
    if (!inserted) {
        throw std::invalid_argument("EpollReactor fd is already registered");
    }

    if (::epoll_ctl(state_->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        const int saved_errno = errno;
        state_->watchers.erase(it);
        errno = saved_errno;
        throw systemError("epoll_ctl add");
    }
#else
    (void)events;
    (void)callback;
    throw std::runtime_error("EpollReactor is only supported on Linux");
#endif
}

void EpollReactor::modify(int fd, std::uint32_t events) {
#ifdef __linux__
    epoll_event event{};
    fillNativeEvent(event, fd, events);

    std::unique_lock<std::mutex> lock(state_->mutex);
    auto it = state_->watchers.find(fd);
    if (it == state_->watchers.end()) {
        throw std::invalid_argument("EpollReactor fd is not registered");
    }

    if (::epoll_ctl(state_->epoll_fd, EPOLL_CTL_MOD, fd, &event) < 0) {
        throw systemError("epoll_ctl modify");
    }
    it->second.events = events;
#else
    (void)fd;
    (void)events;
    throw std::runtime_error("EpollReactor is only supported on Linux");
#endif
}

bool EpollReactor::remove(int fd) {
#ifdef __linux__
    std::unique_lock<std::mutex> lock(state_->mutex);
    const auto it = state_->watchers.find(fd);
    if (it == state_->watchers.end()) {
        return false;
    }

    const int rc = ::epoll_ctl(state_->epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    const int saved_errno = errno;
    state_->watchers.erase(it);

    if (rc < 0 && saved_errno != ENOENT && saved_errno != EBADF) {
        errno = saved_errno;
        throw systemError("epoll_ctl remove");
    }

    return true;
#else
    (void)fd;
    throw std::runtime_error("EpollReactor is only supported on Linux");
#endif
}

int EpollReactor::runOnce(std::chrono::milliseconds timeout) {
#ifdef __linux__
    const int timeout_ms = timeoutToMilliseconds(timeout);
    int ready_count = 0;

    do {
        ready_count = ::epoll_wait(state_->epoll_fd,
                                   state_->ready.data(),
                                   static_cast<int>(state_->ready.size()),
                                   timeout_ms);
    } while (ready_count < 0 && errno == EINTR);

    if (ready_count < 0) {
        throw systemError("epoll_wait");
    }

    std::vector<std::pair<EpollEvent, Callback>> callbacks;
    callbacks.reserve(static_cast<std::size_t>(ready_count));

    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        for (int i = 0; i < ready_count; ++i) {
            const auto& native_event = state_->ready[static_cast<std::size_t>(i)];
            const int fd = native_event.data.fd;

            if (fd == state_->wake_fd) {
                state_->drainWake();
                continue;
            }

            const auto it = state_->watchers.find(fd);
            if (it == state_->watchers.end()) {
                continue;
            }

            callbacks.push_back({
                EpollEvent{fd, fromNativeEvents(native_event.events)},
                it->second.callback,
            });
        }
    }

    for (const auto& item : callbacks) {
        item.second(item.first);
    }

    return static_cast<int>(callbacks.size());
#else
    (void)timeout;
    throw std::runtime_error("EpollReactor is only supported on Linux");
#endif
}

void EpollReactor::run() {
    state_->stopping.store(false);
    while (!state_->stopping.load()) {
        runOnce(std::chrono::milliseconds{-1});
    }
}

void EpollReactor::stop() noexcept {
    state_->stopping.store(true);
    state_->wake();
}

bool EpollReactor::contains(int fd) const {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->watchers.find(fd) != state_->watchers.end();
}

std::size_t EpollReactor::size() const {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->watchers.size();
}

bool setNonBlocking(int fd) {
#ifdef __linux__
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#else
    (void)fd;
    return false;
#endif
}

} // namespace foundation
