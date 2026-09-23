//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//
#include "WssdRegistryService.h"
#include "WssdRegistry.h"
#include <drogon/drogon.h>
#include <drogon/orm/CoroMapper.h>
#include <drogon/orm/Criteria.h>

using namespace drogon;
using namespace drogon::orm;
using namespace drogon_model::WssdApi;

namespace wssd_api::domain_services {

    drogon::Task<dto::BaseApiResponse> WssdRegistryService::getAll(int pageNo, int pageSize, const std::string &query) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<WssdRegistry> mapper(dbClient);

            // 1. Build the search criteria
            Criteria searchCriteria;
            if (!query.empty()) {
                std::string likeQuery = "%" + query + "%";
                searchCriteria =
                    Criteria(WssdRegistry::Cols::_wssd_short_name, CompareOperator::Like, likeQuery) ||
                    Criteria(WssdRegistry::Cols::_ussd_code, CompareOperator::Like, likeQuery) ||
                    Criteria(WssdRegistry::Cols::_business_name, CompareOperator::Like, likeQuery);
            }

            size_t offset = (pageNo > 0) ? (pageNo - 1) * pageSize : 0;
            
            std::vector<WssdRegistry> items;
            if (!query.empty()) {
                items = co_await mapper.offset(offset).limit(pageSize).findBy(searchCriteria);
            } else {
                items = co_await mapper.offset(offset).limit(pageSize).findAll();
            }
            
            Json::Value jsonArray(Json::arrayValue);
            for (const auto &item : items) {
                Json::Value modelJson = item.toJson();
                Json::Value jsonItem;
                jsonItem["id"] = modelJson["id"];
                jsonItem["wssdShortName"] = modelJson["wssd_short_name"];
                jsonItem["ussdCode"] = modelJson["ussd_code"];
                jsonItem["displayTitle"] = modelJson["display_title"];
                jsonItem["description"] = modelJson["description"];
                jsonItem["activatedOn"] = modelJson["activated_on"];
                jsonItem["logoUrl"] = modelJson["logo_url"];
                jsonItem["isWssdActive"] = modelJson["is_wssd_active"];
                jsonItem["isUssdActive"] = modelJson["is_ussd_active"];
                jsonItem["useAsSecondaryService"] = modelJson["use_as_secondary_service"];
                jsonItem["tenantId"] = modelJson["tenant_id"];
                jsonItem["rank"] = modelJson["rank"];
                jsonItem["merchantIdentifier"] = modelJson["merchant_identifier"];
                jsonItem["category"] = modelJson["category"];
                jsonItem["businessName"] = modelJson["business_name"];
                jsonItem["createdAt"] = modelJson["created_at"];
                jsonItem["updatedAt"] = modelJson["updated_at"];
                jsonArray.append(jsonItem);
            }
            
