//
//  Sapo Engine — in-memory Redis double (mirrors `sapo::http::MockTransport`).
//
//  Implements enough of the keyspace and RESP reply shapes to exercise
//  `RedisStateStore` with no server, no network and no port: hashes, strings,
//  sorted sets, TTL expiry on a caller-driven clock, `SET … NX PX`, pipelining,
//  and the two Lua scripts the store ships.
//
//  Scope, stated plainly: this is a *behavioural* double. `installSapoScripts()`
//  re-implements the store's Lua in C++, so the tests prove the store's key
//  layout, argument order, reply parsing and index maintenance — not that Redis
//  itself accepts the scripts. That second half is what
//  tests/test_redis_state_store.cpp's live-server section covers (opt-in via
//  SAPO_REDIS_URL). If you edit `RedisStateStore::saveScript()`, edit the mirror
//  here in the same commit.
//
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "redis/IRedisClient.hpp"

namespace sapo::redis {

    class MockRedisClient final : public IRedisClient {
    public:
        /// Mirrors one EVAL script. `keys`/`args` are exactly the KEYS/ARGV the
        /// script would see (0-based here, 1-based in Lua).
        using ScriptHandler =
            std::function<RedisValue(const std::vector<std::string> &keys, const std::vector<std::string> &args)>;

        MockRedisClient();
        ~MockRedisClient() override;

        MockRedisClient(const MockRedisClient &) = delete;
        MockRedisClient &operator=(const MockRedisClient &) = delete;

        [[nodiscard]] RedisValue command(const std::vector<std::string> &args) override;
        [[nodiscard]] std::vector<RedisValue> pipeline(const std::vector<std::vector<std::string>> &commands) override;
        [[nodiscard]] std::string name() const override { return "mock"; }
        [[nodiscard]] bool healthy() override;

        /// Registers the C++ mirror of `RedisStateStore`'s scripts. Called by
        /// the constructor, so a default-constructed mock is ready to use.
        void installSapoScripts();
        /// Registers (or replaces) a handler for scripts whose text contains
        /// `marker`. The marker is the first comment line of each script.
        void onScript(const std::string &marker, ScriptHandler handler);

        // --- test hooks -----------------------------------------------------
        /// Makes every command throw a transport error, to exercise the
        /// socket client's retry/propagation path without a real outage.
        void setDown(bool down);
        /// Next `count` commands answer `-ERR <message>` instead of executing.
        void failNextCommands(size_t count, std::string message);
        /// Moves the mock clock forward so TTLs expire deterministically.
        void advanceTime(int64_t milliseconds);
        void clear();

        // --- inspection -----------------------------------------------------
        [[nodiscard]] size_t keyCount() const;
        [[nodiscard]] bool hasKey(const std::string &key) const;
        [[nodiscard]] std::optional<std::string> hashField(const std::string &key, const std::string &field) const;
        [[nodiscard]] std::optional<std::string> stringKey(const std::string &key) const;
        /// Members in ascending (score, member) order, as ZRANGE would return.
        [[nodiscard]] std::vector<std::string> sortedMembers(const std::string &key) const;
        [[nodiscard]] std::optional<int64_t> ttlSeconds(const std::string &key) const;
        /// Every command seen, in order — for asserting round-trip counts.
        [[nodiscard]] std::vector<std::vector<std::string>> recordedCommands() const;
        /// `recordedCommands()` filtered to those whose verb is `verb`.
        [[nodiscard]] size_t countCalls(const std::string &verb) const;

    private:
        struct Entry {
            enum class Kind { String, Hash, ZSet };
            Kind kind{Kind::String};
            std::string string_value;
            std::vector<std::pair<std::string, std::string>> hash; // insertion-ordered, as Redis replies
            std::map<std::string, double> zset;                    // member → score
            int64_t expires_at_ms{0};                              // 0 ⇒ no expiry
        };

        /// Returns the live entry or nullptr, erasing it first if it expired.
        [[nodiscard]] Entry *findLive(const std::string &key);
        [[nodiscard]] const Entry *findLive(const std::string &key) const;
        [[nodiscard]] Entry &entryFor(const std::string &key, Entry::Kind kind);

        RedisValue dispatch(const std::vector<std::string> &args);
        RedisValue evaluate(const std::vector<std::string> &args);

        // keyspace helpers used by the mirrored scripts
        void hashSet(Entry &entry, const std::string &field, const std::string &value);
        [[nodiscard]] std::optional<std::string> hashGet(const Entry &entry, const std::string &field) const;
        void zadd(const std::string &key, double score, const std::string &member);
        void zrem(const std::string &key, const std::string &member);

        mutable std::recursive_mutex m_mutex;
        std::unordered_map<std::string, Entry> m_data;
        std::vector<std::pair<std::string, ScriptHandler>> m_scripts;
        std::vector<std::vector<std::string>> m_recorded;
        int64_t m_now_ms{0};
        bool m_down{false};
        size_t m_failures_left{0};
        std::string m_failure_message;
    };

    using MockRedisClientPtr = std::shared_ptr<MockRedisClient>;

} // namespace sapo::redis
