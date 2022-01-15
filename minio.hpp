#pragma once

#include <coroutine>
#include <utility>
#include <exception>
#include <vector>
#include <cstdint>
#include <cassert>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>


using std::coroutine_handle;
using std::suspend_never;
using std::suspend_always;
using std::noop_coroutine;

using std::uint32_t;
using std::vector;

#define MINIO_NS_BEG namespace minio {
#define MINIO_NS_END }


MINIO_NS_BEG


namespace utils {
    int dup_fd(int fd) noexcept {
        return dup(fd);
        // fd flags are not derived
    }

    int set_non_blocking(int fd) noexcept {
        int old_opt = fcntl(fd, F_GETFL);
        int new_opt = old_opt | O_NONBLOCK;
        fcntl(fd, F_SETFL, new_opt);
        return old_opt;
    }

    int set_reuse_addr(int fd) noexcept {
        int opt = 1;
        return setsockopt(
            fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)
        );
    }
}



namespace coro {

namespace detail {
    namespace promise {
        enum class State {
            Pending = 0,
            Ready,
            Fail,
            Detach,
        };

        template<typename T>
        struct PromiseBase {
            T value;
            State state = State::Pending;

            // co_return value
            void return_value(T&& value) && noexcept {
                this->value = std::move(value);
                this->state = State::Ready;
            }

            void return_value(const T& value) & noexcept {
                this->value = value;
                this->state = State::Ready;
            }

            // must within the coroutine's lifetime
            // the coroutine should be suspended at final_suspend
            T& get_result() & noexcept {
                return value;
            }

            T&& get_result() && noexcept {
                return std::move(value);
            }
        };

        template<>
        struct PromiseBase<void> {
            State state = State::Pending;

            void return_void() noexcept {
                this->state = State::Ready;
            }

            void get_result() noexcept {}
        };

        template<typename Promise>
        struct FinalAwaiterBase {
            // alwyas suspend
            // manually delete coroutine frame if detached
            constexpr bool await_ready() const noexcept { return false; }

            // nothing todo, the promise is fulfilled
            void await_resume() const noexcept {};

            coroutine_handle<>
            await_suspend(coroutine_handle<Promise> handle) noexcept {
                auto& promise = handle.promise();
                auto super_handle = promise.super_handle;
                auto state = promise.state;

                // go back to outer coroutine
                if (state != State::Detach) {
                    return super_handle ? super_handle : noop_coroutine();
                }

                // detached
                handle.destroy();
                return noop_coroutine();
            }
        };
    }

    namespace task_awaiter {
        template<typename Promise>
        struct Base {
            coroutine_handle<Promise> this_handle;

            constexpr bool await_ready() const noexcept { return false; }

            // save outer coroutine handler
            // and execute inner coroutine
            coroutine_handle<Promise>
            await_suspend(coroutine_handle<> super_handle) noexcept {
                this_handle.promise().super_handle = super_handle;
                return this_handle;
            }
        };

        // return value of co_return
        template<typename T, typename Promise>
        struct AwaiterBase: Base<Promise> {
            T&& await_resume() noexcept {
                return std::move(
                    this->this_handle.promise().get_result()
                );
            }
        };

        template<typename Promise>
        struct AwaiterBase<void, Promise>: Base<Promise> {
            void await_resume() noexcept {}
        };
    }
}

template<typename T>
struct Task {
    struct Promise;
    using promise_type = Promise;

    coroutine_handle<promise_type> this_handle;

    struct Promise: detail::promise::PromiseBase<T> {
        using State = detail::promise::State;
        using FinalAwaiter = detail::promise::FinalAwaiterBase<promise_type>;

        coroutine_handle<> super_handle;
        // from PromiseBase<T>
        // State state
        // T value

        Task get_return_object() noexcept {
            return { coroutine_handle<promise_type>::from_promise(*this) };
        }

        void unhandled_exception() noexcept { 
            this->state = State::Fail;
            // R.I.P
            std::terminate();
        }

        // this is a lazy coroutine!!
        constexpr suspend_always 
        initial_suspend() const noexcept { return {}; }
        
        FinalAwaiter final_suspend() const noexcept { return {}; }
    };
    
    // make the task itself awaitable, aka nested task
    using Awaiter = detail::task_awaiter::AwaiterBase<T, promise_type>;
    Awaiter operator co_await() noexcept {
        return { this_handle };
    }

