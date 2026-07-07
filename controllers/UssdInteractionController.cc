#include "UssdInteractionController.h"
#include "plugins/WssdServicePlugin.h"


Task<HttpResponsePtr> UssdInteractionController::handleNaloUssdInteraction(HttpRequestPtr req) {

    auto jsonBody = req->getJsonObject();
    if (!jsonBody) {
        wssd_api::dto::BaseApiResponse response;
        response.success = false;
        response.error["message"] = "Invalid JSON body";
        auto resp = HttpResponse::newHttpJsonResponse(response.toJson());
        resp->setStatusCode(k400BadRequest);
        co_return resp;
    }

    wssd_api::dto::NaloUssdSessionRequestDto dto;
    dto.fromJson(*jsonBody);

    auto plugin = app().getPlugin<WssdServicePlugin>();
    auto &ussdSessionService = plugin->getUssdSessionService();

    auto result = co_await ussdSessionService.handleNaloUssdSessionInteraction(dto);

    auto resp = HttpResponse::newHttpJsonResponse(result.toJson());
    resp->setStatusCode(k200OK);
    co_return resp;

}


Task<HttpResponsePtr> UssdInteractionController::handleHubtelUssdInteraction(HttpRequestPtr req) {

    auto jsonBody = req->getJsonObject();
    if (!jsonBody) {
        wssd_api::dto::BaseApiResponse response;
        response.success = false;
        response.error["message"] = "Invalid JSON body";
        auto resp = HttpResponse::newHttpJsonResponse(response.toJson());
        resp->setStatusCode(k400BadRequest);
        co_return resp;
    }

    wssd_api::dto::HubtelUssdSessionRequestDto dto;
    dto.fromJson(*jsonBody);

    auto plugin = app().getPlugin<WssdServicePlugin>();
    auto &ussdSessionService = plugin->getUssdSessionService();

    auto result = co_await ussdSessionService.handleHubtelUssdSessionInteraction(dto);
    auto resp = HttpResponse::newHttpJsonResponse(result.toJson());
    resp->setStatusCode(k200OK);
    co_return resp;


}
