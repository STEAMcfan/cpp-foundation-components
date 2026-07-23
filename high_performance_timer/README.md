# High-Performance Timer

这个目录包含一个 C++17 高性能定时器实现，适合学习、运行和面试讲解。

在 C++ 后端里，“高性能定时器”通常不是指简单测量一段代码耗时的 `std::chrono` 秒表，而是指一个能管理大量定时任务的调度器：

- 延迟执行：例如 100ms 后关闭空闲连接
- 周期执行：例如每 1s 发送心跳
- 超时控制：例如 RPC、HTTP、数据库请求超时
- 资源回收：例如缓存过期、连接池 idle cleanup
- 网络服务：例如重传、会话过期、限流窗口刷新

核心目标是：不要为每个定时任务创建一个线程，而是用一个或少量线程统一管理成千上万个定时任务。

## Layout

```text
high_performance_timer/
+-- include/high_performance_timer.hpp
+-- src/high_performance_timer.cpp
+-- examples/demo.cpp
+-- tests/high_performance_timer_test.cpp
+-- CMakeLists.txt
+-- README.md
```

## Build And Run

```bash
cd high_performance_timer
cmake -S . -B build
cmake --build build
./build/high_performance_timer_demo
ctest --test-dir build --output-on-failure
```

On multi-config generators, the executable may be under `build/Debug/`.

## Basic Usage

```cpp
using namespace std::chrono_literals;

foundation::HighPerformanceTimer::Config config;
config.callback_threads = 2;

foundation::HighPerformanceTimer timer(config);

auto once = timer.scheduleAfter(100ms, [] {
    // run once after 100ms
});

auto heartbeat = timer.scheduleEvery(1s, [] {
    // run every second
});

timer.cancel(once);
timer.shutdown();
```

## How It Works

这个完整版本使用的是“最小堆 + 调度线程 + 回调工作线程池”：

- `std::chrono::steady_clock`：使用单调时钟，避免系统时间被调快/调慢影响定时器。
- 最小堆 `priority_queue`：堆顶永远是最近要触发的任务，插入是 `O(log n)`，取最近任务也是 `O(log n)`。
- 一个 scheduler 线程：只等待最近的到期时间，不忙等，不给每个 timer 单独开线程。
- callback worker 线程：到期任务的回调放到工作队列执行，避免慢回调阻塞调度线程。
- TimerId + lazy cancellation：取消时从 `unordered_map` 删除 timer，堆里的旧节点之后被惰性跳过，避免在堆中间删除造成 `O(n)`。
- 周期 timer：每次触发后重新计算下一次 deadline；如果系统短暂卡顿，会跳过已经错过的周期，避免瞬间补偿触发一大堆回调。

## Key Interview Points

面试里重点讲这几部分：

- 时间源：用 `steady_clock`，不要用 `system_clock` 做超时逻辑。
- 数据结构：少量 timer 用最小堆；海量、高频、精度固定的 timer 可以用时间轮。
- 等待方式：调度线程用 `condition_variable::wait_until` 等最近 deadline，不忙等。
- 并发控制：增删 timer 要加锁；回调必须在锁外执行。
- 取消逻辑：用 id 标记任务，取消时从 map 删除，堆里的过期副本惰性清理。
- 周期任务：要说明固定频率、漂移、错过 tick 后是否补偿。

## Interview Handwritten Version

手撕时保留核心即可：一个最小堆、一个 map、一个后台线程、`add/cancel/loop` 三块逻辑。

```cpp
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

class Timer {
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    using Callback = std::function<void()>;

    struct Node {
        Time when;
        int id;
        bool operator>(const Node& other) const {
            return when > other.when;
        }
    };

    struct Task {
        Time when;
        Callback cb;
    };

    std::mutex mtx;
    std::condition_variable cv;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> heap;
    std::unordered_map<int, Task> tasks;
    std::thread worker;
    bool stop = false;
    int next_id = 1;

public:
    Timer() {
        worker = std::thread([this] { run(); });
    }

    ~Timer() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }

    int add(std::chrono::milliseconds delay, Callback cb) {
        std::lock_guard<std::mutex> lock(mtx);
        int id = next_id++;
        Time when = Clock::now() + delay;
        tasks[id] = Task{when, std::move(cb)};
        heap.push(Node{when, id});
        cv.notify_one();
        return id;
    }

    bool cancel(int id) {
        std::lock_guard<std::mutex> lock(mtx);
        return tasks.erase(id) > 0;
    }

private:
    void run() {
        while (true) {
            Callback cb;

            {
                std::unique_lock<std::mutex> lock(mtx);

                while (!stop) {
                    while (!heap.empty()) {
                        auto it = tasks.find(heap.top().id);
                        if (it != tasks.end() && it->second.when == heap.top().when) break;
                        heap.pop();
                    }

                    if (heap.empty()) {
                        cv.wait(lock);
                    } else if (heap.top().when > Clock::now()) {
                        cv.wait_until(lock, heap.top().when);
                    } else {
                        break;
                    }
                }

                if (stop) return;

                int id = heap.top().id;
                heap.pop();
                auto it = tasks.find(id);
                if (it == tasks.end()) continue;

                cb = std::move(it->second.cb);
                tasks.erase(it);
            }

            cb(); // 回调一定在锁外执行
        }
    }
};
```

如果追问“为什么这算高性能”，可以这样答：

这个实现没有一个 timer 一个线程，也没有循环扫描全部 timer。新增 timer 是 `O(log n)`，到期时只处理堆顶附近的任务；线程睡到最近 deadline，有新任务或取消时再唤醒。真正业务回调在锁外跑，所以调度结构不会被慢业务长期占住。