    // may cause memory leak if the coroutine is not resumed
    void detach() noexcept {
        using State = detail::promise::State;
        if (this_handle == nullptr) return;
        this_handle.promise().state = State::Detach;
        this_handle = nullptr;
    }

    template<typename U>
    friend void spawn(Task<U>&& task) noexcept ;

    Task(coroutine_handle<promise_type> h = nullptr)noexcept: this_handle(h) {};
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task& operator=(Task&& rhs) = delete;
    Task(Task&& rhs) noexcept: this_handle(rhs.this_handle) {
        rhs.this_handle = nullptr;
    }
    ~Task() noexcept {
        // detached or moved
        if (!this_handle) return;

        // release coroutine frame
        this_handle.destroy();
    }
};

template<typename T>
void spawn(Task<T>&& task) noexcept {
    auto handle = task.this_handle;
    task.detach();
    handle.resume();
}

auto this_coro() noexcept {
    struct Awaiter {
        coroutine_handle<> this_handle;
        constexpr bool await_ready() { return false; }
        constexpr bool await_suspend(coroutine_handle<> h) {
            this_handle = h;
            // resume immediately
            return false;
        }
        coroutine_handle<> await_resume() {
            return this_handle;
        }
    };

    return Awaiter{};
}

}


namespace epoll {
    struct Poller {
        const int epfd;
        int count = 0;
        vector<epoll_event> events;

        Poller() noexcept: epfd(epoll_create1(0)) {
            events.reserve(256);
        }

        Poller(const Poller&) = delete;
        Poller& operator=(const Poller&) = delete;
        Poller(Poller&&) = delete;
        Poller& operator=(Poller&&) = delete;

        ~Poller() noexcept {
            close(epfd);
        }

        void add(int fd, uint32_t ev, void *ptr) noexcept {
            epoll_event ex {.events = ev, .data{.ptr = ptr}};
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ex) == 0) {
                ++count;
            }
        }

        void remove(int fd) noexcept {
            if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr) == 0) {
                --count;
            }
        }

        void start_loop() noexcept {
            auto evdata = events.data();
            while(count > 0) {
                int nfds = epoll_wait(epfd, evdata, count, -1);
                for(int i = 0; i < nfds; ++i) {
                    auto ev = evdata[i];
                    auto handle = coroutine_handle<>::from_address(ev.data.ptr);
                    handle.resume();
                }
            }
        }

        static Poller& instance() {
            static Poller instance{};
            return instance;
        }
    };

    auto ready(int fd, uint32_t ev) noexcept {
        struct Awaiter {
            int fd;
            uint32_t ev;

            constexpr bool await_ready() noexcept { return false; }

            // register event
            void await_suspend(coroutine_handle<> h) noexcept {
                auto& poller = Poller::instance();
                poller.add(fd, ev, h.address());
            }

            // remove event
            void await_resume() noexcept {
                auto& poller = Poller::instance();
                poller.remove(fd);
                if (ev & EPOLLOUT) {
                    close(fd);
                }
            }
        };

        // to avoid conflict
        if (ev & EPOLLIN) {
            return Awaiter{fd, ev};
        } else {
            int fd2 = utils::dup_fd(fd);
            return Awaiter{fd2, ev};
        }
    }

    enum class Mode {
        LT = 0,
        ET = 1
    };

    template<Mode mode = Mode::LT>
    auto readable(int fd) noexcept {
        if constexpr (mode == Mode::ET) {
            return ready(fd, EPOLLIN|EPOLLET);
        } else {
            return ready(fd, EPOLLIN);
        }
    }

    template<Mode mode = Mode::LT>
    auto writable(int fd) noexcept {
        if constexpr (mode == Mode::ET) {
            return ready(fd, EPOLLOUT|EPOLLET);
        } else {
            return ready(fd, EPOLLOUT);
        }
    }

    using coro::Task;
    template<typename T>
    T block_on(Task<T>&& task) noexcept {
        // take ownership
        Task<T> xtask = std::move(task);
        auto handle = xtask.this_handle;
        auto& poller = Poller::instance();
        handle.resume();
        poller.start_loop();
        return handle.promise().get_result();
        // dropped
    }

    template<>
    void block_on(Task<void>&& task) noexcept {
        Task<void> xtask = std::move(task);
        auto handle = xtask.this_handle;
        auto& poller = Poller::instance();
        handle.resume();
        poller.start_loop();
    }
}



using coro::Task;
using coro::spawn;
using coro::this_coro;

using epoll::readable;
using epoll::writable;
using epoll::block_on;

MINIO_NS_END
