//
// DrogonHttpTransport.h — Sapo IHttpTransport backed by Drogon's HttpClient.
//
// Blueprint `command` nodes (http.get/http.post/...) build a sapo::http::Request
// and the engine hands it here. This adapter:
//
//   * parses the absolute URL into a Drogon client base (scheme://host:port)
//     plus path, merging sapo's structured `query` object into the query string;
//   * maps every HTTP method, headers, bearer/basic auth and string/JSON bodies;
//   * never blocks a Drogon IO thread: Sapo invokes transports on its own
//     worker threads, and the async Drogon call below is joined with a
//     promise/future pair bounded by the request timeout.
//
// Header-only and dependency-light on purpose: the only Sapo surface used is
// the transport seam, so this file compiles against either the vendored
// headers or a FetchContent checkout.
//

#pragma once

#include <chrono>
#include <cctype>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>

#include "http/IHttpTransport.hpp"

class HostDrogonTransport final : public sapo::http::IHttpTransport {
  public:
    [[nodiscard]] sapo::http::Response send(const sapo::http::Request &request) override {
        const auto started = std::chrono::steady_clock::now();
        sapo::http::Response response;

        drogon::HttpMethod initialMethod = drogon::Get;
        if (!toDrogonMethod(request.method, initialMethod)) {
            response.transport_error = "drogon transport does not support HTTP method: " + request.method;
                return finish(response, started);
        }
        // One line per outbound call (INFO, not DEBUG: blueprints typically
        // swallow HTTP failures into on_error branches, so without this the
        // service log shows nothing at all). The query string is stripped —
        // it is the usual place for tokens and subscriber ids.
        LOG_INFO << "[sapo] http " << request.method << " "
                 << request.url.substr(0, request.url.find('?'));
        const long timeoutMs = request.timeout_ms > 0 ? request.timeout_ms : 10000;
        const double timeoutSec = static_cast<double>(timeoutMs) / 1000.0;

        // The engine sets follow_redirects by default and blueprints rely on
        // it (http -> https upgrades are the common case), but Drogon's client
        // never follows redirects itself — so the loop below does, bounded to
        // 5 hops. Non-redirect responses take exactly one hop, as before.
        std::string url = request.url;
        drogon::HttpMethod method = initialMethod;
        bool sendBody = request.body.has_value();
        for (int hop = 0; hop < 5; ++hop) {
            std::string base;
            std::string path;
            if (!splitUrl(url, base, path)) {
                response.transport_error = "drogon transport requires an absolute http(s) URL, got: " + url;
                return finish(response, started);
            }

            try {
                auto client = drogon::HttpClient::newHttpClient(base);
                auto drogonRequest = drogon::HttpRequest::newHttpRequest();
                drogonRequest->setMethod(method);
                drogonRequest->setPath(path + querySuffix(path, hop == 0 ? request.query
                                                                         : nlohmann::json::object()));

                if (request.headers.is_object()) {
                    for (auto it = request.headers.begin(); it != request.headers.end(); ++it) {
                        if (equalsIgnoreCase(it.key(), "content-type")) {
                            drogonRequest->setContentTypeString(headerValue(it.value()));
                        } else {
                            drogonRequest->addHeader(it.key(), headerValue(it.value()));
                        }
                    }
                }

                if (request.basic_auth_user.has_value()) {
                    const std::string password = request.basic_auth_password.value_or("");
                    drogonRequest->addHeader(
                        "Authorization", "Basic " + base64Encode(*request.basic_auth_user + ":" + password));
                } else if (request.bearer_token.has_value()) {
                    drogonRequest->addHeader("Authorization", "Bearer " + *request.bearer_token);
                }

                if (sendBody && request.body.has_value()) {
                    if (request.body->is_string()) {
                        drogonRequest->setBody(request.body->get<std::string>());
                    } else {
                        if (!hasHeader(request.headers, "content-type")) {
                            drogonRequest->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                        }
                        drogonRequest->setBody(request.body->dump());
                    }
                }

                // Join the async client with a shared promise so a late gateway
                // reply after our wait deadline cannot touch a dead stack frame.
                auto promise =
                    std::make_shared<std::promise<std::pair<drogon::ReqResult, drogon::HttpResponsePtr>>>();
                auto future = promise->get_future();
                client->sendRequest(
                    drogonRequest,
                    [promise](drogon::ReqResult result, const drogon::HttpResponsePtr &resp) {
                        try {
                            promise->set_value({result, resp});
                        } catch (...) {
                            // The waiter already left (timeout path); nothing to do.
                        }
                    },
                    timeoutSec);

                // Drogon's own timeout fires first; this deadline only guards the
                // callback itself from never arriving.
                if (future.wait_for(std::chrono::milliseconds(timeoutMs + 2000)) != std::future_status::ready) {
                    response.transport_error = "drogon HTTP request timed out: " + url;
                return finish(response, started);
                }

                const auto [result, drogonResponse] = future.get();
                if (result == drogon::ReqResult::Ok && drogonResponse) {
                    const int statusCode = static_cast<int>(drogonResponse->getStatusCode());
                    response.status_code = statusCode;
                    response.body = std::string(drogonResponse->getBody());
                    std::string location;
                    for (const auto &[name, value] : drogonResponse->getHeaders()) {
                        response.headers[name] = value;
                        if (location.empty() && equalsIgnoreCase(name, "location")) {
                            location = value;
                        }
                    }
                    if (isRedirectStatus(statusCode) && !location.empty()) {
                        if (!request.follow_redirects || hop + 1 >= 5) {
                            // Redirects off, or hop budget spent: surface the
                            // 3xx itself so the failure stays diagnosable.
                return finish(response, started);
                        }
                        url = resolveRedirectUrl(base, path, location);
                        if (statusCode == 303 ||
                            ((statusCode == 301 || statusCode == 302) && method == drogon::Post)) {
                            method = drogon::Get;
                            sendBody = false;
                        }
                        response = sapo::http::Response{};
                        continue;
                    }
                return finish(response, started);
                }
                response.transport_error =
                    "drogon HTTP failure (" + reqResultName(result) + "): " + url;
                return finish(response, started);
            } catch (const std::exception &e) {
                response.transport_error = std::string("drogon transport exception: ") + e.what();
                return finish(response, started);
            } catch (...) {
                response.transport_error = "drogon transport threw an unknown exception: " + url;
                return finish(response, started);
            }
        }
        // Unreachable: every hop above returns or continues.
                return finish(response, started);
    }

