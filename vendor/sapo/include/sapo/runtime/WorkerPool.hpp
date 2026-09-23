//
//  Sapo Engine — bounded worker pool (implementation_plan_2.md T4.2).
//
//  Replaces the unbounded `std::async` fan-out: parallel branches, subflow
//  launches and the wait-poller share one fixed-size pool, so a blueprint with
//  10 000 concurrent branches cannot spawn 10 000 threads.
//
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <tuple>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace sapo::runtime {

    class WorkerPool {
    public:
        explicit WorkerPool(size_t threads = 0) {
            if (threads == 0) {
                threads = std::thread::hardware_concurrency();
                if (threads == 0) threads = 4;
            }
            m_max_threads = threads;
            spawn(threads);
        }

        ~WorkerPool() { shutdown(); }

        WorkerPool(const WorkerPool &) = delete;
        WorkerPool &operator=(const WorkerPool &) = delete;

        /// Submits work; the returned future carries the callable's value
        /// (exceptions are transported, never swallowed).
        template<typename Function>
        [[nodiscard]] auto submit(Function &&function) -> std::future<std::invoke_result_t<Function>> {
            using Result = std::invoke_result_t<Function>;
            auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Function>(function));
            std::future<Result> future = task->get_future();
            {
                std::scoped_lock lock(m_mutex);
                m_queue.emplace_back([task] { (*task)(); });
                ++m_submitted;
                if (m_queue.size() > m_max_threads * 4) growLocked();
            }
            m_condition.notify_one();
            return future;
        }

        /// Runs every function on the pool and returns once all finished.
        /// The first exception (if any) is rethrown after the batch completes.
        template<typename Function>
        void runAll(std::vector<Function> &functions) {
            if (functions.empty()) return;
            std::atomic<size_t> remaining{functions.size()};
            std::mutex error_mutex;
            std::exception_ptr first_error;
            std::promise<void> done;
            std::future<void> done_future = done.get_future();
            for (auto &function : functions) {
                std::ignore = submit([&function, &remaining, &error_mutex, &first_error, &done] {
                    try {
                        function();
                    } catch (...) {
                        std::scoped_lock lock(error_mutex);
                        if (!first_error) first_error = std::current_exception();
                    }
                    if (remaining.fetch_sub(1) == 1) done.set_value();
                });
            }
            done_future.wait();
            if (first_error) std::rethrow_exception(first_error);
        }

        /// Blocks until the queue drains (used by tests and by `shutdown`).
        void waitIdle() {
            std::unique_lock lock(m_mutex);
            m_idle.wait(lock, [this] { return m_queue.empty() && m_active == 0; });
        }

        [[nodiscard]] size_t size() const {
            std::scoped_lock lock(m_mutex);
            return m_threads.size();
        }
        [[nodiscard]] size_t queued() const {
            std::scoped_lock lock(m_mutex);
            return m_queue.size();
        }
        [[nodiscard]] size_t submitted() const { return m_submitted.load(); }
        [[nodiscard]] size_t maxThreads() const { return m_max_threads * 4; }

        void shutdown() {
            {
                std::scoped_lock lock(m_mutex);
                if (m_stopping) return;
                m_stopping = true;
            }
            m_condition.notify_all();
            for (auto &thread : m_threads) {
                if (thread.joinable()) thread.join();
            }
            m_threads.clear();
        }

        /// Shared pool for the process (services default to this).
        static WorkerPool &shared() {
            static WorkerPool pool;
            return pool;
        }

    private:
        void growLocked() {
            if (m_threads.size() >= m_max_threads) return;
            spawn(1);
        }

        void spawn(size_t count) {
            for (size_t i = 0; i < count; ++i) m_threads.emplace_back([this] { workerLoop(); });
        }

        void workerLoop() {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lock(m_mutex);
                    m_condition.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
                    if (m_stopping && m_queue.empty()) {
                        --m_active;
                        m_idle.notify_all();
                        return;
                    }
                    job = std::move(m_queue.front());
                    m_queue.pop_front();
                    ++m_active;
                }
                try {
                    job();
                } catch (...) {
                    // packaged_task already transports the exception to the future;
                    // a bare job (runAll) handles its own.
                }
                std::scoped_lock lock(m_mutex);
                --m_active;
                if (m_queue.empty() && m_active == 0) m_idle.notify_all();
            }
        }

        mutable std::mutex m_mutex;
        std::condition_variable m_condition;
        std::condition_variable m_idle;
        std::deque<std::function<void()>> m_queue;
        std::vector<std::thread> m_threads;
        size_t m_active{0};
        size_t m_max_threads{64};
        std::atomic_size_t m_submitted{0};
        bool m_stopping{false};
    };

    /// Process-wide pool handed out as a `shared_ptr` with non-owning semantics,
    /// so `TaskServices` can hold one without shutting the pool down on copy.
    /// Inject your own pool in tests.
    [[nodiscard]] inline std::shared_ptr<WorkerPool> sharedWorkerPool() {
        static std::shared_ptr<WorkerPool> pool(&WorkerPool::shared(), [](WorkerPool *) {});
        return pool;
    }

} // namespace sapo::runtime
