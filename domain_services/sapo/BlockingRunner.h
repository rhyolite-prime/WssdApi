//
// BlockingRunner.h — offload for blocking Sapo calls.
//
// Drogon serves requests on a small pool of IO threads; a Sapo turn does
// blocking socket/file I/O (Redis state store, HTTP capability calls) and
// must never run there. BlockingRunner owns a fixed worker pool and exposes
// a coroutine awaitable so handlers stay linear:
//
//     auto outcome = co_await BlockingRunner::instance().run([&] {
//         return engine.executeUssdTurn(...);  // runs on a worker thread
//     });
//
// Exceptions thrown by the functor are rethrown at the co_await site.
//

#pragma once

#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <trantor/utils/Logger.h>

namespace wssd_api::sapo_host {

template <typename T>
class BlockingAwaitable;

class BlockingRunner {
  public:
    static BlockingRunner &instance() {
        static BlockingRunner runner;
        return runner;
    }

    BlockingRunner(const BlockingRunner &) = delete;
    BlockingRunner &operator=(const BlockingRunner &) = delete;

    /// Starts the pool (idempotent). A count of 0 picks a hardware default.
    void start(std::size_t threads = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        startLocked(threads == 0 ? defaultThreadCount() : threads);
    }

    /// Drains and joins every worker; the pool auto-restarts on next use.
    void stop() {
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!started_) {
                return;
            }
            shutdown_ = true;
            workers.swap(workers_);
            started_ = false;
        }
        cv_.notify_all();
        for (auto &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = false;
        std::queue<std::function<void()>> empty;
        queue_.swap(empty);
    }

    /// Queues a fire-and-forget task (exceptions are logged, never escape).
    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!started_) {
                startLocked(defaultThreadCount());
            }
            queue_.push(std::move(task));
        }
        cv_.notify_one();
    }

    /// Runs `func` on a worker and makes the result awaitable from a
    /// Drogon coroutine. The functor must be self-contained: it is copied
    /// onto the worker thread.
    template <typename F>
    auto run(F &&func);

  private:
    BlockingRunner() = default;
    ~BlockingRunner() {
        stop();
    }

    static std::size_t defaultThreadCount() {
        const auto hardware = std::thread::hardware_concurrency();
        return hardware <= 4 ? 4 : hardware;
    }

    void startLocked(std::size_t threads) {
        if (started_) {
            return;
        }
        shutdown_ = false;
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
        started_ = true;
    }

    void workerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return shutdown_ || !queue_.empty(); });
                if (shutdown_ && queue_.empty()) {
                    return;
                }
                task = std::move(queue_.front());
                queue_.pop();
            }
            try {
                task();
            } catch (const std::exception &e) {
                LOG_ERROR << "[sapo] blocking task threw: " << e.what();
            } catch (...) {
                LOG_ERROR << "[sapo] blocking task threw an unknown exception";
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool started_ = false;
    bool shutdown_ = false;
};

template <typename T>
class BlockingAwaitable {
  public:
    explicit BlockingAwaitable(std::function<T()> func)
        : state_(std::make_shared<State>(std::move(func))) {}

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        state_->handle = handle;
        // The shared state (not `this`) crosses threads, so the awaitable
        // itself may die while the worker is still running.
        BlockingRunner::instance().post([state = state_] {
            try {
                state->result.emplace(state->func());
            } catch (...) {
                state->exception = std::current_exception();
            }
            state->handle.resume();
        });
    }

    T await_resume() {
        if (state_->exception) {
            std::rethrow_exception(state_->exception);
        }
        return std::move(*state_->result);
    }

  private:
    struct State {
        explicit State(std::function<T()> func) : func(std::move(func)) {}
        std::function<T()> func;
        std::optional<T> result;
        std::exception_ptr exception;
        std::coroutine_handle<> handle;
    };
    std::shared_ptr<State> state_;
};

template <>
class BlockingAwaitable<void> {
  public:
    explicit BlockingAwaitable(std::function<void()> func)
        : state_(std::make_shared<State>(std::move(func))) {}

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        state_->handle = handle;
        BlockingRunner::instance().post([state = state_] {
            try {
                state->func();
            } catch (...) {
                state->exception = std::current_exception();
            }
            state->handle.resume();
        });
    }

    void await_resume() {
        if (state_->exception) {
            std::rethrow_exception(state_->exception);
        }
    }

  private:
    struct State {
        explicit State(std::function<void()> func) : func(std::move(func)) {}
        std::function<void()> func;
        std::exception_ptr exception;
        std::coroutine_handle<> handle;
    };
    std::shared_ptr<State> state_;
};

template <typename F>
auto BlockingRunner::run(F &&func) {
    using R = std::invoke_result_t<std::decay_t<F>>;
    return BlockingAwaitable<R>(std::function<R()>(std::forward<F>(func)));
}

}  // namespace wssd_api::sapo_host
