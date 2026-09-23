//
//  Sapo Engine — HTTP transport seam.
//
//  Tasks never talk to a network library directly: they build a `Request`, the
//  host supplies an `IHttpTransport`. `sapo_core` ships a mock + record/replay
//  transport (so blueprint tests never touch live APIs, plan T5.2) and an
//  optional libcurl/cpr adapter compiled with SAPO_ENABLE_CPR.
//
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sapo::http {

    struct Request {
        std::string method = "GET";              // GET/POST/PUT/PATCH/DELETE/HEAD
        std::string url;
        nlohmann::json headers = nlohmann::json::object();
        nlohmann::json query = nlohmann::json::object();
        std::optional<nlohmann::json> body;      // string or structured
        int timeout_ms{10000};
        bool follow_redirects{true};
        std::optional<std::string> basic_auth_user;
        std::optional<std::string> basic_auth_password;
        std::optional<std::string> bearer_token;

        [[nodiscard]] std::string describe() const;
    };

    struct Response {
        int status_code{0};
        std::string body;
        nlohmann::json headers = nlohmann::json::object();
        double elapsed_ms{0.0};
        std::string transport_error;             // non-empty ⇒ connection-level failure

        [[nodiscard]] bool ok() const { return transport_error.empty() && status_code >= 200 && status_code < 300; }
        [[nodiscard]] bool successful() const { return transport_error.empty() && status_code > 0; }
        [[nodiscard]] nlohmann::json jsonBody() const;
    };

    class IHttpTransport {
    public:
        virtual ~IHttpTransport() = default;
        [[nodiscard]] virtual Response send(const Request &request) = 0;
        /// Optional: transports that cannot reach the network report why.
        [[nodiscard]] virtual std::string name() const { return "unknown"; }
    };

    using TransportPtr = std::shared_ptr<IHttpTransport>;

    /// Fails loudly rather than pretending (used when CPR is not compiled in).
    class NullTransport final : public IHttpTransport {
    public:
        [[nodiscard]] Response send(const Request &request) override;
        [[nodiscard]] std::string name() const override { return "null"; }
    };

    /**
     * @brief Scriptable transport for tests and offline runs.
     *
     * Routes are keyed by `"METHOD url-without-query"` (or the full URL when a
     * query match is required) and may be programmed with a status/body pair or
     * a handler callback (used to simulate retries, latency, malformed bodies).
     */
    class MockTransport final : public IHttpTransport {
    public:
        using Handler = std::function<Response(const Request &)>;

        MockTransport();
        ~MockTransport() override;

        void on(const std::string &method_url, Response response);
        void on(const std::string &method_url, Handler handler);
        /// Convenience: JSON body with status 200.
        void jsonResponse(const std::string &method_url, const nlohmann::json &payload, int status = 200);
        /// Response served once the route has been hit `after_failures` times.
        void failThenSucceed(const std::string &method_url, int failures, Response success);

        [[nodiscard]] Response send(const Request &request) override;
        [[nodiscard]] std::string name() const override { return "mock"; }

        [[nodiscard]] std::vector<Request> requests() const;
        [[nodiscard]] size_t callCount() const;
        [[nodiscard]] std::optional<Request> lastRequest() const;
        void clear();

    private:
        mutable std::mutex m_mutex;
        std::map<std::string, Handler> m_routes;
        std::vector<Request> m_calls;
    };

    using MockTransportPtr = std::shared_ptr<MockTransport>;

    /**
     * @brief Cassettes: wraps a live transport, persists every exchange to a
     *        JSON file and replays it afterwards (HTTP record/replay testing).
     */
    class RecordReplayTransport final : public IHttpTransport {
    public:
        RecordReplayTransport(TransportPtr inner, std::string cassette_path, bool record);

        [[nodiscard]] Response send(const Request &request) override;
        [[nodiscard]] std::string name() const override { return m_record ? "record" : "replay"; }

        static nlohmann::json requestToJson(const Request &request);
        static Request requestFromJson(const nlohmann::json &json);
        static nlohmann::json responseToJson(const Response &response);
        static Response responseFromJson(const nlohmann::json &json);

    private:
        TransportPtr m_inner;
        std::string m_path;
        bool m_record;
        mutable std::mutex m_mutex;
        std::vector<nlohmann::json> m_tape;
        size_t m_cursor{0};
    };

    /// The transport a run should use when the host did not configure one.
    [[nodiscard]] TransportPtr defaultTransport();

} // namespace sapo::http
