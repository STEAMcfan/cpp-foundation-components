#include "ring_buffer.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

namespace {

using namespace std::chrono_literals;

void pushesAndPopsInFifoOrder() {
    foundation::RingBuffer<int> buffer(3);

    assert(buffer.empty());
    assert(buffer.tryPush(1));
    assert(buffer.tryPush(2));
    assert(buffer.size() == 2);

    auto first = buffer.tryPop();
    auto second = buffer.tryPop();
    auto third = buffer.tryPop();

    assert(first && *first == 1);
    assert(second && *second == 2);
    assert(!third);
    assert(buffer.empty());
}

void wrapsAroundWithoutChangingOrder() {
    foundation::RingBuffer<int> buffer(3);

    assert(buffer.tryPush(1));
    assert(buffer.tryPush(2));
    assert(*buffer.tryPop() == 1);
    assert(buffer.tryPush(3));
    assert(buffer.tryPush(4));
    assert(buffer.full());

    assert(*buffer.tryPop() == 2);
    assert(*buffer.tryPop() == 3);
    assert(*buffer.tryPop() == 4);
    assert(buffer.empty());
}

void rejectsWhenFullByDefault() {
    foundation::RingBuffer<int> buffer(2);

    assert(buffer.empty());
    assert(buffer.tryPush(10));
    assert(buffer.tryPush(20));
    assert(buffer.full());
    assert(!buffer.tryPush(30));

    assert(*buffer.tryPop() == 10);
    assert(*buffer.tryPop() == 20);

    const auto stats = buffer.stats();
    assert(stats.pushes == 2);
    assert(stats.pops == 2);
    assert(stats.failed_pushes == 1);
    assert(stats.overwrites == 0);
}

void canOverwriteOldestItem() {
    foundation::RingBuffer<int> buffer(foundation::RingBufferOptions{
        3,
        foundation::RingBufferFullPolicy::OverwriteOldest,
    });

    assert(buffer.tryPush(1));
    assert(buffer.tryPush(2));
    assert(buffer.tryPush(3));
    assert(buffer.tryPush(4));
    assert(buffer.size() == 3);

    assert(*buffer.tryPop() == 2);
    assert(*buffer.tryPop() == 3);
    assert(*buffer.tryPop() == 4);
    assert(buffer.empty());

    const auto stats = buffer.stats();
    assert(stats.pushes == 4);
    assert(stats.pops == 3);
    assert(stats.overwrites == 1);
}

void waitPushBlocksUntilSpaceIsAvailable() {
    foundation::RingBuffer<int> buffer(1);
    assert(buffer.tryPush(1));

    std::atomic<bool> pushed{false};
    std::thread producer([&] {
        pushed = buffer.waitPush(2);
    });

    std::this_thread::sleep_for(20ms);
    assert(!pushed.load());

    auto first = buffer.waitPop();
    assert(first && *first == 1);

    producer.join();
    assert(pushed.load());

    auto second = buffer.waitPopFor(100ms);
    assert(second && *second == 2);
}

void waitPopBlocksUntilAnItemArrives() {
    foundation::RingBuffer<int> buffer(2);
    std::atomic<int> observed{0};

    std::thread consumer([&] {
        auto value = buffer.waitPop();
        assert(value);
        observed.store(*value);
    });

    std::this_thread::sleep_for(20ms);
    assert(observed.load() == 0);

    assert(buffer.waitPush(42));
    consumer.join();
    assert(observed.load() == 42);
}

void closeWakesWaitersAndRejectsLaterPushes() {
    foundation::RingBuffer<int> buffer(1);
    std::atomic<bool> woke{false};

    std::thread consumer([&] {
        auto value = buffer.waitPop();
        assert(!value);
        woke = true;
    });

    std::this_thread::sleep_for(20ms);
    buffer.close();
    consumer.join();

    assert(woke.load());
    assert(buffer.isClosed());
    assert(!buffer.tryPush(7));

    const auto stats = buffer.stats();
    assert(stats.closed_pushes == 1);
}

void supportsMoveOnlyValues() {
    foundation::RingBuffer<std::unique_ptr<int>> buffer(2);

    assert(buffer.tryPush(std::make_unique<int>(7)));
    auto value = buffer.tryPop();

    assert(value);
    assert(*value);
    assert(**value == 7);
}

struct Counted {
    explicit Counted(std::atomic<int>& alive_count) noexcept
        : alive(&alive_count) {
        alive->fetch_add(1);
    }

    Counted(Counted&& other) noexcept
        : alive(other.alive) {
        other.alive = nullptr;
    }

    Counted& operator=(Counted&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        release();
        alive = other.alive;
        other.alive = nullptr;
        return *this;
    }

    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;

    ~Counted() {
        release();
    }

    void release() noexcept {
        if (alive) {
            alive->fetch_sub(1);
            alive = nullptr;
        }
    }

    std::atomic<int>* alive = nullptr;
};

void destroysRemainingObjects() {
    std::atomic<int> alive{0};

    {
        foundation::RingBuffer<Counted> buffer(4);
        assert(buffer.tryEmplace(alive));
        assert(buffer.tryEmplace(alive));
        assert(alive.load() == 2);
    }

    assert(alive.load() == 0);
}

} // namespace

int main() {
    pushesAndPopsInFifoOrder();
    wrapsAroundWithoutChangingOrder();
    rejectsWhenFullByDefault();
    canOverwriteOldestItem();
    waitPushBlocksUntilSpaceIsAvailable();
    waitPopBlocksUntilAnItemArrives();
    closeWakesWaitersAndRejectsLaterPushes();
    supportsMoveOnlyValues();
    destroysRemainingObjects();
    return 0;
}
