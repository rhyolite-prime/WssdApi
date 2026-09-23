/**
 * SapoEnginePlugin.cc
 */

#include "SapoEnginePlugin.h"

#include <stdexcept>

#include <drogon/drogon.h>

#include "domain_services/sapo/BlockingRunner.h"
#include "domain_services/sapo/SapoEngineService.h"
#include "domain_services/sapo/SapoSettings.h"
#include "domain_services/sapo/UssdSessionOrchestrator.h"

SapoEnginePlugin::~SapoEnginePlugin() = default;

void SapoEnginePlugin::initAndStart(const Json::Value &config) {
    auto settings = wssd_api::sapo_host::SapoSettings::fromJson(config);
    settings.applyEnvOverrides();

    wssd_api::sapo_host::BlockingRunner::instance().start(settings.blockingThreads);

    auto &engine = wssd_api::sapo_host::SapoEngineService::instance();
    if (!engine.configure(settings)) {
        LOG_ERROR << "[sapo] engine configuration failed; USSD endpoints will answer unavailable";
    } else {
        const auto problems = engine.start();
        for (const auto &problem : problems) {
            LOG_ERROR << "[sapo] " << problem;
        }
        if (!engine.running()) {
            LOG_ERROR << "[sapo] engine did not start; USSD endpoints will answer unavailable";
        }
    }

    orchestrator_ = std::make_unique<wssd_api::sapo_host::UssdSessionOrchestrator>(settings);
}

void SapoEnginePlugin::shutdown() {
    orchestrator_.reset();
    wssd_api::sapo_host::SapoEngineService::instance().stop();
    wssd_api::sapo_host::BlockingRunner::instance().stop();
}

wssd_api::sapo_host::UssdSessionOrchestrator &SapoEnginePlugin::orchestrator() {
    if (!orchestrator_) {
        throw std::runtime_error("SapoEnginePlugin is not initialized");
    }
    return *orchestrator_;
}

wssd_api::sapo_host::SapoEngineService &SapoEnginePlugin::engine() {
    return wssd_api::sapo_host::SapoEngineService::instance();
}
