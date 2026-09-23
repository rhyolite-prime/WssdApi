//
// SapoEngineService.cc
//

#include "SapoEngineService.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <utility>

#include <drogon/drogon.h>

#include "config/ProviderConfig.hpp"
#include "redis/RedisStateStore.hpp"
#if defined(SAPO_ENABLE_REDIS)
#include "redis/SocketRedisClient.hpp"
#endif
#include "runtime/SapoError.hpp"

#include "DrogonLogSink.h"
#include "domain_services/adapters/DrogonHttpTransport.h"

namespace wssd_api::sapo_host {
namespace {

sapo::obs::LogLevel parseLogLevel(const std::string &level) {
    if (level == "trace") {
        return sapo::obs::LogLevel::Trace;
    }
    if (level == "debug") {
        return sapo::obs::LogLevel::Debug;
    }
    if (level == "warn" || level == "warning") {
        return sapo::obs::LogLevel::Warn;
    }
    if (level == "error") {
        return sapo::obs::LogLevel::Error;
    }
    if (level == "off") {
        return sapo::obs::LogLevel::Off;
    }
    return sapo::obs::LogLevel::Info;
}

#if defined(SAPO_ENABLE_REDIS)
sapo::redis::RedisOptions parseRedisOptions(const std::string &url) {
    if (url.rfind("redis://", 0) == 0) {
        std::string error;
        auto options = sapo::redis::RedisOptions::fromUrl(url, &error);
        if (!options.has_value()) {
            throw std::runtime_error("invalid Sapo redis URL: " + error);
        }
        return *options;
    }
    // Plain "host[:port]" shorthand (no auth, db 0).
    sapo::redis::RedisOptions options;
    const auto colon = url.find(':');
    if (colon == std::string::npos) {
        options.host = url;
    } else {
        options.host = url.substr(0, colon);
        options.port = std::stoi(url.substr(colon + 1));
    }
    if (options.host.empty()) {
        throw std::runtime_error("empty Sapo redis host");
    }
    return options;
}
#endif

std::shared_ptr<sapo::runtime::IStateStore> buildStateStore(
    const SapoSettings &settings, const std::string &redisUrl) {
    if (!redisUrl.empty()) {
#if defined(SAPO_ENABLE_REDIS)
        try {
            sapo::redis::RedisOptions options = parseRedisOptions(redisUrl);
            options.pool_size = settings.redisPoolSize == 0 ? 8 : settings.redisPoolSize;
            options.client_name = "wssdapi-sapo";
            auto client = std::make_shared<sapo::redis::SocketRedisClient>(std::move(options));
            // Dial check: a lazy client would only fail per-turn (every lookup
            // throwing looks like "no session" downstream). Fail fast here so
            // an unreachable Redis falls back to the file store with one loud
            // error instead of breaking every subscriber turn. describe()
            // never includes the password.
            if (!client->healthy()) {
                throw std::runtime_error("cannot reach " + client->options().describe());
            }
            sapo::redis::RedisStateStoreOptions storeOptions;
            storeOptions.ttl_seconds = settings.redisTtlSeconds;
            storeOptions.atomic_index = settings.redisAtomicIndex;
            LOG_INFO << "[sapo] state store: redis (" << client->options().describe() << ")";
            return std::make_shared<sapo::redis::RedisStateStore>(client, storeOptions);
        } catch (const std::exception &e) {
            LOG_ERROR << "[sapo] redis state store unavailable (" << e.what()
                      << "); falling back to the file store";
        }
#else
        LOG_ERROR << "[sapo] redis configured but the engine was built without "
                     "SAPO_ENABLE_REDIS; falling back to the file store";
#endif
    }

    std::error_code ec;
    std::filesystem::create_directories(settings.stateDirectory, ec);
    if (ec) {
        LOG_ERROR << "[sapo] cannot create state directory '" << settings.stateDirectory
                  << "' (" << ec.message() << "); using in-memory state (NOT durable)";
        return std::make_shared<sapo::runtime::InMemoryStateStore>();
    }
    if (redisUrl.empty()) {
        LOG_INFO << "[sapo] state store: file (" << settings.stateDirectory
                 << ") — redis_url is not set (set SAPO_REDIS_URL or config redis_url for Redis)";
    } else {
        // A redis_url WAS configured but unusable; the reason was already
        // logged as an error above, this line just confirms the fallback.
        LOG_INFO << "[sapo] state store: file (" << settings.stateDirectory << ")";
    }
    return std::make_shared<sapo::runtime::FileStateStore>(settings.stateDirectory);
}

void applyEngineLimits(sapo::runtime::TaskServices &services) {
    if (!services.provider_config) {
        return;
    }
    const nlohmann::json engine = services.provider_config->engine();
    if (!engine.is_object()) {
        return;
    }
    // nlohmann parses non-negative literals as number_unsigned and negatives
    // as number_integer, so exact-type checks would silently miss values.
    // Accept any JSON number and range-check it explicitly instead.
    auto sizeLimit = [&engine](const char *key, size_t &out) {
        const auto it = engine.find(key);
        if (it == engine.end() || !it->is_number()) {
            return;
        }
        const double value = it->get<double>();
        if (value >= 0 && value <= 9007199254740991.0) {
            out = static_cast<size_t>(value);
        }
    };
    auto millisLimit = [&engine](const char *key, int64_t &out) {
        const auto it = engine.find(key);
        if (it == engine.end() || !it->is_number()) {
            return;
        }
        out = static_cast<int64_t>(it->get<double>());
    };
    sizeLimit("max_node_visits", services.limits.max_node_visits);
    sizeLimit("max_depth", services.limits.max_depth);
    sizeLimit("max_branch_visits", services.limits.max_branch_visits);
    if (engine.contains("default_timeout_ms") && engine["default_timeout_ms"].is_number()) {
        int64_t timeoutMs = 0;
        millisLimit("default_timeout_ms", timeoutMs);
        services.limits.default_timeout_ms = timeoutMs;
    }
    millisLimit("max_retry_delay_ms", services.limits.max_retry_delay_ms);
    millisLimit("inline_wait_limit_ms", services.limits.inline_wait_limit_ms);
}

}  // namespace

SapoEngineService::~SapoEngineService() {
    stop();
}

bool SapoEngineService::configure(const SapoSettings &settings) {
    std::lock_guard<std::mutex> lock(registryMutex_);
    if (vm_) {
        LOG_WARN << "[sapo] engine already configured; ignoring reconfigure";
        return true;
    }
    settings_ = settings;
    settings_.applyEnvOverrides();

    // One-line effective configuration (values, never secrets: the Redis
    // URL can carry a password, so only set/unset + source are logged).
    // Relative paths resolve from the process working directory, which is
    // why the CWD is part of this line.
    {
        const char *envUrl = std::getenv("SAPO_REDIS_URL");
        const char *envHost = std::getenv("SAPO_REDIS_HOST");
        const bool fromEnv = (envUrl != nullptr && *envUrl != '\0') ||
                             (envHost != nullptr && *envHost != '\0');
        std::error_code cwdError;
        const auto cwd = std::filesystem::current_path(cwdError);
        LOG_INFO << "[sapo] settings: cwd='"
                 << (cwdError ? std::string("<unknown>") : cwd.string()) << "' config_path='"
                 << settings_.configPath << "' workflow_dir='" << settings_.workflowDirectory
                 << "' state_dir='" << settings_.stateDirectory << "' redis="
                 << (settings_.redisUrl.empty() ? "unset" : "set")
                 << " (source: " << (fromEnv ? "env" : (!settings_.redisUrl.empty() ? "config.json" : "none"))
                 << ") default_workflow='" << settings_.defaultWorkflowFile << "' log_level='"
                 << settings_.logLevel << "'";
    }

    try {
        auto services = sapo::runtime::TaskServices::defaults();

        auto logger = std::make_shared<sapo::obs::Logger>();
        logger->setLevel(parseLogLevel(settings_.logLevel));
        logger->addSink(std::make_shared<DrogonLogSink>());
        services.logger = logger;

        // Outbound HTTP from blueprints reuses Drogon's client/IO loops.
        services.transport = std::make_shared<HostDrogonTransport>();

        // Provider config first: it carries bindings and engine limits
        // (never the Redis URL — see the state_redis NOTE below).
        std::shared_ptr<sapo::config::ProviderConfigStore> providerConfig;
        if (!settings_.configPath.empty() && std::filesystem::exists(settings_.configPath)) {
            try {
                auto store = sapo::config::ProviderConfigStore::load(settings_.configPath);
                for (const auto &problem : store.validate()) {
                    LOG_WARN << "[sapo] provider config: " << problem;
                }
                services.bindings = store.bindingProvider();
                providerConfig = std::make_shared<sapo::config::ProviderConfigStore>(std::move(store));
                services.provider_config = providerConfig;
                services.applySecretRedaction();
                LOG_INFO << "[sapo] provider config: " << settings_.configPath;
            } catch (const std::exception &e) {
                LOG_WARN << "[sapo] ignoring provider config '" << settings_.configPath
                         << "': " << e.what();
            }
        } else if (!settings_.configPath.empty()) {
            LOG_INFO << "[sapo] no provider config at '" << settings_.configPath
                     << "' (optional); continuing without it";
        }

        // NOTE: engine.state_redis from the config file is intentionally NOT
        // consulted here. VirtualMachine::start() takes over state-store
        // selection whenever that key exists (and fails startup when it is
        // unresolvable), so honoring it in two places would split-brain Redis
        // configuration. The plugin redis_url / SAPO_REDIS_URL env is the
        // single source of truth; sapo-config.json must not set state_redis.
        services.state_store = buildStateStore(settings_, settings_.redisUrl);

        applyEngineLimits(services);

        vm_ = std::make_unique<sapo::runtime::VirtualMachine>(std::move(services));

        if (!settings_.workflowDirectory.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(settings_.workflowDirectory, ec);
            if (ec) {
                LOG_WARN << "[sapo] cannot create workflow directory '" << settings_.workflowDirectory
                         << "': " << ec.message();
            }
            vm_->setWorkflowDirectory(settings_.workflowDirectory);
        }
        if (!settings_.configPath.empty() && std::filesystem::exists(settings_.configPath)) {
            vm_->setConfigPath(settings_.configPath);
        }

        for (const auto &problem : vm_->services().startupCheck()) {
            LOG_WARN << "[sapo] startup check: " << problem;
        }
    } catch (const std::exception &e) {
        LOG_ERROR << "[sapo] configure failed: " << e.what();
        vm_.reset();
        return false;
    }

    configured_ = true;
    return true;
}

std::vector<std::string> SapoEngineService::start() {
    if (!configured_ || !vm_) {
        return {"sapo engine is not configured"};
    }
    std::vector<std::string> problems;
    try {
        // vm_->start() mixes blueprint-validator WARNINGs (e.g. control-flow
        // cycle reports) into the same vector as fatal errors, and marks the
        // VM started either way. Warnings are logged and tolerated so they do
        // not abort application startup; only fatal entries are returned.
        for (const auto &problem : vm_->start()) {
            if (isWarningProblem(problem)) {
                LOG_WARN << "[sapo] engine warning at startup: " << problem;
            } else {
                LOG_ERROR << "[sapo] start problem: " << problem;
                problems.push_back(problem);
            }
        }
        if (!problems.empty()) {
            started_.store(false);
            return problems;
        }
        // Input-driven USSD turns need no ticking, but `wait` nodes and prompt
        // timeouts only fire while the scheduler runs.
        vm_->startBackgroundTick(std::chrono::milliseconds(500));
    } catch (const std::exception &e) {
        started_.store(false);
        return {std::string("sapo engine start threw: ") + e.what()};
    }
    started_.store(true);
    std::string workflowIds;
    for (const auto &id : vm_->workflows().ids()) {
        if (!workflowIds.empty()) {
            workflowIds += ",";
        }
        workflowIds += id;
    }
    LOG_INFO << "[sapo] engine started (workflows=" << vm_->workflows().size() << " [" << workflowIds
             << "], store=" << vm_->services().state_store->kind() << ")";
    return {};
}

void SapoEngineService::stop() {
    try {
        if (vm_) {
            vm_->stop();
        }
    } catch (const std::exception &e) {
        LOG_ERROR << "[sapo] stop failed: " << e.what();
    }
    started_.store(false);
}

bool SapoEngineService::hasWorkflow(const std::string &workflowId) const {
    std::lock_guard<std::mutex> lock(registryMutex_);
    return vm_ != nullptr && vm_->workflows().find(workflowId) != nullptr;
}

std::string SapoEngineService::ensureBlueprint(const std::string &workflowId,
                                               const std::string &blueprintJson,
                                               std::string &error) {
    std::lock_guard<std::mutex> lock(registryMutex_);
    error.clear();
    if (!vm_) {
        error = "sapo engine is not configured";
        return "";
    }
    if (blueprintJson.empty()) {
        if (vm_->workflows().find(workflowId) == nullptr) {
            error = "unknown workflow '" + workflowId + "' (not registered, no blueprint supplied)";
            return "";
        }
        return workflowId;
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(blueprintJson);
    } catch (const std::exception &e) {
        error = std::string("invalid blueprint JSON for '") + workflowId + "': " + e.what();
        return "";
    }
    if (!document.is_object()) {
        error = "blueprint for '" + workflowId + "' must be a JSON object";
        return "";
    }
    // Force the registry id so USSD code -> workflow mapping stays
    // deterministic regardless of the stored blueprint's own `name`.
    document["name"] = workflowId;
    const std::string text = document.dump();
    const std::size_t hash = std::hash<std::string>{}(text);

    const auto cached = blueprintHashes_.find(workflowId);
    if (cached != blueprintHashes_.end() && cached->second == hash &&
        vm_->workflows().find(workflowId) != nullptr) {
        return workflowId;
    }

    try {
        if (vm_->workflows().find(workflowId) != nullptr) {
            vm_->workflows().remove(workflowId);
        }
        vm_->addBlueprintText(text, "wssd:" + workflowId);
    } catch (const sapo::runtime::SapoError &e) {
        error = std::string("blueprint '") + workflowId +
                "' rejected (" + sapo::runtime::toString(e.code()) + "): " + e.what();
        return "";
    } catch (const std::exception &e) {
        error = std::string("blueprint '") + workflowId + "' failed to load: " + e.what();
        return "";
    }
    blueprintHashes_[workflowId] = hash;
    LOG_INFO << "[sapo] blueprint registered: " << workflowId;
    return workflowId;
}

sapo::runtime::ExecutionOutcome SapoEngineService::startUssdSession(
    const std::string &workflowId,
    const nlohmann::json &input,
    const std::string &sapoSessionId,
    const std::string &correlationId) {
    if (!vm_) {
        return failedOutcome(workflowId, sapoSessionId, "NOT_CONFIGURED",
                             "sapo engine is not configured", "");
    }
    sapo::runtime::StartSessionOptions options;
    options.session_id = sapoSessionId;
    options.correlation_id = correlationId;
    options.persist = true;
    try {
        return vm_->startSession(workflowId, input, options);
    } catch (const sapo::runtime::SapoError &e) {
        return failedOutcome(workflowId, sapoSessionId, sapo::runtime::toString(e.code()), e.what(),
                             e.nodeId());
    } catch (const std::exception &e) {
        return failedOutcome(workflowId, sapoSessionId, "INTERNAL_ERROR", e.what(), "");
    }
}

sapo::runtime::ExecutionOutcome SapoEngineService::resumeUssdSession(const std::string &sapoSessionId,
                                                                     const nlohmann::json &input) {
    if (!vm_) {
        return failedOutcome("", sapoSessionId, "NOT_CONFIGURED", "sapo engine is not configured", "");
    }
    try {
        return vm_->resumeSession(sapoSessionId, input);
    } catch (const sapo::runtime::SapoError &e) {
        return failedOutcome("", sapoSessionId, sapo::runtime::toString(e.code()), e.what(),
                             e.nodeId());
    } catch (const std::exception &e) {
        return failedOutcome("", sapoSessionId, "INTERNAL_ERROR", e.what(), "");
    }
}

sapo::runtime::ExecutionOutcome SapoEngineService::executeUssdTurn(const std::string &workflowId,
                                                                   const std::string &sapoSessionId,
                                                                   nlohmann::json baseInput,
                                                                   const std::string &rawInput,
                                                                   const std::string &dialCode,
                                                                   const std::string &correlationId,
                                                                   bool forceStart) {
    if (!vm_) {
        return failedOutcome(workflowId, sapoSessionId, "NOT_CONFIGURED",
                             "sapo engine is not configured", "");
    }

    // Initiation always starts fresh (a redial overwrites the parked
    // checkpoint under the same id); continuations resume when possible.
    const auto snapshot = forceStart ? std::optional<sapo::runtime::SessionSnapshot>{}
                                     : findSession(sapoSessionId);
    const bool fresh = forceStart || !snapshot.has_value() || !snapshot->resumable;

    nlohmann::json input = std::move(baseInput);
    if (fresh) {
        // On the first hit the gateway sends the dial string (e.g. "*713#");
        // it identifies the service, it is not a menu choice.
        if (!dialCode.empty() && rawInput == dialCode) {
            input["input"] = "";
        } else {
            input["input"] = rawInput;
        }
        input["is_start"] = true;
        return startUssdSession(workflowId, input, sapoSessionId, correlationId);
    }

    input["input"] = rawInput;
    input["is_start"] = false;
    // NOTE: resume takes the subscriber's raw reply as a scalar, NOT the
    // context object. The engine stores the resume argument verbatim into
    // the prompt's input_variable (and $input), so an object here would
    // poison every choice route with a non-string $choice. `input` below
    // only serves the restart-as-fresh path, where full context is correct.
    auto outcome = resumeUssdSession(sapoSessionId, rawInput);
    if (outcome.status == "failed") {
        // The checkpoint may have expired between the lookup and the resume.
        // Restart once under the same id instead of failing the subscriber.
        LOG_WARN << "[sapo] resume failed for " << sapoSessionId << " (" << outcome.error_code
                 << "); restarting session";
        input["is_start"] = true;
        input["restarted_after_expiry"] = true;
        outcome = startUssdSession(workflowId, input, sapoSessionId, correlationId);
    }
    return outcome;
}

std::optional<sapo::runtime::SessionSnapshot> SapoEngineService::findSession(
    const std::string &sapoSessionId) const {
    if (!vm_) {
        return std::nullopt;
    }
    try {
        return vm_->session(sapoSessionId);
    } catch (const std::exception &e) {
        LOG_ERROR << "[sapo] session lookup failed for " << sapoSessionId << ": " << e.what();
        return std::nullopt;
    }
}

bool SapoEngineService::cancelSession(const std::string &sapoSessionId, const std::string &reason) {
    if (!vm_) {
        return false;
    }
    try {
        return vm_->cancelSession(sapoSessionId, reason);
    } catch (const std::exception &e) {
        LOG_ERROR << "[sapo] cancel failed for " << sapoSessionId << ": " << e.what();
        return false;
    }
}

nlohmann::json SapoEngineService::metrics() const {
    if (!vm_) {
        return nlohmann::json::object();
    }
    try {
        return vm_->metrics();
    } catch (const std::exception &e) {
        LOG_ERROR << "[sapo] metrics failed: " << e.what();
        return nlohmann::json::object();
    }
}

sapo::runtime::ExecutionOutcome SapoEngineService::failedOutcome(const std::string &workflowId,
                                                                 const std::string &sapoSessionId,
                                                                 const std::string &code,
                                                                 const std::string &message,
                                                                 const std::string &node) {
    sapo::runtime::ExecutionOutcome outcome;
    outcome.ok = false;
    outcome.status = "failed";
    outcome.workflow_id = workflowId;
    outcome.session_id = sapoSessionId;
    outcome.error_code = code;
    outcome.error = message;
    outcome.error_node = node;
    return outcome;
}

}  // namespace wssd_api::sapo_host
