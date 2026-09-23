//
// UssdFlowBindingStore.cc
//

#include "UssdFlowBindingStore.h"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <trantor/utils/Logger.h>

namespace wssd_api::sapo_host {
namespace {

constexpr int kDefaultTtlSeconds = 900;
constexpr const char *kKeyPrefix = "wssd:ussd:binding:";

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string bindingKey(const std::string &sessionId) {
    return std::string(kKeyPrefix) + sessionId;
}

std::string serialize(const UssdFlowBinding &binding) {
    return nlohmann::json{{"workflow_id", binding.workflowId},
                          {"blueprint_json", binding.blueprintJson},
                          {"service_key", binding.serviceKey},
                          {"dial_code", binding.dialCode},
                          {"business_subscription_id", binding.businessSubscriptionId},
                          {"updated_ms", binding.updatedMs}}
        .dump();
}

std::optional<UssdFlowBinding> deserialize(const std::string &text) {
    try {
        const auto json = nlohmann::json::parse(text);
        if (!json.is_object()) {
            return std::nullopt;
        }
        UssdFlowBinding binding;
        binding.workflowId = json.value("workflow_id", "");
        binding.blueprintJson = json.value("blueprint_json", "");
        binding.serviceKey = json.value("service_key", "");
        binding.dialCode = json.value("dial_code", "");
        binding.businessSubscriptionId = json.value("business_subscription_id", "");
        binding.updatedMs = json.value("updated_ms", int64_t{0});
        if (binding.workflowId.empty()) {
            return std::nullopt;
        }
        return binding;
    } catch (const std::exception &e) {
        LOG_ERROR << "[ussd] corrupt flow binding in store: " << e.what();
        return std::nullopt;
    }
}

#if defined(SAPO_ENABLE_REDIS)
/// Mirrors SapoEngineService's URL handling: full redis:// URL or plain
/// "host[:port]" shorthand. Throws std::runtime_error when unusable.
sapo::redis::RedisOptions parseBindingRedisOptions(const std::string &url, std::size_t poolSize) {
    sapo::redis::RedisOptions options;
    if (url.rfind("redis://", 0) == 0) {
        std::string error;
        auto parsed = sapo::redis::RedisOptions::fromUrl(url, &error);
        if (!parsed.has_value()) {
            throw std::runtime_error("invalid binding redis URL: " + error);
        }
        options = *parsed;
    } else {
        const auto colon = url.find(':');
        if (colon == std::string::npos) {
            options.host = url;
        } else {
            options.host = url.substr(0, colon);
            options.port = std::stoi(url.substr(colon + 1));
        }
        if (options.host.empty()) {
            throw std::runtime_error("empty binding redis host");
        }
    }
    options.pool_size = poolSize == 0 ? 4 : poolSize;
    options.client_name = "wssdapi-ussd-binding";
    return options;
}
#endif

}  // namespace

UssdFlowBindingStore::UssdFlowBindingStore(std::string redisUrl, int ttlSeconds, std::size_t poolSize)
    : ttlSeconds_(ttlSeconds > 0 ? ttlSeconds : kDefaultTtlSeconds) {
#if defined(SAPO_ENABLE_REDIS)
    if (!redisUrl.empty()) {
        try {
            auto client = std::make_unique<sapo::redis::SocketRedisClient>(
                parseBindingRedisOptions(redisUrl, poolSize));
            if (!client->healthy()) {
                throw std::runtime_error("cannot reach " + client->options().describe());
            }
            LOG_INFO << "[ussd] flow bindings: redis (" << client->options().describe() << ")";
            redis_ = std::move(client);
            redisMode_ = true;
            return;
        } catch (const std::exception &e) {
            LOG_ERROR << "[ussd] flow bindings: redis unavailable (" << e.what()
                      << "); using in-memory bindings (single-node only)";
        }
    }
#else
    (void) redisUrl;
    (void) poolSize;
#endif
    redisMode_ = false;
    LOG_INFO << "[ussd] flow bindings: in-memory (set redis_url for multi-node)";
}

UssdFlowBindingStore::~UssdFlowBindingStore() = default;

void UssdFlowBindingStore::save(const std::string &sessionId, const UssdFlowBinding &binding) {
    if (sessionId.empty()) {
        return;
    }
    UssdFlowBinding stamped = binding;
    stamped.updatedMs = nowMs();
#if defined(SAPO_ENABLE_REDIS)
    if (redisMode_ && redis_) {
        try {
            (void) redis_->command(
                {"SET", bindingKey(sessionId), serialize(stamped), "EX", std::to_string(ttlSeconds_)});
            return;
        } catch (const std::exception &e) {
            LOG_ERROR << "[ussd] flow binding save failed: " << e.what();
            return;
        }
    }
#endif
    saveMemory(sessionId, stamped);
}

std::optional<UssdFlowBinding> UssdFlowBindingStore::find(const std::string &sessionId) {
    if (sessionId.empty()) {
        return std::nullopt;
    }
#if defined(SAPO_ENABLE_REDIS)
    if (redisMode_ && redis_) {
        try {
            const auto value = redis_->command({"GET", bindingKey(sessionId)});
            if (value.isNil()) {
                return std::nullopt;
            }
            const auto text = value.toString();
            if (!text.has_value()) {
                return std::nullopt;
            }
            return deserialize(*text);
        } catch (const std::exception &e) {
            LOG_ERROR << "[ussd] flow binding lookup failed: " << e.what();
            return std::nullopt;
        }
    }
#endif
    return findMemory(sessionId);
}

void UssdFlowBindingStore::remove(const std::string &sessionId) {
    if (sessionId.empty()) {
        return;
    }
#if defined(SAPO_ENABLE_REDIS)
    if (redisMode_ && redis_) {
        try {
            (void) redis_->command({"DEL", bindingKey(sessionId)});
            return;
        } catch (const std::exception &e) {
            LOG_ERROR << "[ussd] flow binding delete failed: " << e.what();
            return;
        }
    }
#endif
    removeMemory(sessionId);
}

void UssdFlowBindingStore::saveMemory(const std::string &sessionId, const UssdFlowBinding &binding) {
    std::lock_guard<std::mutex> lock(memoryMutex_);
    const auto now = std::chrono::steady_clock::now();
    // Opportunistic sweep: the map stays tiny in single-node/dev use.
    for (auto it = memory_.begin(); it != memory_.end();) {
        it = (it->second.expiresAt <= now) ? memory_.erase(it) : std::next(it);
    }
    memory_[sessionId] = MemoryEntry{binding, now + std::chrono::seconds(ttlSeconds_)};
}

std::optional<UssdFlowBinding> UssdFlowBindingStore::findMemory(const std::string &sessionId) {
    std::lock_guard<std::mutex> lock(memoryMutex_);
    const auto it = memory_.find(sessionId);
    if (it == memory_.end()) {
        return std::nullopt;
    }
    if (it->second.expiresAt <= std::chrono::steady_clock::now()) {
        memory_.erase(it);
        return std::nullopt;
    }
    return it->second.binding;
}

void UssdFlowBindingStore::removeMemory(const std::string &sessionId) {
    std::lock_guard<std::mutex> lock(memoryMutex_);
    memory_.erase(sessionId);
}

}  // namespace wssd_api::sapo_host
