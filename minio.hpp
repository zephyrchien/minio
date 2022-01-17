#pragma once

#include <coroutine>
#include <concepts>
#include <utility>
#include <exception>
#include <vector>
#include <list>
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
using std::list;

#define MINIO_NS_BEG namespace minio {
#define MINIO_NS_END }


MINIO_NS_BEG

namespace concepts {
    template<typename T>
    concept AsRawFd = requires(T x, uint32_t ev) {
        { x.as_raw_fd() } -> std::same_as<int>;
        { x.is_registered() } -> std::same_as<bool>;
        { x.is_ready(ev) } -> std::same_as<bool>;
        x.set_readiness(ev);
        x.clear_readiness(ev);
    };
}




namespace sys {
    struct RawFd {
        struct State {
            unsigned int owned: 1;
            unsigned int registered: 1;
        };

        int fd;
        State state;
        uint32_t ev;

        explicit RawFd(int fd) noexcept: fd(fd), state({1,0}), ev(0) {}
        RawFd(const RawFd&) = delete;
        RawFd& operator=(const RawFd&) = delete;
        RawFd(RawFd&& rawfd) noexcept: 
            fd(rawfd.fd), state(rawfd.state), ev(rawfd.ev) {
            rawfd.fd = -1;
            rawfd.state.owned = 0;
        }
        RawFd& operator=(RawFd&& rawfd) noexcept {
            fd = rawfd.fd;
            state = rawfd.state;
            rawfd.fd = -1;
            rawfd.state.owned = 0;
            return *this;
        }
        ~RawFd() noexcept {
            if(fd > 0 && state.owned) close(fd);
        }

        // impl AsRawFd
        int as_raw_fd() const noexcept { return fd; }
        bool is_registered() const noexcept { return state.registered; }
        bool is_ready(uint32_t ex) const noexcept { return ev & ex; }
        void set_readiness(uint32_t ex) noexcept { ev |= ex; }
        void clear_readiness(uint32_t ex) noexcept { ev &= ~ex; }
    };
}



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

// affect initial_suspend
enum class Mode {
    LAZY = 0,
    EAGER = 1
};

template<typename T, Mode mode = Mode::LAZY>
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

        // this is a lazy coroutine by default!!
        constexpr std::conditional_t<
            mode==Mode::LAZY,
            suspend_always,
            suspend_never>
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
    using sys::RawFd;
    struct LT{};
    struct ET{};
    constexpr auto use_lt = LT{};
    constexpr auto use_et = ET{};

    namespace detail {
        struct PollerBase {
            const int epfd;
            int count = 0;

            PollerBase() noexcept: epfd(epoll_create1(0)) {}

            PollerBase(const PollerBase&) = delete;
            PollerBase& operator=(const PollerBase&) = delete;
            PollerBase(PollerBase&&) = delete;
            PollerBase& operator=(PollerBase&&) = delete;

            ~PollerBase() noexcept {
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
        };
    }

    template<typename T = LT>
    struct Poller: detail::PollerBase {
        static Poller& instance() {
            static Poller instance{};
            return instance;
        }

        void start_loop() noexcept {
            vector<epoll_event> events{};
            events.reserve(1024);

            while(count > 0) {
                events.resize(count);
                auto evdata = events.data();
                
                int nfds = epoll_wait(epfd, evdata, count, -1);

                for(int i = 0; i < nfds; ++i) {
                    auto ev = evdata[i];
                    auto handle = coroutine_handle<>::from_address(ev.data.ptr);
                    handle.resume();
                }
            }
        }
    };

    template<>
    struct Poller<ET>: detail::PollerBase {
        struct EventHandle {
            uint32_t ev;
            RawFd *rawfd;
            coroutine_handle<> h;
        };

        list<EventHandle> pending_list{};

        static Poller& instance() {
            static Poller instance{};
            return instance;
        }

        void start_loop() noexcept {
            vector<epoll_event> events{};
            events.reserve(1024);

            while(count > 0) {
                events.resize(count);
                auto evdata = events.data();
                
                int nfds = epoll_wait(epfd, evdata, count, -1);

                for(int i = 0; i < nfds; ++i) {
                    auto ev = evdata[i];
                    auto rawfd = static_cast<RawFd*>(ev.data.ptr);
                    rawfd->set_readiness(ev.events);
                }
                for(auto iter = pending_list.begin();
                    iter != pending_list.end();) {
                    if (!iter->rawfd->is_ready(iter->ev)) {
                        ++iter;
                        continue;
                    }
                    iter->h.resume();
                    iter = pending_list.erase(iter);
                }
            }
        }
    };

    // LT
    auto ready(int fd, uint32_t ev, LT = use_lt) noexcept {
        struct Awaiter {
            int fd;
            uint32_t ev;

            constexpr bool await_ready() noexcept { return false; }

            // register event
            void await_suspend(coroutine_handle<> h) noexcept {
                auto& poller = Poller<LT>::instance();
                poller.add(fd, ev, h.address());
            }

            // remove event
            void await_resume() noexcept {
                auto& poller = Poller<LT>::instance();
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


    // ET
    template<typename T>
    requires concepts::AsRawFd<T>
    auto ready(T& rawfd, uint32_t ev, ET) noexcept {
        struct Awaiter {
            RawFd *rawfd;
            uint32_t ev;

            bool await_ready() noexcept {
                return rawfd->is_ready(ev); 
            }

            void await_suspend(coroutine_handle<> h) noexcept {
                auto& poller = Poller<ET>::instance();
                if (!rawfd->is_registered()) {
                    poller.add(
                        rawfd->as_raw_fd(),
                        EPOLLIN|EPOLLOUT|EPOLLET,
                        rawfd
                    );
                }
                poller.pending_list.emplace_back(
                    ev, rawfd, h
                );
            }

            void await_resume() noexcept {}
        };
        return Awaiter{&rawfd, ev};
    }


    auto readable(int fd, LT = use_lt) noexcept {
        return ready(fd, EPOLLIN);
    }

    template<typename T>
    requires concepts::AsRawFd<T>
    auto readable(T& rawfd, ET) noexcept {
        return ready(rawfd, EPOLLIN, use_et);
    }

    auto writable(int fd, LT = use_lt) noexcept {
        return ready(fd, EPOLLOUT);
    }

    template<typename T>
    requires concepts::AsRawFd<T>
    auto writable(T& rawfd, ET) noexcept {
        return ready(rawfd, EPOLLOUT, use_et);
    }

    using coro::Task;
    template<typename T, typename U = LT>
    T block_on(Task<T>&& task) noexcept {
        // take ownership
        Task<T> xtask = std::move(task);
        auto handle = xtask.this_handle;
        auto& poller = Poller<U>::instance();
        handle.resume();
        poller.start_loop();
        return handle.promise().get_result();
        // dropped
    }

    template<typename U = LT>
    void block_on(Task<void>&& task) noexcept {
        Task<void> xtask = std::move(task);
        auto handle = xtask.this_handle;
        auto& poller = Poller<U>::instance();
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
