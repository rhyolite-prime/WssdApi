//
//  Sapo Engine — dotted/bracketed path access over JSON documents.
//
//  Used by the runtime context (flat `a.b` keys stay addressable for backwards
//  compatibility with the HTTP output extractor) and by the expression
//  language's path operator (`$result.items[0].id`).
//
#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sapo::util {

    struct PathStep {
        enum class Kind { Field, Index, Wildcard };
        Kind kind{Kind::Field};
        std::string field;
        int index{0};

        [[nodiscard]] std::string toString() const;
    };

    /**
     * @brief Parses `a.b[0].c`, `$.a.b`, `items["two words"]`.
     * @return nullopt when the path is malformed (surfaced as a parse error).
     */
    [[nodiscard]] std::optional<std::vector<PathStep>> parsePath(std::string_view path);

    /** Walks `root`; returns nullptr when any step is missing. */
    [[nodiscard]] const nlohmann::json *walk(const nlohmann::json &root, const std::vector<PathStep> &steps);

    /** Convenience: returns a copy of the value at `path`, or nullopt. */
    [[nodiscard]] std::optional<nlohmann::json> getPath(const nlohmann::json &root, std::string_view path);

    /** Creates intermediate objects/arrays as needed. */
    bool setPath(nlohmann::json &root, std::string_view path, const nlohmann::json &value);

    [[nodiscard]] std::string joinPath(std::string_view base, std::string_view suffix);

} // namespace sapo::util
