#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace foundation {

enum class RingBufferFullPolicy {
    Reject,
    OverwriteOldest,
};

struct RingBufferOptions {
    std::size_t capacity = 0;
    RingBufferFullPolicy full_policy = RingBufferFullPolicy::Reject;
};

struct RingBufferStats {
    std::size_t pushes = 0;
    std::size_t pops = 0;
    std::size_t failed_pushes = 0;
    std::size_t failed_pops = 0;
    std::size_t overwrites = 0;
    std::size_t closed_pushes = 0;
};

template <typename T>
class RingBuffer {
public:
    static_assert(std::is_nothrow_move_constructible<T>::value,
                  "RingBuffer requires T to be nothrow move constructible");

    explicit RingBuffer(std::size_t capacity)
        : RingBuffer(RingBufferOptions{capacity}) {
    }

    explicit RingBuffer(RingBufferOptions options)
        : buffer_(options.capacity),
          capacity_(options.capacity),
          full_policy_(options.full_policy) {
        if (capacity_ == 0) {
            throw std::invalid_argument("RingBuffer capacity must be greater than 0");
        }
    }

    ~RingBuffer() {
        clear();
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    bool tryPush(const T& value) {
        T copy(value);
        return tryPush(std::move(copy));
    }

    bool tryPush(T&& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool pushed = pushLocked(std::move(value));
        lock.unlock();

        if (pushed) {
            not_empty_.notify_one();
        }

        return pushed;
    }

    template <typename... Args>
    bool tryEmplace(Args&&... args) {
        T value(std::forward<Args>(args)...);
        return tryPush(std::move(value));
    }

    bool waitPush(const T& value) {
        T copy(value);
        return waitPush(std::move(copy));
    }

    bool waitPush(T&& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        waitForWritableSlot(lock);

        const bool pushed = pushLocked(std::move(value));
        lock.unlock();

        if (pushed) {
            not_empty_.notify_one();
        }

        return pushed;
    }

    template <typename Rep, typename Period>
    bool waitPushFor(const T& value,
                     const std::chrono::duration<Rep, Period>& timeout) {
        T copy(value);
        return waitPushFor(std::move(copy), timeout);
    }

    template <typename Rep, typename Period>
    bool waitPushFor(T&& value,
                     const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);

        if (!hasWritableSlotLocked()) {
            const bool ready = not_full_.wait_for(lock, timeout, [this] {
                return closed_ || hasWritableSlotLocked();
            });

            if (!ready) {
                ++stats_.failed_pushes;
                return false;
            }
        }

        const bool pushed = pushLocked(std::move(value));
        lock.unlock();

        if (pushed) {
            not_empty_.notify_one();
        }

        return pushed;
    }

    std::optional<T> tryPop() {
        std::unique_lock<std::mutex> lock(mutex_);

        if (size_ == 0) {
            ++stats_.failed_pops;
            return std::nullopt;
        }

        auto result = popLocked();
        lock.unlock();
        not_full_.notify_one();
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

    std::optional<T> waitPop() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] {
            return closed_ || size_ > 0;
        });

        if (size_ == 0) {
            ++stats_.failed_pops;
            return std::nullopt;
        }

        auto result = popLocked();
        lock.unlock();
        not_full_.notify_one();
        return result;
    }

    template <typename Rep, typename Period>
    std::optional<T> waitPopFor(const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool ready = not_empty_.wait_for(lock, timeout, [this] {
            return closed_ || size_ > 0;
        });

        if (!ready || size_ == 0) {
            ++stats_.failed_pops;
            return std::nullopt;
        }

        auto result = popLocked();
        lock.unlock();
        not_full_.notify_one();
        return result;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }

        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool isClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    void clear() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (size_ > 0) {
                destroyAt(head_);
                head_ = nextIndex(head_);
                --size_;
            }

            head_ = 0;
            tail_ = 0;
        }

        not_full_.notify_all();
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }

    RingBufferFullPolicy fullPolicy() const noexcept {
        return full_policy_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_ == 0;
    }

    bool full() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_ == capacity_;
    }

    RingBufferStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

private:
    using Storage = typename std::aligned_storage<sizeof(T), alignof(T)>::type;

    struct Cell {
        Storage storage;
        bool occupied = false;

        T* value() {
            return std::launder(reinterpret_cast<T*>(&storage));
        }
    };

    bool hasWritableSlotLocked() const {
        return size_ < capacity_ ||
               full_policy_ == RingBufferFullPolicy::OverwriteOldest;
    }

    void waitForWritableSlot(std::unique_lock<std::mutex>& lock) {
        if (full_policy_ == RingBufferFullPolicy::OverwriteOldest) {
            return;
        }

        not_full_.wait(lock, [this] {
            return closed_ || size_ < capacity_;
        });
    }

    bool pushLocked(T&& value) {
        if (closed_) {
            ++stats_.closed_pushes;
            ++stats_.failed_pushes;
            return false;
        }

        if (size_ == capacity_) {
            if (full_policy_ == RingBufferFullPolicy::Reject) {
                ++stats_.failed_pushes;
                return false;
            }

            destroyAt(head_);
            head_ = nextIndex(head_);
            --size_;
            ++stats_.overwrites;
        }

        constructAt(tail_, std::move(value));
        tail_ = nextIndex(tail_);
        ++size_;
        ++stats_.pushes;
        return true;
    }

    std::optional<T> popLocked() {
        std::optional<T> result;
        result.emplace(std::move(*buffer_[head_].value()));
        destroyAt(head_);
        head_ = nextIndex(head_);
        --size_;
        ++stats_.pops;
        return result;
    }

    void constructAt(std::size_t index, T&& value) {
        Cell& cell = buffer_[index];
        new (&cell.storage) T(std::move(value));
        cell.occupied = true;
    }

    void destroyAt(std::size_t index) {
        Cell& cell = buffer_[index];
        if (cell.occupied) {
            cell.value()->~T();
            cell.occupied = false;
        }
    }

    std::size_t nextIndex(std::size_t index) const noexcept {
        return index + 1 == capacity_ ? 0 : index + 1;
    }

    std::vector<Cell> buffer_;
    const std::size_t capacity_;
    const RingBufferFullPolicy full_policy_;

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;

    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t size_ = 0;
    bool closed_ = false;
    RingBufferStats stats_;
};

} // namespace foundation
