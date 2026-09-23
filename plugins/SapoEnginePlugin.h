/**
 * SapoEnginePlugin — Drogon-owned lifecycle for the embedded Sapo engine.
 *
 * Registered in config.json ("plugins"). initAndStart configures and starts
 * the process-wide SapoEngineService before traffic arrives; shutdown stops
 * it with the app. Controllers reach the USSD orchestrator through here:
 *
 *     auto plugin = drogon::app().getPlugin<SapoEnginePlugin>();
 *     UssdResult result = co_await plugin->orchestrator().handle(interaction);
 *
 * If the engine fails to start, the plugin deliberately does NOT throw:
 * the API keeps serving and USSD turns degrade to an "unavailable" message
 * (see UssdSessionOrchestrator), which operators will see in the logs.
 */

#pragma once

#include <memory>

#include <drogon/plugins/Plugin.h>
#include <json/json.h>

namespace wssd_api::sapo_host {
class SapoEngineService;
class UssdSessionOrchestrator;
}  // namespace wssd_api::sapo_host

class SapoEnginePlugin : public drogon::Plugin<SapoEnginePlugin> {
  public:
    SapoEnginePlugin() = default;
    ~SapoEnginePlugin() override;

    void initAndStart(const Json::Value &config) override;
    void shutdown() override;

    wssd_api::sapo_host::UssdSessionOrchestrator &orchestrator();
    wssd_api::sapo_host::SapoEngineService &engine();

  private:
    std::unique_ptr<wssd_api::sapo_host::UssdSessionOrchestrator> orchestrator_;
};
