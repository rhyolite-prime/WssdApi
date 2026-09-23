//
// UssdSessionOrchestrator.h — one Sapo turn per USSD interaction.
//
// The orchestrator is the only component that talks to both the gateway
// world (via UssdInteraction/UssdResult) and the engine (via
// SapoEngineService). Per interaction it:
//
//   1. resolves the workflow (registry executable -> file -> default),
//   2. ensures the blueprint is registered (content-hashed, cheap),
//   3. runs exactly one engine turn off the IO threads, where the engine
//      resumes the "<provider>:<gateway session>" checkpoint or starts it,
//   4. renders the outcome and writes a best-effort audit row.
//
// Gateway release callbacks cancel the engine session instead of running it.
// Every failure path degrades to a close-session apology message; engine
// internals never reach the handset.
//

#pragma once

#include <string>

#include <drogon/utils/coroutine.h>

#include "SapoSettings.h"
#include "UssdInteraction.h"
#include "UssdWorkflowResolver.h"

namespace wssd_api::sapo_host {

class UssdSessionOrchestrator {
  public:
    explicit UssdSessionOrchestrator(SapoSettings settings);

    drogon::Task<UssdResult> handle(const UssdInteraction &interaction);

  private:
    drogon::Task<void> auditSession(const UssdInteraction &interaction, const UssdResult &result);

    static std::string sapoSessionIdFor(const UssdInteraction &interaction);
    static UssdResult unavailable(const std::string &message);

    SapoSettings settings_;
    UssdWorkflowResolver resolver_;
};

}  // namespace wssd_api::sapo_host
