//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//

#ifndef WSSDAPI_WSSDREGISTRYDTO_H
#define WSSDAPI_WSSDREGISTRYDTO_H
#include <json/json.h>
#include <string>

namespace wssd_api::dto {

    class WssdRegistryDto {

    public:

        WssdRegistryDto() = default;

        void fromJson(const Json::Value& json);

        // Getters
        [[nodiscard]] const std::string& getWssdShortName() const { return wssdShortName_; }
        [[nodiscard]] const std::string& getUssdCode() const { return ussdCode_; }
        [[nodiscard]] const std::string& getDisplayTitle() const { return displayTitle_; }
        [[nodiscard]] const std::string& getDescription() const { return description_; }
        [[nodiscard]] const std::string& getActivatedOn() const { return activatedOn_; }
        [[nodiscard]] const std::string& getLogoUrl() const { return logoUrl_; }
        [[nodiscard]] bool getIsWssdActive() const { return isWssdActive_; }
        [[nodiscard]] bool getIsUssdActive() const { return isUssdActive_; }
        [[nodiscard]] bool getUseAsSecondaryService() const { return useAsSecondaryService_; }
        [[nodiscard]] int32_t getTenantId() const { return tenantId_; }
        [[nodiscard]] int32_t getRank() const { return rank_; }
        [[nodiscard]] const std::string& getMerchantIdentifier() const { return merchantIdentifier_; }
        [[nodiscard]] const std::string& getCategory() const { return category_; }
        [[nodiscard]] const std::string& getBusinessName() const { return businessName_; }

        // Setters
        void setWssdShortName(const std::string& value) { wssdShortName_ = value; }
        void setUssdCode(const std::string& value) { ussdCode_ = value; }
        void setDisplayTitle(const std::string& value) { displayTitle_ = value; }
        void setDescription(const std::string& value) { description_ = value; }
        void setActivatedOn(const std::string& value) { activatedOn_ = value; }
        void setLogoUrl(const std::string& value) { logoUrl_ = value; }
        void setIsWssdActive(bool value) { isWssdActive_ = value; }
        void setIsUssdActive(bool value) { isUssdActive_ = value; }
        void setUseAsSecondaryService(bool value) { useAsSecondaryService_ = value; }
        void setTenantId(int32_t value) { tenantId_ = value; }
        void setRank(int32_t value) { rank_ = value; }
        void setMerchantIdentifier(const std::string& value) { merchantIdentifier_ = value; }
        void setCategory(const std::string& value) { category_ = value; }
        void setBusinessName(const std::string& value) { businessName_ = value; }

    private:
        std::string wssdShortName_;
        std::string ussdCode_;
        std::string displayTitle_;
        std::string description_;
        std::string activatedOn_;
        std::string logoUrl_;
        bool isWssdActive_{false};
        bool isUssdActive_{false};
        bool useAsSecondaryService_{false};
        int32_t tenantId_{0};
        int32_t rank_{0};
        std::string merchantIdentifier_;
        std::string category_;
        std::string businessName_;
    };

    inline void WssdRegistryDto::fromJson(const Json::Value& json) {

        if (json.isMember("WssdShortName") && !json["WssdShortName"].isNull()) {
            wssdShortName_ = json["WssdShortName"].asString();
        }
        if (json.isMember("UssdCode") && !json["UssdCode"].isNull()) {
            ussdCode_ = json["UssdCode"].asString();
        }
        if (json.isMember("DisplayTitle") && !json["DisplayTitle"].isNull()) {
            displayTitle_ = json["DisplayTitle"].asString();
        }
        if (json.isMember("Description") && !json["Description"].isNull()) {
            description_ = json["Description"].asString();
        }
        if (json.isMember("ActivatedOn") && !json["ActivatedOn"].isNull()) {
            activatedOn_ = json["ActivatedOn"].asString();
        }
        if (json.isMember("LogoUrl") && !json["LogoUrl"].isNull()) {
            logoUrl_ = json["LogoUrl"].asString();
        }
        if (json.isMember("IsWssdActive") && !json["IsWssdActive"].isNull()) {
            isWssdActive_ = json["IsWssdActive"].asBool();
        }
        if (json.isMember("IsUssdActive") && !json["IsUssdActive"].isNull()) {
            isUssdActive_ = json["IsUssdActive"].asBool();
        }
        if (json.isMember("UseAsSecondaryService") && !json["UseAsSecondaryService"].isNull()) {
            useAsSecondaryService_ = json["UseAsSecondaryService"].asBool();
        }
        if (json.isMember("TenantId") && !json["TenantId"].isNull()) {
            tenantId_ = json["TenantId"].asInt();
        }
        if (json.isMember("Rank") && !json["Rank"].isNull()) {
            rank_ = json["Rank"].asInt();
        }
        if (json.isMember("MerchantIdentifier") && !json["MerchantIdentifier"].isNull()) {
            merchantIdentifier_ = json["MerchantIdentifier"].asString();
        }
        if (json.isMember("Category") && !json["Category"].isNull()) {
            category_ = json["Category"].asString();
        }
        if (json.isMember("BusinessName") && !json["BusinessName"].isNull()) {
            businessName_ = json["BusinessName"].asString();
        }
    }


}
#endif //WSSDAPI_WSSDREGISTRYDTO_H