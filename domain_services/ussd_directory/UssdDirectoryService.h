//
// Created by Emmanuel Addo-Odame on 12/06/2026.
//

#ifndef WSSDAPI_USSDDIRECTORYSERVICE_H
#define WSSDAPI_USSDDIRECTORYSERVICE_H
#include <drogon/utils/coroutine.h>
#include "dto/BaseApiResponse.h"
#include "dto/UssdDirectoryDto.h"


namespace wssd_api::domain_services {

    class UssdDirectoryService {

        public:

        drogon::Task<dto::BaseApiResponse> getAll(int pageNo, int pageSize, const std::string &query, const std::string &countryCode);

        drogon::Task<dto::BaseApiResponse> search(int pageNo, int pageSize,  const std::string &query, const std::string &countryCode, const std::string &category);

        drogon::Task<dto::BaseApiResponse> create(const dto::UssdDirectoryDto &dto);

        drogon::Task<dto::BaseApiResponse> update(const dto::UssdDirectoryDto &dto, const std::string &id);

        drogon::Task<dto::BaseApiResponse> remove(const std::string &id);

    };

}
#endif //WSSDAPI_USSDDIRECTORYSERVICE_H