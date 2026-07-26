#include "multi_thread_reactor.hpp"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

#ifdef __linux__
#include <atomic>
#include <cerrno>
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#endif

namespace {

#ifdef __linux__

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd)
        : fd_(fd) {}

    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept
        : fd_(other.fd_) {
        other.fd_ = -1;
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.fd_);
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept {
        return fd_;
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

void dispatchesReadableEventOnWorker() {
    int fds[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0);
    UniqueFd left(fds[0]);
    UniqueFd right(fds[1]);

    foundation::MultiThreadReactor::Config config;
    config.worker_threads = 2;
    foundation::MultiThreadReactor reactor(config);
    reactor.start();

    std::mutex mutex;
    std::condition_variable cv;
    std::string received;
    std::thread::id callback_thread;

    reactor.add(
        left.get(),
        foundation::ReactorEvents::Read | foundation::ReactorEvents::EdgeTriggered,
        [&](foundation::EventLoop&, const foundation::ReactorEvent& event) {
            callback_thread = std::this_thread::get_id();
            char buffer[64];
            while (true) {
                const auto n = ::read(event.fd, buffer, sizeof(buffer));
                if (n > 0) {
                    std::lock_guard<std::mutex> lock(mutex);
                    received.append(buffer, static_cast<std::size_t>(n));
                    cv.notify_one();
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                break;
            }
        });

    const std::string message = "abc";
    assert(::write(right.get(), message.data(), message.size()) == 3);

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return received == message;
        }));
    }

    assert(callback_thread != std::this_thread::get_id());
    assert(reactor.remove(left.get()));
    reactor.stop();
}

void roundRobinDistributesFileDescriptors() {
    foundation::MultiThreadReactor::Config config;
    config.worker_threads = 2;
    foundation::MultiThreadReactor reactor(config);
    reactor.start();

    std::vector<UniqueFd> reads;
    std::vector<UniqueFd> writes;
    reads.reserve(4);
    writes.reserve(4);

    std::mutex mutex;
    std::condition_variable cv;
    std::set<std::size_t> loop_indexes;
    int callbacks = 0;

    for (int i = 0; i < 4; ++i) {
        int fds[2] = {-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0);
        reads.emplace_back(fds[0]);
        writes.emplace_back(fds[1]);

        reactor.add(
            reads.back().get(),
            foundation::ReactorEvents::Read | foundation::ReactorEvents::EdgeTriggered,
            [&](foundation::EventLoop&, const foundation::ReactorEvent& event) {
                char buffer[8];
                while (true) {
                    const auto n = ::read(event.fd, buffer, sizeof(buffer));
                    if (n > 0) {
                        std::lock_guard<std::mutex> lock(mutex);
                        loop_indexes.insert(event.loop_index);
                        ++callbacks;
                        cv.notify_one();
                        continue;
                    }
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }
                    break;
                }
            });
    }

    for (auto& fd : writes) {
        assert(::write(fd.get(), "x", 1) == 1);
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return callbacks == 4;
        }));
    }

    assert(loop_indexes.size() == 2);
    for (auto& fd : reads) {
        assert(reactor.remove(fd.get()));
    }
    reactor.stop();
}

void modifyAndRemoveWorkAcrossThreads() {
    int fds[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0);
    UniqueFd left(fds[0]);
    UniqueFd right(fds[1]);

    foundation::MultiThreadReactor::Config config;
    config.worker_threads = 1;
    foundation::MultiThreadReactor reactor(config);
    reactor.start();

    std::mutex mutex;
    std::condition_variable cv;
    bool writable = false;

    reactor.add(left.get(), foundation::ReactorEvents::Read, [&](foundation::EventLoop&, const foundation::ReactorEvent& event) {
        if (event.writable()) {
            std::lock_guard<std::mutex> lock(mutex);
            writable = true;
            cv.notify_one();
        }
    });

    reactor.modify(left.get(), foundation::ReactorEvents::Write);

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return writable;
        }));
    }

    assert(reactor.remove(left.get()));
    assert(!reactor.remove(left.get()));
    reactor.stop();
}

void stopWakesWorkerThreads() {
    foundation::MultiThreadReactor::Config config;
    config.worker_threads = 2;
    foundation::MultiThreadReactor reactor(config);

    reactor.start();
    assert(reactor.running());
    reactor.stop();
    assert(!reactor.running());
}

#else

void unsupportedPlatformThrows() {
    assert(!foundation::MultiThreadReactor::isSupported());

    bool threw = false;
    try {
        foundation::MultiThreadReactor reactor;
        (void)reactor;
    } catch (const std::runtime_error&) {
        threw = true;
    }

    assert(threw);
}

#endif

} // namespace

int main() {
#ifdef __linux__
    assert(foundation::MultiThreadReactor::isSupported());
    dispatchesReadableEventOnWorker();
    roundRobinDistributesFileDescriptors();
    modifyAndRemoveWorkAcrossThreads();
    stopWakesWorkerThreads();
#else
    unsupportedPlatformThrows();
#endif

    return 0;
}
