#include "UssdInteractionController.h"

#include "dto/UssdSessionRequestDto.h"
#include "dto/UssdSessionResponse.h"
#include "plugins/WssdServicePlugin.h"


Task<HttpResponsePtr> UssdInteractionController::handleNaloUssdInteraction(HttpRequestPtr req) {

    wssd_api::dto::NaloUssdSessionResponse naloUssdSessionResponse;

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

    auto resp = HttpResponse::newHttpJsonResponse(naloUssdSessionResponse.toJson());
    resp->setStatusCode(k200OK);
    co_return resp;

}


Task<HttpResponsePtr> UssdInteractionController::handleHubtelUssdInteraction(HttpRequestPtr req) {

    wssd_api::dto::HubtelUssdSessionResponse hubtelUssdSessionResponse;

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


    auto resp = HttpResponse::newHttpJsonResponse(hubtelUssdSessionResponse.toJson());
    resp->setStatusCode(k200OK);
    co_return resp;


}
