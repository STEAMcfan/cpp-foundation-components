#include "lock_free_queue.hpp"

#include <atomic>
#include <cassert>
#include <memory>
#include <thread>
#include <vector>

namespace {

void pushesAndPopsInFifoOrder() {
    foundation::LockFreeQueue<int> queue(3);

    assert(queue.empty());
    assert(queue.tryPush(1));
    assert(queue.tryPush(2));
    assert(queue.approximateSize() == 2);

    auto first = queue.tryPop();
    auto second = queue.tryPop();
    auto third = queue.tryPop();

    assert(first && *first == 1);
    assert(second && *second == 2);
    assert(!third);
    assert(queue.empty());
}

void rejectsWhenFullOrClosed() {
    foundation::LockFreeQueue<int> queue(2);

    assert(queue.tryPush(10));
    assert(queue.tryPush(20));
    assert(queue.full());
    assert(!queue.tryPush(30));

    queue.close();
    assert(queue.isClosed());
    assert(!queue.tryPush(40));

    auto first = queue.tryPop();
    auto second = queue.tryPop();
    auto third = queue.tryPop();

    assert(first && *first == 10);
    assert(second && *second == 20);
    assert(!third);

    const auto stats = queue.stats();
    assert(stats.pushes == 2);
    assert(stats.pops == 2);
    assert(stats.failed_pushes == 2);
    assert(stats.closed_pushes == 1);
}

void supportsMoveOnlyValues() {
    foundation::LockFreeQueue<std::unique_ptr<int>> queue(2);

    assert(queue.tryPush(std::make_unique<int>(7)));
    auto value = queue.tryPop();

    assert(value);
    assert(*value);
    assert(**value == 7);
}

struct Counted {
    explicit Counted(std::atomic<int>& alive_count) noexcept
        : alive(&alive_count) {
        alive->fetch_add(1, std::memory_order_relaxed);
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
            alive->fetch_sub(1, std::memory_order_relaxed);
            alive = nullptr;
        }
    }

    std::atomic<int>* alive = nullptr;
};

void destroysRemainingObjects() {
    std::atomic<int> alive{0};

    {
        foundation::LockFreeQueue<Counted> queue(4);
        assert(queue.tryEmplace(alive));
        assert(queue.tryEmplace(alive));
        assert(alive == 2);
    }

    assert(alive == 0);
}

void handlesManyProducersAndConsumers() {
    constexpr int producer_count = 4;
    constexpr int consumer_count = 4;
    constexpr int items_per_producer = 3000;
    constexpr int total_items = producer_count * items_per_producer;

    foundation::LockFreeQueue<int> queue(128);
    std::vector<std::atomic<int>> seen(total_items);
    std::atomic<int> consumed{0};

    std::vector<std::thread> consumers;
    for (int i = 0; i < consumer_count; ++i) {
        consumers.emplace_back([&] {
            while (consumed.load(std::memory_order_acquire) < total_items) {
                auto value = queue.tryPop();
                if (!value) {
                    std::this_thread::yield();
                    continue;
                }

                assert(*value >= 0);
                assert(*value < total_items);
                seen[*value].fetch_add(1, std::memory_order_relaxed);
                consumed.fetch_add(1, std::memory_order_release);
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < producer_count; ++p) {
        producers.emplace_back([&, p] {
            const int base = p * items_per_producer;
            for (int i = 0; i < items_per_producer; ++i) {
                const int value = base + i;
                while (!queue.tryPush(value)) {
                    assert(!queue.isClosed());
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    for (auto& consumer : consumers) {
        consumer.join();
    }

    assert(queue.empty());

    for (const auto& count : seen) {
        assert(count.load(std::memory_order_relaxed) == 1);
    }

    const auto stats = queue.stats();
    assert(stats.pushes == total_items);
    assert(stats.pops == total_items);
}

} // namespace

int main() {
    pushesAndPopsInFifoOrder();
    rejectsWhenFullOrClosed();
    supportsMoveOnlyValues();
    destroysRemainingObjects();
    handlesManyProducersAndConsumers();
    return 0;
}
