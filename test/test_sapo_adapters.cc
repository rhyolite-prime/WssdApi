// Unit tests for the Sapo/USSD mapping layer: provider normalization,
// outcome rendering and JsonCpp/nlohmann bridging. Needs no database, no
// running engine and no Sapo library (only its headers).

#include <drogon/drogon_test.h>

#include "nlohmann/json.hpp"
#include "runtime/VirtualMachine.hpp"

#include "domain_services/sapo/ProviderAdapters.h"
#include "domain_services/sapo/SapoEngineService.h"
#include "domain_services/sapo/UssdFlowBindingStore.h"
#include "utils/JsonBridge.h"

using namespace wssd_api::sapo_host;
using namespace wssd_api::sapo_host::adapters;

namespace {

sapo::runtime::ExecutionOutcome makeOutcome(const std::string &status) {
    sapo::runtime::ExecutionOutcome outcome;
    outcome.session_id = "hubtel:abc123";
    outcome.execution_id = "exec-1";
    outcome.workflow_id = "wssd:123";
    outcome.status = status;
    outcome.ok = (status != "failed");
    outcome.elapsed_ms = 12;
    outcome.node_visits = 3;
    return outcome;
}

}  // namespace

DROGON_TEST(ProviderAdaptersDialCode) {
    CHECK(extractDialCode("*123#") == "*123#");
    CHECK(extractDialCode("  *711*23#  ") == "*711*23#");
    CHECK(extractDialCode("1") == "");
    CHECK(extractDialCode("") == "");
    CHECK(isDialCode("*123#"));
    CHECK(!isDialCode("hello"));
    CHECK(isDialString("*123#"));
    CHECK(isDialString("*711*23#"));
    CHECK(!isDialString("1"));
    CHECK(!isDialString(""));
    CHECK(normalizeMsisdn("  +233241234567 ") == "233241234567");
    CHECK(normalizeMsisdn("233-24-123-4567") == "233241234567");
    CHECK(normalizeMsisdn("") == "");
    CHECK(equalsIgnoreCase("Initiation", "initiation"));
    CHECK(!equalsIgnoreCase("Response", "Release"));
    CHECK(maskMsisdn("233241234567") == "***567");
}

DROGON_TEST(ProviderAdaptersNaloNormalize) {
    wssd_api::dto::NaloUssdSessionRequestDto dto;
    dto.setUserId("wssd-nalo");
    dto.setMsisdn("+233241234567");
    dto.setUserData("*123#");
    dto.setNetwork("MTN");
    dto.setSessionId("sess-1");
    Json::Value raw;
    raw["SESSIONID"] = "sess-1";

    // Initiation: dial string in USERDATA, MSISDN-anchored session (the
    // provider SESSIONID is ignored for identity, kept in raw for audit).
    const UssdInteraction start = normalizeNalo(dto, raw);
    CHECK(start.provider == UssdProvider::Nalo);
    CHECK(start.networkSessionId == "233241234567");
    CHECK(start.msisdn == "233241234567");
    CHECK(start.serviceKey == "wssd-nalo");
    CHECK(start.dialCode == "*123#");
    CHECK(start.isStart);
    CHECK(!start.isRelease);
    CHECK(start.raw["SESSIONID"].asString() == "sess-1");

    // Continuation: same subscriber, plain reply, same session key.
    dto.setUserData("1");
    dto.setSessionId("sess-2");
    const UssdInteraction cont = normalizeNalo(dto, raw);
    CHECK(cont.networkSessionId == "233241234567");
    CHECK(cont.dialCode == "");
    CHECK(!cont.isStart);
    CHECK(!cont.isRelease);
}

DROGON_TEST(UssdFlowBindingStoreMemory) {
    // No Redis URL => in-memory fallback (also the test-binary mode, which
    // never defines SAPO_ENABLE_REDIS).
    UssdFlowBindingStore store("", 900, 1);
    CHECK(!store.usingRedis());
    CHECK(!store.find("nalo:2331").has_value());

    UssdFlowBinding binding;
    binding.workflowId = "wssd:123";
    binding.blueprintJson = "{\"name\":\"wssd:123\"}";
    binding.serviceKey = "wssd-nalo";
    binding.dialCode = "*123#";
    binding.businessSubscriptionId = "sub-1";
    store.save("nalo:2331", binding);

    auto hit = store.find("nalo:2331");
    CHECK(hit.has_value());
    CHECK(hit->workflowId == "wssd:123");
    CHECK(hit->blueprintJson == "{\"name\":\"wssd:123\"}");
    CHECK(hit->serviceKey == "wssd-nalo");
    CHECK(hit->dialCode == "*123#");
    CHECK(hit->businessSubscriptionId == "sub-1");
    CHECK(hit->updatedMs > 0);

    // Redial overwrites the pinned flow.
    binding.workflowId = "wssd:456";
    store.save("nalo:2331", binding);
    CHECK(store.find("nalo:2331")->workflowId == "wssd:456");

    store.remove("nalo:2331");
    CHECK(!store.find("nalo:2331").has_value());

    // Empty ids never touch the store.
    store.save("", binding);
    CHECK(!store.find("").has_value());
}

