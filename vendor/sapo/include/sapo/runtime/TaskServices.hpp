//
//  Sapo Engine — injected services (implementation_plan_2.md T3.1/T4.2/T4.3/T4.4).
//
//  One small struct instead of global state everywhere: every task receives the
//  services it needs (clock, HTTP transport, capability registry, data sources,
//  event bus, scheduler, state store, worker pool, logger/metrics). Tests build
//  a `TaskServices` with a `ManualClock` + `MockTransport` and run a whole
//  blueprint deterministically — no network, no sleeping, no real timers.
//
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#include "capabilities/CapabilityRegistry.hpp"
#include "config/ProviderConfig.hpp"
#include "data/DataSourceProvider.hpp"
#include "http/IHttpTransport.hpp"
#include "observability/Logger.hpp"
#include "observability/Metrics.hpp"
#include "runtime/Bindings.hpp"
#include "runtime/Clock.hpp"
#include "runtime/EventBus.hpp"
#include "runtime/Scheduler.hpp"
#include "runtime/StateStore.hpp"
#include "runtime/WorkerPool.hpp"
#include "runtime/WorkflowRegistry.hpp"

namespace sapo::runtime {

    /// Execution budgets. A workflow that exceeds these is a bug in the blueprint,
    /// and the engine says so instead of spinning forever.
    struct ExecutionLimits {
        /// Node activations per execution (loop iterations count individually).
        size_t max_node_visits{100000};
        /// Subflow nesting cap (`subflow.max_depth` may lower it per call).
        size_t max_depth{8};
        /// Total node activations per parallel branch, guarding branch-local cycles.
        size_t max_branch_visits{10000};
        /// Prompt/timer deadline when a blueprint omits one (0 ⇒ none).
        std::optional<int64_t> default_timeout_ms;
        /// Retry sleeps are executed on the worker; cap them so a misconfigured
        /// backoff cannot pin a thread for minutes.
        int64_t max_retry_delay_ms{5000};
        /// Suspend instead of sleeping for `wait` durations above this many ms
        /// (0 ⇒ always suspend, which is the durable default).
        int64_t inline_wait_limit_ms{0};
    };

    struct TaskServices {
        std::shared_ptr<obs::Logger> logger;
        std::shared_ptr<obs::MetricsRegistry> metrics;
        std::shared_ptr<obs::TraceRecorder> traces;
        ClockPtr clock;
        sapo::http::TransportPtr transport;
        std::shared_ptr<capabilities::CapabilityRegistry> capabilities;
        std::shared_ptr<data::DataSourceRegistry> data_sources;
        BindingProviderPtr bindings;
        std::shared_ptr<EventBus> events;
        std::shared_ptr<Scheduler> scheduler;
        std::shared_ptr<IStateStore> state_store;
        std::shared_ptr<WorkflowRegistry> workflows;
        std::shared_ptr<WorkerPool> pool;
        std::shared_ptr<config::ProviderConfigStore> provider_config;
        ExecutionLimits limits;

        /**
         * @brief How the engine waits (retry backoff). Overridable so tests run in
         * virtual time: install a sink that records the delay instead of sleeping.
         */
        std::function<void(int64_t delay_ms)> delay_sink = [](int64_t delay_ms) {
            if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        };

        /// A usable set of services with no network access: `NullTransport`
        /// (HTTP calls fail loudly), built-in data sources, in-memory state, and a
        /// shared worker pool.
        static TaskServices defaults();

        /// Everything the CLI/service checks before accepting work: capability
        /// registry audit, provider config problems, data-source declarations,
        /// workflow subflow references.
        [[nodiscard]] std::vector<std::string> startupCheck() const;

        /// Registers every secret from the config file with the logger so no log
        /// line can echo it (T3.2).
        void applySecretRedaction();
    };

} // namespace sapo::runtime
