//
// Created by Emmanuel Addo-Odame on 21/09/2026.
//

#ifndef WSSDAPI_USSDSESSIONRESPONSE_H
#define WSSDAPI_USSDSESSIONRESPONSE_H
#include <string>
#include <json/json.h>

namespace wssd_api::dto {

    class NaloUssdSessionResponse {
    public:
        std::string USERID;
        std::string MSISDN;
        std::string SESSIONID;
        std::string USERDATA;
        bool MSGTYPE;
        std::string MSG;

        NaloUssdSessionResponse() : MSGTYPE(false) {}

        [[nodiscard]]
        Json::Value toJson() const {
            Json::Value json;
            json["USERID"] = USERID;
            json["MSISDN"] = MSISDN;
            json["SESSIONID"] = SESSIONID;
            json["USERDATA"] = USERDATA;
            json["MSGTYPE"] = MSGTYPE;
            json["MSG"] = MSG;
            return json;
        }
    };

    class HubtelUssdSessionResponse {
    public:
        std::string SessionId;
        std::string Type;
        std::string Message;
        std::string Label;
        std::string ClientState;
        std::string DataType;
        std::string FieldType;

        [[nodiscard]]
        Json::Value toJson() const {
            Json::Value json;
            json["SessionId"] = SessionId;
            json["Type"] = Type;
            json["Message"] = Message;
            json["Label"] = Label;
            json["ClientState"] = ClientState;
            json["DataType"] = DataType;
            json["FieldType"] = FieldType;
            return json;
        }
    };

}
#endif //WSSDAPI_USSDSESSIONRESPONSE_H
