/**
 *
 *  WssdServicePlugin.h
 *
 */

#pragma once

#include <drogon/plugins/Plugin.h>

#include "domain_services/redis/RedisCacheManager.h"
#include "domain_services/wssd_registry/WssdRegistryService.h"


class WssdServicePlugin : public drogon::Plugin<WssdServicePlugin>
{
  public:
    WssdServicePlugin() = default;
    ~WssdServicePlugin() override = default;
    /// This method must be called by drogon to initialize and start the plugin.
    /// It must be implemented by the user.
    void initAndStart(const Json::Value &config) override;

    /// This method must be called by drogon to shutdown the plugin.
    /// It must be implemented by the user.
    void shutdown() override;

    wssd_api::domain_services::WssdRegistryService &getWssdRegistryService() { return wssdRegistryService_; }
    wssd_api::domain_services::RedisCacheManager &getRedisCacheManager() { return redisCacheManager_; }


private:
    wssd_api::domain_services::RedisCacheManager redisCacheManager_;
    wssd_api::domain_services::WssdRegistryService wssdRegistryService_;
};

