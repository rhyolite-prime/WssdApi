#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <iostream>

namespace wssd_api::domain_services {


class UssdflowToSapoTranslator {
private:
    static std::string safeGetString(const nlohmann::json& obj, const std::string& key, const std::string& default_val = "") {
        if (obj.contains(key) && obj[key].is_string()) {
            return obj[key].get<std::string>();
        }
        return default_val;
    }

public:
    static nlohmann::json Translate(const nlohmann::json& ussdFlowArray) {
        nlohmann::json sapoAst = nlohmann::json::array();

        if (!ussdFlowArray.is_array()) return sapoAst;

        for (const auto& menu : ussdFlowArray) {
            std::string menuId = safeGetString(menu, "id", "unknown_id");
            std::string title = safeGetString(menu, "title", "");
            
            // 1. Map the Menu Prompt to an Action Node (Suspends and waits for user input)
            nlohmann::json promptAction = nlohmann::json::object();
            promptAction["type"] = "action";
            promptAction["id"] = "menu_" + menuId;
            promptAction["capability"] = "user_prompt";
            
            nlohmann::json pConfig = nlohmann::json::object();
            pConfig["message"] = GeneratePrompt(menu);
            pConfig["interaction_type"] = "menu";
            promptAction["prompt_config"] = pConfig;

            if (!title.empty()) {
                promptAction["meta"] = { {"title", title} };
            }
            
            promptAction["outputs"] = nlohmann::json::object({{"user_input", "user_input_" + menuId}});
            
            // Route to a Choice Node that evaluates the user's input
            std::string routerNodeId = "router_" + menuId;
            
            nlohmann::json taskRef = nlohmann::json::object();
            taskRef["task_id"] = routerNodeId;
            promptAction["next_tasks"] = nlohmann::json::array({taskRef});
            
            sapoAst.push_back(promptAction);
            
            nlohmann::json routerNode = nlohmann::json::object();
            routerNode["type"] = "choice";
            routerNode["id"] = routerNodeId;
            routerNode["variable_key"] = "user_input_" + menuId;
            routerNode["cases"] = nlohmann::json::object();
            routerNode["default_target"] = promptAction["id"];
            
            // 2. Map Options to Choices and Sub-Actions
            if (menu.contains("options") && menu["options"].is_array()) {
                int index = 1;
                for (const auto& option : menu["options"]) {
                    std::string actionType = safeGetString(option, "actionType", "");
                    
                    std::string optionTargetNodeId = GenerateOptionNodes(option, sapoAst, menuId);
                    
                    if (actionType != "input" && actionType != "menu-input") {
                        routerNode["cases"][std::to_string(index)] = optionTargetNodeId;
                    } else {
                        // Input nodes capture arbitrary text. The choice node falls through 
                        // to the default_target. We set the default target to the script node.
                        routerNode["default_target"] = optionTargetNodeId;
                    }
                    
                    index++;
                }
            }
            
            sapoAst.push_back(routerNode);
        }
        
        return sapoAst;
    }

private:
    static std::string GeneratePrompt(const nlohmann::json& menu) {
        std::string prompt = "";
        std::string title = safeGetString(menu, "title", "");
        if (!title.empty()) {
            prompt += title + "\n";
        }
        
        if (menu.contains("options") && menu["options"].is_array()) {
            int i = 1;
            for (const auto& option : menu["options"]) {
                std::string displayText = safeGetString(option, "displayText", "");
                if (!displayText.empty()) {
                    prompt += std::to_string(i) + ". " + displayText + "\n";
                    i++;
                }
            }
        }
        
        // trim trailing newline
        if (!prompt.empty() && prompt.back() == '\n') {
            prompt.pop_back();
        }
        return prompt;
    }

    static std::string GenerateOptionNodes(const nlohmann::json& option, nlohmann::json& ast, const std::string& parentMenuId) {
        std::string actionType = safeGetString(option, "actionType", "");
        std::string optionId = safeGetString(option, "id", "unknown_opt");
        std::string nextMenuId = safeGetString(option, "nextMenuId", "");
        
        std::string terminateTarget = "terminate_flow";
        std::string nextTarget = (nextMenuId != "00000000-0000-0000-0000-000000000000" && !nextMenuId.empty()) ? "menu_" + nextMenuId : terminateTarget;

        if (actionType == "menu" || actionType.empty()) {
            return nextTarget;
        }

        if (actionType == "invocation") {
            nlohmann::json commandNode = nlohmann::json::object();
            commandNode["type"] = "command";
            commandNode["id"] = "invoke_" + optionId;
            commandNode["command"] = "http.post";
            
            std::string flexConnectId = safeGetString(option, "flexConnectId", "");
            nlohmann::json req = nlohmann::json::object();
            req["url"] = "https://api.flexconnect.com/execute/" + flexConnectId;
            commandNode["http_request"] = req;
            
            std::string apiOutputVariable = safeGetString(option, "apiOutputVariable", "");
            commandNode["output"] = apiOutputVariable.empty() ? "api_response" : apiOutputVariable;
            commandNode["next"] = nextTarget;
            
            ast.push_back(commandNode);
            return commandNode["id"];
        }

        if (actionType == "input" || actionType == "menu-input") {
            std::string scriptNodeId = "validate_input_" + optionId;
            nlohmann::json scriptNode = nlohmann::json::object();
            scriptNode["type"] = "script";
            scriptNode["id"] = scriptNodeId;
            
            std::string flowControl = safeGetString(option, "flowControlExpression", "true");
            std::string inputFieldName = safeGetString(option, "inputFieldName", "var");
            
            scriptNode["language"] = "expr";
            scriptNode["code"] = "if (!(" + flowControl + ")) { jump_to = 'menu_" + parentMenuId + "'; } else { set_var('" + inputFieldName + "', $user_input_" + parentMenuId + "); }";
            scriptNode["next"] = nextTarget;
            
            ast.push_back(scriptNode);
            return scriptNodeId;
        }

        if (actionType == "display") {
            nlohmann::json displayNode = nlohmann::json::object();
            displayNode["type"] = "terminate";
            displayNode["id"] = "display_" + optionId;
            displayNode["status"] = "success";
            
            ast.push_back(displayNode);
            return displayNode["id"];
        }
        
        if (actionType == "deferred-routine") {
            nlohmann::json scheduleNode = nlohmann::json::object();
            scheduleNode["type"] = "schedule";
            scheduleNode["id"] = "schedule_" + optionId;
            scheduleNode["cron"] = "now";
            scheduleNode["target"] = "background_task";
            scheduleNode["next"] = nextTarget;
            
            ast.push_back(scheduleNode);
            return scheduleNode["id"];
        }

        return terminateTarget;
    }
};

} // namespace WssdApi
