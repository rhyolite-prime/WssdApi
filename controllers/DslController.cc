#include "DslController.h"
#include "dto/BaseApiResponse.h"
#include "domain_services/dsl/UssdflowToSapoTranslator.hpp"
#include "parser/WorkflowParser.hpp"
#include <nlohmann/json.hpp>
#include <json/json.h>

Task<HttpResponsePtr> DslController::translateUssdFlowToSapo(HttpRequestPtr req)
{
    auto jsonBody = req->getJsonObject();
    if (!jsonBody) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k400BadRequest);
        resp->setBody("Missing or invalid JSON body");
        co_return resp;
    }
    
    Json::FastWriter writer;
    std::string rawJson = writer.write(*jsonBody);
    
    try {
        auto ussdFlowArray = nlohmann::json::parse(rawJson);
        auto sapoAst = wssd_api::domain_services::UssdflowToSapoTranslator::Translate(ussdFlowArray);
        
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k200OK);
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->setBody(sapoAst.dump(4));
        co_return resp;
    } catch (const std::exception& e) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody(std::string("Error translating: ") + e.what());
        co_return resp;
    }
}


Task<HttpResponsePtr> DslController::validateSapoSpec(HttpRequestPtr req)
{
    auto jsonBody = req->getJsonObject();
    if (!jsonBody) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k400BadRequest);
        resp->setBody("Missing or invalid JSON body");
        co_return resp;
    }
    
    Json::FastWriter writer;
    std::string rawJson = writer.write(*jsonBody);
    
    wssd_api::dto::BaseApiResponse response;
    try {
        sapo::parser::WorkflowParser::parse(rawJson);
        response.message = "Validation successful. AST is well-formed.";
        response.success = "True";
        auto resp = HttpResponse::newHttpJsonResponse(response.toJson());
        resp->setStatusCode(k200OK);
        co_return resp;
    } catch (const std::exception& e) {
        response.message = std::string("Validation failed: ") + e.what();
        response.success = "False";
        auto resp = HttpResponse::newHttpJsonResponse(response.toJson());
        resp->setStatusCode(k400BadRequest);
        co_return resp;
    }
}
