#include "epoll_reactor.hpp"

#include <cassert>
#include <chrono>
#include <stdexcept>

#ifdef __linux__
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
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

void dispatchesReadablePipeEvent() {
    int fds[2] = {-1, -1};
    assert(::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
    UniqueFd read_fd(fds[0]);
    UniqueFd write_fd(fds[1]);

    foundation::EpollReactor reactor;
    std::string received;

    reactor.add(
        read_fd.get(),
        foundation::EpollEvents::Read | foundation::EpollEvents::EdgeTriggered,
        [&](const foundation::EpollEvent& event) {
            char buffer[64];
            while (true) {
                const auto n = ::read(event.fd, buffer, sizeof(buffer));
                if (n > 0) {
                    received.append(buffer, static_cast<std::size_t>(n));
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                break;
            }
        });

    const std::string message = "abc";
    assert(::write(write_fd.get(), message.data(), message.size()) == 3);
    assert(reactor.runOnce(std::chrono::milliseconds{100}) == 1);
    assert(received == "abc");
}

void modifyChangesInterest() {
    int fds[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0);
    UniqueFd left(fds[0]);
    UniqueFd right(fds[1]);

    foundation::EpollReactor reactor;
    bool writable = false;

    reactor.add(left.get(), foundation::EpollEvents::Read, [&](const foundation::EpollEvent& event) {
        writable = event.writable();
    });

    assert(reactor.runOnce(std::chrono::milliseconds{10}) == 0);
    reactor.modify(left.get(), foundation::EpollEvents::Write);
    assert(reactor.runOnce(std::chrono::milliseconds{100}) == 1);
    assert(writable);
}

void removePreventsDispatch() {
    int fds[2] = {-1, -1};
    assert(::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
    UniqueFd read_fd(fds[0]);
    UniqueFd write_fd(fds[1]);

    foundation::EpollReactor reactor;
    int calls = 0;

    reactor.add(read_fd.get(), foundation::EpollEvents::Read, [&](const foundation::EpollEvent&) {
        ++calls;
    });
    assert(reactor.remove(read_fd.get()));

    const std::string message = "x";
    assert(::write(write_fd.get(), message.data(), message.size()) == 1);
    assert(reactor.runOnce(std::chrono::milliseconds{20}) == 0);
    assert(calls == 0);
}

void stopWakesRunLoop() {
    foundation::EpollReactor reactor;
    std::atomic<bool> done{false};

    std::thread worker([&] {
        reactor.run();
        done.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    reactor.stop();
    worker.join();

    assert(done.load());
}

#else

void unsupportedPlatformThrows() {
    assert(!foundation::EpollReactor::isSupported());

    bool threw = false;
    try {
        foundation::EpollReactor reactor;
    } catch (const std::runtime_error&) {
        threw = true;
    }

    assert(threw);
}

#endif

} // namespace

int main() {
#ifdef __linux__
    assert(foundation::EpollReactor::isSupported());
    dispatchesReadablePipeEvent();
    modifyChangesInterest();
    removePreventsDispatch();
    stopWakesRunLoop();
#else
    unsupportedPlatformThrows();
#endif

    return 0;
}
