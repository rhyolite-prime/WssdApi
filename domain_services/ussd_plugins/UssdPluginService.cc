//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//
#include "UssdPluginService.h"
#include "UssdPlugins.h"
#include <drogon/drogon.h>
#include <drogon/orm/CoroMapper.h>
#include <drogon/orm/Criteria.h>

using namespace drogon;
using namespace drogon::orm;
using namespace drogon_model::WssdApi;

namespace wssd_api::domain_services {

    drogon::Task<dto::BaseApiResponse> UssdPluginService::getAll(int pageNo, int pageSize, const std::string &query) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdPlugins> mapper(dbClient);

            Criteria searchCriteria;
            bool hasCriteria = false;

            if (!query.empty()) {
                std::string likeQuery = "%" + query + "%";
                searchCriteria = Criteria(UssdPlugins::Cols::_name, CompareOperator::Like, likeQuery) ||
                                 Criteria(UssdPlugins::Cols::_description, CompareOperator::Like, likeQuery) ||
                                 Criteria(UssdPlugins::Cols::_category, CompareOperator::Like, likeQuery);
                hasCriteria = true;
            }

            size_t offset = (pageNo > 0) ? (pageNo - 1) * pageSize : 0;
            
            std::vector<UssdPlugins> items;
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
                jsonItem["name"] = modelJson["name"];
                jsonItem["description"] = modelJson["description"];
                jsonItem["category"] = modelJson["category"];
                jsonItem["defaultConfig"] = modelJson["default_config"];
                jsonItem["spec"] = modelJson["spec"];
                jsonItem["isActive"] = modelJson["is_active"];
                jsonItem["isBuiltIn"] = modelJson["is_built_in"];
                jsonItem["isPreinstalled"] = modelJson["is_preinstalled"];
                jsonItem["businessId"] = modelJson["business_id"];
                jsonItem["version"] = modelJson["version"];
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

    drogon::Task<dto::BaseApiResponse> UssdPluginService::getAll(const std::string &businessId, int pageNo, int pageSize, const std::string &query) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdPlugins> mapper(dbClient);

            Criteria searchCriteria(UssdPlugins::Cols::_business_id, CompareOperator::EQ, businessId);

            if (!query.empty()) {
                std::string likeQuery = "%" + query + "%";
                Criteria textCriteria = Criteria(UssdPlugins::Cols::_name, CompareOperator::Like, likeQuery) ||
                                        Criteria(UssdPlugins::Cols::_description, CompareOperator::Like, likeQuery) ||
                                        Criteria(UssdPlugins::Cols::_category, CompareOperator::Like, likeQuery);
                searchCriteria = searchCriteria && textCriteria;
            }

            size_t offset = (pageNo > 0) ? (pageNo - 1) * pageSize : 0;
            auto items = co_await mapper.offset(offset).limit(pageSize).findBy(searchCriteria);
            
            Json::Value jsonArray(Json::arrayValue);
            for (const auto &item : items) {
                Json::Value modelJson = item.toJson();
                Json::Value jsonItem;
                jsonItem["id"] = modelJson["id"];
                jsonItem["name"] = modelJson["name"];
                jsonItem["description"] = modelJson["description"];
                jsonItem["category"] = modelJson["category"];
                jsonItem["defaultConfig"] = modelJson["default_config"];
                jsonItem["spec"] = modelJson["spec"];
                jsonItem["isActive"] = modelJson["is_active"];
                jsonItem["isBuiltIn"] = modelJson["is_built_in"];
                jsonItem["isPreinstalled"] = modelJson["is_preinstalled"];
                jsonItem["businessId"] = modelJson["business_id"];
                jsonItem["version"] = modelJson["version"];
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

    drogon::Task<dto::BaseApiResponse> UssdPluginService::create(const dto::PluginDto &dto) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdPlugins> mapper(dbClient);

            UssdPlugins model;
            
            if (!dto.getName().empty()) model.setName(dto.getName());
            if (!dto.getDescription().empty()) model.setDescription(dto.getDescription());
            if (!dto.getCategory().empty()) model.setCategory(dto.getCategory());
            if (!dto.getDefaultConfig().empty()) model.setDefaultConfig(dto.getDefaultConfig());
            if (!dto.getSpec().empty()) model.setSpec(dto.getSpec());
            
            model.setIsActive(dto.getIsActive());
            model.setIsBuiltIn(dto.getIsBuiltIn());
            model.setIsPreinstalled(dto.getIsPreinstalled());

            if (!dto.getBusinessId().empty()) model.setBusinessId(dto.getBusinessId());
            if (!dto.getVersion().empty()) model.setVersion(dto.getVersion());

            auto result = co_await mapper.insert(model);

            response.success = true;
            response.message = "Created successfully";
            
            Json::Value modelJson = result.toJson();
            Json::Value jsonItem;
            jsonItem["id"] = modelJson["id"];
            jsonItem["name"] = modelJson["name"];
            jsonItem["description"] = modelJson["description"];
            jsonItem["category"] = modelJson["category"];
            jsonItem["defaultConfig"] = modelJson["default_config"];
            jsonItem["spec"] = modelJson["spec"];
            jsonItem["isActive"] = modelJson["is_active"];
            jsonItem["isBuiltIn"] = modelJson["is_built_in"];
            jsonItem["isPreinstalled"] = modelJson["is_preinstalled"];
            jsonItem["businessId"] = modelJson["business_id"];
            jsonItem["version"] = modelJson["version"];
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

    drogon::Task<dto::BaseApiResponse> UssdPluginService::update(const dto::PluginDto &dto, const std::string &id) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdPlugins> mapper(dbClient);

            auto existingModel = co_await mapper.findByPrimaryKey(id);
            
            if (existingModel.getValueOfIsBuiltIn()) {
                response.success = false;
                response.message = "Built-in plugins cannot be updated";
                co_return response;
            }
            
            if (!dto.getName().empty()) existingModel.setName(dto.getName());
            if (!dto.getDescription().empty()) existingModel.setDescription(dto.getDescription());
            if (!dto.getCategory().empty()) existingModel.setCategory(dto.getCategory());
            if (!dto.getDefaultConfig().empty()) existingModel.setDefaultConfig(dto.getDefaultConfig());
            if (!dto.getSpec().empty()) existingModel.setSpec(dto.getSpec());
            
            existingModel.setIsActive(dto.getIsActive());
            existingModel.setIsPreinstalled(dto.getIsPreinstalled());

            if (!dto.getVersion().empty()) existingModel.setVersion(dto.getVersion());

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

    drogon::Task<dto::BaseApiResponse> UssdPluginService::install(const std::string &businessId, const std::string &id) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdPlugins> mapper(dbClient);

            auto existingModel = co_await mapper.findByPrimaryKey(id);
            
            existingModel.setBusinessId(businessId);
            existingModel.setIsActive(true);

            auto result = co_await mapper.update(existingModel);

            response.success = true;
            response.message = "Installed successfully";
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

    drogon::Task<dto::BaseApiResponse> UssdPluginService::remove(const std::string &id) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdPlugins> mapper(dbClient);

            auto existingModel = co_await mapper.findByPrimaryKey(id);
            
            if (existingModel.getValueOfIsBuiltIn()) {
                response.success = false;
                response.message = "Built-in plugins cannot be deleted";
                co_return response;
            }

            co_await mapper.deleteByPrimaryKey(id);

            response.success = true;
            response.message = "Deleted successfully";
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

}