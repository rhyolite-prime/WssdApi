#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class DslController : public drogon::HttpController<DslController>
{
  public:
  static constexpr const char *PREFIX = "/api/v1/dsl/";
  METHOD_LIST_BEGIN
      ADD_METHOD_TO(DslController::translateUssdFlowToSapo, std::string(PREFIX) + "ussd-to-sapo", Post);
      ADD_METHOD_TO(DslController::validateSapoSpec, std::string(PREFIX) + "validate-sapo-spec", Post);
      //ADD_METHOD_TO(DslController::translateUssd, std::string(PREFIX) + "generate-spec", Post);
  METHOD_LIST_END

  Task<HttpResponsePtr> translateUssdFlowToSapo(HttpRequestPtr req);
  Task<HttpResponsePtr> validateSapoSpec(HttpRequestPtr req);


};
