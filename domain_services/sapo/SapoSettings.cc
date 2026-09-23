//
// SapoSettings.cc
//

#include "SapoSettings.h"

#include <cstdlib>

namespace wssd_api::sapo_host {
namespace {

std::string getenvOr(const char *name) {
    const char *value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

}  // namespace

SapoSettings SapoSettings::fromJson(const Json::Value &config) {
    SapoSettings settings;
    if (config.isMember("workflow_directory") && config["workflow_directory"].isString()) {
        settings.workflowDirectory = config["workflow_directory"].asString();
    }
    if (config.isMember("config_path") && config["config_path"].isString()) {
        settings.configPath = config["config_path"].asString();
    }
    if (config.isMember("state_directory") && config["state_directory"].isString()) {
        settings.stateDirectory = config["state_directory"].asString();
    }
    if (config.isMember("redis_url") && config["redis_url"].isString()) {
        settings.redisUrl = config["redis_url"].asString();
    }
    if (config.isMember("redis_ttl_seconds") && config["redis_ttl_seconds"].isInt()) {
        settings.redisTtlSeconds = config["redis_ttl_seconds"].asInt();
    }
    if (config.isMember("redis_pool_size") && config["redis_pool_size"].isUInt()) {
        settings.redisPoolSize = config["redis_pool_size"].asUInt();
    }
    if (config.isMember("redis_atomic_index") && config["redis_atomic_index"].isBool()) {
        settings.redisAtomicIndex = config["redis_atomic_index"].asBool();
    }
    if (config.isMember("log_level") && config["log_level"].isString()) {
        settings.logLevel = config["log_level"].asString();
    }
    if (config.isMember("default_workflow") && config["default_workflow"].isString()) {
        settings.defaultWorkflowFile = config["default_workflow"].asString();
    }
    if (config.isMember("blocking_threads") && config["blocking_threads"].isUInt()) {
        settings.blockingThreads = config["blocking_threads"].asUInt();
    }
    return settings;
}

void SapoSettings::applyEnvOverrides() {
    const std::string workflowDir = getenvOr("SAPO_WORKFLOW_DIR");
    if (!workflowDir.empty()) {
        workflowDirectory = workflowDir;
    }
    const std::string configPath = getenvOr("SAPO_CONFIG_PATH");
    if (!configPath.empty()) {
        this->configPath = configPath;
    }
    const std::string stateDir = getenvOr("SAPO_STATE_DIR");
    if (!stateDir.empty()) {
        stateDirectory = stateDir;
    }
    const std::string redisUrl = getenvOr("SAPO_REDIS_URL");
    if (!redisUrl.empty()) {
        this->redisUrl = redisUrl;
    } else {
        const std::string redisHost = getenvOr("SAPO_REDIS_HOST");
        if (!redisHost.empty()) {
            const std::string redisPort = getenvOr("SAPO_REDIS_PORT");
            this->redisUrl = redisPort.empty() ? redisHost : redisHost + ":" + redisPort;
        }
    }
    const std::string logLevel = getenvOr("SAPO_LOG_LEVEL");
    if (!logLevel.empty()) {
        this->logLevel = logLevel;
    }
    const std::string defaultWorkflow = getenvOr("SAPO_DEFAULT_WORKFLOW");
    if (!defaultWorkflow.empty()) {
        defaultWorkflowFile = defaultWorkflow;
    }
}

}  // namespace wssd_api::sapo_host
