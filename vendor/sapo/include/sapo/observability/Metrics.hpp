//
//  Sapo Engine — metrics + trace recording (implementation_plan_2.md T4.4).
//
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace sapo::obs {

    class Logger;

    /**
     * @brief Tiny in-process metrics registry: counters, gauges and latency
     *        histograms. Exported as Prometheus text format by `sapo-server`
     *        (`/metrics`) and as JSON by `MetricsRegistry::toJson()`.
     */
    class MetricsRegistry {
    public:
        void increment(const std::string &name, double delta = 1.0);
        void setGauge(const std::string &name, double value);
        void observe(const std::string &name, double value_ms);

        [[nodiscard]] double counter(const std::string &name) const;
        [[nodiscard]] double gauge(const std::string &name) const;

        struct Summary {
            size_t count{0};
            double total{0.0};
            double p50{0.0};
            double p99{0.0};
            double max{0.0};
        };
        [[nodiscard]] Summary summary(const std::string &name) const;

        [[nodiscard]] nlohmann::json toJson() const;
        /// Prometheus text exposition (v0.0.4) — `# TYPE` lines + samples.
        [[nodiscard]] std::string toPrometheus() const;

        void reset();

        [[nodiscard]] static std::shared_ptr<MetricsRegistry> global();

    private:
        static constexpr size_t kMaxSamples = 4096;

        [[nodiscard]] static nlohmann::json summarizeLocked(const std::vector<double> &samples);


        mutable std::mutex m_mutex;
        std::unordered_map<std::string, double> m_counters;
        std::unordered_map<std::string, double> m_gauges;
        std::unordered_map<std::string, std::vector<double>> m_samples;
    };

    using MetricsPtr = std::shared_ptr<MetricsRegistry>;

    /**
     * @brief One span per executed node: start/end/duration/outcome/error.
     *
     * Spans are optionally streamed as JSON lines (trace file) and always
     * retained in a bounded ring buffer for `sapo-server` introspection.
     */
    struct TraceSpan {
        std::string execution_id;
        std::string node_id;
        std::string node_type;
        std::string outcome;             // success | failed | suspended | jumped | terminated
        std::string error_code;
        std::string error_message;
        int64_t start_ms{0};
        int64_t end_ms{0};
        int64_t duration_ms{0};
        size_t depth{0};                 // subflow/loop nesting level
    };

    class TraceRecorder {
    public:
        TraceRecorder() = default;
        explicit TraceRecorder(std::shared_ptr<Logger> logger, size_t ring_capacity = 4096);

        void begin(TraceSpan span);
        void end(const std::string &execution_id, const std::string &node_id,
                 const std::string &outcome, const std::string &error_code = {},
                 const std::string &error_message = {});

        [[nodiscard]] std::vector<TraceSpan> spans() const;
        [[nodiscard]] std::vector<TraceSpan> spansFor(const std::string &execution_id) const;
        void clear();

    private:
        mutable std::mutex m_mutex;
        std::shared_ptr<Logger> m_logger;
        size_t m_capacity{4096};
        std::vector<TraceSpan> m_open;
        std::vector<TraceSpan> m_completed;
    };

    using TracePtr = std::shared_ptr<TraceRecorder>;

} // namespace sapo::obs
