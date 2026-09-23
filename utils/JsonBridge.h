//
// JsonBridge.h — JsonCpp (Drogon) <-> nlohmann::json (Sapo) conversions.
//
// Header-only; needs no Sapo library symbols, only the vendored
// nlohmann/json header, so it is safe to use from unit tests.
//

#pragma once

#include <cstdint>
#include <string>

#include <json/json.h>

#include <nlohmann/json.hpp>

namespace wssd_api::utils {

/// Converts a JsonCpp value (Drogon's JSON type) to the nlohmann::json type
/// used across the Sapo engine boundary. Lossless for all JSON value kinds.
inline nlohmann::json toNlohmann(const Json::Value &value) {
    using nlohmann::json;
    switch (value.type()) {
        case Json::nullValue:
            return json(nullptr);
        case Json::booleanValue:
            return json(value.asBool());
        case Json::intValue:
            return json(value.asInt64());
        case Json::uintValue:
            return json(value.asUInt64());
        case Json::realValue:
            return json(value.asDouble());
        case Json::stringValue:
            return json(value.asString());
        case Json::arrayValue: {
            json out = json::array();
            for (const auto &item : value) {
                out.push_back(toNlohmann(item));
            }
            return out;
        }
        case Json::objectValue: {
            json out = json::object();
            for (auto it = value.begin(); it != value.end(); ++it) {
                out[it.name()] = toNlohmann(*it);
            }
            return out;
        }
    }
    return json(nullptr);
}

/// Converts an nlohmann::json value back to JsonCpp.
inline Json::Value toJsonCpp(const nlohmann::json &value) {
    using nlohmann::json;
    switch (value.type()) {
        case json::value_t::null:
            return Json::Value(Json::nullValue);
        case json::value_t::boolean:
            return Json::Value(value.get<bool>());
        case json::value_t::number_integer:
            return Json::Value(value.get<Json::Int64>());
        case json::value_t::number_unsigned:
            return Json::Value(value.get<Json::UInt64>());
        case json::value_t::number_float:
            return Json::Value(value.get<double>());
        case json::value_t::string:
            return Json::Value(value.get<std::string>());
        case json::value_t::array: {
            Json::Value out(Json::arrayValue);
            for (const auto &element : value) {
                out.append(toJsonCpp(element));
            }
            return out;
        }
        case json::value_t::object: {
            Json::Value out(Json::objectValue);
            for (auto it = value.begin(); it != value.end(); ++it) {
                out[it.key()] = toJsonCpp(it.value());
            }
            return out;
        }
        case json::value_t::binary: {
            const auto bytes = value.get_binary();
            std::string raw;
            raw.reserve(bytes.size());
            for (const auto byte : bytes) {
                raw.push_back(static_cast<char>(byte));
            }
            return Json::Value(raw);
        }
        case json::value_t::discarded:
            return Json::Value(Json::nullValue);
    }
    return Json::Value(Json::nullValue);
}

}  // namespace wssd_api::utils
