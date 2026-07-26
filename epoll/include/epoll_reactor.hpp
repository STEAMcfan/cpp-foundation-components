#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace foundation {

struct EpollEvents {
    static constexpr std::uint32_t None = 0;
    static constexpr std::uint32_t Read = 1u << 0;
    static constexpr std::uint32_t Write = 1u << 1;
    static constexpr std::uint32_t EdgeTriggered = 1u << 2;
    static constexpr std::uint32_t OneShot = 1u << 3;
    static constexpr std::uint32_t Error = 1u << 4;
    static constexpr std::uint32_t HangUp = 1u << 5;
    static constexpr std::uint32_t PeerClosed = 1u << 6;
};

struct EpollEvent {
    int fd = -1;
    std::uint32_t events = EpollEvents::None;

    bool readable() const noexcept {
        return (events & EpollEvents::Read) != 0;
    }

    bool writable() const noexcept {
        return (events & EpollEvents::Write) != 0;
    }

    bool error() const noexcept {
        return (events & EpollEvents::Error) != 0;
    }

    bool hangUp() const noexcept {
        return (events & EpollEvents::HangUp) != 0;
    }

    bool peerClosed() const noexcept {
        return (events & EpollEvents::PeerClosed) != 0;
    }
};

class EpollReactor {
public:
    using Callback = std::function<void(const EpollEvent&)>;

    explicit EpollReactor(std::size_t max_events = 64);
    ~EpollReactor();

    EpollReactor(const EpollReactor&) = delete;
    EpollReactor& operator=(const EpollReactor&) = delete;
    EpollReactor(EpollReactor&&) = delete;
    EpollReactor& operator=(EpollReactor&&) = delete;

    static bool isSupported() noexcept;

    void add(int fd, std::uint32_t events, Callback callback);
    void modify(int fd, std::uint32_t events);
    bool remove(int fd);

    int runOnce(std::chrono::milliseconds timeout);
    void run();
    void stop() noexcept;

    bool contains(int fd) const;
    std::size_t size() const;

private:
    struct State;

    std::shared_ptr<State> state_;
};

bool setNonBlocking(int fd);

} // namespace foundation
