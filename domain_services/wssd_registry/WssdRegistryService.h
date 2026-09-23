//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//

#ifndef WSSDAPI_WSSDREGISTRYSERVCE_H
#define WSSDAPI_WSSDREGISTRYSERVCE_H
#include <drogon/utils/coroutine.h>
#include "dto/BaseApiResponse.h"
#include "dto/WssdRegistryDto.h"
#include "WssdRegistry.h"

namespace wssd_api::domain_services {

    class WssdRegistryService {

        public:

        drogon::Task<dto::BaseApiResponse> getAll(int pageNo, int pageSize, const std::string &query);

        drogon::Task<drogon_model::WssdApi::WssdRegistry> getByUssdCode(const std::string &ussdCode);

        drogon::Task<dto::BaseApiResponse> create(const dto::WssdRegistryDto &dto);

        drogon::Task<dto::BaseApiResponse> update(const dto::WssdRegistryDto &dto, const std::string &id);

        drogon::Task<dto::BaseApiResponse> remove(const std::string &id);

    };


}
#endif //WSSDAPI_WSSDREGISTRYSERVCE_H