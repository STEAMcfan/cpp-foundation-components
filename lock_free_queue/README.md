# Lock-Free Queue

This directory contains a small C++17 bounded MPMC lock-free queue implementation for learning and interview discussion.

The queue uses Dmitry Vyukov's bounded ring-buffer idea: every cell owns a sequence number, producers advance an enqueue cursor with CAS, and consumers advance a dequeue cursor with CAS. The fixed-size ring avoids the memory-reclamation problem that makes unbounded linked-list lock-free queues hard to implement correctly.

## Layout

```text
lock_free_queue/
+-- include/lock_free_queue.hpp
+-- examples/demo.cpp
+-- tests/lock_free_queue_test.cpp
+-- CMakeLists.txt
+-- README.md
```

## Build And Run

```bash
cd lock_free_queue
cmake -S . -B build
cmake --build build
./build/lock_free_queue_demo
ctest --test-dir build --output-on-failure
```

On multi-config generators, the executable may be under `build/Debug/`.

## Basic Usage

```cpp
foundation::LockFreeQueue<int> queue(1024);

if (!queue.tryPush(42)) {
    // The queue is full or closed.
}

auto value = queue.tryPop();
if (value) {
    // use *value
}

queue.close(); // rejects later pushes; existing items can still be popped
```

## Design Notes

- `LockFreeQueue<T>` is bounded. Capacity is fixed at construction time.
- The implementation is MPMC: many producers and many consumers may call `tryPush()` and `tryPop()` concurrently.
- Operations are non-blocking. `tryPush()` returns `false` when the ring is full or closed; `tryPop()` returns `std::nullopt` when the ring is currently empty.
- Each ring cell has a monotonically increasing sequence number. A producer can write only when `sequence == enqueue_pos`; a consumer can read only when `sequence == dequeue_pos + 1`.
- The queue requires `T` to be nothrow move-constructible. That keeps the queue state valid after a thread has successfully reserved a slot.
- `approximateSize()`, `empty()`, and `full()` are snapshots. They are useful for metrics and tests, but concurrent producers or consumers can change the answer immediately.

## Interview Handwritten Version

For a whiteboard or handwritten interview, keep only the core fields and two operations:

```cpp
template <class T>
class MPMCQueue {
    struct Cell {
        std::atomic<size_t> seq;
        T data;
    };

    std::vector<Cell> buf;
    size_t cap;
    std::atomic<size_t> enq{0};
    std::atomic<size_t> deq{0};

public:
    explicit MPMCQueue(size_t n) : buf(n), cap(n) {
        for (size_t i = 0; i < n; ++i) buf[i].seq.store(i);
    }

    bool push(const T& x) {
        size_t pos = enq.load();
        for (;;) {
            Cell& c = buf[pos % cap];
            size_t s = c.seq.load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)s - (intptr_t)pos;
            if (diff == 0) {
                if (enq.compare_exchange_weak(pos, pos + 1)) {
                    c.data = x;
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = enq.load();
            }
        }
    }

    bool pop(T& out) {
        size_t pos = deq.load();
        for (;;) {
            Cell& c = buf[pos % cap];
            size_t s = c.seq.load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)s - (intptr_t)(pos + 1);
            if (diff == 0) {
                if (deq.compare_exchange_weak(pos, pos + 1)) {
                    out = c.data;
                    c.seq.store(pos + cap, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = deq.load();
            }
        }
    }
};
```

In an interview, explain three points:

- the sequence number distinguishes "this slot is empty for this producer round" from "this slot still contains data"
- `enq` and `deq` are tickets; a successful CAS reserves one slot for exactly one thread
- the ring is bounded on purpose, because a linked-list lock-free queue also needs hazard pointers, epoch reclamation, or another safe memory-reclamation scheme
