//
//  Sapo Engine — Redis client seam.
//
//  `RedisStateStore` never touches a socket or a third-party client directly: it
//  issues RESP commands through `IRedisClient` and reads back a `RedisValue`.
//  `sapo_core` ships a mock (so the store's tests run with no server and no
//  network) and an optional POSIX-socket RESP2 client compiled with
//  SAPO_ENABLE_REDIS. Hosts that already link hiredis or redis-plus-plus can
//  wrap them in ~30 lines instead — the same shape as `IHttpTransport`.
//
//  Deliberately synchronous: `IStateStore` is called on the engine's own worker
//  threads (docs/INTEGRATING.md §4), so an async client would only add a
//  blocking `.wait()` on top. Parallelism comes from the connection pool, not
//  from futures.
//
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sapo::redis {

    /**
     * @brief One RESP reply: everything a Redis command can return.
     *
     * RESP2 has five wire types; `Nil` is modelled explicitly rather than as an
     * empty bulk string, because "key missing" and "key holds \"\"" mean
     * different things to a state store.
     */
    class RedisValue {
    public:
        enum class Type { Nil, Status, Error, Integer, Bulk, Array };

        RedisValue() = default;
        [[nodiscard]] static RedisValue nil() { return RedisValue(); }
        [[nodiscard]] static RedisValue status(std::string text);
        [[nodiscard]] static RedisValue error(std::string text);
        [[nodiscard]] static RedisValue integer(int64_t value);
        [[nodiscard]] static RedisValue bulk(std::string text);
        [[nodiscard]] static RedisValue array(std::vector<RedisValue> items);

        [[nodiscard]] Type type() const { return m_type; }
        [[nodiscard]] bool isNil() const { return m_type == Type::Nil; }
        [[nodiscard]] bool isError() const { return m_type == Type::Error; }
        [[nodiscard]] bool isInteger() const { return m_type == Type::Integer; }
        [[nodiscard]] bool isArray() const { return m_type == Type::Array; }
        /// Bulk or simple-string reply.
        [[nodiscard]] bool isString() const { return m_type == Type::Bulk || m_type == Type::Status; }

        /// Integer reply, or `fallback` for anything else.
        [[nodiscard]] int64_t toInt(int64_t fallback = 0) const;
        /// Bulk/status payload; `nullopt` for Nil, arrays and integers.
        [[nodiscard]] std::optional<std::string> toString() const;
        /// Array elements; empty for every other type.
        [[nodiscard]] const std::vector<RedisValue> &toArray() const;

        /// True when this is the `+OK` status reply (`SET ... NX` returns Nil
        /// when the guard failed, which is *not* an error).
        [[nodiscard]] bool isOkStatus() const;

        /**
         * @brief Throws `SapoError(ErrorCode::Store)` when this is an error reply.
         *
         * Redis reports most failures out-of-band as `-ERR …` replies rather
         * than by closing the connection, so every command path funnels through
         * here. `context` names the operation for the message.
         */
        void throwIfError(const std::string &context) const;

        /// Human-readable form for logs and error messages (truncates bulk data).
        [[nodiscard]] std::string describe(size_t max_length = 120) const;

    private:
        RedisValue(Type type, std::string text, int64_t number, std::vector<RedisValue> items);

        Type m_type{Type::Nil};
        std::string m_text;
        int64_t m_number{0};
        std::vector<RedisValue> m_items;
    };

    /// Connection + pool settings for the socket client.
    struct RedisOptions {
        std::string host{"127.0.0.1"};
        int port{6379};
        std::string username;              // empty ⇒ plain `AUTH password` (pre-ACL server)
        std::string password;              // empty ⇒ no AUTH
        int database{0};                   // `SELECT n` after connect; 0 ⇒ no SELECT
        std::string client_name{"sapo-engine"};

        int connect_timeout_ms{2000};      // TCP connect deadline
        int socket_timeout_ms{2000};       // per-read/write deadline (SO_RCVTIMEO/SO_SNDTIMEO)
        size_t pool_size{8};               // connections; must be >= the engine's worker count
        size_t max_command_bytes{16u * 1024u * 1024u}; // reply size guard (runaway blob / bad server)

        /// Parses `redis://[[user]:password@]host[:port][/db]`. Returns nullopt
        /// on a malformed URL. Only the `redis://` scheme is accepted: the
        /// socket client has no TLS, so `rediss://` is rejected rather than
        /// silently downgraded to plaintext.
        [[nodiscard]] static std::optional<RedisOptions> fromUrl(const std::string &url, std::string *error = nullptr);

        [[nodiscard]] std::string describe() const; // host:port/db, never the password
    };

    class IRedisClient {
    public:
        virtual ~IRedisClient() = default;

        /// Runs one command and returns its reply. `args[0]` is the command name.
        /// Throws `SapoError(ErrorCode::Store)` on transport failure; returns an
        /// error `RedisValue` for server-side errors (call `throwIfError`).
        [[nodiscard]] virtual RedisValue command(const std::vector<std::string> &args) = 0;

        /**
         * @brief Sends every command, then reads every reply — one round trip.
         *
         * Replies come back in request order. A failing command does not abort
         * the batch: its slot holds an error `RedisValue`, matching real Redis
         * pipeline semantics.
         */
        [[nodiscard]] virtual std::vector<RedisValue> pipeline(const std::vector<std::vector<std::string>> &commands) = 0;

        /// "socket", "mock", "null" — identifies the implementation in `kind()`.
        [[nodiscard]] virtual std::string name() const = 0;

        /// Cheap liveness probe (PING). Used before handing a pooled connection
        /// to a caller that has been idle.
        [[nodiscard]] virtual bool healthy() = 0;
    };

    using RedisClientPtr = std::shared_ptr<IRedisClient>;

    /// Fails loudly rather than pretending, mirroring `sapo::http::NullTransport`.
    /// This is what `RedisStateStore` gets when the build has SAPO_ENABLE_REDIS
    /// off and the host injected nothing.
    class NullRedisClient final : public IRedisClient {
    public:
        [[nodiscard]] RedisValue command(const std::vector<std::string> &args) override;
        [[nodiscard]] std::vector<RedisValue> pipeline(const std::vector<std::vector<std::string>> &commands) override;
        [[nodiscard]] std::string name() const override { return "null"; }
        [[nodiscard]] bool healthy() override { return false; }
    };

} // namespace sapo::redis
