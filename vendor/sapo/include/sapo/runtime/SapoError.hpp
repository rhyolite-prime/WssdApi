//
//  Sapo Engine — canonical error type.
//
//  Every subsystem reports failures through `SapoError` so the interpreter can
//  build a structured `$error` object (code / message / node / data) for the
//  error-handling model (implementation_plan_2.md, T2.6) instead of relying on
//  ad-hoc log lines.
//
#pragma once

#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace sapo::runtime {

    /**
     * @brief Stable, machine-readable error families. Serialized verbatim into
     *        `$error.code` and trace records.
     */
    enum class ErrorCode {
        Parse,            // blueprint rejected by the parser / validator
        Validation,       // expression or schema validation failed at runtime
        Routing,          // unknown / unreachable jump target
        NotFound,         // variable, node, workflow, capability, data source missing
        Expression,       // SEL compile or evaluation error
        Http,             // transport-level failure (connection, DNS, TLS, timeout)
        HttpStatus,       // non-2xx response captured from an HTTP call
        Capability,       // plugin/capability execution failure
        DataSource,       // query provider failure
        Store,            // state-store failure (connection, CAS conflict, corrupt checkpoint)
        Timeout,          // a wait / prompt / capability deadline expired
        Limit,            // max_iterations / recursion depth / rate limit hit
        NotImplemented,   // explicitly deferred capability or provider
        Internal          // anything unmapped
    };

    [[nodiscard]] inline const char *toString(ErrorCode code) {
        switch (code) {
            case ErrorCode::Parse: return "PARSE_ERROR";
            case ErrorCode::Validation: return "VALIDATION_ERROR";
            case ErrorCode::Routing: return "ROUTING_ERROR";
            case ErrorCode::NotFound: return "NOT_FOUND";
            case ErrorCode::Expression: return "EXPRESSION_ERROR";
            case ErrorCode::Http: return "HTTP_ERROR";
            case ErrorCode::HttpStatus: return "HTTP_STATUS_ERROR";
            case ErrorCode::Capability: return "CAPABILITY_ERROR";
            case ErrorCode::DataSource: return "DATA_SOURCE_ERROR";
            case ErrorCode::Store: return "STORE_ERROR";
            case ErrorCode::Timeout: return "TIMEOUT";
            case ErrorCode::Limit: return "LIMIT_EXCEEDED";
            case ErrorCode::NotImplemented: return "NOT_IMPLEMENTED";
            case ErrorCode::Internal: return "INTERNAL_ERROR";
        }
        return "INTERNAL_ERROR";
    }

    /**
     * @brief Recoverable workflow failure. `data` carries node-specific detail
     *        (e.g. the captured HTTP response body for HttpStatus errors).
     */
    class SapoError : public std::runtime_error {
    public:
        SapoError(ErrorCode code, std::string message, nlohmann::json data = nullptr,
                  std::string node_id = {})
            : std::runtime_error(message),
              m_code(code),
              m_message(std::move(message)),
              m_data(std::move(data)),
              m_node_id(std::move(node_id)) {}

        [[nodiscard]] ErrorCode code() const noexcept { return m_code; }
        [[nodiscard]] const std::string &codeString() const noexcept { return m_codeString; }
        [[nodiscard]] const std::string &message() const noexcept { return m_message; }
        [[nodiscard]] const nlohmann::json &data() const noexcept { return m_data; }
        [[nodiscard]] const std::string &nodeId() const noexcept { return m_node_id; }

        void setNodeId(std::string node_id) { m_node_id = std::move(node_id); }

        /** Materializes the `$error` context object consumed by catch blocks. */
        [[nodiscard]] nlohmann::json toJson() const {
            nlohmann::json j = {
                {"code", m_codeString},
                {"message", m_message},
                {"node", m_node_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(m_node_id)}
            };
            if (!m_data.is_null()) j["data"] = m_data;
            return j;
        }

    private:
        ErrorCode m_code;
        std::string m_codeString{::sapo::runtime::toString(m_code)};
        std::string m_message;
        nlohmann::json m_data;
        std::string m_node_id;
    };

} // namespace sapo::runtime
