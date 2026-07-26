#include "epoll_reactor.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef __linux__
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

int main() {
    if (!foundation::EpollReactor::isSupported()) {
        std::cout << "epoll demo skipped: epoll is available on Linux only\n";
        return 0;
    }

#ifdef __linux__
    int pipe_fds[2] = {-1, -1};
    if (::pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) < 0) {
        throw std::runtime_error("pipe2 failed");
    }

    foundation::EpollReactor reactor;
    std::string received;

    reactor.add(
        pipe_fds[0],
        foundation::EpollEvents::Read | foundation::EpollEvents::EdgeTriggered,
        [&](const foundation::EpollEvent& event) {
            char buffer[128];
            while (true) {
                const auto n = ::read(event.fd, buffer, sizeof(buffer));
                if (n > 0) {
                    received.append(buffer, static_cast<std::size_t>(n));
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                reactor.remove(event.fd);
                break;
            }
        });

    const std::string message = "hello epoll\n";
    const auto written = ::write(pipe_fds[1], message.data(), message.size());
    if (written < 0) {
        throw std::runtime_error("write failed");
    }

    reactor.runOnce(std::chrono::milliseconds{100});

    std::cout << "received from pipe: " << received;

    reactor.remove(pipe_fds[0]);
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
#endif

    return 0;
}
