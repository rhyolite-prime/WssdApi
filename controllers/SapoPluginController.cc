//
// SapoPluginController.cc
//

#include "SapoPluginController.h"

#include <utility>

#include <drogon/drogon.h>

#include "domain_services/sapo/BlockingRunner.h"
#include "domain_services/sapo/PluginOutcomeMapper.h"
#include "domain_services/sapo/SapoEngineService.h"
#include "domain_services/sapo/UssdSessionOrchestrator.h"
#include "dto/BaseApiResponse.h"
#include "dto/SapoPluginDto.h"
#include "plugins/SapoEnginePlugin.h"
#include "utils/IdGeneratorUtils.h"
#include "utils/JsonBridge.h"

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr &)>;

/// Folds a mapped outcome into the API's standard envelope.
drogon::HttpResponsePtr outcomeResponse(const wssd_api::sapo_host::PluginOutcomeApiView &view,
                                        drogon::HttpStatusCode statusCode = drogon::k200OK) {
    wssd_api::dto::BaseApiResponse response;
    response.success = view.success;
    response.message = view.message;
    response.result = view.result;
    response.error = view.error;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(response.toJson());
    resp->setStatusCode(statusCode);
    return resp;
}

drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode statusCode, const std::string &code,
                                      const std::string &message) {
    wssd_api::dto::BaseApiResponse response;
    response.success = false;
    response.message = message;
    response.error["code"] = code;
    response.error["message"] = message;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(response.toJson());
    resp->setStatusCode(statusCode);
    return resp;
}

bool engineAvailable(drogon::HttpResponsePtr &out) {
    auto *plugin = drogon::app().getPlugin<SapoEnginePlugin>();
    if (plugin == nullptr) {
        out = errorResponse(drogon::k503ServiceUnavailable, "ENGINE_NOT_REGISTERED",
                            "SapoEnginePlugin is not registered (see config.json)");
        return false;
    }
    if (!plugin->engine().running()) {
        out = errorResponse(drogon::k503ServiceUnavailable, "ENGINE_NOT_RUNNING",
                            "Sapo engine is not running");
        return false;
    }
    return true;
}

/// Slug for the generated session id: URL-path safe (never contains '/').
std::string sessionSlug(const std::string &pluginId) {
    std::string slug;
    slug.reserve(pluginId.size());
    for (const char c : pluginId) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || c == '.';
        slug.push_back(ok ? c : '-');
    }
    return slug.empty() ? std::string("adhoc") : slug;
}

/// Sends the outcome at the right HTTP status for each failure mode.
drogon::HttpResponsePtr executeOutcomeResponse(
    const wssd_api::sapo_host::PluginOutcomeApiView &view,
    const sapo::runtime::ExecutionOutcome &outcome) {
    if (!view.success && outcome.error_code == "UNKNOWN_PLUGIN") {
        return outcomeResponse(view, drogon::k404NotFound);
    }
    if (!view.success && outcome.error_code == "BLUEPRINT_REJECTED") {
        return outcomeResponse(view, drogon::k422UnprocessableEntity);
    }
    if (!view.success &&
        (outcome.error_code == "NOT_CONFIGURED" || outcome.error_code == "INTERNAL_ERROR")) {
        return outcomeResponse(view, drogon::k500InternalServerError);
    }
    return outcomeResponse(view);
}

}  // namespace