DROGON_TEST(ProviderAdaptersHubtelNormalize) {
    wssd_api::dto::HubtelUssdSessionRequestDto dto;
    dto.setType("Initiation");
    dto.setMobile("233201234567");
    dto.setSessionId("hub-1");
    dto.setServiceCode("713");
    dto.setMessage("*713#");
    dto.setOperator("vodafone");
    dto.setSequence(1);

    const UssdInteraction start = normalizeHubtel(dto, Json::Value());
    CHECK(start.isStart);
    CHECK(!start.isRelease);
    CHECK(start.dialCode == "*713#");
    CHECK(start.serviceKey == "713");

    dto.setType("response");
    dto.setMessage("2");
    dto.setSequence(2);
    const UssdInteraction cont = normalizeHubtel(dto, Json::Value());
    CHECK(!cont.isStart);
    CHECK(!cont.isRelease);
    CHECK(cont.dialCode == "");

    dto.setType("Release");
    const UssdInteraction release = normalizeHubtel(dto, Json::Value());
    CHECK(release.isRelease);
}

DROGON_TEST(ProviderAdaptersRenderOutcome) {
    auto suspended = makeOutcome("awaiting_input");
    suspended.prompt = nlohmann::json{{"message", "1. Balance\n2. Help"},
                                      {"interaction_type", "menu"}};
    suspended.cursor = "route_choice";
    UssdResult cont = renderOutcome(suspended);
    CHECK(cont.cont);
    CHECK(cont.message == "1. Balance\n2. Help");
    CHECK(cont.dataType == "menu");
    CHECK(cont.clientState == "route_choice");

    auto done = makeOutcome("terminated");
    done.output = nlohmann::json{{"message", "Thank you. Goodbye."}};
    UssdResult end = renderOutcome(done);
    CHECK(!end.cont);
    CHECK(end.message == "Thank you. Goodbye.");
    CHECK(end.clientState == "End");

    auto failed = makeOutcome("failed");
    failed.error = "connection refused";
    failed.error_code = "HTTP_ERROR";
    UssdResult apology = renderOutcome(failed);
    CHECK(!apology.cont);
    CHECK(apology.message.find("connection refused") == std::string::npos);
    CHECK(!apology.message.empty());
}

DROGON_TEST(ProviderAdaptersRenderProviders) {
    wssd_api::dto::NaloUssdSessionRequestDto nalo;
    nalo.setUserId("u");
    nalo.setMsisdn("m");
    nalo.setSessionId("s");
    nalo.setUserData("*123#");

    UssdResult cont;
    cont.cont = true;
    cont.message = "Pick one:";
    const Json::Value naloJson = renderNalo(nalo, cont);
    CHECK(naloJson["MSGTYPE"].asBool() == true);
    CHECK(naloJson["MSG"].asString() == "Pick one:");
    CHECK(naloJson["SESSIONID"].asString() == "s");

    wssd_api::dto::HubtelUssdSessionRequestDto hubtel;
    hubtel.setSessionId("hub-1");
    hubtel.setClientState("old");
    UssdResult end;
    end.cont = false;
    end.message = "Bye.";
    const Json::Value hubtelJson = renderHubtel(hubtel, end);
    CHECK(hubtelJson["Type"].asString() == "Release");
    CHECK(hubtelJson["SessionId"].asString() == "hub-1");
    CHECK(hubtelJson["ClientState"].asString() == "End");
}

DROGON_TEST(JsonBridgeRoundTrip) {
    Json::Value original;
    original["str"] = "hello";
    original["int"] = Json::Int64(-42);
    original["uint"] = Json::UInt64(42);
    original["dbl"] = 1.5;
    original["flag"] = true;
    original["nil"] = Json::Value(Json::nullValue);
    original["arr"].append("x");
    original["obj"]["nested"] = 7;

    const nlohmann::json between = wssd_api::utils::toNlohmann(original);
    const Json::Value back = wssd_api::utils::toJsonCpp(between);
    CHECK(back["str"].asString() == "hello");
    CHECK(back["int"].asInt64() == -42);
    CHECK(back["uint"].asUInt64() == Json::UInt64(42));
    CHECK(back["dbl"].asDouble() == 1.5);
    CHECK(back["flag"].asBool() == true);
    CHECK(back["nil"].isNull());
    CHECK(back["arr"][0].asString() == "x");
    CHECK(back["obj"]["nested"].asInt() == 7);
}

DROGON_TEST(SapoStartProblemSeverity) {
    // Validator warnings are tolerated at startup; everything else is fatal.
    CHECK(SapoEngineService::isWarningProblem(
        "workflow 'default': WARNING [main_menu] control-flow cycle: main_menu → route_choice"));
    CHECK(!SapoEngineService::isWarningProblem(
        "config: engine.state_redis must be a non-empty redis:// URL string"));
    CHECK(!SapoEngineService::isWarningProblem("config: cannot open config file 'x'"));
    CHECK(!SapoEngineService::isWarningProblem("sapo-dev engine is not configured"));
    CHECK(!SapoEngineService::isWarningProblem(""));
}
