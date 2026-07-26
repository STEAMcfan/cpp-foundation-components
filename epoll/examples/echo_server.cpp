#include "epoll_reactor.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#ifdef __linux__
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
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

    UniqueFd(UniqueFd&& other) noexcept
        : fd_(other.release()) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

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

std::runtime_error socketError(const char* operation) {
    return std::runtime_error(std::string(operation) + " failed: " + std::strerror(errno));
}

class EchoServer {
public:
    explicit EchoServer(std::uint16_t port)
        : reactor_(256) {
        listen_fd_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (listen_fd_.get() < 0) {
            throw socketError("socket");
        }

        const int enabled = 1;
        if (::setsockopt(listen_fd_.get(),
                         SOL_SOCKET,
                         SO_REUSEADDR,
                         &enabled,
                         sizeof(enabled)) < 0) {
            throw socketError("setsockopt SO_REUSEADDR");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);

        if (::bind(listen_fd_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw socketError("bind");
        }

        if (::listen(listen_fd_.get(), SOMAXCONN) < 0) {
            throw socketError("listen");
        }

        sockaddr_in actual{};
        socklen_t actual_len = sizeof(actual);
        if (::getsockname(listen_fd_.get(),
                          reinterpret_cast<sockaddr*>(&actual),
                          &actual_len) < 0) {
            throw socketError("getsockname");
        }
        port_ = ntohs(actual.sin_port);

        reactor_.add(
            listen_fd_.get(),
            foundation::EpollEvents::Read | foundation::EpollEvents::EdgeTriggered,
            [this](const foundation::EpollEvent&) {
                acceptClients();
            });
    }

    void run() {
        std::cout << "echo server listening on 127.0.0.1:" << port_ << "\n";
        std::cout << "try: nc 127.0.0.1 " << port_ << "\n";
        reactor_.run();
    }

private:
    struct Client {
        std::string out;
    };

    void acceptClients() {
        while (true) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            const int client_fd = ::accept4(listen_fd_.get(),
                                            reinterpret_cast<sockaddr*>(&client_addr),
                                            &client_len,
                                            SOCK_NONBLOCK | SOCK_CLOEXEC);

            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                if (errno == EINTR) {
                    continue;
                }
                throw socketError("accept4");
            }

            clients_.emplace(client_fd, Client{});
            reactor_.add(
                client_fd,
                foundation::EpollEvents::Read | foundation::EpollEvents::EdgeTriggered,
                [this](const foundation::EpollEvent& event) {
                    handleClient(event);
                });
        }
    }

    void handleClient(const foundation::EpollEvent& event) {
        const int fd = event.fd;

        if (event.error() || event.hangUp()) {
            closeClient(fd);
            return;
        }

        if (event.readable() || event.peerClosed()) {
            char buffer[4096];
            while (true) {
                const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
                if (n > 0) {
                    clients_[fd].out.append(buffer, static_cast<std::size_t>(n));
                    if (!flushClient(fd)) {
                        return;
                    }
                    continue;
                }
                if (n == 0) {
                    closeClient(fd);
                    return;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                closeClient(fd);
                return;
            }
        }

        if (event.writable() && !flushClient(fd)) {
            return;
        }

        const auto it = clients_.find(fd);
        if (it == clients_.end()) {
            return;
        }

        std::uint32_t interest =
            foundation::EpollEvents::Read | foundation::EpollEvents::EdgeTriggered;
        if (!it->second.out.empty()) {
            interest |= foundation::EpollEvents::Write;
        }
        reactor_.modify(fd, interest);
    }

    bool flushClient(int fd) {
        auto it = clients_.find(fd);
        if (it == clients_.end()) {
            return false;
        }

        auto& out = it->second.out;
        while (!out.empty()) {
            const auto n = ::send(fd, out.data(), out.size(), MSG_NOSIGNAL);
            if (n > 0) {
                out.erase(0, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            closeClient(fd);
            return false;
        }

        return true;
    }

    void closeClient(int fd) {
        reactor_.remove(fd);
        clients_.erase(fd);
        ::close(fd);
    }

    foundation::EpollReactor reactor_;
    UniqueFd listen_fd_;
    std::uint16_t port_ = 0;
    std::unordered_map<int, Client> clients_;
};

#endif

} // namespace

int main(int argc, char** argv) {
    if (!foundation::EpollReactor::isSupported()) {
        std::cout << "epoll echo server skipped: epoll is available on Linux only\n";
        return 0;
    }

#ifdef __linux__
    ::signal(SIGPIPE, SIG_IGN);

    std::uint16_t port = 9090;
    if (argc > 1) {
        port = static_cast<std::uint16_t>(std::strtoul(argv[1], nullptr, 10));
    }

    EchoServer server(port);
    server.run();
#else
    (void)argc;
    (void)argv;
#endif

    return 0;
}
