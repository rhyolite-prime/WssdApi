//
// Created by Emmanuel Addo-Odame on 04/07/2026.
//
#include "UssdDirectoryService.h"
#include "UssdDirectory.h"
#include <drogon/drogon.h>
#include <drogon/orm/CoroMapper.h>
#include <drogon/orm/Criteria.h>

using namespace drogon;
using namespace drogon::orm;
using namespace drogon_model::WssdApi;

namespace wssd_api::domain_services {

    drogon::Task<dto::BaseApiResponse> UssdDirectoryService::getAll(int pageNo, int pageSize, const std::string &query, const std::string &countryCode) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdDirectory> mapper(dbClient);

            Criteria searchCriteria;
            bool hasCriteria = false;

            if (!query.empty()) {
                std::string likeQuery = "%" + query + "%";
                searchCriteria = Criteria(UssdDirectory::Cols::_trade_name, CompareOperator::Like, likeQuery) ||
                                 Criteria(UssdDirectory::Cols::_ussd_code, CompareOperator::Like, likeQuery) ||
                                 Criteria(UssdDirectory::Cols::_business_name, CompareOperator::Like, likeQuery);
                hasCriteria = true;
            }

            if (!countryCode.empty()) {
                Criteria ccCriteria = Criteria(UssdDirectory::Cols::_country_code, CompareOperator::EQ, countryCode);
                if (hasCriteria) {
                    searchCriteria = searchCriteria && ccCriteria;
                } else {
                    searchCriteria = ccCriteria;
                    hasCriteria = true;
                }
            }

            size_t offset = (pageNo > 0) ? (pageNo - 1) * pageSize : 0;
            
            std::vector<UssdDirectory> items;
            if (hasCriteria) {
                items = co_await mapper.offset(offset).limit(pageSize).findBy(searchCriteria);
            } else {
                items = co_await mapper.offset(offset).limit(pageSize).findAll();
            }
            
