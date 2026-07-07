//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//

#ifndef WSSDAPI_USSDDIRECTORYDTO_H
#define WSSDAPI_USSDDIRECTORYDTO_H

#include <json/json.h>

namespace wssd_api::dto {

    class UssdDirectoryDto {

  public:
    UssdDirectoryDto() = default;

    void fromJson(const Json::Value &json);

    // Getters
    [[nodiscard]] const std::string &getTradeName() const { return trade_name_; }
    [[nodiscard]] const std::string &getUssdCode() const { return ussdCode_; }
    [[nodiscard]] const std::string &getBusinessName() const { return businessName_; }

    [[nodiscard]] const std::string &getKeywords() const { return keywords_; }
    [[nodiscard]] const std::string &getDescription() const {  return description_; }
    [[nodiscard]] const std::string &getCountryCode() const {  return countryCode_; }
    [[nodiscard]] const std::string &getCategory() const { return category_; }
    [[nodiscard]] bool getIsDedicated() const { return isDedicated_; }
    [[nodiscard]] bool getIsWssdLinked() const { return isWssdLinked_; }
    [[nodiscard]] int getRating() const { return rating_; }

    // Setters
    void setName(const std::string &tradeName) { trade_name_ = tradeName; }
    void setUssdCode(const std::string &ussdCode) { ussdCode_ = ussdCode; }
    void setBusinessName(const std::string &businessName) { businessName_ = businessName; }
    void setKeywords(const std::string &keywords) { keywords_ = keywords; }
    void setDescription(const std::string &description) { description_ = description; }
    void setCountryCode(const std::string &countryCode) { countryCode_ = countryCode; }
    void setCategory(const std::string &category) { category_ = category; }
    void setIsDedicated(bool isDedicated) { isDedicated_ = isDedicated; }
    void setIsWssdLinked(bool isWssdLinked) { isWssdLinked_ = isWssdLinked; }
    void setRating(int rating) { rating_ = rating; }

  private:
    std::string trade_name_;
    std::string ussdCode_;
    std::string businessName_;
    std::string keywords_;
    std::string description_;
    std::string countryCode_;
    std::string category_;
    bool isDedicated_ {false};
    bool isWssdLinked_ {false};
    int rating_ {0};
  };

  inline void UssdDirectoryDto::fromJson(const Json::Value &json) {

    if (json.isMember("tradeName") && !json["tradeName"].isNull()) {
      trade_name_ = json["tradeName"].asString();
    }

    if (json.isMember("ussdCode") && !json["ussdCode"].isNull()) {
      ussdCode_ = json["ussdCode"].asString();
    }

    if (json.isMember("businessName") && !json["businessName"].isNull()) {
      businessName_ = json["businessName"].asString();
    }

    if (json.isMember("keywords") && !json["keywords"].isNull()) {
      keywords_ = json["keywords"].asString();
    }

    if (json.isMember("description") && !json["description"].isNull()) {
      description_ = json["description"].asString();
    }

    if (json.isMember("countryCode") && !json["countryCode"].isNull()) {
      countryCode_ = json["countryCode"].asString();
    }

    if (json.isMember("category") && !json["category"].isNull()) {
      category_ = json["category"].asString();
    }

    if (json.isMember("isDedicated") && !json["isDedicated"].isNull()) {
      isDedicated_ = json["isDedicated"].asBool();
    }

    if (json.isMember("isWssdLinked") && !json["isWssdLinked"].isNull()) {
      isWssdLinked_ = json["isWssdLinked"].asBool();
    }

    if (json.isMember("rating") && !json["rating"].isNull()) {
      rating_ = json["rating"].asInt();
    }
  };


}
#endif //WSSDAPI_USSDDIRECTORYDTO_H