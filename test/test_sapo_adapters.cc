// Unit tests for the Sapo/USSD mapping layer: provider normalization,
// outcome rendering and JsonCpp/nlohmann bridging. Needs no database, no
// running engine and no Sapo library (only its headers).

#include <drogon/drogon_test.h>

#include "runtime/VirtualMachine.hpp"

#include "domain_services/sapo/ProviderAdapters.h"
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
    CHECK(equalsIgnoreCase("Initiation", "initiation"));
    CHECK(!equalsIgnoreCase("Response", "Release"));
    CHECK(maskMsisdn("233241234567") == "***567");
}

DROGON_TEST(ProviderAdaptersNaloNormalize) {
    wssd_api::dto::NaloUssdSessionRequestDto dto;
    dto.setUserId("wssd-nalo");
    dto.setMsisdn("233241234567");
    dto.setUserData("*123#");
    dto.setNetwork("MTN");
    dto.setSessionId("sess-1");
    Json::Value raw;
    raw["SESSIONID"] = "sess-1";

    const UssdInteraction interaction = normalizeNalo(dto, raw);
    CHECK(interaction.provider == UssdProvider::Nalo);
    CHECK(interaction.networkSessionId == "sess-1");
    CHECK(interaction.msisdn == "233241234567");
    CHECK(interaction.serviceKey == "wssd-nalo");
    CHECK(interaction.dialCode == "*123#");
    CHECK(!interaction.isStart);
    CHECK(!interaction.isRelease);
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