            Json::Value jsonArray(Json::arrayValue);
            for (const auto &item : items) {
                Json::Value modelJson = item.toJson();
                Json::Value jsonItem;
                jsonItem["id"] = modelJson["id"];
                jsonItem["tradeName"] = modelJson["trade_name"];
                jsonItem["ussdCode"] = modelJson["ussd_code"];
                jsonItem["businessName"] = modelJson["business_name"];
                jsonItem["description"] = modelJson["description"];
                jsonItem["countryCode"] = modelJson["country_code"];
                jsonItem["category"] = modelJson["category"];
                jsonItem["keywords"] = modelJson["keywords"];
                jsonItem["isDedicated"] = modelJson["is_dedicated"];
                jsonItem["isWssdLinked"] = modelJson["is_wssd_linked"];
                jsonItem["rating"] = modelJson["rating"];
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

    drogon::Task<dto::BaseApiResponse> UssdDirectoryService::search(int pageNo, int pageSize,  const std::string &query, const std::string &countryCode, const std::string &category) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdDirectory> mapper(dbClient);

            Criteria searchCriteria;
            bool hasCriteria = false;

            if (!query.empty()) {
                std::string likeQuery = "%" + query + "%";
                searchCriteria = Criteria(UssdDirectory::Cols::_trade_name, CompareOperator::Like, likeQuery) ||
                                 Criteria(UssdDirectory::Cols::_ussd_code, CompareOperator::Like, likeQuery) ||
                                 Criteria(UssdDirectory::Cols::_business_name, CompareOperator::Like, likeQuery) ||
                                 Criteria(UssdDirectory::Cols::_keywords, CompareOperator::Like, likeQuery);
                hasCriteria = true;
            }

            if (!countryCode.empty()) {
                Criteria ccCriteria = Criteria(UssdDirectory::Cols::_country_code, CompareOperator::EQ, countryCode);
                if (hasCriteria) {
                    searchCriteria = searchCriteria && ccCriteria;
                } else {
                    searchCriteria = ccCriteria;
                    hasCriteria = true;
                }
            }

            if (!category.empty()) {
                Criteria catCriteria = Criteria(UssdDirectory::Cols::_category, CompareOperator::EQ, category);
                if (hasCriteria) {
                    searchCriteria = searchCriteria && catCriteria;
                } else {
                    searchCriteria = catCriteria;
                    hasCriteria = true;
                }
            }

            size_t offset = (pageNo > 0) ? (pageNo - 1) * pageSize : 0;
            
            std::vector<UssdDirectory> items;
            if (hasCriteria) {
                items = co_await mapper.offset(offset).limit(pageSize).findBy(searchCriteria);
            } else {
                items = co_await mapper.offset(offset).limit(pageSize).findAll();
            }
            
            Json::Value jsonArray(Json::arrayValue);
            for (const auto &item : items) {
                Json::Value modelJson = item.toJson();
                Json::Value jsonItem;
                jsonItem["id"] = modelJson["id"];
                jsonItem["tradeName"] = modelJson["trade_name"];
                jsonItem["ussdCode"] = modelJson["ussd_code"];
                jsonItem["businessName"] = modelJson["business_name"];
                jsonItem["description"] = modelJson["description"];
                jsonItem["countryCode"] = modelJson["country_code"];
                jsonItem["category"] = modelJson["category"];
                jsonItem["keywords"] = modelJson["keywords"];
                jsonItem["isDedicated"] = modelJson["is_dedicated"];
                jsonItem["isWssdLinked"] = modelJson["is_wssd_linked"];
                jsonItem["rating"] = modelJson["rating"];
                jsonItem["createdAt"] = modelJson["created_at"];
                jsonItem["updatedAt"] = modelJson["updated_at"];
                jsonArray.append(jsonItem);
            }
            
            response.success = true;
            response.message = "Searched successfully";
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

    drogon::Task<dto::BaseApiResponse> UssdDirectoryService::create(const dto::UssdDirectoryDto &dto) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdDirectory> mapper(dbClient);

            UssdDirectory model;
            
            if (!dto.getTradeName().empty()) model.setTradeName(dto.getTradeName());
            if (!dto.getUssdCode().empty()) model.setUssdCode(dto.getUssdCode());
            if (!dto.getBusinessName().empty()) model.setBusinessName(dto.getBusinessName());
            if (!dto.getDescription().empty()) model.setDescription(dto.getDescription());
            if (!dto.getCountryCode().empty()) model.setCountryCode(dto.getCountryCode());
            if (!dto.getCategory().empty()) model.setCategory(dto.getCategory());
            if (!dto.getKeywords().empty()) model.setKeywords(dto.getKeywords());
            
            model.setIsDedicated(dto.getIsDedicated());
            model.setIsWssdLinked(dto.getIsWssdLinked());
            model.setRating(dto.getRating());

            auto result = co_await mapper.insert(model);

            response.success = true;
            response.message = "Created successfully";
            
            Json::Value modelJson = result.toJson();
            Json::Value jsonItem;
            jsonItem["id"] = modelJson["id"];
            jsonItem["tradeName"] = modelJson["trade_name"];
            jsonItem["ussdCode"] = modelJson["ussd_code"];
            jsonItem["businessName"] = modelJson["business_name"];
            jsonItem["description"] = modelJson["description"];
            jsonItem["countryCode"] = modelJson["country_code"];
            jsonItem["category"] = modelJson["category"];
            jsonItem["keywords"] = modelJson["keywords"];
            jsonItem["isDedicated"] = modelJson["is_dedicated"];
            jsonItem["isWssdLinked"] = modelJson["is_wssd_linked"];
            jsonItem["rating"] = modelJson["rating"];
            jsonItem["createdAt"] = modelJson["created_at"];
            jsonItem["updatedAt"] = modelJson["updated_at"];
            
            response.result = jsonItem;
        } catch (const DrogonDbException &e) {
            response.success = false;
            response.message = std::string("Database error: ") + e.base().what();
        } catch (const std::exception &e) {
            response.success = false;
            response.message = std::string("Error: ") + e.what();
        }
        co_return response;
    }

    drogon::Task<dto::BaseApiResponse> UssdDirectoryService::update(const dto::UssdDirectoryDto &dto, const std::string &id) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdDirectory> mapper(dbClient);

            auto existingModel = co_await mapper.findByPrimaryKey(id);
            
            if (!dto.getTradeName().empty()) existingModel.setTradeName(dto.getTradeName());
            if (!dto.getUssdCode().empty()) existingModel.setUssdCode(dto.getUssdCode());
            if (!dto.getBusinessName().empty()) existingModel.setBusinessName(dto.getBusinessName());
            if (!dto.getDescription().empty()) existingModel.setDescription(dto.getDescription());
            if (!dto.getCountryCode().empty()) existingModel.setCountryCode(dto.getCountryCode());
            if (!dto.getCategory().empty()) existingModel.setCategory(dto.getCategory());
            if (!dto.getKeywords().empty()) existingModel.setKeywords(dto.getKeywords());
            
            existingModel.setIsDedicated(dto.getIsDedicated());
            existingModel.setIsWssdLinked(dto.getIsWssdLinked());
            existingModel.setRating(dto.getRating());

            auto result = co_await mapper.update(existingModel);

            response.success = true;
            response.message = "Updated successfully";
            response.result = Json::Value(); 
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

    drogon::Task<dto::BaseApiResponse> UssdDirectoryService::remove(const std::string &id) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdDirectory> mapper(dbClient);

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