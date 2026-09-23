//
// UssdSessionOrchestrator.cc
//

#include "UssdSessionOrchestrator.h"

#include <utility>

#include <drogon/drogon.h>
#include <drogon/orm/CoroMapper.h>
#include <drogon/orm/Criteria.h>
#include <nlohmann/json.hpp>

#include "UssdSessions.h"
#include "utils/IdGeneratorUtils.h"

#include "BlockingRunner.h"
#include "ProviderAdapters.h"
#include "SapoEngineService.h"

namespace wssd_api::sapo_host {

UssdSessionOrchestrator::UssdSessionOrchestrator(SapoSettings settings)
    : settings_(std::move(settings)),
      resolver_(settings_),
      bindings_(settings_.redisUrl, settings_.redisTtlSeconds, settings_.redisPoolSize) {
    LOG_INFO << "[ussd] flow bindings via " << (bindings_.usingRedis() ? "redis" : "in-memory store");
}

drogon::Task<UssdResult> UssdSessionOrchestrator::handle(const UssdInteraction &interaction) {
    auto &engine = SapoEngineService::instance();
    LOG_DEBUG << "[ussd] " << toString(interaction.provider)
              << " session=" << interaction.networkSessionId
              << " msisdn=" << adapters::maskMsisdn(interaction.msisdn)
              << " seq=" << interaction.sequence << " start=" << interaction.isStart
              << " release=" << interaction.isRelease;

    if (!engine.running()) {
        LOG_ERROR << "[ussd] engine is not running; refusing turn";
        UssdResult result = unavailable("Service temporarily unavailable. Please try again later.");
        co_await auditSession(interaction, result);
        co_return std::move(result);
    }

    const std::string sapoSessionId = sapoSessionIdFor(interaction);

    if (interaction.isRelease) {
        // Read-then-drop: the binding carries the subscription id the audit
        // row needs, so capture it before removing the binding.
        const std::string subscriptionId = co_await BlockingRunner::instance().run(
            [this, &engine, sapoSessionId] {
                engine.cancelSession(sapoSessionId, "gateway release");
                const auto bound = bindings_.find(sapoSessionId);
                bindings_.remove(sapoSessionId);
                return bound.has_value() ? bound->businessSubscriptionId : std::string{};
            });
        UssdResult result;
        result.cont = false;
        result.message = "Session ended.";
        result.clientState = "End";
        result.label = "Session ended.";
        result.sapoSessionId = sapoSessionId;
        result.status = "cancelled";
        co_await auditSession(interaction, result, subscriptionId);
        co_return std::move(result);
    }

    // Initiation turns resolve the flow from the dialed service and pin it
    // to the session; continuations read the pinned binding back (the dial
    // string is gone by then — USERDATA carries only the menu reply). A
    // missed binding (expired, cold store) falls back to resolving from
    // the request, exactly as before.
    std::optional<ResolvedWorkflow> resolved;
    bool fromBinding = false;
    // Continuations recover the initiation dial string from the pinned
    // binding, so $dial_code/$ussd_code stay stable across turns.
    std::string dialCode = interaction.dialCode;
    if (!interaction.isStart) {
        auto bound = co_await BlockingRunner::instance().run([this, sapoSessionId] {
            return bindings_.find(sapoSessionId);
        });
        if (bound.has_value()) {
            fromBinding = true;
            LOG_DEBUG << "[ussd] session " << sapoSessionId << " continues bound workflow "
                      << bound->workflowId;
            ResolvedWorkflow pinned;
            pinned.workflowId = bound->workflowId;
            pinned.blueprintJson = bound->blueprintJson;
            pinned.businessSubscriptionId = bound->businessSubscriptionId;
            pinned.matchedKey = "session-binding";
            resolved = std::move(pinned);
            if (!bound->dialCode.empty()) {
                dialCode = bound->dialCode;
            }
        }
    }
    if (!resolved.has_value()) {
        resolved = co_await resolver_.resolve(interaction.serviceKey, interaction.dialCode);
    }
    if (!resolved.has_value()) {
        UssdResult result = unavailable("Service not available for this code. Please try again later.");
        co_await auditSession(interaction, result);
        co_return std::move(result);
    }

    std::string blueprintError;
    const std::string workflowId =
        engine.ensureBlueprint(resolved->workflowId, resolved->blueprintJson, blueprintError);
    if (workflowId.empty()) {
        LOG_ERROR << "[ussd] blueprint failed: " << blueprintError;
        UssdResult result = unavailable("Service temporarily unavailable. Please try again later.");
        co_await auditSession(interaction, result, resolved->businessSubscriptionId);
        co_return std::move(result);
    }

    // Pin on initiation, and re-pin whenever a turn resolved fresh (missed
    // binding, initiation signal absent): the pin memoizes what was just
    // resolved, so later turns reuse it instead of re-querying.
    if (interaction.isStart || !fromBinding) {
        UssdFlowBinding binding;
        binding.workflowId = workflowId;
        binding.blueprintJson = resolved->blueprintJson;
        binding.serviceKey = interaction.serviceKey;
        binding.dialCode = interaction.dialCode;
        binding.businessSubscriptionId = resolved->businessSubscriptionId;
        co_await BlockingRunner::instance().run([this, sapoSessionId, binding] {
            bindings_.save(sapoSessionId, binding);
        });
    }

    nlohmann::json baseInput = nlohmann::json::object();
    baseInput["user_input"] = interaction.userInput;
    baseInput["msisdn"] = interaction.msisdn;
    baseInput["phone"] = interaction.msisdn;
    baseInput["session_id"] = interaction.networkSessionId;
    baseInput["provider"] = toString(interaction.provider);
    baseInput["network"] = interaction.network;
    baseInput["service_key"] = interaction.serviceKey;
    baseInput["ussd_code"] = !dialCode.empty() ? dialCode : interaction.serviceKey;
    baseInput["dial_code"] = dialCode;
    baseInput["sequence"] = interaction.sequence;
    baseInput["client_state"] = interaction.clientState;
    baseInput["input"] = interaction.userInput;  // refined for fresh sessions by the engine call
    const std::string correlation =
        toString(interaction.provider) + ":" + interaction.networkSessionId;

    // Blocking engine call (state store + blueprint HTTP): runs on the
    // BlockingRunner pool, never on a Drogon IO thread.
    auto outcome = co_await BlockingRunner::instance().run(
        [&engine, workflowId, sapoSessionId, baseInput, dialCode,
         rawInput = interaction.userInput, forceStart = interaction.isStart,
         correlation]() mutable {
            return engine.executeUssdTurn(workflowId, sapoSessionId, std::move(baseInput), rawInput,
                                          dialCode, correlation, forceStart);
        });

    UssdResult result = adapters::renderOutcome(outcome);
    result.workflowId = workflowId;
    LOG_INFO << "[ussd] " << toString(interaction.provider)
             << " session=" << interaction.networkSessionId << " workflow=" << workflowId
             << " status=" << result.status << " cont=" << (result.cont ? "1" : "0")
             << " visits=" << result.nodeVisits << " elapsed=" << result.elapsedMs << "ms";
    if (outcome.status == "failed") {
        LOG_ERROR << "[ussd] turn failed code=" << outcome.error_code << " node=" << outcome.error_node
                  << " error=" << outcome.error;
    }

    co_await auditSession(interaction, result, resolved->businessSubscriptionId);
    co_return std::move(result);
}

drogon::Task<void> UssdSessionOrchestrator::auditSession(const UssdInteraction &interaction,
                                                         const UssdResult &result,
                                                         const std::string &businessSubscriptionId) {
    try {
        auto db = drogon::app().getDbClient();
        drogon::orm::CoroMapper<drogon_model::WssdApi::UssdSessions> mapper(db);
        auto rows = co_await mapper.limit(1).findBy(drogon::orm::Criteria(
            drogon_model::WssdApi::UssdSessions::Cols::_network_session_id,
            drogon::orm::CompareOperator::EQ, interaction.networkSessionId));

        const auto now = trantor::Date::now();
        Json::Value details(Json::objectValue);
        details["workflow"] = result.workflowId;
        details["sapoSession"] = result.sapoSessionId;
        details["status"] = result.status;
        details["nodeVisits"] = static_cast<Json::UInt64>(result.nodeVisits);
        details["elapsedMs"] = static_cast<Json::Int64>(result.elapsedMs);
        const std::string detailsText = Json::writeString(Json::StreamWriterBuilder(), details);
        const std::string status =
            result.cont ? "active" : (result.status == "failed" ? "failed" : "closed");

        if (rows.empty()) {
            if (businessSubscriptionId.empty()) {
                // business_subscription_id is NOT NULL with no default: a
                // session we cannot attribute to a subscription cannot be
                // audited. The subscriber's turn already succeeded; only the
                // audit row is lost.
                LOG_WARN << "[ussd] skipping audit insert for session "
                         << interaction.networkSessionId << ": no business subscription resolved";
                co_return;
            }
            drogon_model::WssdApi::UssdSessions row;
            row.setId(utils::IdGeneratorUtils::generateGuid());
            row.setNetworkSessionId(interaction.networkSessionId);
            row.setBusinessSubscriptionId(businessSubscriptionId);
            row.setMsisdn(interaction.msisdn);
            row.setNetworkProvider(toString(interaction.provider));
            row.setStartTime(now);
            if (!result.cont) {
                row.setEndTime(now);
            }
            row.setDetails(detailsText);
            row.setStatus(status);
            co_await mapper.insert(row);
        } else {
            auto row = rows.front();
            if (!businessSubscriptionId.empty()) {
                // Same session key, possibly redialled service: re-attribute.
                row.setBusinessSubscriptionId(businessSubscriptionId);
            }
            row.setDetails(detailsText);
            row.setStatus(status);
            if (!result.cont) {
                row.setEndTime(now);
            }
            co_await mapper.update(row);
        }
    } catch (const std::exception &e) {
        // Audit must never fail the subscriber's turn.
        LOG_ERROR << "[ussd] audit write failed: " << e.what();
    }
    co_return;
}

std::string UssdSessionOrchestrator::sapoSessionIdFor(const UssdInteraction &interaction) {
    std::string id = toString(interaction.provider) + ":";
    for (const char c : interaction.networkSessionId) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '@';
        id.push_back(ok ? c : '_');
    }
    return id;
}

UssdResult UssdSessionOrchestrator::unavailable(const std::string &message) {
    UssdResult result;
    result.cont = false;
    result.message = message;
    result.clientState = "End";
    result.label = "Service unavailable";
    result.status = "unavailable";
    return result;
}

}  // namespace wssd_api::sapo_host
