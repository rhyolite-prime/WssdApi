//
// UssdFlowBindingStore.h — session -> blueprint binding for USSD continuations.
//
// Nalo passes no usable session id, so the subscriber's MSISDN *is* the
// session key — and on continuation turns the dial string is gone (USERDATA
// carries only the menu reply), so the flow's blueprint can no longer be
// resolved from the request. The orchestrator therefore pins the resolved
// blueprint to the session id at initiation time and reads it back on every
// continuation turn (Hubtel uses the same path for uniformity; its passed
// SessionId is the key).
//
// Primary store is Redis (SET ... EX ttl / GET / DEL against the configured
// redis_url, so bindings survive restarts and work across nodes); without a
// Redis URL — or when Redis is unreachable at construction — an in-memory
// map with the same TTL backs the store (single-node degraded mode, loudly
// logged). Mid-run Redis errors fail open (miss on read, drop on write)
// with an error log; the orchestrator then falls back to resolving the
// blueprint from the request.
//
// All methods are blocking (Redis socket I/O) and thread-safe; call them
// off the Drogon IO threads (see BlockingRunner).
//

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#if defined(SAPO_ENABLE_REDIS)
#include "redis/SocketRedisClient.hpp"
#endif

namespace wssd_api::sapo_host {

/// Blueprint pinned to one USSD session at initiation time.
struct UssdFlowBinding {
    std::string workflowId;
    std::string blueprintJson;
    std::string serviceKey;
    std::string dialCode;
    /// ussd_subscriptions.id resolved at initiation ("" when the service
    /// has no subscription row). Continuations reuse it for the audit row.
    std::string businessSubscriptionId;
    int64_t updatedMs = 0;
};

class UssdFlowBindingStore {
  public:
    /// ttlSeconds <= 0 selects the 900s default (matches the engine's
    /// session-checkpoint TTL so bindings expire with their sessions).
    explicit UssdFlowBindingStore(std::string redisUrl, int ttlSeconds = 900,
                                  std::size_t poolSize = 4);
    ~UssdFlowBindingStore();

    UssdFlowBindingStore(const UssdFlowBindingStore &) = delete;
    UssdFlowBindingStore &operator=(const UssdFlowBindingStore &) = delete;

    /// True when Redis backs the store (vs the in-memory fallback).
    [[nodiscard]] bool usingRedis() const {
        return redisMode_;
    }

    /// Pins `binding` to `sessionId` (initiation / redial overwrite).
    /// Never throws: Redis errors are logged and the write is dropped.
    void save(const std::string &sessionId, const UssdFlowBinding &binding);

    /// Reads the binding for a continuation turn. Misses (unknown id,
    /// expired TTL, Redis error) return nullopt, never throw.
    [[nodiscard]] std::optional<UssdFlowBinding> find(const std::string &sessionId);

    /// Drops the binding (gateway release / explicit session end).
    /// Never throws.
    void remove(const std::string &sessionId);

  private:
    struct MemoryEntry {
        UssdFlowBinding binding;
        std::chrono::steady_clock::time_point expiresAt;
    };

    void saveMemory(const std::string &sessionId, const UssdFlowBinding &binding);
    std::optional<UssdFlowBinding> findMemory(const std::string &sessionId);
    void removeMemory(const std::string &sessionId);

    const int ttlSeconds_;
    bool redisMode_ = false;
#if defined(SAPO_ENABLE_REDIS)
    std::unique_ptr<sapo::redis::SocketRedisClient> redis_;
#endif
    std::mutex memoryMutex_;
    std::unordered_map<std::string, MemoryEntry> memory_;
};

}  // namespace wssd_api::sapo_host
