//
//  Sapo Engine — Redis-backed session state store (implementation_plan_2.md T4.1).
//
//  Replaces the ~40-line sketch in docs/INTEGRATING.md §4 with something that
//  survives a multi-node USSD fleet. The sketch's `save()` is an unconditional
//  SET, which makes `Interpreter::resumeSession` a read-modify-write race: a
//  gateway retry, a redial, or an API node and a queue worker touching one
//  session all produce a silent last-writer-wins that can re-run a side-effecting
//  `command` node. Everything here exists to close that, and to make
//  `list()`/`count()` cheap instead of a SCAN over a million keys.
//
//  Key layout (all under configurable prefixes):
//
//    sapo:session:<id>      HASH  blob | version | status | updated_ms
//    sapo:idx:all           ZSET  member=<id> score=updated_ms
//    sapo:idx:<status>      ZSET  member=<id> score=updated_ms   (6 statuses)
//    sapo:claim:<id>        STRING token, PX-bounded             (tryClaim)
//
//  A HASH rather than a plain string so the compare-and-swap can read a
//  one-field `version` instead of `cjson.decode`-ing a ~1.5 KB blob inside Lua.
//  Expensive work in a script blocks Redis's single command thread for every
//  other client (docs/STATE-STORE-REDIS-VS-TARANTOOL.md §4.4) — the whole point
//  of the layout is to keep the atomic path O(1) and byte-cheap.
//
//  Round trips on the hot path: `load` 1, `saveIf` 1 (2 with
//  `atomic_index=false`). Both are sub-millisecond on a LAN, i.e. ~0.1 % of the
//  2-second USSD gateway window.
//
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "redis/IRedisClient.hpp"
#include "runtime/StateStore.hpp"

namespace sapo::redis {

    struct RedisStateStoreOptions {
        /// Prefix for session hashes. Ends in ':' by convention.
        std::string key_prefix{"sapo:session:"};
        /// Prefix for the status ZSETs. Keep it distinct from `key_prefix`:
        /// `list()` SCANs nothing, but an operator will `SCAN` by hand.
        std::string index_prefix{"sapo:idx:"};
        /// Prefix for the idempotency claim keys (`tryClaim`).
        std::string claim_prefix{"sapo:claim:"};

        /// TTL applied to every session hash on write. 0 ⇒ no expiry, which
        /// means abandoned sessions are never reclaimed — not recommended.
        /// 900s (15 min) comfortably outlasts a USSD conversation.
        int ttl_seconds{900};

        /**
         * @brief Fold the index ZSET updates into the same EVAL as the write.
         *
         * true  — one round trip, and the index can never disagree with the
         *         blob. Requires every key the script touches to live in one
         *         Redis instance: **Redis Cluster rejects it with CROSSSLOT**,
         *         because `sapo:session:<id>` and `sapo:idx:<status>` hash to
         *         different slots.
         * false — the EVAL touches only the session hash, so it is always
         *         cluster-safe; the index is then updated in a second pipelined
         *         batch. The index is advisory (it only serves `list()` and
         *         `count()`, never correctness), so the brief inconsistency
         *         window is acceptable.
         *
         * Set false on Redis Cluster / sharded Valkey.
         */
        bool atomic_index{true};

        /// Upper bound on `list()`. `IStateStore::list()` has no pagination, and
        /// returning a million checkpoints would exhaust the caller long before
        /// Redis complained. `list()` logs (via the returned size) when it
        /// truncates; use `pruneExpired()` + a narrower query for real tooling.
        size_t max_list{1000};

        /// `dump()` (compact) rather than `dump(2)`. Measured on a real
        /// checkpoint: 1 539 vs 1 976 bytes — pretty-printing costs ~28 % on
        /// every write and every reply. Files want pretty; a wire store does not.
        bool compact_json{true};

        [[nodiscard]] std::string describe() const;
    };

