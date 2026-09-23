//
//  Sapo Engine — structured logging (implementation_plan_2.md T4.4).
//
//  Replaces the `std::cout` diagnostics that used to be the only observability
//  surface. Records are emitted as JSON lines so they can be piped into any
//  APM; secrets registered through the provider-config layer are redacted by
//  value before anything leaves the process.
//
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sapo::obs {

    enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

    [[nodiscard]] const char *toString(LogLevel level);
    [[nodiscard]] bool levelAtLeast(LogLevel configured, LogLevel message);

    struct LogRecord {
        LogLevel level{LogLevel::Info};
        std::string component;         // e.g. "interpreter", "capability.fs"
        std::string message;
        std::string execution_id;      // correlation id when available
        std::string node_id;
        nlohmann::json fields = nlohmann::json::object();
        int64_t timestamp_ms{0};
    };

    /**
     * @brief Sink interface so hosts can route logs wherever they like
     *        (Drogon logger, file, in-memory ring buffer for tests, ...).
     */
    class ILogSink {
    public:
        virtual ~ILogSink() = default;
        virtual void write(const LogRecord &record) = 0;
    };

    /// Human readable single line (default sink) — good enough for `sapoc run`.
    class ConsoleSink final : public ILogSink {
    public:
        explicit ConsoleSink(bool /*colorize*/ = false) {}
        void write(const LogRecord &record) override;
    };

    /// JSON lines — one object per record; the format tests and APM agents read.
    class JsonLineSink final : public ILogSink {
    public:
        explicit JsonLineSink(std::ostream &stream) : m_stream(&stream) {}
        /// Owning variant: the sink keeps its stream (typically an open file) alive
        /// for as long as the sink is registered.
        explicit JsonLineSink(std::shared_ptr<std::ostream> stream)
            : m_owner(std::move(stream)), m_stream(m_owner.get()) {}
        void write(const LogRecord &record) override;

    private:
        std::shared_ptr<std::ostream> m_owner;
        std::ostream *m_stream;
    };

    /// Collects records in memory (tests, `sapo-server` debug endpoint).
    class MemorySink final : public ILogSink {
    public:
        void write(const LogRecord &record) override;
        [[nodiscard]] std::vector<LogRecord> records() const;
        [[nodiscard]] size_t size() const;
        void clear();
        bool contains(const std::string &needle) const;

    private:
        mutable std::mutex m_mutex;
        std::vector<LogRecord> m_records;
    };

    /**
     * @brief Logger: level filter + shared sinks + secret redaction.
     *
     * Thread-safe; cheap to copy via shared_ptr (contexts carry a `LoggerPtr`).
     */
    class Logger : public std::enable_shared_from_this<Logger> {
    public:
        Logger() = default;

        [[nodiscard]] std::shared_ptr<Logger> withComponent(std::string component) const;

        void setLevel(LogLevel level) { m_level = level; }
        [[nodiscard]] LogLevel level() const { return m_level; }

        void addSink(std::shared_ptr<ILogSink> sink);
        void clearSinks();

        /// Register a literal value that must never appear in a log line.
        void addSecret(const std::string &value);
        void clearSecrets();

        void log(LogLevel level, const std::string &component, const std::string &message,
                 const nlohmann::json &fields = nullptr,
                 const std::string &execution_id = {},
                 const std::string &node_id = {}) const;

        void debug(const std::string &component, const std::string &message,
                   const nlohmann::json &fields = nullptr) const {
            log(LogLevel::Debug, component, message, fields);
        }
        void info(const std::string &component, const std::string &message,
                  const nlohmann::json &fields = nullptr) const {
            log(LogLevel::Info, component, message, fields);
        }
        void warn(const std::string &component, const std::string &message,
                  const nlohmann::json &fields = nullptr) const {
            log(LogLevel::Warn, component, message, fields);
        }
        void error(const std::string &component, const std::string &message,
                   const nlohmann::json &fields = nullptr) const {
            log(LogLevel::Error, component, message, fields);
        }

        /// Applies the redaction set to arbitrary text (used for traces too).
        [[nodiscard]] std::string redact(std::string_view text) const;

        static std::shared_ptr<Logger> defaultLogger();
        static void setGlobal(const std::shared_ptr<Logger> &logger);
        [[nodiscard]] static std::shared_ptr<Logger> global();

    private:
        mutable std::mutex m_mutex;
        LogLevel m_level{LogLevel::Info};
        std::vector<std::shared_ptr<ILogSink>> m_sinks;
        std::vector<std::string> m_secrets;
    };

    using LoggerPtr = std::shared_ptr<Logger>;

    [[nodiscard]] LoggerPtr defaultLogger();

} // namespace sapo::obs
