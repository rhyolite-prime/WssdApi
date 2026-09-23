//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//
//  Cron scheduler + durable timer queue (implementation_plan_2.md T2.2, T2.3).
//
//  Two responsibilities that share one mechanism — "wake me at time T":
//    • scheduled jobs (cron / @macros / `in 5 minutes`) that start workflows
//      or jump to a node, and
//    • durable wait timers created by `wait` nodes, which let the VM suspend
//      instead of parking a thread.
//
//  Time zones: fixed numeric offsets only ("UTC", "+03:00", "Africa/…" is a
//  documented deferral — see docs/LIMITATIONS.md). All storage is UTC epoch ms.
//
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/Clock.hpp"

namespace sapo::runtime {

    /// Parsed schedule. Either a calendar expression or a fixed relative delay.
    class CronExpression {
    public:
        enum class Kind { Cron, Relative };

        [[nodiscard]] static std::optional<CronExpression> parse(const std::string &text, std::string *error = nullptr);

        [[nodiscard]] Kind kind() const { return m_kind; }
        [[nodiscard]] const std::string &text() const { return m_text; }
        [[nodiscard]] int64_t intervalMs() const { return m_interval_ms; }
        /// `in 5 minutes` style schedules fire once; `every 5m` repeats.
        [[nodiscard]] bool oneShot() const { return m_one_shot; }
        [[nodiscard]] bool matches(int year, int month, int day, int weekday, int hour, int minute) const;

        /// First fire time strictly after `epoch_ms` (UTC), evaluated in `offset_seconds`.
        [[nodiscard]] std::optional<int64_t> nextAfter(int64_t epoch_ms, int offset_seconds) const;

    private:
        Kind m_kind{Kind::Cron};
        std::string m_text;
        std::set<int> m_minute, m_hour, m_dom, m_month, m_dow, m_year;
        bool m_dom_restricted{false};
        bool m_dow_restricted{false};
        int64_t m_interval_ms{0};
        bool m_one_shot{false};

        /// Parses one cron field ("*", "5", "1-5", "[*]/2", "mon,fri") into `out`.
        /// Returns false and fills `error` on malformed input; `restricted` reports
        /// whether the field was narrowed (needed for day-of-month/day-of-week OR).
        [[nodiscard]] static bool parseField(const std::string &field, int min_value, int max_value, bool is_dow,
                                             std::set<int> &out, std::string *error, bool *restricted = nullptr);
        /// Calendar-day part of the match (month / day-of-month / day-of-week / year).
        [[nodiscard]] bool dayMatches(int year, int month, int day, int weekday) const;
    };

    /// Grammar-level check used by the parser: is this a legal schedule string?
    [[nodiscard]] bool isValidCronExpression(const std::string &text);

    /// Next fire time for a schedule string. Returns nullopt when invalid.
    [[nodiscard]] std::optional<int64_t> nextScheduledTime(const std::string &text, int64_t from_ms,
                                                            const std::string &timezone, std::string *error = nullptr);

    /// A workflow job registered with the scheduler.
    struct ScheduledJob {
        std::string id;                    // stable identity (persisted)
        std::string schedule;              // cron / @macro / "in 5 minutes"
        std::string timezone{"UTC"};
        std::string workflow;              // workflow id to start…
        std::string node_id;               // …or the scheduling node (for `schedule` tasks)
        std::string entry_node;            // node inside `workflow` to start at (0-based entry override)
        nlohmann::json input = nlohmann::json::object();
        bool enabled{true};
        /// Fire every missed occurrence (bounded by max_catch_up) instead of
        /// skipping ahead after downtime.
        bool catch_up{false};
        int max_catch_up{5};
        int64_t next_fire_ms{0};
        int64_t last_fire_ms{0};
        size_t fire_count{0};

        [[nodiscard]] nlohmann::json toJson() const;
        [[nodiscard]] static ScheduledJob fromJson(const nlohmann::json &json);
    };

    /// A durable timer attached to a suspended session.
    struct Timer {
        std::string id;
        std::string session_id;
        int64_t due_ms{0};
        std::string resume_node;            // node to run when it fires
        std::string reason;                 // "wait" | "prompt_timeout" | …
        std::string context_checkpoint_key; // StateStore key holding the resume payload

        [[nodiscard]] nlohmann::json toJson() const;
        [[nodiscard]] static Timer fromJson(const nlohmann::json &json);
    };

    /**
     * @brief Job + timer registry with a real clock.
     *
     * Threading model: `tick()` is called by whoever owns time (the CLI loop, the
     * service poller, or this class's own background thread via `start()`). Tests
     * drive it with a `ManualClock`.
     */
    class Scheduler {
    public:
        using JobCallback = std::function<void(const ScheduledJob &, int64_t fired_at_ms)>;
        using TimerCallback = std::function<void(const Timer &)>;

        explicit Scheduler(ClockPtr clock = defaultClock());
        ~Scheduler();

        void onJobFired(JobCallback callback) { m_job_callback = std::move(callback); }
        void onTimerDue(TimerCallback callback) { m_timer_callback = std::move(callback); }

        // --- jobs -----------------------------------------------------------
        /// Registers (or replaces) a job; computes the first fire time.
        /// Throws SapoError(ErrorCode::Parse) for an invalid schedule.
        std::string addJob(ScheduledJob job);
        bool removeJob(const std::string &id);
        bool enableJob(const std::string &id, bool enabled);
        [[nodiscard]] std::optional<ScheduledJob> job(const std::string &id) const;
        [[nodiscard]] std::vector<ScheduledJob> jobs() const;

        // --- timers ---------------------------------------------------------
        std::string addTimer(Timer timer);
        bool cancelTimer(const std::string &id);
        bool cancelTimersForSession(const std::string &session_id);
        [[nodiscard]] std::optional<Timer> timer(const std::string &id) const;
        [[nodiscard]] std::vector<Timer> timers() const;
        [[nodiscard]] std::vector<Timer> timersForSession(const std::string &session_id) const;

        /// Fires everything due at `clock->nowMs()`. Returns how many fired.
        size_t tick();
        /// Same, evaluated against an explicit instant (used by tests / catch-up).
        size_t tickUntil(int64_t now_ms);
        /// Next instant anything is due (0 when idle).
        [[nodiscard]] int64_t nextWakeMs() const;
        [[nodiscard]] size_t pendingCount() const;

        // --- driving --------------------------------------------------------
        void start(std::chrono::milliseconds poll_interval = std::chrono::milliseconds(250));
        void stop();
        [[nodiscard]] bool running() const;

        // --- persistence (T2.3 "persistent job registry") -------------------
        bool saveToFile(const std::string &path) const;
        bool loadFromFile(const std::string &path);
        [[nodiscard]] nlohmann::json toJson() const;

    private:
        void rescheduleLocked(ScheduledJob &job, int64_t from_ms);

        ClockPtr m_clock;
        mutable std::mutex m_mutex;
        std::vector<ScheduledJob> m_jobs;
        std::vector<Timer> m_timers;
        JobCallback m_job_callback;
        TimerCallback m_timer_callback;
        std::atomic_bool m_running{false};
        std::unique_ptr<std::thread> m_thread;
    };

} // namespace sapo::runtime
