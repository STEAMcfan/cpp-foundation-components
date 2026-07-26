#include "multi_thread_reactor.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

    if ((events & ReactorEvents::Read) != 0) {
        native_events |= EPOLLIN;
    }
    if ((events & ReactorEvents::Write) != 0) {
        native_events |= EPOLLOUT;
    }
    if ((events & ReactorEvents::EdgeTriggered) != 0) {
        native_events |= EPOLLET;
    }
    if ((events & ReactorEvents::OneShot) != 0) {
        native_events |= EPOLLONESHOT;
    }

    return native_events;
}

std::uint32_t fromNativeEvents(std::uint32_t events) {
    std::uint32_t translated = ReactorEvents::None;

    if ((events & EPOLLIN) != 0) {
        translated |= ReactorEvents::Read;
    }
    if ((events & EPOLLOUT) != 0) {
        translated |= ReactorEvents::Write;
    }
    if ((events & EPOLLERR) != 0) {
        translated |= ReactorEvents::Error;
    }
    if ((events & EPOLLHUP) != 0) {
        translated |= ReactorEvents::HangUp;
    }
    if ((events & EPOLLRDHUP) != 0) {
        translated |= ReactorEvents::PeerClosed;
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

    const auto max_timeout = static_cast<long long>(std::numeric_limits<int>::max());
    if (timeout.count() > max_timeout) {
        return std::numeric_limits<int>::max();
    }

    return static_cast<int>(timeout.count());
}

#endif

std::size_t normalizeWorkerCount(std::size_t configured) noexcept {
    if (configured > 0) {
        return configured;
    }

    const auto hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
        return 2;
    }

    return static_cast<std::size_t>(hardware);
}

} // namespace

struct EventLoop::State {
    struct Watcher {
        std::uint32_t events = ReactorEvents::None;
        Callback callback;
    };

    State(std::size_t max_events, std::size_t loop_index)
        : index(loop_index) {
        if (max_events == 0) {
            throw std::invalid_argument("EventLoop max_events must be positive");
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
        throw std::runtime_error("MultiThreadReactor is only supported on Linux");
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
        if (wake_fd < 0) {
            return;
        }

        std::uint64_t value = 0;
        while (::read(wake_fd, &value, sizeof(value)) == sizeof(value)) {
        }
#endif
    }

    std::size_t index = 0;

#ifdef __linux__
    int epoll_fd = -1;
    int wake_fd = -1;
    std::vector<epoll_event> ready;
#endif

    mutable std::mutex mutex;
    std::unordered_map<int, Watcher> watchers;
    std::atomic<bool> stopping{false};
};

EventLoop::EventLoop(std::size_t max_events, std::size_t index)
    : state_(std::make_shared<State>(max_events, index)) {}

EventLoop::~EventLoop() = default;

bool EventLoop::isSupported() noexcept {
#ifdef __linux__
    return true;
#else
    return false;
#endif
}

void EventLoop::add(int fd, std::uint32_t events, Callback callback) {
    if (fd < 0) {
        throw std::invalid_argument("EventLoop cannot add a negative fd");
    }
    if (!callback) {
        throw std::invalid_argument("EventLoop callback cannot be empty");
    }

#ifdef __linux__
    if (fd == state_->wake_fd) {
        throw std::invalid_argument("EventLoop wake fd is reserved");
    }

    epoll_event event{};
    fillNativeEvent(event, fd, events);

    std::unique_lock<std::mutex> lock(state_->mutex);
    auto [it, inserted] = state_->watchers.emplace(fd, State::Watcher{events, std::move(callback)});
    if (!inserted) {
        throw std::invalid_argument("EventLoop fd is already registered");
    }

    if (::epoll_ctl(state_->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        const int saved_errno = errno;
        state_->watchers.erase(it);
        errno = saved_errno;
        throw systemError("epoll_ctl add");
    }

    lock.unlock();
    wake();
#else
    (void)events;
    (void)callback;
    throw std::runtime_error("MultiThreadReactor is only supported on Linux");
#endif
}

void EventLoop::modify(int fd, std::uint32_t events) {
#ifdef __linux__
    epoll_event event{};
    fillNativeEvent(event, fd, events);

    std::unique_lock<std::mutex> lock(state_->mutex);
    auto it = state_->watchers.find(fd);
    if (it == state_->watchers.end()) {
        throw std::invalid_argument("EventLoop fd is not registered");
    }

    if (::epoll_ctl(state_->epoll_fd, EPOLL_CTL_MOD, fd, &event) < 0) {
        throw systemError("epoll_ctl modify");
    }
    it->second.events = events;

    lock.unlock();
    wake();
#else
    (void)fd;
    (void)events;
    throw std::runtime_error("MultiThreadReactor is only supported on Linux");
#endif
}

bool EventLoop::remove(int fd) {
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

    lock.unlock();
    wake();
    return true;
#else
    (void)fd;
    throw std::runtime_error("MultiThreadReactor is only supported on Linux");
#endif
}

int EventLoop::runOnce(std::chrono::milliseconds timeout) {
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

    std::vector<std::pair<ReactorEvent, Callback>> callbacks;
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
                ReactorEvent{fd, fromNativeEvents(native_event.events), state_->index},
                it->second.callback,
            });
        }
    }

    for (const auto& item : callbacks) {
        item.second(*this, item.first);
    }

    return static_cast<int>(callbacks.size());
#else
    (void)timeout;
    throw std::runtime_error("MultiThreadReactor is only supported on Linux");
#endif
}

