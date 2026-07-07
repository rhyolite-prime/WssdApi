//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//

#include "UssdSessionService.h"

#include "dto/HubtelUssdSessioResponse.h"
#include "dto/NaloUssdSessioResponse.h"
#include "domain_services/dsl/UssdflowToSapoTranslator.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

#include "plugins/WssdServicePlugin.h"

namespace wssd_api::domain_services {


    drogon::Task<dto::NaloUssdSessionResponse> UssdSessionService::handleNaloUssdSessionInteraction(const dto::NaloUssdSessionRequestDto &dto) {

        auto plugin = drogon::app().getPlugin<WssdServicePlugin>();
        auto &redisCacheManager = plugin->getRedisCacheManager();
        auto &wssdRegistryService = plugin->getWssdRegistryService();

        std::string sessionId = dto.getMsisdn() + "_" + dto.getNetwork();
        bool isInitiatingPhase = dto.getUserData().starts_with("*");
        bool isError = false; // Add error logic mapping if needed

        if (isError) {
            dto::NaloUssdSessionResponse errRes;
            errRes.USERID = "Rhyolite";
            errRes.MSISDN = dto.getMsisdn();
            errRes.USERDATA = "";
            errRes.MSG = "Invalid input !\nSession terminated.";
            errRes.MSGTYPE = false;
            co_return errRes;
        }

        sapo::runtime::VirtualMachine vm;
        sapo::runtime::RuntimeContext context;

        std::string promptText = "";
        std::string json_blueprint;

        if (isInitiatingPhase) {
            //nlohmann::json ussdFlowArray = nlohmann::json::array();

            //nlohmann::json ast = UssdflowToSapoTranslator::Translate(ussdFlowArray);
            //json_blueprint = ast.dump();



            try {
                auto wssdRegistry = co_await wssdRegistryService.getByUssdCode(dto.getUserData());
                // The executable property already contains the translated Sapo JSON AST
                json_blueprint = wssdRegistry.getValueOfExecutable();
            } catch (const drogon::orm::UnexpectedRows &) {
                dto::NaloUssdSessionResponse errRes;
                errRes.USERID = "Rhyolite";
                errRes.MSISDN = dto.getMsisdn();
                errRes.USERDATA = "";
                errRes.MSG = "WSSD/USSD Service has not been setup or cannot be found";
                errRes.MSGTYPE = false;
                co_return errRes;
            }

            // Populate initial context variables
            context.setVariable("MSISDN", dto.getMsisdn());
            context.setVariable("NETWORK", dto.getNetwork());

            // Execute blueprint
            vm.runBlueprint(json_blueprint, context);

            // Retrieve prompt from Context
            auto promptOpt = context.getVariable("__SYS_SUSPENDED_PROMPT");
            promptText = promptOpt ? promptOpt->get<std::string>() : "Welcome.";

            // Save state and blueprint with TTL (e.g. 5 mins = 300s)
            co_await redisCacheManager.setValueWithTtl("sapo_state_" + sessionId, context.serializeState(), 300);
            co_await redisCacheManager.setValueWithTtl("sapo_flow_" + sessionId, json_blueprint, 300);

        } else {
            // Continuation Phase
            std::string serializedState = co_await redisCacheManager.getValue("sapo_state_" + sessionId);
            json_blueprint = co_await redisCacheManager.getValue("sapo_flow_" + sessionId);

            if (serializedState.empty() || json_blueprint.empty()) {
                dto::NaloUssdSessionResponse errRes;
                errRes.USERID = "Rhyolite";
                errRes.MSISDN = dto.getMsisdn();
                errRes.USERDATA = "";
                errRes.MSG = "Session expired or invalid.";
                errRes.MSGTYPE = false;
                co_return errRes;
            }

            // Restore state
            context.deserializeState(serializedState);

            // this place needs a rethink ....

            // Inject the user's input
            auto activeVarOpt = context.getVariable("__active_input_var");
            std::string activeInputVar = activeVarOpt ? activeVarOpt->get<std::string>() : "user_input";
            context.setVariable(activeInputVar, dto.getUserData());

            // Reset session expiration time
            co_await redisCacheManager.setValueWithTtl("sapo_state_" + sessionId, serializedState, 300);
            co_await redisCacheManager.setValueWithTtl("sapo_flow_" + sessionId, json_blueprint, 300);

            // Resume execution
            vm.resumeBlueprint(json_blueprint, context);
            
            // Check status
            if (vm.getStatus() == sapo::runtime::VMStatus::Success || vm.getStatus() == sapo::runtime::VMStatus::ExecutionError) {
                auto promptOpt = context.getVariable("__SYS_SUSPENDED_PROMPT");
                promptText = promptOpt ? promptOpt->get<std::string>() : (vm.getStatus() == sapo::runtime::VMStatus::Success ? "Thank you." : "An error occurred.");
                
                // Cleanup session
                co_await redisCacheManager.removeValue("sapo_state_" + sessionId);
                co_await redisCacheManager.removeValue("sapo_flow_" + sessionId);
            } else {
                auto promptOpt = context.getVariable("__SYS_SUSPENDED_PROMPT");
                promptText = promptOpt ? promptOpt->get<std::string>() : "Continue.";
                
                // Save mutated state back
                co_await redisCacheManager.setValueWithTtl("sapo_state_" + sessionId, context.serializeState(), 300);
            }
        }

        dto::NaloUssdSessionResponse response;
        response.USERID = "Rhyolite";
        response.MSISDN = dto.getMsisdn();
        response.USERDATA = dto.getUserData();
        response.MSG = promptText;
        
        // true for continuation, false to terminate connection
        response.MSGTYPE = (vm.getStatus() != sapo::runtime::VMStatus::Success && vm.getStatus() != sapo::runtime::VMStatus::ExecutionError);

        co_return response;
    }

    drogon::Task<dto::HubtelUssdSessionResponse> UssdSessionService::handleHubtelUssdSessionInteraction(const dto::HubtelUssdSessionRequestDto &dto) {
        dto::HubtelUssdSessionResponse response;
        response.Message = "Not implemented";
        response.Type = "Release";
        co_return response;
    }

}
