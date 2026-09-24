//
// SapoSettings.h — configuration for the embedded Sapo engine.
//
// Values come from the SapoEnginePlugin "config" block in config.json with
// environment overrides applied afterwards (twelve-factor style), so the
// same container image runs against file or Redis state without a rebuild.
//

#pragma once

#include <cstddef>
#include <string>

#include <json/json.h>

namespace wssd_api::sapo_host {

struct SapoSettings {
    /// Directory of *.json blueprints loaded once at startup.
    std::string workflowDirectory = "sapo-dev/workflows";
    /// Optional sapo-dev-config.json (provider config, secrets, engine tunables).
    std::string configPath = "sapo-dev/sapo-dev-config.json";
    /// File state-store directory (used when no Redis URL is configured).
    std::string stateDirectory = "sapo-dev/state-store";
    /// Redis URL for durable multi-node state ("", "host[:port]" or
    /// "redis://[[user]:pass@]host[:port][/db]"). Empty => file store.
    std::string redisUrl;
    /// TTL applied to every Redis session hash (15 min outlasts a call).
    int redisTtlSeconds = 900;
    /// Socket client pool; keep >= the engine worker count.
    std::size_t redisPoolSize = 8;
    /// Fold index writes into the CAS script (single Redis only; set false
    /// on Redis Cluster, see RedisStateStoreOptions::atomic_index).
    bool redisAtomicIndex = true;
    /// trace|debug|info|warn|error|off — forwarded to the Sapo logger.
    std::string logLevel = "info";
    /// Fallback blueprint stem: sapo-dev/workflows/<name>.json.
    std::string defaultWorkflowFile = "default";
    /// Threads for blocking engine calls; 0 => hardware-based default.
    std::size_t blockingThreads = 8;

    /// Reads the plugin "config" object; unknown keys are ignored.
    static SapoSettings fromJson(const Json::Value &config);

    /// Applies SAPO_* environment overrides on top of file configuration:
    /// SAPO_WORKFLOW_DIR, SAPO_CONFIG_PATH, SAPO_STATE_DIR, SAPO_REDIS_URL
    /// (or SAPO_REDIS_HOST + SAPO_REDIS_PORT), SAPO_REDIS_PASSWORD,
    /// SAPO_LOG_LEVEL, SAPO_DEFAULT_WORKFLOW.
    void applyEnvOverrides();
};

}  // namespace wssd_api::sapo_host
