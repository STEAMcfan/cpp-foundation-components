#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

class EventLoop {
public:
    using Callback = std::function<void(int, uint32_t)>;

    EventLoop() {
        epfd_ = epoll_create1(EPOLL_CLOEXEC);
        wakefd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = wakefd_;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, wakefd_, &ev);
    }

    ~EventLoop() {
        close(wakefd_);
        close(epfd_);
    }

    void add(int fd, uint32_t events, Callback cb) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;

        std::lock_guard<std::mutex> lock(mtx_);
        callbacks_[fd] = std::move(cb);
        epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
        wake();
    }

    void mod(int fd, uint32_t events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;

        std::lock_guard<std::mutex> lock(mtx_);
        epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
        wake();
    }

    void del(int fd) {
        std::lock_guard<std::mutex> lock(mtx_);
        callbacks_.erase(fd);
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        wake();
    }

    void loop() {
        epoll_event events[1024];
        while (!stop_) {
            int n = epoll_wait(epfd_, events, 1024, -1);
            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;

                if (fd == wakefd_) {
                    uint64_t value = 0;
                    while (read(wakefd_, &value, sizeof(value)) > 0) {
                    }
                    continue;
                }

                Callback cb;
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    auto it = callbacks_.find(fd);
                    if (it == callbacks_.end()) {
                        continue;
                    }
                    cb = it->second;
                }

                cb(fd, events[i].events);
            }
        }
    }

    void stop() {
        stop_ = true;
        wake();
    }

private:
    void wake() {
        uint64_t value = 1;
        write(wakefd_, &value, sizeof(value));
    }

    int epfd_ = -1;
    int wakefd_ = -1;
    std::mutex mtx_;
    std::unordered_map<int, Callback> callbacks_;
    std::atomic<bool> stop_{false};
};

class ReactorPool {
public:
    explicit ReactorPool(int thread_num)
        : loops_(thread_num) {}

    void start() {
        for (auto& loop : loops_) {
            threads_.emplace_back([&loop] {
                loop.loop();
            });
        }
    }

    void stop() {
        for (auto& loop : loops_) {
            loop.stop();
        }
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    EventLoop& nextLoop() {
        int idx = next_++ % static_cast<int>(loops_.size());
        return loops_[idx];
    }

private:
    std::vector<EventLoop> loops_;
    std::vector<std::thread> threads_;
    std::atomic<int> next_{0};
};

int setNonBlock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Usage sketch:
// 1. main EventLoop listens on listenfd.
// 2. listenfd readable means accept new connections until EAGAIN.
// 3. each connfd is set nonblocking and registered to ReactorPool::nextLoop().
// 4. worker EventLoop reads/writes connfd; slow business work should go to a task pool.