void SapoPluginController::executePlugin(const HttpRequestPtr &req, Callback &&callback) {
    auto jsonBody = req->getJsonObject();
    if (!jsonBody || !jsonBody->isObject()) {
        callback(errorResponse(drogon::k400BadRequest, "INVALID_BODY", "Invalid JSON body"));
        return;
    }
    drogon::HttpResponsePtr unavailable;
    if (!engineAvailable(unavailable)) {
        callback(unavailable);
        return;
    }

    wssd_api::dto::SapoPluginExecuteRequest request;
    request.fromJson(*jsonBody);

    // A plugin is a registered blueprint id OR an inline DSL blueprint.
    // Inline blueprints may be a workflow object ({name, nodes:[...]}) or a
    // bare node array; both are normalized to the registry object form.
    std::string pluginId = request.getPlugin();
    std::string blueprintText;
    if (request.hasBlueprint()) {
        nlohmann::json document = wssd_api::utils::toNlohmann(request.getBlueprint());
        if (document.is_array()) {
            nlohmann::json wrapped = nlohmann::json::object();
            wrapped["nodes"] = std::move(document);
            document = std::move(wrapped);
        }
        if (!document.is_object()) {
            callback(errorResponse(drogon::k400BadRequest, "INVALID_BLUEPRINT",
                                   "'blueprint' must be a DSL workflow object or a node array"));
            return;
        }
        if (pluginId.empty() && document.contains("name") && document["name"].is_string()) {
            pluginId = document["name"].get<std::string>();
        }
        if (pluginId.empty()) {
            pluginId = "adhoc/" + wssd_api::utils::IdGeneratorUtils::generateGuid();
        }
        // Registry ids stay deterministic regardless of the posted name.
        document["name"] = pluginId;
        blueprintText = document.dump();
    }
    if (pluginId.empty()) {
        callback(errorResponse(
            drogon::k400BadRequest, "MISSING_PLUGIN",
            "Provide 'plugin' (a registered plugin blueprint id) or 'blueprint' (an inline DSL blueprint)"));
        return;
    }

    // Context: metadata keys first, variables overlay them (variables win),
    // and the raw metadata stays available to the blueprint as $metadata.
    nlohmann::json input = nlohmann::json::object();
    if (request.getMetadata().isObject()) {
        nlohmann::json metadata = wssd_api::utils::toNlohmann(request.getMetadata());
        for (auto it = metadata.begin(); it != metadata.end(); ++it) {
            input[it.key()] = it.value();
        }
        input["metadata"] = std::move(metadata);
    }
    if (request.getVariables().isObject()) {
        nlohmann::json variables = wssd_api::utils::toNlohmann(request.getVariables());
        for (auto it = variables.begin(); it != variables.end(); ++it) {
            input[it.key()] = it.value();
        }
    }

    const std::string sessionId =
        request.getSessionId().empty()
            ? "plugin:" + sessionSlug(pluginId) + ":" + wssd_api::utils::IdGeneratorUtils::generateGuid()
            : request.getSessionId();
    const std::string correlationId = request.getCorrelationId().empty()
                                          ? std::string("plugin:") + sessionId
                                          : request.getCorrelationId();
    const bool includeContext = request.getIncludeContext();
    const bool persist = request.getPersist();

    LOG_INFO << "[sapo-plugin] execute: plugin=" << pluginId << " session=" << sessionId
             << " blueprint=" << (blueprintText.empty() ? "registered" : "inline")
             << " persist=" << (persist ? "1" : "0");

    auto cb = std::make_shared<Callback>(std::move(callback));
    // Blocking engine call (state store + blueprint HTTP): runs on the
    // BlockingRunner pool, never on a Drogon IO thread. The drogon response
    // callback is safe to invoke from any thread.
    wssd_api::sapo_host::BlockingRunner::instance().post(
        [cb, pluginId, blueprintText, input = std::move(input), sessionId, correlationId,
         persist, includeContext]() mutable {
            auto &engine = SapoPluginController::engineInstance();
            auto outcome = engine.executePlugin(pluginId, blueprintText, std::move(input),
                                                sessionId, correlationId, persist);

            const auto view = wssd_api::sapo_host::mapPluginOutcome(outcome, pluginId, PREFIX,
                                                                    includeContext);
            if (!view.success && outcome.error_code == "UNKNOWN_PLUGIN") {
                LOG_WARN << "[sapo-plugin] unknown plugin '" << pluginId << "'";
            } else if (!view.success && outcome.error_code == "BLUEPRINT_REJECTED") {
                LOG_WARN << "[sapo-plugin] blueprint rejected for " << pluginId << ": "
                         << outcome.error;
            } else if (!view.success && (outcome.error_code == "NOT_CONFIGURED" ||
                                         outcome.error_code == "INTERNAL_ERROR")) {
                LOG_ERROR << "[sapo-plugin] engine failure for " << pluginId << ": "
                          << outcome.error;
            }
            LOG_INFO << "[sapo-plugin] executed: plugin=" << pluginId
                     << " session=" << outcome.session_id << " status=" << outcome.status
                     << " visits=" << outcome.node_visits << " elapsed=" << outcome.elapsed_ms
                     << "ms";
            if (outcome.status == "failed") {
                LOG_ERROR << "[sapo-plugin] plugin failed code=" << outcome.error_code
                          << " node=" << outcome.error_node << " error=" << outcome.error;
            }
            (*cb)(executeOutcomeResponse(view, outcome));
        });
}

