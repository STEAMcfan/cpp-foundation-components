#include "multi_thread_reactor.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
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

    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
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

struct Connection {
    explicit Connection(int input_fd)
        : fd(input_fd) {}

    ~Connection() {
        if (!closed.exchange(true) && fd >= 0) {
            ::close(fd);
        }
    }

    int fd = -1;
    std::string output;
    std::atomic<bool> closed{false};
};

void closeConnection(foundation::MultiThreadReactor& reactor, const std::shared_ptr<Connection>& connection) {
    if (connection->closed.exchange(true)) {
        return;
    }

    reactor.remove(connection->fd);
    ::close(connection->fd);
}

bool flushOutput(foundation::MultiThreadReactor& reactor, const std::shared_ptr<Connection>& connection) {
    while (!connection->output.empty()) {
        const auto n = ::send(connection->fd,
                              connection->output.data(),
                              connection->output.size(),
#ifdef MSG_NOSIGNAL
                              MSG_NOSIGNAL
#else
                              0
#endif
        );

        if (n > 0) {
            connection->output.erase(0, static_cast<std::size_t>(n));
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }

        closeConnection(reactor, connection);
        return false;
    }

    return true;
}

void handleConnection(foundation::MultiThreadReactor& reactor,
                      const foundation::ReactorEvent& event,
                      const std::shared_ptr<Connection>& connection) {
    if (event.error() || event.hangUp() || event.peerClosed()) {
        closeConnection(reactor, connection);
        return;
    }

    if (event.readable()) {
        char buffer[4096];
        while (true) {
            const auto n = ::recv(connection->fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                connection->output.append(buffer, static_cast<std::size_t>(n));
                continue;
            }

            if (n == 0) {
                closeConnection(reactor, connection);
                return;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            closeConnection(reactor, connection);
            return;
        }
    }

    if (event.writable() || !connection->output.empty()) {
        if (!flushOutput(reactor, connection)) {
            return;
        }
    }

    std::uint32_t interests = foundation::ReactorEvents::Read |
                              foundation::ReactorEvents::EdgeTriggered;
    if (!connection->output.empty()) {
        interests |= foundation::ReactorEvents::Write;
    }
    reactor.modify(connection->fd, interests);
}

int createListenSocket(int port) {
    UniqueFd listen_fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (listen_fd.get() < 0) {
        throw std::runtime_error("socket failed");
    }

    int on = 1;
    if (::setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
        throw std::runtime_error("setsockopt SO_REUSEADDR failed");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(port));

    if (::bind(listen_fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        throw std::runtime_error("bind failed");
    }

    if (::listen(listen_fd.get(), SOMAXCONN) != 0) {
        throw std::runtime_error("listen failed");
    }

    return listen_fd.release();
}

#endif

} // namespace

int main(int argc, char** argv) {
    if (!foundation::MultiThreadReactor::isSupported()) {
        std::cout << "multi_thread_reactor echo server is only supported on Linux\n";
        return 0;
    }

#ifdef __linux__
    const int port = argc > 1 ? std::atoi(argv[1]) : 9090;

    try {
        UniqueFd listen_fd(createListenSocket(port));

        foundation::MultiThreadReactor::Config worker_config;
        worker_config.worker_threads = 4;
        foundation::MultiThreadReactor workers(worker_config);
        workers.start();

        foundation::EventLoop accept_loop;
        accept_loop.add(
            listen_fd.get(),
            foundation::ReactorEvents::Read | foundation::ReactorEvents::EdgeTriggered,
            [&](foundation::EventLoop&, const foundation::ReactorEvent&) {
                while (true) {
                    int connfd = ::accept4(listen_fd.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (connfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        std::cerr << "accept failed: " << std::strerror(errno) << "\n";
                        continue;
                    }

                    auto connection = std::make_shared<Connection>(connfd);
                    workers.add(
                        connfd,
                        foundation::ReactorEvents::Read | foundation::ReactorEvents::EdgeTriggered,
                        [&workers, connection](foundation::EventLoop&, const foundation::ReactorEvent& event) {
                            handleConnection(workers, event, connection);
                        });
                }
            });

        std::cout << "multi-thread echo server listening on 0.0.0.0:" << port << "\n";
        std::cout << "try: nc 127.0.0.1 " << port << "\n";
        accept_loop.run();
        workers.stop();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
#endif
}
