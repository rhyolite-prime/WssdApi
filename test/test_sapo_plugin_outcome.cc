// Unit tests for the plugin API surface: ExecutionOutcome -> API envelope
// mapping (domain_services/sapo/PluginOutcomeMapper.h) and the plugin
// request DTOs (dto/SapoPluginDto.h). Needs no database, no running engine
// and no Sapo library (only its headers).

#include <drogon/drogon_test.h>

#include "nlohmann/json.hpp"
#include "runtime/VirtualMachine.hpp"

#include "domain_services/sapo/PluginOutcomeMapper.h"
#include "dto/SapoPluginDto.h"

using namespace wssd_api::sapo_host;

namespace {

sapo::runtime::ExecutionOutcome makeOutcome(const std::string &status) {
    sapo::runtime::ExecutionOutcome outcome;
    outcome.session_id = "plugin:paystack_checkout:9f86d081";
    outcome.execution_id = "exec-7";
    outcome.workflow_id = "paystack_checkout";
    outcome.status = status;
    outcome.ok = status != "failed" && status != "cancelled";
    outcome.elapsed_ms = 21;
    outcome.node_visits = 6;
    return outcome;
}

}  // namespace

DROGON_TEST(PluginOutcomeTerminatedMapsOutput) {
    auto outcome = makeOutcome("terminated");
    outcome.output = {{"checkout_url", "https://checkout.paystack.com/abc"},
                      {"client_reference", "ref-1"},
                      {"message", "Paystack transaction initialized"}};
    outcome.context = {{"amount", 10}, {"secret_noise", "hidden-by-default"}};

    const auto view = mapPluginOutcome(outcome, "paystack_checkout", "/api/v1/sapo/plugins/", false);
    CHECK(view.success == true);
    CHECK(view.message == "Paystack transaction initialized");
    CHECK(view.error.empty());
    CHECK(view.result["status"].asString() == "terminated");
    CHECK(view.result["resumable"].asBool() == false);
    CHECK(view.result["output"]["checkout_url"].asString() == "https://checkout.paystack.com/abc");
    CHECK(view.result["output"]["client_reference"].asString() == "ref-1");
    // Context stays out unless explicitly requested (it can be large).
    CHECK(!view.result.isMember("context"));
    CHECK(view.result["session_id"].asString() == outcome.session_id);
    CHECK(view.result["node_visits"].asUInt64() == 6);

    const auto withContext = mapPluginOutcome(outcome, "paystack_checkout", "/api/v1/sapo/plugins/", true);
    CHECK(withContext.result["context"]["amount"].asInt() == 10);
}

DROGON_TEST(PluginOutcomeAwaitingInputCarriesResumeHint) {
    auto outcome = makeOutcome("awaiting_input");
    outcome.prompt = {{"message", "Enter your PIN"}, {"input_variable", "pin"}};

    const auto view = mapPluginOutcome(outcome, "kyc_flow", "/api/v1/sapo/plugins/", false);
    CHECK(view.success == true);
    CHECK(view.result["resumable"].asBool() == true);
    CHECK(view.result["prompt"]["message"].asString() == "Enter your PIN");
    CHECK(view.result["resume_path"].asString() ==
          "/api/v1/sapo/plugins/plugin:paystack_checkout:9f86d081/resume");

    // No resume hint when the mapper is given no base path.
    const auto bare = mapPluginOutcome(outcome, "kyc_flow", "", false);
    CHECK(!bare.result.isMember("resume_path"));
}

DROGON_TEST(PluginOutcomeFailureMapsStructuredError) {
    auto outcome = makeOutcome("failed");
    outcome.error_code = "HUBTEL_INITIATE_CALL_FAILED";
    outcome.error = "Hubtel initiate call failed: Invalid merchant account";
    outcome.error_node = "hubtel_initiate";
    outcome.error_data = {{"status", 400}};

    const auto view = mapPluginOutcome(outcome, "hubtel_web_checkout", "/api/v1/sapo/plugins/", false);
    CHECK(view.success == false);
    CHECK(view.error["code"].asString() == "HUBTEL_INITIATE_CALL_FAILED");
    CHECK(view.error["message"].asString().find("Invalid merchant account") != std::string::npos);
    CHECK(view.error["node"].asString() == "hubtel_initiate");
    CHECK(view.error["data"]["status"].asInt() == 400);
    CHECK(view.result["status"].asString() == "failed");
}

DROGON_TEST(PluginOutcomeSuspendedAndCancelled) {
    auto suspended = makeOutcome("suspended");
    CHECK(mapPluginOutcome(suspended, "flow", "/p/", false).result["resumable"].asBool() == true);

    auto cancelled = makeOutcome("cancelled");
    cancelled.error = "cancelled by caller";
    const auto view = mapPluginOutcome(cancelled, "flow", "/p/", false);
    CHECK(view.success == false);
    CHECK(view.error["code"].asString() == "CANCELLED");
}

DROGON_TEST(PluginExecuteRequestParsing) {
    Json::Value body(Json::objectValue);
    body["plugin"] = "paystack_checkout";
    body["variables"] = Json::Value(Json::objectValue);
    body["variables"]["amount"] = 2500;
    body["variables"]["callback_url"] = "https://cb";
    body["metadata"] = Json::Value(Json::objectValue);
    body["metadata"]["order_id"] = "order-1";
    body["session_id"] = "sess-1";
    body["correlation_id"] = "corr-1";
    body["persist"] = false;
    body["include_context"] = true;

    wssd_api::dto::SapoPluginExecuteRequest dto;
    dto.fromJson(body);
    CHECK(dto.getPlugin() == "paystack_checkout");
    CHECK(dto.hasBlueprint() == false);
    CHECK(dto.getVariables()["amount"].asInt() == 2500);
    CHECK(dto.getMetadata()["order_id"].asString() == "order-1");
    CHECK(dto.getSessionId() == "sess-1");
    CHECK(dto.getPersist() == false);
    CHECK(dto.getIncludeContext() == true);

    // Inline blueprint: node array form is accepted and the blueprint's own
    // name is picked up when no explicit plugin id is given.
    Json::Value inlineBody(Json::objectValue);
    inlineBody["blueprint"]["nodes"][0]["id"] = "a";
    inlineBody["blueprint"]["nodes"][0]["type"] = "terminate";
    dto = wssd_api::dto::SapoPluginExecuteRequest();
    dto.fromJson(inlineBody);
    CHECK(dto.hasBlueprint() == true);
    CHECK(dto.getBlueprint().isObject());
    CHECK(dto.getPersist() == true);
    CHECK(dto.getIncludeContext() == false);

    // workflow_id alias.
    Json::Value aliasBody(Json::objectValue);
    aliasBody["workflow_id"] = "hubtel_web_checkout";
    dto = wssd_api::dto::SapoPluginExecuteRequest();
    dto.fromJson(aliasBody);
    CHECK(dto.getPlugin() == "hubtel_web_checkout");
}

DROGON_TEST(PluginResumeRequestParsing) {
    Json::Value body(Json::objectValue);
    body["input"] = "1";
    wssd_api::dto::SapoPluginResumeRequest dto;
    dto.fromJson(body);
    CHECK(dto.getInput().asString() == "1");

    // A bare object body is the resume payload itself.
    Json::Value bare(Json::objectValue);
    bare["otp"] = "1234";
    dto = wssd_api::dto::SapoPluginResumeRequest();
    dto.fromJson(bare);
    CHECK(dto.getInput()["otp"].asString() == "1234");
}
