//
//  Sapo Engine — value helpers over nlohmann::json.
//
//  The DSL is dynamically typed (JSON), so "typed evaluator" means: consistent
//  coercion, comparison and truthiness rules shared by every task instead of
//  each task improvising its own (gap E1 / P1-1 in the plan).
//
#pragma once

#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <type_traits>

#include <nlohmann/json.hpp>

namespace sapo::v {

    using json = nlohmann::json;

    /** Attempts numeric coercion; `false` when the value is not number-like. */
    inline bool toNumber(const json &value, double &out) {
        if (value.is_number()) {
            out = value.get<double>();
            return true;
        }
        if (value.is_boolean()) {
            out = value.get<bool>() ? 1.0 : 0.0;
            return true;
        }
        if (value.is_string()) {
            const std::string &s = value.get_ref<const std::string &>();
            if (s.empty()) return false;
            try {
                size_t consumed = 0;
                const double parsed = std::stod(s, &consumed);
                while (consumed < s.size() && std::isspace(static_cast<unsigned char>(s[consumed]))) consumed++;
                if (consumed == s.size()) {
                    out = parsed;
                    return true;
                }
            } catch (...) {
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] inline double numberOr(const json &value, double fallback = 0.0) {
        double out = 0.0;
        return toNumber(value, out) ? out : fallback;
    }

    /**
     * @brief Rounds a double to 12 significant decimal digits.
     *
     * DSL authors do payment math (`$amount * 1.15`); IEEE-754 noise would
     * otherwise leak into prompts and API payloads as "114.99999999999999".
     * Values outside the 1e-9..1e15 window are left untouched so ids and
     * high-precision data survive round-trips.
     */
    [[nodiscard]] inline double tidy(double value) {
        if (!std::isfinite(value) || value == 0.0) return value;
        const double magnitude = std::abs(value);
        if (magnitude >= 1e15 || magnitude < 1e-9) return value;
        const double exponent = std::floor(std::log10(magnitude));
        const double factor = std::pow(10.0, 12.0 - exponent);
        return std::round(value * factor) / factor;
    }

    /** Truthiness: JSON semantics with useful string rules for DSL input. */
    [[nodiscard]] inline bool truthy(const json &value) {
        if (value.is_null()) return false;
        if (value.is_boolean()) return value.get<bool>();
        if (value.is_number()) return value.get<double>() != 0.0;
        if (value.is_string()) {
            const std::string &s = value.get_ref<const std::string &>();
            if (s.empty()) return false;
            if (s == "false" || s == "0" || s == "null" || s == "no") return false;
            double numeric = 0.0;
            if (toNumber(value, numeric)) return numeric != 0.0;
            return true;
        }
        if (value.is_array() || value.is_object()) return !value.empty();
        return false;
    }

    /** Canonical stringification (objects/arrays keep JSON form). */
    [[nodiscard]] inline std::string str(const json &value) {
        if (value.is_string()) return value.get<std::string>();
        if (value.is_null()) return "";
        if (value.is_number_integer()) return std::to_string(value.get<long long>());
        if (value.is_number()) {
            const double d = value.get<double>();
            if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 1e15) {
                // Render 42.0 as "42" so interpolation into URLs/pins stays clean.
                return std::to_string(static_cast<long long>(d));
            }
            std::string s = value.dump();
            while (!s.empty() && (s.back() == '0')) s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
            return s;
        }
        return value.dump();
    }

    [[nodiscard]] inline bool isIntegerValue(const json &value) {
        double d = 0.0;
        if (!toNumber(value, d)) return false;
        return std::isfinite(d) && std::floor(d) == d;
    }

    /** Type name as surfaced by the `type()` SEL function. */
    [[nodiscard]] inline std::string typeName(const json &value) {
        if (value.is_null()) return "null";
        if (value.is_boolean()) return "boolean";
        if (value.is_number_integer()) return "integer";
        if (value.is_number()) return "number";
        if (value.is_string()) return "string";
        if (value.is_array()) return "array";
        if (value.is_object()) return "object";
        return "unknown";
    }

    /**
     * @brief Structural equality with numeric cross-type tolerance so a value
     *        that arrived as "5" (HTTP header) still matches the number 5.
     */
    [[nodiscard]] inline bool equals(const json &a, const json &b) {
        if (a.type() == b.type() && !a.is_number()) return a == b;
        if ((a.is_number() || a.is_boolean() || a.is_string()) &&
            (b.is_number() || b.is_boolean() || b.is_string())) {
            double da = 0.0, db = 0.0;
            const bool na = toNumber(a, da);
            const bool nb = toNumber(b, db);
            if (na && nb) return da == db;
            // Non-numeric string-ish comparison.
            const std::string sa = a.is_string() ? a.get<std::string>() : v::str(a);
            const std::string sb = b.is_string() ? b.get<std::string>() : v::str(b);
            if (a.is_boolean() || b.is_boolean()) {
                return truthy(a) == truthy(b);
            }
            return sa == sb;
        }
        return a == b;
    }

    /** Three-way ordering; returns <0, 0 or >0. `std::nullopt` when not comparable. */
    [[nodiscard]] inline std::optional<int> compare(const json &a, const json &b) {
        double da = 0.0, db = 0.0;
        if (toNumber(a, da) && toNumber(b, db)) {
            if (da < db) return -1;
            if (da > db) return 1;
            return 0;
        }
        if (a.is_string() && b.is_string()) {
            const int c = a.get_ref<const std::string &>().compare(b.get_ref<const std::string &>());
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
        if (a.is_boolean() && b.is_boolean()) {
            const int av = a.get<bool>() ? 1 : 0;
            const int bv = b.get<bool>() ? 1 : 0;
            return av - bv;
        }
        return std::nullopt;
    }

} // namespace sapo::v
