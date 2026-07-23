# Ring Buffer

这个目录包含一个 C++17 ring buffer / circular buffer 组件，适合学习、运行和面试讲解。

RingBuffer 的本质是：用一段固定大小的连续数组当作循环队列，`head` 指向下一次读取的位置，`tail` 指向下一次写入的位置。每次移动下标都做一次取模，数组末尾之后回到数组开头，因此可以复用已经消费过的空间。

## Layout

```text
ring_buffer/
+-- include/ring_buffer.hpp
+-- examples/demo.cpp
+-- tests/ring_buffer_test.cpp
+-- CMakeLists.txt
+-- README.md
```

## Build And Run

```bash
cd ring_buffer
cmake -S . -B build
cmake --build build
./build/ring_buffer_demo
ctest --test-dir build --output-on-failure
```

On multi-config generators, the executable may be under `build/Debug/`.

## Basic Usage

```cpp
foundation::RingBuffer<int> buffer(1024);

buffer.tryPush(1);
buffer.tryPush(2);

auto value = buffer.tryPop();
if (value) {
    // use *value
}

buffer.close(); // wakes waiting consumers and rejects later pushes
```

满时覆盖旧数据的模式：

```cpp
foundation::RingBuffer<int> latest_values(foundation::RingBufferOptions{
    1024,
    foundation::RingBufferFullPolicy::OverwriteOldest,
});
```

## Design Notes

- `RingBuffer<T>` 是有界结构，容量固定，内存连续，push/pop 都是 `O(1)`。
- 默认满了以后拒绝写入，`tryPush()` 返回 `false`；也可以选择 `OverwriteOldest`，满了以后丢掉最旧元素，保留最新数据。
- `tryPush()` / `tryPop()` 是非阻塞接口；`waitPush()` / `waitPop()` 是阻塞接口，适合生产者消费者场景。
- `close()` 会拒绝后续写入，并唤醒等待中的线程；已经写入的数据仍然可以被消费。
- 实现里使用原始存储 `aligned_storage`，所以 `T` 不需要默认构造；析构时会正确销毁仍留在 buffer 里的对象。
- 这个版本用 mutex + condition_variable 做线程安全。它不是 lock-free 版本，但更适合作为业务组件和面试讲解第一版。

## Key Points

RingBuffer 的关键不是数组本身，而是三个边界问题：

1. 如何判断空和满。
2. `head` / `tail` 绕回之后如何保持 FIFO 顺序。
3. 满了以后采取什么策略：拒绝写入、阻塞等待，还是覆盖最旧数据。

常见的空满判断有两种：

- 维护 `size`：`size == 0` 为空，`size == capacity` 为满，代码直观。
- 浪费一个槽位：`head == tail` 为空，`(tail + 1) % capacity == head` 为满，少存一个元素，但不用额外维护 `size`。

## Common Uses

- 生产者消费者队列：日志线程、网络收包、音视频帧处理。
- 固定窗口数据：最近 N 条请求、最近 N 个指标点、滑动统计。
- 高性能缓冲区：避免频繁申请释放内存。
- 嵌入式 / 驱动 / IO 场景：固定内存、低延迟、行为可预测。
- 最新值缓存：满时覆盖旧数据，只关心最近的数据。

## Interview Handwritten Version

面试手撕时先写维护 `size` 的版本，最稳、最容易讲清楚：

```cpp
#include <vector>

class RingBuffer {
    std::vector<int> buf;
    int head = 0; // next position to read
    int tail = 0; // next position to write
    int sz = 0;
    int cap = 0;

public:
    explicit RingBuffer(int n) : buf(n), cap(n) {}

    bool push(int x) {
        if (sz == cap) return false;
        buf[tail] = x;
        tail = (tail + 1) % cap;
        ++sz;
        return true;
    }

    bool pop(int& x) {
        if (sz == 0) return false;
        x = buf[head];
        head = (head + 1) % cap;
        --sz;
        return true;
    }

    bool empty() const { return sz == 0; }
    bool full() const { return sz == cap; }
    int size() const { return sz; }
};
```

如果面试官要求不用 `size`，可以写浪费一个槽位的版本：

```cpp
#include <vector>

class RingBuffer {
    std::vector<int> buf;
    int head = 0;
    int tail = 0;
    int cap = 0; // real vector size is user capacity + 1

public:
    explicit RingBuffer(int n) : buf(n + 1), cap(n + 1) {}

    bool push(int x) {
        int next = (tail + 1) % cap;
        if (next == head) return false;
        buf[tail] = x;
        tail = next;
        return true;
    }

    bool pop(int& x) {
        if (head == tail) return false;
        x = buf[head];
        head = (head + 1) % cap;
        return true;
    }

    bool empty() const { return head == tail; }
    bool full() const { return (tail + 1) % cap == head; }
};
```

讲解顺序建议：

1. 先说固定数组避免频繁分配，`head` 读、`tail` 写。
2. 再说下标每次 `(idx + 1) % capacity`，所以末尾能回到开头。
3. 然后说空满判断：用 `size` 最直观，不用 `size` 就牺牲一个槽位。
4. 最后补充满时策略：队列语义通常拒绝或阻塞，实时数据场景常覆盖最旧值。

## Possible Interview Questions

- 为什么 ring buffer 的 push/pop 是 `O(1)`？
- `head == tail` 到底表示空还是满？如何消除这个歧义？
- 维护 `size` 和浪费一个槽位两种方案有什么取舍？
- 满了以后应该拒绝、阻塞还是覆盖？分别适合什么业务？
- 容量是 2 的幂时，为什么可以用 `index & (capacity - 1)` 替代取模？
- 单生产者单消费者 SPSC 可以怎么做到无锁？
- 多生产者多消费者 MPMC 为什么会复杂很多？
- ring buffer 和普通队列、deque、lock-free queue 的区别是什么？
- 如果元素类型不是 int，而是对象，构造和析构要注意什么？
- 多线程条件下如何避免消费者空等、生产者满等和关闭时线程卡死？
