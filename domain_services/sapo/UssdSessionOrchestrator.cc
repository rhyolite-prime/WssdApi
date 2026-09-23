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
    : settings_(std::move(settings)), resolver_(settings_) {}

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

    auto resolved = co_await resolver_.resolve(interaction.serviceKey, interaction.dialCode);
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
        co_await auditSession(interaction, result);
        co_return std::move(result);
    }

    const std::string sapoSessionId = sapoSessionIdFor(interaction);

    if (interaction.isRelease) {
        co_await BlockingRunner::instance().run([&engine, sapoSessionId] {
            engine.cancelSession(sapoSessionId, "gateway release");
        });
        UssdResult result;
        result.cont = false;
        result.message = "Session ended.";
        result.clientState = "End";
        result.label = "Session ended.";
        result.sapoSessionId = sapoSessionId;
        result.workflowId = workflowId;
        result.status = "cancelled";
        co_await auditSession(interaction, result);
        co_return std::move(result);
    }

    nlohmann::json baseInput = nlohmann::json::object();
    baseInput["user_input"] = interaction.userInput;
    baseInput["msisdn"] = interaction.msisdn;
    baseInput["phone"] = interaction.msisdn;
    baseInput["session_id"] = interaction.networkSessionId;
    baseInput["provider"] = toString(interaction.provider);
    baseInput["network"] = interaction.network;
    baseInput["service_key"] = interaction.serviceKey;
    baseInput["ussd_code"] =
        !interaction.dialCode.empty() ? interaction.dialCode : interaction.serviceKey;
    baseInput["dial_code"] = interaction.dialCode;
    baseInput["sequence"] = interaction.sequence;
    baseInput["client_state"] = interaction.clientState;
    baseInput["input"] = interaction.userInput;  // refined for fresh sessions by the engine call
    const std::string correlation =
        toString(interaction.provider) + ":" + interaction.networkSessionId;

    // Blocking engine call (state store + blueprint HTTP): runs on the
    // BlockingRunner pool, never on a Drogon IO thread.
    auto outcome = co_await BlockingRunner::instance().run(
        [&engine, workflowId, sapoSessionId, baseInput,
         rawInput = interaction.userInput, dialCode = interaction.dialCode,
         correlation]() mutable {
            return engine.executeUssdTurn(workflowId, sapoSessionId, std::move(baseInput), rawInput,
                                          dialCode, correlation);
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

    co_await auditSession(interaction, result);
    co_return std::move(result);
}

drogon::Task<void> UssdSessionOrchestrator::auditSession(const UssdInteraction &interaction,
                                                         const UssdResult &result) {
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
            drogon_model::WssdApi::UssdSessions row;
            row.setId(utils::IdGeneratorUtils::generateGuid());
            row.setNetworkSessionId(interaction.networkSessionId);
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