void EventLoop::run() {
    while (!state_->stopping.load()) {
        runOnce(std::chrono::milliseconds{-1});
    }
}

void EventLoop::stop() noexcept {
    state_->stopping.store(true);
    wake();
}

void EventLoop::wake() noexcept {
    state_->wake();
}

bool EventLoop::contains(int fd) const {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->watchers.find(fd) != state_->watchers.end();
}

std::size_t EventLoop::size() const {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return state_->watchers.size();
}

std::size_t EventLoop::index() const noexcept {
    return state_->index;
}

struct MultiThreadReactor::State {
    explicit State(Config input_config)
        : config(input_config) {
        config.worker_threads = normalizeWorkerCount(config.worker_threads);
        if (config.max_events_per_loop == 0) {
            throw std::invalid_argument("MultiThreadReactor max_events_per_loop must be positive");
        }

        loops.reserve(config.worker_threads);
        for (std::size_t i = 0; i < config.worker_threads; ++i) {
            loops.push_back(std::make_unique<EventLoop>(config.max_events_per_loop, i));
        }
    }

    Config config;
    std::vector<std::unique_ptr<EventLoop>> loops;
    std::vector<std::thread> threads;

    mutable std::mutex owner_mutex;
    std::unordered_map<int, std::size_t> owners;

    mutable std::mutex lifecycle_mutex;
    std::atomic<bool> is_running{false};
    bool stopped_after_start = false;

    std::atomic<std::size_t> next_loop{0};
};

MultiThreadReactor::MultiThreadReactor()
    : MultiThreadReactor(Config{}) {}

MultiThreadReactor::MultiThreadReactor(Config config)
    : state_(std::make_shared<State>(config)) {}

MultiThreadReactor::~MultiThreadReactor() {
    stop();
}

bool MultiThreadReactor::isSupported() noexcept {
    return EventLoop::isSupported();
}

std::size_t MultiThreadReactor::suggestedWorkerCount() noexcept {
    return normalizeWorkerCount(0);
}

void MultiThreadReactor::start() {
    std::unique_lock<std::mutex> lock(state_->lifecycle_mutex);
    if (state_->is_running.load()) {
        return;
    }
    if (state_->stopped_after_start) {
        throw std::runtime_error("MultiThreadReactor cannot be restarted after stop");
    }

    state_->is_running.store(true);
    try {
        state_->threads.reserve(state_->loops.size());
        for (const auto& loop : state_->loops) {
            state_->threads.emplace_back([event_loop = loop.get()] {
                event_loop->run();
            });
        }
    } catch (...) {
        for (const auto& loop : state_->loops) {
            loop->stop();
        }
        for (auto& thread : state_->threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        state_->threads.clear();
        state_->is_running.store(false);
        state_->stopped_after_start = true;
        throw;
    }
}

void MultiThreadReactor::stop() noexcept {
    std::unique_lock<std::mutex> lock(state_->lifecycle_mutex);
    if (!state_->is_running.load()) {
        return;
    }

    state_->is_running.store(false);
    state_->stopped_after_start = true;

    for (const auto& loop : state_->loops) {
        loop->stop();
    }

    const auto self = std::this_thread::get_id();
    for (auto& thread : state_->threads) {
        if (!thread.joinable()) {
            continue;
        }
        if (thread.get_id() == self) {
            thread.detach();
        } else {
            thread.join();
        }
    }
    state_->threads.clear();
}

bool MultiThreadReactor::running() const noexcept {
    return state_->is_running.load();
}

std::size_t MultiThreadReactor::add(int fd, std::uint32_t events, Callback callback) {
    if (fd < 0) {
        throw std::invalid_argument("MultiThreadReactor cannot add a negative fd");
    }
    if (!callback) {
        throw std::invalid_argument("MultiThreadReactor callback cannot be empty");
    }
    if (!running()) {
        throw std::runtime_error("MultiThreadReactor must be started before add");
    }

    const auto loop_index = state_->next_loop.fetch_add(1) % state_->loops.size();

    {
        std::unique_lock<std::mutex> lock(state_->owner_mutex);
        if (state_->owners.find(fd) != state_->owners.end()) {
            throw std::invalid_argument("MultiThreadReactor fd is already registered");
        }
        state_->owners.emplace(fd, loop_index);
    }

    try {
        state_->loops[loop_index]->add(fd, events, std::move(callback));
    } catch (...) {
        std::unique_lock<std::mutex> lock(state_->owner_mutex);
        state_->owners.erase(fd);
        throw;
    }

    return loop_index;
}

void MultiThreadReactor::modify(int fd, std::uint32_t events) {
    std::size_t loop_index = 0;
    {
        std::unique_lock<std::mutex> lock(state_->owner_mutex);
        const auto it = state_->owners.find(fd);
        if (it == state_->owners.end()) {
            throw std::invalid_argument("MultiThreadReactor fd is not registered");
        }
        loop_index = it->second;
    }

    state_->loops[loop_index]->modify(fd, events);
}

bool MultiThreadReactor::remove(int fd) {
    std::size_t loop_index = 0;
    {
        std::unique_lock<std::mutex> lock(state_->owner_mutex);
        const auto it = state_->owners.find(fd);
        if (it == state_->owners.end()) {
            return false;
        }
        loop_index = it->second;
        state_->owners.erase(it);
    }

    return state_->loops[loop_index]->remove(fd);
}

std::size_t MultiThreadReactor::workerCount() const noexcept {
    return state_->loops.size();
}

std::size_t MultiThreadReactor::size() const {
    std::unique_lock<std::mutex> lock(state_->owner_mutex);
    return state_->owners.size();
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
