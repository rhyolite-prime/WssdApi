//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//

#ifndef WSSDAPI_NALOUSSDSESSIONREQUESTDTO_H
#define WSSDAPI_NALOUSSDSESSIONREQUESTDTO_H
#include <json/json.h>
namespace wssd_api::dto {

    class NaloUssdSessionRequestDto {

    public:
        NaloUssdSessionRequestDto() = default;

        void fromJson(const Json::Value& json);

        // Getters
        [[nodiscard]] const std::string& getUserId() const { return userId_; }
        [[nodiscard]] const std::string& getMsisdn() const { return msisdn_; }
        [[nodiscard]] const std::string& getUserData() const { return userData_; }
        [[nodiscard]] bool getMsgType() const { return msgType_; }
        [[nodiscard]] const std::string& getNetwork() const { return network_; }
        [[nodiscard]] const std::string& getSession() const { return sessionId_; }

        // Setters
        void setUserId(const std::string& value) { userId_ = value; }
        void setMsisdn(const std::string& value) { msisdn_ = value; }
        void setUserData(const std::string& value) { userData_ = value; }
        void setMsgType(const bool value) { msgType_ = value; }
        void setNetwork(const std::string& value) { network_ = value; }
        void setSessionId(const std::string& value) { sessionId_ = value; }

    private:

        std::string userId_;
        std::string msisdn_;
        std::string userData_;
        bool msgType_ {false};
        std::string network_;
        std::string sessionId_;


    };

    inline void NaloUssdSessionRequestDto::fromJson(const Json::Value& json) {

        if (json.isMember("USERID") && !json["USERID"].isNull()) {
            userId_ = json["USERID"].asString();
        }

        if (json.isMember("MSISDN") && !json["MSISDN"].isNull()) {
            msisdn_ = json["MSISDN"].asString();
        }

        if (json.isMember("USERDATA") && !json["USERDATA"].isNull()) {
            userData_ = json["USERDATA"].asString();
        }

        if (json.isMember("MSGTYPE") && !json["MSGTYPE"].isNull()) {
            msgType_ = json["MSGTYPE"].asBool();
        }

        if (json.isMember("NETWORK") && !json["NETWORK"].isNull()) {
            network_ = json["NETWORK"].asString();
        }

        if (json.isMember("SESSIONID") && !json["SESSIONID"].isNull()) {
            sessionId_ = json["SESSIONID"].asString();
        }



    }
}
#endif //WSSDAPI_NALOUSSDSESSIONREQUESTDTO_H