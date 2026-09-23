//
// Created by Emmanuel Addo-Odame on 23/09/2026.
//

#ifndef WSSDAPI_DROGONHTTPTRANSPORT_H
#define WSSDAPI_DROGONHTTPTRANSPORT_H
// HostDrogonTransport.hpp
#pragma once
#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include <vendor/sapo/include/sapo/http/IHttpTransport.hpp> // or "http/IHttpTransport.hpp"

class HostDrogonTransport final : public sapo::http::IHttpTransport {
public:
    [[nodiscard]] sapo::http::Response send(const sapo::http::Request &request) override {
        sapo::http::Response response;

        // 1. Get or create a pooled Drogon HttpClient for the target host
        auto client = drogon::HttpClient::newHttpClient(request.url);

        // 2. Prepare Drogon HttpRequest
        auto d_req = drogon::HttpRequest::newHttpRequest();
        d_req->setMethod(request.method == "POST" ? drogon::Post : drogon::Get);

        // Append query parameters if present
        if (!request.query.empty() && request.query.is_object()) {
            std::string query_str;
            for (auto it = request.query.begin(); it != request.query.end(); ++it) {
                query_str += (query_str.empty() ? "?" : "&") + it.key() + "=" +
                             (it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
            }
            d_req->setPath(d_req->getPath() + query_str);
        }

        // Set Headers
        if (request.headers.is_object()) {
            for (auto it = request.headers.begin(); it != request.headers.end(); ++it) {
                d_req->addHeader(it.key(), it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
            }
        }

        // Bearer Token
        if (request.bearer_token.has_value()) {
            d_req->addHeader("Authorization", "Bearer " + *request.bearer_token);
        }

        // Body
        if (request.body.has_value()) {
            if (request.body->is_string()) {
                d_req->setBody(request.body->get<std::string>());
            } else {
                d_req->setBody(request.body->dump());
                d_req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            }
        }

        // 3. Send synchronously on Sapo worker thread
        // (Sapo tasks execute on Sapo worker threads, so synchronous blocking is safe here)
        const double timeout_sec = static_cast<double>(request.timeout_ms) / 1000.0;
        const auto [result, drogon_resp] = client->sendRequest(d_req, timeout_sec);

        if (result == drogon::ReqResult::Ok && drogon_resp) {
            response.status_code = static_cast<int>(drogon_resp->getStatusCode());
            response.body = std::string(drogon_resp->getBody());
            for (const auto &[k, v] : drogon_resp->getHeaders()) {
                response.headers[k] = v;
            }
        } else {
            response.transport_error = "Drogon HTTP client failure (code: " +
                                       std::to_string(static_cast<int>(result)) + ")";
        }

        return response;
    }

    [[nodiscard]] std::string name() const override { return "drogon_host_transport"; }
};
#endif //WSSDAPI_DROGONHTTPTRANSPORT_H
