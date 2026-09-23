//
//  Sapo Engine — POSIX-socket RESP2 client (built with SAPO_ENABLE_REDIS=ON).
//
//  A deliberately small Redis client: the engine needs GET/HSET/EVAL/SCAN-class
//  commands over a blocking socket with a connection pool, and nothing else.
//  Writing it here keeps `sapo_core` dependency-free and offline-buildable
//  (docs/BUILDING.md) the same way the vendored cpr wrapper does for HTTP.
//
//  Not implemented, by design: RESP3 (HELLO), TLS (`rediss://`), Pub/Sub,
//  cluster MOVED/ASK redirection, and transactions other than as raw commands.
//  Hosts needing any of those should inject an `IRedisClient` wrapping hiredis
//  or redis-plus-plus — that is exactly what the seam is for.
//
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "redis/IRedisClient.hpp"

namespace sapo::redis {

    /**
     * @brief Pooled, synchronous RESP2 client.
     *
     * Thread safety: `command()` and `pipeline()` may be called concurrently
     * from any number of threads. Each call leases one connection for its
     * duration, so `pool_size` bounds real concurrency — size it to at least
     * the engine's worker count (docs/INTEGRATING.md §2.4), otherwise the pool
     * becomes the serialization point and Redis's threading model is moot.
     */
    class SocketRedisClient final : public IRedisClient {
    public:
        explicit SocketRedisClient(RedisOptions options);
        ~SocketRedisClient() override;

        SocketRedisClient(const SocketRedisClient &) = delete;
        SocketRedisClient &operator=(const SocketRedisClient &) = delete;

        [[nodiscard]] RedisValue command(const std::vector<std::string> &args) override;
        [[nodiscard]] std::vector<RedisValue> pipeline(const std::vector<std::vector<std::string>> &commands) override;
        [[nodiscard]] std::string name() const override { return "socket:" + m_options.describe(); }
        [[nodiscard]] bool healthy() override;

        [[nodiscard]] const RedisOptions &options() const { return m_options; }

        /// Encodes one RESP2 command. Exposed for tests: the wire format is the
        /// part most worth pinning down without a live server.
        [[nodiscard]] static std::string encodeCommand(const std::vector<std::string> &args);

    private:
        class Connection;
        /// RAII lease: returns the connection to the pool (or discards it when
        /// the caller marked it broken) even when an exception unwinds.
        class Lease;

        [[nodiscard]] Lease acquire();
        void release(std::unique_ptr<Connection> connection, bool broken);

        /// Runs `body` on a leased connection, retrying once on a fresh
        /// connection when the first attempt fails at transport level (a
        /// server-side `-ERR …` reply is *not* a transport failure: it is
        /// returned to the caller untouched).
        ///
        /// The retry is only sound because every command `RedisStateStore`
        /// issues is idempotent or version-guarded; a command whose reply was
        /// lost may still have been applied, so a blind retry of (say) INCR
        /// would double-count. Re-read that before adding commands here.
        RedisValue withRetry(const std::function<RedisValue(Connection &)> &body);

        RedisOptions m_options;

        std::mutex m_mutex;
        std::condition_variable m_available;
        std::deque<std::unique_ptr<Connection>> m_idle;
        size_t m_live{0}; // idle + leased; never exceeds pool_size
        bool m_shutdown{false};
    };

} // namespace sapo::redis
