//
// UssdWorkflowResolver.h — USSD code -> Sapo blueprint resolution.
//
// Gateways identify the dialed service differently (Nalo USERID, Hubtel
// ServiceCode or the *code# dial string), and the registry may store the
// code in several spellings. Resolution therefore tries, in order:
//
//   1. wssd_registry by ussd_code, for the dial code first (most specific)
//      and the gateway service key second; the row's `executable` column
//      IS the Sapo blueprint JSON.
//   2. sapo-dev/workflows/<key>.json files (hot-reloadable through the engine's
//      content-hash check in SapoEngineService::ensureBlueprint).
//   3. sapo-dev/workflows/<default>.json as the last-resort fallback menu.
//
// A resolved workflow whose blueprint text is empty is already registered
// (startup-loaded file); anything else is (re)registered on demand.
//

#pragma once

#include <optional>
#include <string>

#include <drogon/utils/coroutine.h>

#include "SapoSettings.h"

namespace wssd_api::sapo_host {

struct ResolvedWorkflow {
    /// Registry id the blueprint runs under.
    std::string workflowId;
    /// Blueprint JSON to register ("" => already registered, verify only).
    std::string blueprintJson;
    std::string displayTitle;
    std::string matchedKey;
    /// ussd_subscriptions.id whose ussd_code matches the dialed service
    /// ("" when none). Feeds the audit row's NOT NULL
    /// business_subscription_id and is pinned in the flow binding so
    /// continuations reuse it.
    std::string businessSubscriptionId;
    bool fromDatabase = false;
};

class UssdWorkflowResolver {
  public:
    explicit UssdWorkflowResolver(SapoSettings settings) : settings_(std::move(settings)) {}

    /// Resolves the workflow for one USSD turn. Returns nullopt when neither
    /// the registry, nor a keyed file, nor the default blueprint matches.
    /// Never throws: database outages degrade to file/default resolution.
    drogon::Task<std::optional<ResolvedWorkflow>> resolve(const std::string &serviceKey,
                                                           const std::string &dialCode);

  private:
    drogon::Task<std::optional<ResolvedWorkflow>> lookupDatabase(const std::string &key);
    std::optional<ResolvedWorkflow> lookupFile(const std::string &key);
    std::optional<ResolvedWorkflow> lookupFileStem(const std::string &stem);

    SapoSettings settings_;
};

}  // namespace wssd_api::sapo_host
