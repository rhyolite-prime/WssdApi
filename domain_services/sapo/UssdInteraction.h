//
// UssdInteraction.h — provider-neutral USSD request/response model.
//
// The Sapo engine speaks one language (JSON in, ExecutionOutcome out) while
// every USSD aggregator speaks its own dialect (Nalo, Hubtel, ...). These
// structs are the neutral middle: provider adapters normalize inbound
// webhooks into UssdInteraction, the orchestrator runs exactly one Sapo turn
// per interaction, and the adapters render the resulting UssdResult back
// into the provider's response shape.
//
// Deliberately free of Sapo and Drogon includes so unit tests can use it
// with only JsonCpp on the include path.
//

#pragma once

#include <cstdint>
#include <string>

#include <json/json.h>

namespace wssd_api::sapo_host {

enum class UssdProvider {
    Nalo,
    Hubtel,
};

inline std::string toString(UssdProvider provider) {
    switch (provider) {
        case UssdProvider::Nalo:
            return "nalo";
        case UssdProvider::Hubtel:
            return "hubtel";
    }
    return "unknown";
}

/// One inbound USSD webhook, normalized across providers.
struct UssdInteraction {
    UssdProvider provider = UssdProvider::Nalo;
    /// Gateway session id. Hubtel passes one (SessionId); Nalo does not, so
    /// the normalized subscriber MSISDN is the session key (one live USSD
    /// flow per subscriber, matching the single handset USSD channel).
    std::string networkSessionId;
    /// Subscriber MSISDN (Nalo MSISDN / Hubtel Mobile).
    std::string msisdn;
    /// Raw subscriber input (Nalo USERDATA / Hubtel Message).
    std::string userInput;
    /// Service identifier from the gateway (Nalo USERID / Hubtel ServiceCode).
    std::string serviceKey;
    /// Dial string (e.g. "*713#") when the input carries one, else "".
    std::string dialCode;
    /// Mobile network (Nalo NETWORK / Hubtel Operator).
    std::string network;
    /// Hubtel Sequence, 0 when the provider sends none.
    int sequence = 0;
    /// Hubtel ClientState echo, "" when the provider sends none.
    std::string clientState;
    /// True for Hubtel "Initiation" callbacks and for Nalo turns whose
    /// USERDATA is a dial string. Initiation force-starts a fresh engine
    /// session (redial mid-flow restarts it) and re-pins the flow binding.
    bool isStart = false;
    /// True for Hubtel "Release" callbacks (subscriber gone / timed out).
    bool isRelease = false;
    /// Untouched gateway payload, kept for audit rows and debugging.
    Json::Value raw;
};

/// One rendered Sapo turn, before provider-specific encoding.
struct UssdResult {
    /// False => close the session (Nalo MSGTYPE=false / Hubtel Release).
    bool cont = false;
    /// Text to display on the handset.
    std::string message;
    /// Opaque resume cursor; echoed as Hubtel ClientState, "End" on release.
    std::string clientState;
    /// Short label for Hubtel's Label field (first line of message).
    std::string label;
    /// Hubtel DataType hint derived from the prompt (input/menu/display).
    std::string dataType = "input";
    /// Hubtel FieldType hint derived from the prompt (text/password/phone).
    std::string fieldType = "text";
    /// Engine session id ("<provider>:<network session>").
    std::string sapoSessionId;
    /// Registry id of the workflow that ran.
    std::string workflowId;
    /// Engine outcome status (awaiting_input/suspended/terminated/...).
    std::string status;
    int64_t elapsedMs = 0;
    std::size_t nodeVisits = 0;
};

}  // namespace wssd_api::sapo_host
