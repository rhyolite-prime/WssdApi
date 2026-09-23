//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//

#ifndef WSSDAPI_USSDPLUGINSERVICE_H
#define WSSDAPI_USSDPLUGINSERVICE_H

#include <drogon/utils/coroutine.h>
#include "dto/BaseApiResponse.h"
#include "dto/PluginDto.h"

namespace wssd_api::domain_services {

    class UssdPluginService {

    public:

        drogon::Task<dto::BaseApiResponse> getAll(int pageNo, int pageSize, const std::string &query);

        drogon::Task<dto::BaseApiResponse> create(const dto::PluginDto &dto);

        drogon::Task<dto::BaseApiResponse> update(const dto::PluginDto &dto, const std::string &id); // can only update user defined plugins; built-in plugins or system plugins cant be updated

        drogon::Task<dto::BaseApiResponse> install(const std::string &businessId, const std::string &id);

        drogon::Task<dto::BaseApiResponse> remove(const std::string &id); // can only update user defined plugins; built-in plugins or system plugins cant be deleted


    };

}
#endif //WSSDAPI_USSDPLUGINSERVICE_H