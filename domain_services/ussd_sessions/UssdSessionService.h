//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//

#ifndef WSSDAPI_USSDSESSIONSERVICE_H
#define WSSDAPI_USSDSESSIONSERVICE_H

#include <drogon/utils/coroutine.h>
#include "dto/BaseApiResponse.h"
#include "dto/HubtelUssdSessionRequestDto.h"
#include "dto/HubtelUssdSessioResponse.h"
#include "dto/NaloUssdSessionRequestDto.h"
#include "dto/NaloUssdSessioResponse.h"
#include "domain_services/redis/RedisCacheManager.h"
#include "runtime/VirtualMachine.hpp"
#include "runtime/Context.hpp"
#include <memory>

namespace wssd_api::domain_services {

    class UssdSessionService {

    public:

        drogon::Task<dto::NaloUssdSessionResponse> handleNaloUssdSessionInteraction(const dto::NaloUssdSessionRequestDto &dto);

        drogon::Task<dto::HubtelUssdSessionResponse> handleHubtelUssdSessionInteraction(const dto::HubtelUssdSessionRequestDto &dto);

    };

}
#endif //WSSDAPI_USSDSESSIONSERVICE_H