            response.success = true;
            response.message = "Fetched successfully";
            response.result = jsonArray;
        } catch (const DrogonDbException &e) {
            response.success = false;
            response.message = std::string("Database error: ") + e.base().what();
        } catch (const std::exception &e) {
            response.success = false;
            response.message = std::string("Error: ") + e.what();
        }
        co_return response;
    }

    drogon::Task<drogon_model::WssdApi::WssdRegistry> WssdRegistryService::getByUssdCode(const std::string &ussdCode) {
        auto dbClient = drogon::app().getDbClient();
        CoroMapper<WssdRegistry> mapper(dbClient);

        auto items = co_await mapper.findBy(Criteria(WssdRegistry::Cols::_ussd_code, CompareOperator::EQ, ussdCode));
        if (items.empty()) {
            throw drogon::orm::UnexpectedRows("Record not found");
        }

        co_return items.front();
    }

    drogon::Task<dto::BaseApiResponse> WssdRegistryService::create(const dto::WssdRegistryDto &dto) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<drogon_model::WssdApi::WssdRegistry> mapper(dbClient);

            drogon_model::WssdApi::WssdRegistry model;
            
            if(!dto.getWssdShortName().empty()) model.setWssdShortName(dto.getWssdShortName());
            if(!dto.getUssdCode().empty()) model.setUssdCode(dto.getUssdCode());
            if(!dto.getDisplayTitle().empty()) model.setDisplayTitle(dto.getDisplayTitle());
            if(!dto.getDescription().empty()) model.setDescription(dto.getDescription());
            if(!dto.getLogoUrl().empty()) model.setLogoUrl(dto.getLogoUrl());
            model.setIsWssdActive(dto.getIsWssdActive());
            model.setIsUssdActive(dto.getIsUssdActive());
            model.setUseAsSecondaryService(dto.getUseAsSecondaryService());
            model.setTenantId(dto.getTenantId());
            model.setRank(dto.getRank());
            if(!dto.getMerchantIdentifier().empty()) model.setMerchantIdentifier(dto.getMerchantIdentifier());
            if(!dto.getCategory().empty()) model.setCategory(dto.getCategory());
            if(!dto.getBusinessName().empty()) model.setBusinessName(dto.getBusinessName());

            auto result = co_await mapper.insert(model);

            response.success = true;
            response.message = "Created successfully";
            response.result = result.toJson();
        } catch (const DrogonDbException &e) {
            response.success = false;
            response.message = std::string("Database error: ") + e.base().what();
        } catch (const std::exception &e) {
            response.success = false;
            response.message = std::string("Error: ") + e.what();
        }
        co_return response;
    }

    drogon::Task<dto::BaseApiResponse> WssdRegistryService::update(const dto::WssdRegistryDto &dto, const std::string &id) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<drogon_model::WssdApi::WssdRegistry> mapper(dbClient);

            auto existingModel = co_await mapper.findByPrimaryKey(id);
            
            if(!dto.getWssdShortName().empty()) existingModel.setWssdShortName(dto.getWssdShortName());
            if(!dto.getUssdCode().empty()) existingModel.setUssdCode(dto.getUssdCode());
            if(!dto.getDisplayTitle().empty()) existingModel.setDisplayTitle(dto.getDisplayTitle());
            if(!dto.getDescription().empty()) existingModel.setDescription(dto.getDescription());
            if(!dto.getLogoUrl().empty()) existingModel.setLogoUrl(dto.getLogoUrl());
            existingModel.setIsWssdActive(dto.getIsWssdActive());
            existingModel.setIsUssdActive(dto.getIsUssdActive());
            existingModel.setUseAsSecondaryService(dto.getUseAsSecondaryService());
            existingModel.setTenantId(dto.getTenantId());
            existingModel.setRank(dto.getRank());
            if(!dto.getMerchantIdentifier().empty()) existingModel.setMerchantIdentifier(dto.getMerchantIdentifier());
            if(!dto.getCategory().empty()) existingModel.setCategory(dto.getCategory());
            if(!dto.getBusinessName().empty()) existingModel.setBusinessName(dto.getBusinessName());

            auto result = co_await mapper.update(existingModel);

            response.success = true;
            response.message = "Updated successfully";
            response.result = Json::Value(); // Optional: send updated object back
        } catch (const UnexpectedRows &) {
             response.success = false;
             response.message = "Record not found";
        } catch (const DrogonDbException &e) {
            response.success = false;
            response.message = std::string("Database error: ") + e.base().what();
        } catch (const std::exception &e) {
            response.success = false;
            response.message = std::string("Error: ") + e.what();
        }
        co_return response;
    }

    drogon::Task<dto::BaseApiResponse> WssdRegistryService::remove(const std::string &id) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<drogon_model::WssdApi::WssdRegistry> mapper(dbClient);

            co_await mapper.deleteByPrimaryKey(id);

            response.success = true;
            response.message = "Deleted successfully";
        } catch (const DrogonDbException &e) {
            response.success = false;
            response.message = std::string("Database error: ") + e.base().what();
        } catch (const std::exception &e) {
            response.success = false;
            response.message = std::string("Error: ") + e.what();
        }
        co_return response;
    }

}