//
//  Sapo Engine — time source abstraction.
//
//  Waits, schedules, timeouts and metrics all read wall-clock time through
//  `IClock`. Production uses `SystemClock`; tests and the accelerated-clock
//  acceptance criteria (implementation_plan_2.md T2.2/T2.3) drive a
//  `ManualClock` so cron/timer behaviour is deterministic.
//
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

namespace sapo::runtime {

    /// Milliseconds since the unix epoch (UTC).
    using EpochMs = std::chrono::milliseconds;

    class IClock {
    public:
        virtual ~IClock() = default;

        [[nodiscard]] virtual EpochMs now() const = 0;

        /// Monotonic millisecond stamp, used for durations/latency.
        [[nodiscard]] virtual EpochMs steadyNow() const = 0;
    };

    class SystemClock final : public IClock {
    public:
        [[nodiscard]] EpochMs now() const override {
            return std::chrono::duration_cast<EpochMs>(
                std::chrono::system_clock::now().time_since_epoch());
        }

        [[nodiscard]] EpochMs steadyNow() const override {
            return std::chrono::duration_cast<EpochMs>(
                std::chrono::steady_clock::now().time_since_epoch());
        }
    };

    /**
     * @brief Test double: time only moves when the test says so.
     */
    class ManualClock final : public IClock {
    public:
        explicit ManualClock(EpochMs start = std::chrono::milliseconds(1'700'000'000'000))
            : m_now(start), m_steady(start) {}

        [[nodiscard]] EpochMs now() const override {
            std::scoped_lock lock(m_mutex);
            return m_now;
        }

        [[nodiscard]] EpochMs steadyNow() const override {
            std::scoped_lock lock(m_mutex);
            return m_steady;
        }

        void advanceMs(int64_t ms) { advance(std::chrono::milliseconds(ms)); }

        void advance(EpochMs delta) {
            std::scoped_lock lock(m_mutex);
            m_now += delta;
            m_steady += delta;
        }

        void setNow(EpochMs value) {
            std::scoped_lock lock(m_mutex);
            m_now = value;
            m_steady = value;
        }

    private:
        mutable std::mutex m_mutex;
        EpochMs m_now;
        EpochMs m_steady;
    };

    using ClockPtr = std::shared_ptr<IClock>;

    inline ClockPtr defaultClock() {
        static ClockPtr clock = std::make_shared<SystemClock>();
        return clock;
    }

} // namespace sapo::runtime
