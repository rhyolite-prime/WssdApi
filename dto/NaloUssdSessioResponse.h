//
// Created by Emmanuel Addo-Odame on 05/07/2026.
//

#ifndef WSSDAPI_NALOUSSDSESSIORESPONSE_H
#define WSSDAPI_NALOUSSDSESSIORESPONSE_H
#include <string>
#include <json/json.h>

namespace wssd_api::dto {

    class NaloUssdSessionResponse {
    public:
        std::string USERID;
        std::string MSISDN;
        std::string USERDATA;
        bool MSGTYPE;
        std::string MSG;

        NaloUssdSessionResponse() : MSGTYPE(false) {}

        [[nodiscard]]
        Json::Value toJson() const {
            Json::Value json;
            json["USERID"] = USERID;
            json["MSISDN"] = MSISDN;
            json["USERDATA"] = USERDATA;
            json["MSGTYPE"] = MSGTYPE;
            json["MSG"] = MSG;
            return json;
        }
    };

}
#endif //WSSDAPI_NALOUSSDSESSIORESPONSE_H