//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//

#ifndef WSSDAPI_PLUGINDTO_H
#define WSSDAPI_PLUGINDTO_H
#include <json/json.h>
#include <string>

namespace wssd_api::dto {

    class PluginDto {
      public:
        PluginDto() = default;

        void fromJson(const Json::Value &json) {
            if (json.isMember("name") && !json["name"].isNull()) name_ = json["name"].asString();
            if (json.isMember("description") && !json["description"].isNull()) description_ = json["description"].asString();
            if (json.isMember("category") && !json["category"].isNull()) category_ = json["category"].asString();
            if (json.isMember("defaultConfig") && !json["defaultConfig"].isNull()) defaultConfig_ = json["defaultConfig"].asString();
            if (json.isMember("spec") && !json["spec"].isNull()) spec_ = json["spec"].asString();
            if (json.isMember("isActive") && !json["isActive"].isNull()) isActive_ = json["isActive"].asBool();
            if (json.isMember("isBuiltIn") && !json["isBuiltIn"].isNull()) isBuiltIn_ = json["isBuiltIn"].asBool();
            if (json.isMember("isPreinstalled") && !json["isPreinstalled"].isNull()) isPreinstalled_ = json["isPreinstalled"].asBool();
            if (json.isMember("businessId") && !json["businessId"].isNull()) businessId_ = json["businessId"].asString();
            if (json.isMember("version") && !json["version"].isNull()) version_ = json["version"].asString();
        }

        [[nodiscard]] const std::string &getName() const { return name_; }
        [[nodiscard]] const std::string &getDescription() const { return description_; }
        [[nodiscard]] const std::string &getCategory() const { return category_; }
        [[nodiscard]] const std::string &getDefaultConfig() const { return defaultConfig_; }
        [[nodiscard]] const std::string &getSpec() const { return spec_; }
        [[nodiscard]] bool getIsActive() const { return isActive_; }
        [[nodiscard]] bool getIsBuiltIn() const { return isBuiltIn_; }
        [[nodiscard]] bool getIsPreinstalled() const { return isPreinstalled_; }
        [[nodiscard]] const std::string &getBusinessId() const { return businessId_; }
        [[nodiscard]] const std::string &getVersion() const { return version_; }

        void setName(const std::string &name) { name_ = name; }
        void setDescription(const std::string &desc) { description_ = desc; }
        void setCategory(const std::string &cat) { category_ = cat; }
        void setDefaultConfig(const std::string &conf) { defaultConfig_ = conf; }
        void setSpec(const std::string &s) { spec_ = s; }
        void setIsActive(bool active) { isActive_ = active; }
        void setIsBuiltIn(bool builtin) { isBuiltIn_ = builtin; }
        void setIsPreinstalled(bool preinstalled) { isPreinstalled_ = preinstalled; }
        void setBusinessId(const std::string &bid) { businessId_ = bid; }
        void setVersion(const std::string &ver) { version_ = ver; }

      private:
        std::string name_;
        std::string description_;
        std::string category_;
        std::string defaultConfig_;
        std::string spec_;
        bool isActive_{false};
        bool isBuiltIn_{false};
        bool isPreinstalled_{false};
        std::string businessId_;
        std::string version_;
    };

}
#endif //WSSDAPI_PLUGINDTO_H