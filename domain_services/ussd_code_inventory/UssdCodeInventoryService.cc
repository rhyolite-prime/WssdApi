//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//
#include "UssdCodeInventoryService.h"
#include "../../models/UssdCodeInventory.h"
#include <drogon/drogon.h>
#include <drogon/orm/CoroMapper.h>
#include <drogon/orm/Criteria.h>

using namespace drogon;
using namespace drogon::orm;
using namespace drogon_model::WssdApi;

namespace wssd_api::domain_services {

    drogon::Task<dto::BaseApiResponse> UssdCodeInventoryService::getAll(int pageNo, int pageSize, const std::string &query) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdCodeInventory> mapper(dbClient);

            Criteria searchCriteria;
            if (!query.empty()) {
                std::string likeQuery = "%" + query + "%";
                searchCriteria = Criteria(UssdCodeInventory::Cols::_code, CompareOperator::Like, likeQuery);
            }

            size_t offset = (pageNo > 0) ? (pageNo - 1) * pageSize : 0;
            
            std::vector<UssdCodeInventory> items;
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
                jsonItem["code"] = modelJson["code"];
                jsonItem["codeType"] = modelJson["code_type"];
                jsonItem["sellingPrice"] = modelJson["selling_price"];
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

    drogon::Task<dto::BaseApiResponse> UssdCodeInventoryService::create(const Json::Value &dto) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdCodeInventory> mapper(dbClient);

            UssdCodeInventory model;
            
            if (dto.isMember("code") && !dto["code"].isNull()) {
                model.setCode(dto["code"].asString());
            }
            if (dto.isMember("codeType") && !dto["codeType"].isNull()) {
                model.setCodeType(dto["codeType"].asString());
            }
            if (dto.isMember("sellingPrice") && !dto["sellingPrice"].isNull()) {
                model.setSellingPrice(dto["sellingPrice"].asString());
            }

            auto result = co_await mapper.insert(model);

            response.success = true;
            response.message = "Created successfully";
            
            Json::Value modelJson = result.toJson();
            Json::Value jsonItem;
            jsonItem["id"] = modelJson["id"];
            jsonItem["code"] = modelJson["code"];
            jsonItem["codeType"] = modelJson["code_type"];
            jsonItem["sellingPrice"] = modelJson["selling_price"];
            
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

    drogon::Task<dto::BaseApiResponse> UssdCodeInventoryService::update(const Json::Value &dto, const std::string &id) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdCodeInventory> mapper(dbClient);

            auto existingModel = co_await mapper.findByPrimaryKey(id);
            
            if (dto.isMember("code") && !dto["code"].isNull()) {
                existingModel.setCode(dto["code"].asString());
            }
            if (dto.isMember("codeType") && !dto["codeType"].isNull()) {
                existingModel.setCodeType(dto["codeType"].asString());
            }
            if (dto.isMember("sellingPrice") && !dto["sellingPrice"].isNull()) {
                existingModel.setSellingPrice(dto["sellingPrice"].asString());
            }

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

    drogon::Task<dto::BaseApiResponse> UssdCodeInventoryService::remove(const std::string &id) {
        dto::BaseApiResponse response;
        try {
            auto dbClient = drogon::app().getDbClient();
            CoroMapper<UssdCodeInventory> mapper(dbClient);

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