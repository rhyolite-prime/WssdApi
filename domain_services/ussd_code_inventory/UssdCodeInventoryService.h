//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//

#ifndef WSSDAPI_USSDCODEINVENTORY_H
#define WSSDAPI_USSDCODEINVENTORY_H

#include <drogon/utils/coroutine.h>
#include "../../dto/BaseApiResponse.h"
#include <json/json.h>
#include <string>

namespace wssd_api::domain_services {

    class UssdCodeInventoryService {

    public:

        drogon::Task<dto::BaseApiResponse> getAll(int pageNo, int pageSize, const std::string &query);

        drogon::Task<dto::BaseApiResponse> create(const Json::Value &dto);

        drogon::Task<dto::BaseApiResponse> update(const Json::Value &dto, const std::string &id);

        drogon::Task<dto::BaseApiResponse> remove(const std::string &id);

    };

}
#endif //WSSDAPI_USSDCODEINVENTORY_H