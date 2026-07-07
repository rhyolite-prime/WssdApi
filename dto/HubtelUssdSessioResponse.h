//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//

#ifndef WSSDAPI_HUBTELUSSDSESSIORESPONSE_H
#define WSSDAPI_HUBTELUSSDSESSIORESPONSE_H
#include <string>
#include <json/json.h>

namespace wssd_api::dto {

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
#endif //WSSDAPI_HUBTELUSSDSESSIORESPONSE_H