    /**
     * @brief `IStateStore` over any `IRedisClient`.
     *
     * Thread safety: all methods are safe to call concurrently — the store
     * holds no mutable session state of its own, and the client leases one
     * pooled connection per call. This matters because the engine calls the
     * store from worker threads (`WorkerPool`) and from the scheduler's tick.
     *
     * Errors: transport and server failures throw `SapoError(ErrorCode::Store)`
     * (timeouts: `ErrorCode::Timeout`). They are never swallowed into an empty
     * `optional`, so a Redis outage cannot masquerade as "no such session" and
     * silently restart a conversation from scratch.
     */
    class RedisStateStore final : public runtime::IStateStore {
    public:
        explicit RedisStateStore(RedisClientPtr client, RedisStateStoreOptions options = {});

        // --- runtime::IStateStore ------------------------------------------
        void save(const runtime::SessionCheckpoint &checkpoint) override;
        runtime::SaveOutcome saveIf(const runtime::SessionCheckpoint &checkpoint, int64_t expected_version) override;
        [[nodiscard]] bool supportsCompareAndSwap() const override { return true; }
        [[nodiscard]] std::optional<runtime::SessionCheckpoint> load(const std::string &session_id) const override;
        bool remove(const std::string &session_id) override;
        [[nodiscard]] std::vector<runtime::SessionCheckpoint> list() const override;
        [[nodiscard]] size_t count(std::optional<runtime::SessionStatus> status) const override;
        [[nodiscard]] std::string kind() const override;

        // --- Redis-specific ------------------------------------------------
        /**
         * @brief Takes the per-session mutex that makes a resume idempotent.
         *
         * `SET … NX PX` — one O(1) command. Call it before resuming a session
         * and release (or let it expire) afterwards; a duplicate USSD request
         * that loses the claim should return the previously rendered prompt
         * rather than re-executing the flow. Cheap insurance that does not
         * depend on `saveIf` catching the race after the damage is done.
         *
         * @return true when the claim was taken, false when someone holds it.
         */
        [[nodiscard]] bool tryClaim(const std::string &session_id, const std::string &token, int ttl_ms = 5000);

        /// Releases a claim, but only if `token` still owns it (a Lua
        /// compare-and-delete, so a late release cannot drop another caller's
        /// claim after the original expired).
        bool releaseClaim(const std::string &session_id, const std::string &token);

        /**
         * @brief Drops index members whose session hash has expired.
         *
         * A session TTLs out of Redis without telling the index, so `count()`
         * drifts upward until this runs. `list()` prunes lazily as it goes;
         * call this from a maintenance tick (or the scheduler) to keep
         * `count()` honest. Returns the number of stale members removed.
         */
        size_t pruneExpired();

        [[nodiscard]] const RedisStateStoreOptions &options() const { return m_options; }
        [[nodiscard]] const RedisClientPtr &client() const { return m_client; }

        // --- wire contract --------------------------------------------------
        /// The CAS+write script. Exposed so the mock can mirror it and so a live
        /// integration test can assert the server accepts it verbatim.
        [[nodiscard]] static const char *saveScript();
        /// Compare-and-delete for `releaseClaim`.
        [[nodiscard]] static const char *releaseClaimScript();

        [[nodiscard]] std::string sessionKey(const std::string &session_id) const;
        [[nodiscard]] std::string indexKey(std::optional<runtime::SessionStatus> status) const;
        [[nodiscard]] std::string claimKey(const std::string &session_id) const;

    private:
        /// Shared implementation of `save` (expected < 0) and `saveIf`.
        runtime::SaveOutcome write(const runtime::SessionCheckpoint &checkpoint, int64_t expected_version) const;
        void updateIndex(const std::string &session_id, std::optional<runtime::SessionStatus> previous,
                         runtime::SessionStatus next, int64_t updated_ms) const;
        [[nodiscard]] std::string serialize(const runtime::SessionCheckpoint &checkpoint) const;

        /// The six per-status ZSET keys, in `SessionStatus` enumerator order —
        /// the order the save script indexes `KEYS[3..8]` by.
        [[nodiscard]] std::vector<std::string> statusIndexKeys() const;
        /// `ZREM <session_id>` from `all` and every per-status index, used when
        /// the hash is already gone so the previous status is unknowable.
        [[nodiscard]] std::vector<std::vector<std::string>> removeFromEveryIndex(const std::string &session_id) const;

        RedisClientPtr m_client;
        RedisStateStoreOptions m_options;
    };

} // namespace sapo::redis
