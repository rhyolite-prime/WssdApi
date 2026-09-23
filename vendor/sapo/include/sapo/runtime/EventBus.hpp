//
//  Sapo Engine — event bus (implementation_plan_2.md T2.4).
//
//  In-process pub/sub used by `event` nodes, `on_event` handlers and by
//  workflows whose root `trigger.event` starts them. Correlation ids tie an
//  event to the session that emitted it so a whole conversation can be traced.
//
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sapo::runtime {

    struct Event {
        std::string name;                       // "payment.confirmed"
        nlohmann::json payload = nlohmann::json::object();
        std::string session_id;                 // emitter session (may be empty)
        std::string execution_id;
        std::string correlation_id;             // stable across a trigger chain
        std::string source_node;
        int64_t timestamp_ms{0};

        [[nodiscard]] nlohmann::json toJson() const;
        [[nodiscard]] static Event fromJson(const nlohmann::json &json);
    };

    /// Delivery guarantees (deliberately modest, and documented as such):
    /// synchronous dispatch to subscribers in subscription order; a throwing
    /// handler is logged and skipped so one bad listener cannot abort a
    /// workflow. No persistence, no replay beyond the recent-event ring.
    class EventBus {
    public:
        using SubscriptionId = uint64_t;
        using Handler = std::function<void(const Event &)>;

        /// `pattern` is an exact event name or a `prefix.*` wildcard
        /// (`payment.*` matches `payment.confirmed`). `*` matches everything.
        SubscriptionId subscribe(const std::string &pattern, Handler handler);
        bool unsubscribe(SubscriptionId id);

        void publish(Event event);
        void publish(const std::string &name, nlohmann::json payload = nlohmann::json::object());

        /// Events starting with this name (exact or wildcard) — used to find the
        /// workflows a trigger should start.
        [[nodiscard]] size_t subscriberCount() const;
        [[nodiscard]] std::vector<std::string> patterns() const;

        /// Bounded ring buffer of published events (debug + tests).
        [[nodiscard]] std::vector<Event> recent(size_t limit = 50) const;
        void setRecentCapacity(size_t capacity);
        void clearRecent();

        static EventBus &global();

    private:
        struct Subscriber {
            SubscriptionId id;
            std::string pattern;
            Handler handler;
        };

        [[nodiscard]] static bool matches(const std::string &pattern, const std::string &name);

        mutable std::mutex m_mutex;
        std::vector<Subscriber> m_subscribers;
        std::deque<Event> m_recent;
        size_t m_recent_capacity{256};
        SubscriptionId m_next_id{1};
    };

} // namespace sapo::runtime