void SapoPluginController::resumePluginSession(const HttpRequestPtr &req, Callback &&callback,
                                               std::string sessionId) {
    if (sessionId.empty()) {
        callback(errorResponse(drogon::k400BadRequest, "MISSING_SESSION", "session id is required"));
        return;
    }
    drogon::HttpResponsePtr unavailable;
    if (!engineAvailable(unavailable)) {
        callback(unavailable);
        return;
    }

    // Resume payload: {"input": <any json>} if present, else the whole body.
    // An empty body resumes with a null input (timer-style continuation).
    nlohmann::json input;
    if (const auto jsonBody = req->getJsonObject()) {
        wssd_api::dto::SapoPluginResumeRequest request;
        request.fromJson(*jsonBody);
        input = wssd_api::utils::toNlohmann(request.getInput());
    }
    const bool includeContext = req->getParameter("include_context") == "true";

    LOG_INFO << "[sapo-plugin] resume: session=" << sessionId;

    auto cb = std::make_shared<Callback>(std::move(callback));
    wssd_api::sapo_host::BlockingRunner::instance().post(
        [cb, sessionId, input = std::move(input), includeContext]() mutable {
            auto &engine = SapoPluginController::engineInstance();
            auto outcome = engine.resumePluginSession(sessionId, input);

            const std::string plugin =
                outcome.workflow_id.empty() ? sessionId : outcome.workflow_id;
            const auto view =
                wssd_api::sapo_host::mapPluginOutcome(outcome, plugin, PREFIX, includeContext);
            if (!view.success && outcome.error_code == "SESSION_NOT_FOUND") {
                (*cb)(outcomeResponse(view, drogon::k404NotFound));
                return;
            }
            if (!view.success && outcome.error_code == "SESSION_NOT_SUSPENDED") {
                (*cb)(outcomeResponse(view, drogon::k409Conflict));
                return;
            }
            (*cb)(outcomeResponse(view));
        });
}

void SapoPluginController::getPluginSession(const HttpRequestPtr &req, Callback &&callback,
                                            std::string sessionId) {
    (void)req;
    if (sessionId.empty()) {
        callback(errorResponse(drogon::k400BadRequest, "MISSING_SESSION", "session id is required"));
        return;
    }
    drogon::HttpResponsePtr unavailable;
    if (!engineAvailable(unavailable)) {
        callback(unavailable);
        return;
    }

    auto cb = std::make_shared<Callback>(std::move(callback));
    wssd_api::sapo_host::BlockingRunner::instance().post([cb, sessionId] {
        auto &engine = SapoPluginController::engineInstance();
        auto snapshot = engine.findSession(sessionId);
        if (!snapshot.has_value()) {
            (*cb)(errorResponse(drogon::k404NotFound, "SESSION_NOT_FOUND",
                                "No session is stored for id '" + sessionId + "'"));
            return;
        }

        wssd_api::dto::BaseApiResponse response;
        response.success = true;
        response.message = "Plugin session snapshot";
        response.result = wssd_api::utils::toJsonCpp(snapshot->toJson());
        auto resp = drogon::HttpResponse::newHttpJsonResponse(response.toJson());
        resp->setStatusCode(drogon::k200OK);
        (*cb)(resp);
    });
}

void SapoPluginController::listPlugins(const HttpRequestPtr &req, Callback &&callback) {
    (void)req;
    drogon::HttpResponsePtr unavailable;
    if (!engineAvailable(unavailable)) {
        callback(unavailable);
        return;
    }

    auto cb = std::make_shared<Callback>(std::move(callback));
    wssd_api::sapo_host::BlockingRunner::instance().post([cb] {
        auto &engine = SapoPluginController::engineInstance();
        auto description = engine.describe();

        wssd_api::dto::BaseApiResponse response;
        response.success = true;
        response.message = "Registered plugin blueprints";
        response.result = Json::Value(Json::objectValue);
        response.result["ready"] = description.value("ready", false);
        if (description.contains("blueprints") && description["blueprints"].is_array()) {
            response.result["plugins"] = wssd_api::utils::toJsonCpp(description["blueprints"]);
        } else {
            response.result["plugins"] = Json::Value(Json::arrayValue);
        }
        auto resp = drogon::HttpResponse::newHttpJsonResponse(response.toJson());
        resp->setStatusCode(drogon::k200OK);
        (*cb)(resp);
    });
}

wssd_api::sapo_host::SapoEngineService &SapoPluginController::engineInstance() {
    return wssd_api::sapo_host::SapoEngineService::instance();
}