    [[nodiscard]] std::string name() const override {
        return "drogon_host_transport";
    }

  private:
    static sapo::http::Response finish(sapo::http::Response response,
                                       std::chrono::steady_clock::time_point started) {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        response.elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();
        // Every exit funnels through here, so this one line captures failures
        // even when the blueprint swallows them into an on_error branch.
        if (!response.transport_error.empty()) {
            LOG_WARN << "[sapo] http call failed: " << response.transport_error;
        }
        return response;
    }

    static bool isRedirectStatus(int status) {
        return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    }

    /// Resolves a redirect `Location` against the request that produced it:
    /// absolute URLs pass through, root-relative paths keep the client base,
    /// anything else resolves against the request directory.
    static std::string resolveRedirectUrl(const std::string &base,
                                          const std::string &path,
                                          const std::string &location) {
        if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
            return location;
        }
        if (!location.empty() && location.front() == '/') {
            return base + location;
        }
        std::string directory = path;
        const auto queryBegin = directory.find('?');
        if (queryBegin != std::string::npos) {
            directory.resize(queryBegin);
        }
        const auto lastSlash = directory.rfind('/');
        directory = (lastSlash == std::string::npos) ? "/" : directory.substr(0, lastSlash + 1);
        return base + directory + location;
    }

    /// Splits "https://host:port/path?query" into a Drogon client base
    /// ("https://host:port") and a path ("/path?query", default "/").
    static bool splitUrl(const std::string &url, std::string &base, std::string &path) {
        const auto schemeEnd = url.find("://");
        if (schemeEnd == std::string::npos) {
            return false;
        }
        const std::string scheme = url.substr(0, schemeEnd);
        if (scheme != "http" && scheme != "https") {
            return false;
        }
        const auto pathBegin = url.find('/', schemeEnd + 3);
        if (pathBegin == std::string::npos) {
            base = url;
            path = "/";
            return true;
        }
        base = url.substr(0, pathBegin);
        path = url.substr(pathBegin);
        if (path.empty()) {
            path = "/";
        }
        return true;
    }

    /// Encodes sapo's structured query object as "?k=v&..." (or "&k=v..."
    /// when the path already carries a query string).
    static std::string querySuffix(const std::string &path, const nlohmann::json &query) {
        if (!query.is_object() || query.empty()) {
            return "";
        }
        std::string suffix = (path.find('?') == std::string::npos) ? "?" : "&";
        bool first = true;
        for (auto it = query.begin(); it != query.end(); ++it) {
            if (!first) {
                suffix += "&";
            }
            first = false;
            suffix += percentEncode(it.key());
            suffix += "=";
            suffix += percentEncode(queryValue(it.value()));
        }
        return suffix;
    }

    static std::string queryValue(const nlohmann::json &value) {
        if (value.is_string()) {
            return value.get<std::string>();
        }
        if (value.is_boolean()) {
            return value.get<bool>() ? "true" : "false";
        }
        if (value.is_null()) {
            return "";
        }
        return value.dump();
    }

    static std::string headerValue(const nlohmann::json &value) {
        if (value.is_string()) {
            return value.get<std::string>();
        }
        return value.dump();
    }

    static bool hasHeader(const nlohmann::json &headers, const std::string &name) {
        if (!headers.is_object()) {
            return false;
        }
        for (auto it = headers.begin(); it != headers.end(); ++it) {
            if (equalsIgnoreCase(it.key(), name)) {
                return true;
            }
        }
        return false;
    }

    static bool toDrogonMethod(const std::string &method, drogon::HttpMethod &out) {
        if (equalsIgnoreCase(method, "GET")) {
            out = drogon::Get;
        } else if (equalsIgnoreCase(method, "POST")) {
            out = drogon::Post;
        } else if (equalsIgnoreCase(method, "PUT")) {
            out = drogon::Put;
        } else if (equalsIgnoreCase(method, "PATCH")) {
            out = drogon::Patch;
        } else if (equalsIgnoreCase(method, "DELETE")) {
            out = drogon::Delete;
        } else if (equalsIgnoreCase(method, "HEAD")) {
            out = drogon::Head;
        } else if (equalsIgnoreCase(method, "OPTIONS")) {
            out = drogon::Options;
        } else {
            return false;
        }
        return true;
    }

    static std::string reqResultName(drogon::ReqResult result) {
        switch (result) {
            case drogon::ReqResult::Ok:
                return "ok";
            case drogon::ReqResult::BadResponse:
                return "bad_response";
            case drogon::ReqResult::NetworkFailure:
                return "network_failure";
            case drogon::ReqResult::BadServerAddress:
                return "bad_server_address";
            case drogon::ReqResult::Timeout:
                return "timeout";
            default:
                return "code_" + std::to_string(static_cast<int>(result));
        }
    }

    static bool equalsIgnoreCase(const std::string &a, const std::string &b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }

    static std::string percentEncode(const std::string &text) {
        static const char *hex = "0123456789ABCDEF";
        std::string out;
        out.reserve(text.size());
        for (const unsigned char c : text) {
            const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
            if (unreserved) {
                out.push_back(static_cast<char>(c));
            } else {
                out.push_back('%');
                out.push_back(hex[(c >> 4) & 0xF]);
                out.push_back(hex[c & 0xF]);
            }
        }
        return out;
    }

    static std::string base64Encode(const std::string &text) {
        static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((text.size() + 2) / 3) * 4);
        for (std::size_t i = 0; i < text.size(); i += 3) {
            const auto b0 = static_cast<unsigned char>(text[i]);
            const auto b1 = static_cast<unsigned char>(i + 1 < text.size() ? text[i + 1] : 0);
            const auto b2 = static_cast<unsigned char>(i + 2 < text.size() ? text[i + 2] : 0);
            const unsigned triple = (static_cast<unsigned>(b0) << 16) |
                                    (static_cast<unsigned>(b1) << 8) | static_cast<unsigned>(b2);
            out.push_back(alphabet[(triple >> 18) & 0x3F]);
            out.push_back(alphabet[(triple >> 12) & 0x3F]);
            out.push_back(i + 1 < text.size() ? alphabet[(triple >> 6) & 0x3F] : '=');
            out.push_back(i + 2 < text.size() ? alphabet[triple & 0x3F] : '=');
        }
        return out;
    }
};
