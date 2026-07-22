#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace foundation {

struct LockFreeQueueStats {
    std::size_t pushes = 0;
    std::size_t pops = 0;
    std::size_t failed_pushes = 0;
    std::size_t failed_pops = 0;
    std::size_t closed_pushes = 0;
};

template <typename T>
class LockFreeQueue {
public:
    static_assert(std::is_nothrow_move_constructible<T>::value,
                  "LockFreeQueue requires T to be nothrow move constructible");

    explicit LockFreeQueue(std::size_t capacity)
        : buffer_(capacity),
          capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("LockFreeQueue capacity must be greater than 0");
        }

        for (std::size_t i = 0; i < capacity_; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~LockFreeQueue() {
        while (tryPop().has_value()) {
        }
    }

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;
    LockFreeQueue(LockFreeQueue&&) = delete;
    LockFreeQueue& operator=(LockFreeQueue&&) = delete;

    bool tryPush(const T& value) {
        T copy(value);
        return tryPush(std::move(copy));
    }

    bool tryPush(T&& value) {
        if (closed_.load(std::memory_order_acquire)) {
            closed_pushes_.fetch_add(1, std::memory_order_relaxed);
            failed_pushes_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        Cell* cell = nullptr;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        while (true) {
            cell = &buffer_[pos % capacity_];
            const auto seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) -
                              static_cast<std::ptrdiff_t>(pos);

            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos,
                                                       pos + 1,
                                                       std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                failed_pushes_.fetch_add(1, std::memory_order_relaxed);
                return false;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        new (&cell->storage) T(std::move(value));
        cell->sequence.store(pos + 1, std::memory_order_release);
        pushes_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    template <typename... Args>
    bool tryEmplace(Args&&... args) {
        T value(std::forward<Args>(args)...);
        return tryPush(std::move(value));
    }

    std::optional<T> tryPop() {
        Cell* cell = nullptr;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        while (true) {
            cell = &buffer_[pos % capacity_];
            const auto seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) -
                              static_cast<std::ptrdiff_t>(pos + 1);

            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos,
                                                       pos + 1,
                                                       std::memory_order_relaxed,
                                                       std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                failed_pops_.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        auto* value = reinterpret_cast<T*>(&cell->storage);
        std::optional<T> result;
        result.emplace(std::move(*value));
        value->~T();
        cell->sequence.store(pos + capacity_, std::memory_order_release);
        pops_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    bool tryPop(T& out) {
        auto value = tryPop();
        if (!value) {
            return false;
        }

        out = std::move(*value);
        return true;
    }

    void close() noexcept {
        closed_.store(true, std::memory_order_release);
    }

    bool isClosed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }

    std::size_t approximateSize() const noexcept {
        const auto enqueued = enqueue_pos_.load(std::memory_order_acquire);
        const auto dequeued = dequeue_pos_.load(std::memory_order_acquire);
        if (enqueued < dequeued) {
            return 0;
        }

        return std::min(enqueued - dequeued, capacity_);
    }

    bool empty() const noexcept {
        return approximateSize() == 0;
    }

    bool full() const noexcept {
        return approximateSize() == capacity_;
    }

    bool isLockFree() const noexcept {
        return enqueue_pos_.is_lock_free() &&
               dequeue_pos_.is_lock_free() &&
               closed_.is_lock_free();
    }

    LockFreeQueueStats stats() const noexcept {
        return LockFreeQueueStats{
            pushes_.load(std::memory_order_relaxed),
            pops_.load(std::memory_order_relaxed),
            failed_pushes_.load(std::memory_order_relaxed),
            failed_pops_.load(std::memory_order_relaxed),
            closed_pushes_.load(std::memory_order_relaxed),
        };
    }

private:
    using Storage = typename std::aligned_storage<sizeof(T), alignof(T)>::type;

    struct Cell {
        std::atomic<std::size_t> sequence{0};
        Storage storage;
    };

    std::vector<Cell> buffer_;
    const std::size_t capacity_;

    alignas(64) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(64) std::atomic<std::size_t> dequeue_pos_{0};
    alignas(64) std::atomic<bool> closed_{false};

    alignas(64) std::atomic<std::size_t> pushes_{0};
    std::atomic<std::size_t> pops_{0};
    std::atomic<std::size_t> failed_pushes_{0};
    std::atomic<std::size_t> failed_pops_{0};
    std::atomic<std::size_t> closed_pushes_{0};
};

} // namespace foundation
