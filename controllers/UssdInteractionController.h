#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

using namespace drogon;

/// USSD aggregator webhooks. Each provider keeps its own wire model (see
/// dto/UssdSessionRequestDto.h / dto/UssdSessionResponse.h); the handlers
/// below stay thin: validate -> normalize -> one Sapo turn -> render.
/// All engine work happens in UssdSessionOrchestrator (off the IO threads).
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
