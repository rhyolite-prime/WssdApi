#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class UssdInteractionController : public drogon::HttpController<UssdInteractionController>
{
  public:
    static constexpr const char *PREFIX = "/api/v1/ussd-interaction/";
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(UssdInteractionController::handleNaloUssdInteraction, std::string(PREFIX) + "nalo", Post);
        ADD_METHOD_TO(UssdInteractionController::handleHubtelUssdInteraction, std::string(PREFIX) + "hubtel", Post);
    METHOD_LIST_END

    Task<HttpResponsePtr> handleNaloUssdInteraction(HttpRequestPtr req);
    Task<HttpResponsePtr> handleHubtelUssdInteraction(HttpRequestPtr req);

};
