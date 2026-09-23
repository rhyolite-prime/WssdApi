//
// ProviderAdapters.h — per-aggregator USSD request/response mapping.
//
// Every provider keeps its own wire model (see dto/UssdSessionRequestDto.h
// and dto/UssdSessionResponse.h); these pure functions translate:
//
//   inbound DTO  --normalize-->  UssdInteraction  --engine-->
//   ExecutionOutcome  --renderOutcome-->  UssdResult  --render-->  wire JSON
//
// Pure and allocation-light so the whole file is unit-testable without a
// database, a running engine, or the Sapo library (only the outcome's
// header-declared shape is touched).
//

#pragma once

#include <string>

#include <json/json.h>

#include "dto/UssdSessionRequestDto.h"
#include "dto/UssdSessionResponse.h"

#include "domain_services/sapo/UssdInteraction.h"

// Forward declaration only: the header never needs the outcome's layout,
// keeping test binaries free of the Sapo include path.
namespace sapo::runtime {
struct ExecutionOutcome;
}  // namespace sapo::runtime

namespace wssd_api::sapo_host::adapters {

/// Nalo -> neutral. Nalo passes no usable session id, so the normalized
/// MSISDN *is* the session key (one live flow per subscriber), and a
/// USERDATA dial string marks initiation (see isDialString); everything
/// else continues the bound flow (see UssdFlowBindingStore).
UssdInteraction normalizeNalo(const dto::NaloUssdSessionRequestDto &dto, const Json::Value &raw);

/// Hubtel -> neutral. "Initiation" starts, "Response" continues, "Release"
/// cancels (compared case-insensitively; Hubtel mixes casings in the wild).
/// Initiation pins the session's flow binding (see UssdFlowBindingStore);
/// continuations read it back via the passed SessionId.
UssdInteraction normalizeHubtel(const dto::HubtelUssdSessionRequestDto &dto, const Json::Value &raw);

/// Neutral result -> Nalo wire JSON ({USERID, MSISDN, SESSIONID, USERDATA,
/// MSGTYPE, MSG}; MSGTYPE=true keeps the session alive).
Json::Value renderNalo(const dto::NaloUssdSessionRequestDto &dto, const UssdResult &result);

/// Neutral result -> Hubtel wire JSON. Type is "Response" (continue) or
/// "Release" (close); ClientState carries the resume cursor while open.
Json::Value renderHubtel(const dto::HubtelUssdSessionRequestDto &dto, const UssdResult &result);

/// Engine outcome -> neutral result: continuation flag, handset message,
/// Hubtel datatype hints. Failed outcomes become a generic apology — engine
/// internals are logged, never sent to the handset.
UssdResult renderOutcome(const ::sapo::runtime::ExecutionOutcome &outcome);

/// True when the text is a dial string ("*123#", "*711*23#").
bool isDialCode(const std::string &text);

/// True when the text carries a dial string anywhere (contains both '*'
/// and '#'). Looser than isDialCode on purpose: Nalo initiation is "the
/// user entered a short code", and any '*'+'#' input restarts the flow
/// rather than being misread as a menu reply.
bool isDialString(const std::string &text);

/// The trimmed dial string, or "" when the text is subscriber input.
std::string extractDialCode(const std::string &text);

/// Normalizes an MSISDN into a stable session key: trims whitespace,
/// drops visual separators, strips one leading '+'.
std::string normalizeMsisdn(const std::string &msisdn);

std::string trimCopy(const std::string &text);
bool equalsIgnoreCase(const std::string &a, const std::string &b);

/// Masks an MSISDN for logs ("233241234567" -> "***567").
std::string maskMsisdn(const std::string &msisdn);

}  // namespace wssd_api::sapo_host::adapters
