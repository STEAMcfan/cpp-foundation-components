#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace foundation {

struct ReactorEvents {
    static constexpr std::uint32_t None = 0;
    static constexpr std::uint32_t Read = 1u << 0;
    static constexpr std::uint32_t Write = 1u << 1;
    static constexpr std::uint32_t EdgeTriggered = 1u << 2;
    static constexpr std::uint32_t OneShot = 1u << 3;
    static constexpr std::uint32_t Error = 1u << 4;
    static constexpr std::uint32_t HangUp = 1u << 5;
    static constexpr std::uint32_t PeerClosed = 1u << 6;
};

struct ReactorEvent {
    int fd = -1;
    std::uint32_t events = ReactorEvents::None;
    std::size_t loop_index = 0;

    bool readable() const noexcept {
        return (events & ReactorEvents::Read) != 0;
    }

    bool writable() const noexcept {
        return (events & ReactorEvents::Write) != 0;
    }

    bool error() const noexcept {
        return (events & ReactorEvents::Error) != 0;
    }

    bool hangUp() const noexcept {
        return (events & ReactorEvents::HangUp) != 0;
    }

    bool peerClosed() const noexcept {
        return (events & ReactorEvents::PeerClosed) != 0;
    }
};

class EventLoop {
public:
    using Callback = std::function<void(EventLoop&, const ReactorEvent&)>;

    explicit EventLoop(std::size_t max_events = 128, std::size_t index = 0);
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    static bool isSupported() noexcept;

    void add(int fd, std::uint32_t events, Callback callback);
    void modify(int fd, std::uint32_t events);
    bool remove(int fd);

    int runOnce(std::chrono::milliseconds timeout);
    void run();
    void stop() noexcept;
    void wake() noexcept;

    bool contains(int fd) const;
    std::size_t size() const;
    std::size_t index() const noexcept;

private:
    struct State;

    std::shared_ptr<State> state_;
};

class MultiThreadReactor {
public:
    using Callback = EventLoop::Callback;

    struct Config {
        std::size_t worker_threads = 0;
        std::size_t max_events_per_loop = 128;
    };

    MultiThreadReactor();
    explicit MultiThreadReactor(Config config);
    ~MultiThreadReactor();

    MultiThreadReactor(const MultiThreadReactor&) = delete;
    MultiThreadReactor& operator=(const MultiThreadReactor&) = delete;
    MultiThreadReactor(MultiThreadReactor&&) = delete;
    MultiThreadReactor& operator=(MultiThreadReactor&&) = delete;

    static bool isSupported() noexcept;
    static std::size_t suggestedWorkerCount() noexcept;

    void start();
    void stop() noexcept;
    bool running() const noexcept;

    std::size_t add(int fd, std::uint32_t events, Callback callback);
    void modify(int fd, std::uint32_t events);
    bool remove(int fd);

    std::size_t workerCount() const noexcept;
    std::size_t size() const;

private:
    struct State;

    std::shared_ptr<State> state_;
};

bool setNonBlocking(int fd);

} // namespace foundation
