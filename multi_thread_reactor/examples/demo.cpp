#include "multi_thread_reactor.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

#ifdef __linux__
#include <cerrno>
#include <sys/socket.h>
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

#endif

} // namespace

int main() {
    if (!foundation::MultiThreadReactor::isSupported()) {
        std::cout << "multi_thread_reactor demo is only supported on Linux\n";
        return 0;
    }

#ifdef __linux__
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0) {
        std::cerr << "socketpair failed\n";
        return 1;
    }

    UniqueFd left(fds[0]);
    UniqueFd right(fds[1]);

    foundation::MultiThreadReactor::Config config;
    config.worker_threads = 2;

    foundation::MultiThreadReactor reactor(config);
    reactor.start();

    std::mutex mutex;
    std::condition_variable cv;
    std::string received;
    bool done = false;
    std::size_t owner_loop = reactor.add(
        left.get(),
        foundation::ReactorEvents::Read | foundation::ReactorEvents::EdgeTriggered,
        [&](foundation::EventLoop&, const foundation::ReactorEvent& event) {
            char buffer[128];
            while (true) {
                const auto n = ::read(event.fd, buffer, sizeof(buffer));
                if (n > 0) {
                    std::lock_guard<std::mutex> lock(mutex);
                    received.append(buffer, static_cast<std::size_t>(n));
                    done = true;
                    cv.notify_one();
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                break;
            }
        });

    const std::string message = "hello reactor";
    const auto written = ::write(right.get(), message.data(), message.size());
    if (written < 0) {
        std::cerr << "write failed\n";
        reactor.stop();
        return 1;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return done;
        });
    }

    reactor.remove(left.get());
    reactor.stop();

    std::cout << "worker loop " << owner_loop << " received: " << received << "\n";
    return received == message ? 0 : 1;
#endif
}
