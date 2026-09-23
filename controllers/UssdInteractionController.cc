#include "UssdInteractionController.h"

#include <stdexcept>

#include <drogon/drogon.h>

#include "domain_services/sapo/ProviderAdapters.h"
#include "domain_services/sapo/UssdSessionOrchestrator.h"
#include "dto/BaseApiResponse.h"
#include "dto/UssdSessionRequestDto.h"
#include "dto/UssdSessionResponse.h"
#include "plugins/SapoEnginePlugin.h"

namespace {

drogon::HttpResponsePtr badRequest(const std::string &message) {
    wssd_api::dto::BaseApiResponse response;
    response.success = false;
    response.error["message"] = message;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(response.toJson());
    resp->setStatusCode(drogon::k400BadRequest);
    return resp;
}

}  // namespace

Task<HttpResponsePtr> UssdInteractionController::handleNaloUssdInteraction(HttpRequestPtr req) {
    auto jsonBody = req->getJsonObject();
    if (!jsonBody) {
        co_return badRequest("Invalid JSON body");
    }

    try {
        wssd_api::dto::NaloUssdSessionRequestDto dto;
        dto.fromJson(*jsonBody);
        // SESSIONID is deliberately NOT required: Nalo passes no natural
        // session id, so the normalized MSISDN anchors the session (see
        // normalizeNalo). Only the service key and subscriber are mandatory.
        if (dto.getMsisdn().empty() || dto.getUserId().empty()) {
            co_return badRequest("Missing required fields: USERID, MSISDN");
        }

        auto *plugin = drogon::app().getPlugin<SapoEnginePlugin>();
        if (plugin == nullptr) {
            throw std::runtime_error("SapoEnginePlugin is not registered (see config.json)");
        }
        const auto interaction = wssd_api::sapo_host::adapters::normalizeNalo(dto, *jsonBody);
        const auto result = co_await plugin->orchestrator().handle(interaction);

        auto resp = HttpResponse::newHttpJsonResponse(wssd_api::sapo_host::adapters::renderNalo(dto, result));
        resp->setStatusCode(k200OK);
        co_return resp;
    } catch (const std::exception &e) {
        // Gateways need a well-formed 200 even on internal failure so the
        // handset session can close gracefully instead of hanging.
        LOG_ERROR << "[ussd][nalo] handler error: " << e.what();
        wssd_api::dto::NaloUssdSessionResponse fallback;
        fallback.USERID = (*jsonBody).get("USERID", "").asString();
        fallback.MSISDN = (*jsonBody).get("MSISDN", "").asString();
        fallback.SESSIONID = (*jsonBody).get("SESSIONID", "").asString();
        fallback.USERDATA = (*jsonBody).get("USERDATA", "").asString();
        fallback.MSGTYPE = false;
        fallback.MSG = "Service temporarily unavailable. Please try again later.";
        auto resp = HttpResponse::newHttpJsonResponse(fallback.toJson());
        resp->setStatusCode(k200OK);
        co_return resp;
    }
}

Task<HttpResponsePtr> UssdInteractionController::handleHubtelUssdInteraction(HttpRequestPtr req) {
    auto jsonBody = req->getJsonObject();
    if (!jsonBody) {
        co_return badRequest("Invalid JSON body");
    }

    try {
        wssd_api::dto::HubtelUssdSessionRequestDto dto;
        dto.fromJson(*jsonBody);
        if (dto.getSessionId().empty() || dto.getMobile().empty()) {
            co_return badRequest("Missing required fields: SessionId, Mobile");
        }

        auto *plugin = drogon::app().getPlugin<SapoEnginePlugin>();
        if (plugin == nullptr) {
            throw std::runtime_error("SapoEnginePlugin is not registered (see config.json)");
        }
        const auto interaction = wssd_api::sapo_host::adapters::normalizeHubtel(dto, *jsonBody);
        const auto result = co_await plugin->orchestrator().handle(interaction);

        auto resp = HttpResponse::newHttpJsonResponse(
            wssd_api::sapo_host::adapters::renderHubtel(dto, result));
        resp->setStatusCode(k200OK);
        co_return resp;
    } catch (const std::exception &e) {
        LOG_ERROR << "[ussd][hubtel] handler error: " << e.what();
        wssd_api::dto::HubtelUssdSessionResponse fallback;
        fallback.SessionId = (*jsonBody).get("SessionId", "").asString();
        fallback.Type = "Release";
        fallback.Message = "Service temporarily unavailable. Please try again later.";
        fallback.Label = "Service unavailable";
        fallback.ClientState = "End";
        fallback.DataType = "input";
        fallback.FieldType = "text";
        auto resp = HttpResponse::newHttpJsonResponse(fallback.toJson());
        resp->setStatusCode(k200OK);
        co_return resp;
    }
}
