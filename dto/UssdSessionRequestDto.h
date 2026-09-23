//
// Created by Emmanuel Addo-Odame on 21/09/2026.
//

#ifndef WSSDAPI_USSDSESSIONREQUESTDTO_H
#define WSSDAPI_USSDSESSIONREQUESTDTO_H

#include <string>

#include <json/json.h>

namespace wssd_api::dto {

    class HubtelUssdSessionRequestDto {

    public:

        HubtelUssdSessionRequestDto() = default;

        void fromJson(const Json::Value& json);

        // Getters
        [[nodiscard]] const std::string& getType() const { return type_; }
        [[nodiscard]] const std::string& getMobile() const { return mobile_; }
        [[nodiscard]] const std::string& getSessionId() const { return sessionId_; }
        [[nodiscard]] const std::string& getServiceCode() const { return serviceCode_; }
        [[nodiscard]] const std::string& getMessage() const { return message_; }
        [[nodiscard]] const std::string& getOperator() const { return operator_; }
        [[nodiscard]] int getSequence() const { return sequence_; }
        [[nodiscard]] const std::string& getClientState() const { return clientState_; }
        [[nodiscard]] const std::string& getPlatform() const { return platform_; }

        // Setters
        void setType(const std::string& value) { type_ = value; }
        void setMobile(const std::string& value) { mobile_ = value; }
        void setSessionId(const std::string& value) { sessionId_ = value; }
        void setServiceCode(const std::string& value) { serviceCode_ = value; }
        void setMessage(const std::string& value) { message_ = value; }
        void setOperator(const std::string& value) { operator_ = value; }
        void setSequence(const int value) { sequence_ = value; }
        void setClientState(const std::string& value) { clientState_ = value; }
        void setPlatform(const std::string& value) { platform_ = value; }
    private:

        std::string type_;
        std::string mobile_;
        std::string sessionId_;
        std::string serviceCode_;
        std::string message_;
        std::string operator_;
        int sequence_ {0};
        std::string clientState_;
        std::string platform_;
    };

    inline void HubtelUssdSessionRequestDto::fromJson(const Json::Value& json) {

        if (json.isMember("Type") && !json["Type"].isNull()) {
            type_ = json["Type"].asString();
        }

        if (json.isMember("Mobile") && !json["Mobile"].isNull()) {
            mobile_ = json["Mobile"].asString();
        }

        if (json.isMember("SessionId") && !json["SessionId"].isNull()) {
            sessionId_ = json["SessionId"].asString();
        }

        if (json.isMember("ServiceCode") && !json["ServiceCode"].isNull()) {
            serviceCode_ = json["ServiceCode"].asString();
        }

        if (json.isMember("Message") && !json["Message"].isNull()) {
            message_ = json["Message"].asString();
        }

        if (json.isMember("Operator") && !json["Operator"].isNull()) {
            operator_ = json["Operator"].asString();
        }

        if (json.isMember("Sequence") && !json["Sequence"].isNull()) {
            sequence_ = json["Sequence"].asInt();
        }

        if (json.isMember("ClientState") && !json["ClientState"].isNull()) {
            clientState_ = json["ClientState"].asString();
        }

        if (json.isMember("Platform") && !json["Platform"].isNull()) {
            platform_ = json["Platform"].asString();
        }



    }


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
#endif //WSSDAPI_USSDSESSIONREQUESTDTO_H
