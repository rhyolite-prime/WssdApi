//
// SapoPluginDto.h — request DTOs for the Sapo plugin execution endpoints.
//
// A plugin execution request carries either a registered plugin id OR an
// inline DSL blueprint, plus the execution variables ("as meta data") in
// JSON. The controller converts variables/metadata to the engine context
// via JsonBridge; the DTO keeps the raw JsonCpp shapes so nothing is lost.
//

#ifndef WSSDAPI_SAPOPLUGINDTO_H
#define WSSDAPI_SAPOPLUGINDTO_H

#include <json/json.h>
#include <string>

namespace wssd_api::dto {

    class SapoPluginExecuteRequest {
      public:
        SapoPluginExecuteRequest() = default;

        void fromJson(const Json::Value &json) {
            if (!json.isObject()) {
                return;
            }
            // Registered plugin id; common aliases accepted for convenience.
            if (json.isMember("plugin") && json["plugin"].isString()) {
                plugin_ = json["plugin"].asString();
            } else if (json.isMember("workflow_id") && json["workflow_id"].isString()) {
                plugin_ = json["workflow_id"].asString();
            } else if (json.isMember("plugin_id") && json["plugin_id"].isString()) {
                plugin_ = json["plugin_id"].asString();
            }
            // Inline blueprint: object ({name, nodes:[...]}) or bare node array.
            if (json.isMember("blueprint") &&
                (json["blueprint"].isObject() || json["blueprint"].isArray())) {
                blueprint_ = json["blueprint"];
            }
            // Execution variables (initial context). "input" is an accepted
            // alias so simple clients mirror the engine's startSession shape.
            if (json.isMember("variables") && json["variables"].isObject()) {
                variables_ = json["variables"];
            } else if (json.isMember("input") && json["input"].isObject()) {
                variables_ = json["input"];
            }
            // Additional metadata: merged into the context (variables win on
            // key conflicts) and exposed under the `metadata` context key (referenced as `${metadata.x}`).
            if (json.isMember("metadata") && json["metadata"].isObject()) {
                metadata_ = json["metadata"];
            }
            if (json.isMember("session_id") && json["session_id"].isString()) {
                sessionId_ = json["session_id"].asString();
            }
            if (json.isMember("correlation_id") && json["correlation_id"].isString()) {
                correlationId_ = json["correlation_id"].asString();
            }
            if (json.isMember("persist") && json["persist"].isBool()) {
                persist_ = json["persist"].asBool();
            }
            if (json.isMember("include_context") && json["include_context"].isBool()) {
                includeContext_ = json["include_context"].asBool();
            }
        }

        [[nodiscard]] const std::string &getPlugin() const { return plugin_; }
        [[nodiscard]] const Json::Value &getBlueprint() const { return blueprint_; }
        [[nodiscard]] const Json::Value &getVariables() const { return variables_; }
        [[nodiscard]] const Json::Value &getMetadata() const { return metadata_; }
        [[nodiscard]] const std::string &getSessionId() const { return sessionId_; }
        [[nodiscard]] const std::string &getCorrelationId() const { return correlationId_; }
        [[nodiscard]] bool getPersist() const { return persist_; }
        [[nodiscard]] bool getIncludeContext() const { return includeContext_; }
        [[nodiscard]] bool hasBlueprint() const { return !blueprint_.isNull(); }

      private:
        std::string plugin_;
        Json::Value blueprint_{Json::nullValue};
        Json::Value variables_{Json::nullValue};
        Json::Value metadata_{Json::nullValue};
        std::string sessionId_;
        std::string correlationId_;
        bool persist_{true};
        bool includeContext_{false};
    };

    /// Resume input for a suspended plugin session. The engine writes the
    /// resume payload verbatim into the prompt's input variable, so any JSON
    /// value is legal: {"input": "1"} for a scalar reply, or an object.
    class SapoPluginResumeRequest {
      public:
        SapoPluginResumeRequest() = default;

        void fromJson(const Json::Value &json) {
            if (json.isObject() && json.isMember("input")) {
                input_ = json["input"];
            } else if (json.isObject() && json.isMember("variables") && json["variables"].isObject()) {
                input_ = json["variables"];
            } else {
                input_ = json;
            }
        }

        [[nodiscard]] const Json::Value &getInput() const { return input_; }

      private:
        Json::Value input_{Json::nullValue};
    };

}  // namespace wssd_api::dto

#endif  // WSSDAPI_SAPOPLUGINDTO_H
