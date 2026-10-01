//
// PluginOutcomeMapper.h — ExecutionOutcome -> plugin API response mapping.
//
// Header-only and free of engine linkage (ExecutionOutcome is a plain
// struct); needs only JsonCpp + the vendored nlohmann/json header, so unit
// tests compile it exactly like ProviderAdapters/JsonBridge.
//
// The plugin definition decides what the API returns ("may or may not
// return direct outputs"): a blueprint that terminates with an `output`
// payload surfaces it verbatim under result.output; blueprints that ask
// for input suspend instead and answer with a resumable session handle.
//

#pragma once

#include <string>

#include <json/json.h>
#include <nlohmann/json.hpp>

#include "runtime/VirtualMachine.hpp"

#include "utils/JsonBridge.h"

namespace wssd_api::sapo_host {

/// Decomposed API envelope; the controller folds it into BaseApiResponse.
struct PluginOutcomeApiView {
    bool success{false};
    std::string message;
    Json::Value result{Json::objectValue};
    Json::Value error{Json::objectValue};
};

inline bool isSuspendedStatus(const std::string &status) {
    return status == "awaiting_input" || status == "suspended";
}

/// Maps one engine outcome. `plugin` is the caller-facing plugin id;
/// `resumeBasePath` (e.g. "/api/v1/sapo/plugins/") is only used to build the
/// resume hint on suspended outcomes — pass "" to omit it. `includeContext`
/// additionally exposes the full execution context (large; opt-in because
/// context keys can carry provider envelopes).
inline PluginOutcomeApiView mapPluginOutcome(const sapo::runtime::ExecutionOutcome &outcome,
                                             const std::string &plugin, const std::string &resumeBasePath,
                                             bool includeContext) {
    PluginOutcomeApiView view;
    const std::string &status = outcome.status;

    Json::Value result(Json::objectValue);
    result["plugin"] = plugin;
    result["workflow_id"] = outcome.workflow_id;
    result["session_id"] = outcome.session_id;
    result["execution_id"] = outcome.execution_id;
    result["status"] = status;
    result["resumable"] = isSuspendedStatus(status);
    result["node_visits"] = static_cast<Json::UInt64>(outcome.node_visits);
    result["elapsed_ms"] = static_cast<Json::Int64>(outcome.elapsed_ms);
    if (!outcome.cursor.empty() && status != "terminated" && status != "completed") {
        result["cursor"] = outcome.cursor;
    }
    // The output payload is the plugin's declared surface: terminate.output
    // when the blueprint produced one, otherwise the final context (engine
    // semantics). Always include it so direct-response plugins answer here.
    if (!outcome.output.is_null() && !outcome.output.empty()) {
        result["output"] = wssd_api::utils::toJsonCpp(outcome.output);
    }
    if (includeContext && !outcome.context.is_null() && !outcome.context.empty()) {
        result["context"] = wssd_api::utils::toJsonCpp(outcome.context);
    }
    if (!outcome.prompt.is_null() && !outcome.prompt.empty()) {
        result["prompt"] = wssd_api::utils::toJsonCpp(outcome.prompt);
    }
    if (isSuspendedStatus(status) && !resumeBasePath.empty()) {
        result["resume_path"] = resumeBasePath + "sessions/" + outcome.session_id + "/resume";
    }
    if (!outcome.warnings.empty()) {
        Json::Value warnings(Json::arrayValue);
        for (const auto &warning : outcome.warnings) {
            warnings.append(warning);
        }
        result["warnings"] = std::move(warnings);
    }

    if (status == "failed") {
        view.success = false;
        view.message = outcome.error.empty() ? "Plugin execution failed" : outcome.error;
        Json::Value error(Json::objectValue);
        error["code"] = outcome.error_code.empty() ? "EXECUTION_FAILED" : outcome.error_code;
        error["message"] = outcome.error;
        if (!outcome.error_node.empty()) {
            error["node"] = outcome.error_node;
        }
        if (!outcome.error_data.is_null() && !outcome.error_data.empty()) {
            error["data"] = wssd_api::utils::toJsonCpp(outcome.error_data);
        }
        view.error = std::move(error);
    } else if (status == "cancelled") {
        view.success = false;
        view.message = outcome.error.empty() ? "Plugin session was cancelled" : outcome.error;
        Json::Value error(Json::objectValue);
        error["code"] = outcome.error_code.empty() ? "CANCELLED" : outcome.error_code;
        error["message"] = view.message;
        view.error = std::move(error);
    } else {
        view.success = true;
        if (status == "terminated") {
            // A terminate node's own message (folded into output.message by
            // the engine) is the most operator-meaningful summary we have.
            std::string terminatedMessage;
            if (outcome.output.is_object() && outcome.output.contains("message") &&
                outcome.output["message"].is_string()) {
                terminatedMessage = outcome.output["message"].get<std::string>();
            }
            view.message = !terminatedMessage.empty()
                               ? terminatedMessage
                               : (outcome.error.empty() ? "Plugin executed successfully" : outcome.error);
        } else if (status == "completed") {
            view.message = "Plugin executed successfully";
        } else if (status == "awaiting_input") {
            view.message = "Plugin is waiting for input; resume the session to continue";
        } else if (status == "suspended") {
            view.message = "Plugin is suspended (timer/event); it will resume without a call";
        } else {
            view.message = "Plugin execution finished";
        }
    }

    view.result = std::move(result);
    return view;
}

}  // namespace wssd_api::sapo_host
