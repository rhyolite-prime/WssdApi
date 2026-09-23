//
// UssdWorkflowResolver.cc
//

#include "UssdWorkflowResolver.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include <drogon/drogon.h>
#include <drogon/orm/CoroMapper.h>
#include <drogon/orm/Criteria.h>
#include <nlohmann/json.hpp>

#include "WssdRegistry.h"

namespace wssd_api::sapo_host {
namespace {

std::string trimCopy(const std::string &text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

/// Candidate spellings for one gateway key: the raw value, hash/star
/// stripped variants, and — for dial strings with parameters such as
/// "*711*23#" — the base service code ("*711#", "711").
std::vector<std::string> expandCandidates(const std::string &raw) {
    std::vector<std::string> out;
    const std::string key = trimCopy(raw);
    if (key.empty()) {
        return out;
    }
    auto push = [&](std::string value) {
        value = trimCopy(value);
        if (!value.empty() && std::find(out.begin(), out.end(), value) == out.end()) {
            out.push_back(std::move(value));
        }
    };
    push(key);
    if (key.back() == '#') {
        push(key.substr(0, key.size() - 1));
    }
    std::string stripped = key;
    if (!stripped.empty() && stripped.front() == '*') {
        stripped.erase(stripped.begin());
    }
    if (!stripped.empty() && stripped.back() == '#') {
        stripped.pop_back();
    }
    push(stripped);
    if (key.size() > 1 && key.front() == '*') {
        const auto star = key.find('*', 1);
        if (star != std::string::npos) {
            push(key.substr(0, star) + "#");
            push(key.substr(1, star - 1));
        }
    }
    return out;
}

std::string sanitizeWorkflowId(const std::string &key) {
    std::string id = "wssd:";
    for (const char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '*' ||
                        c == '#';
        id.push_back(ok ? c : '_');
    }
    return id;
}

/// Maps a gateway key to a safe file stem ("*123#" -> "123"). Never yields
/// a path separator, so directory traversal through the dial string is
/// impossible.
std::string sanitizeFileStem(const std::string &key) {
    std::string stem;
    for (const char c : key) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
            c == '-') {
            stem.push_back(c);
        } else if (c == '*' || c == '#') {
            continue;  // "*123#" and "123" share one file
        } else if (c == '.' || c == '/' || c == '\\') {
            return "";
        } else {
            stem.push_back('_');
        }
    }
    if (stem.empty() || stem == "." || stem == "..") {
        return "";
    }
    return stem;
}

}  // namespace

drogon::Task<std::optional<ResolvedWorkflow>> UssdWorkflowResolver::resolve(
    const std::string &serviceKey,
    const std::string &dialCode) {
    std::vector<std::string> ordered;
    for (const auto &key : expandCandidates(dialCode)) {
        ordered.push_back(key);
    }
    for (const auto &key : expandCandidates(serviceKey)) {
        if (std::find(ordered.begin(), ordered.end(), key) == ordered.end()) {
            ordered.push_back(key);
        }
    }

    for (const auto &key : ordered) {
        if (auto hit = co_await lookupDatabase(key)) {
            co_return std::move(hit);
        }
    }
    for (const auto &key : ordered) {
        if (auto hit = lookupFile(key)) {
            co_return std::move(hit);
        }
    }
    if (auto fallback = lookupFileStem(settings_.defaultWorkflowFile)) {
        LOG_WARN << "[ussd] no workflow for service='" << serviceKey << "' dial='" << dialCode
                 << "'; using default blueprint";
        co_return std::move(fallback);
    }
    LOG_ERROR << "[ussd] no workflow for service='" << serviceKey << "' dial='" << dialCode
              << "' and no default blueprint";
    co_return std::optional<ResolvedWorkflow>{};
}

drogon::Task<std::optional<ResolvedWorkflow>> UssdWorkflowResolver::lookupDatabase(
    const std::string &key) {
    using drogon_model::WssdApi::WssdRegistry;
    try {
        auto db = drogon::app().getDbClient();
        drogon::orm::CoroMapper<WssdRegistry> mapper(db);
        const std::string columns[] = {WssdRegistry::Cols::_ussd_code};
        for (const auto &column : columns) {
            auto rows = co_await mapper.limit(1).findBy(
                drogon::orm::Criteria(column, drogon::orm::CompareOperator::EQ, key));
            if (rows.empty()) {
                continue;
            }
            const auto &row = rows.front();
            const auto executable = row.getExecutable();
            if (!executable || executable->empty()) {
                LOG_WARN << "[ussd] registry entry for '" << key << "' has no executable blueprint";
                continue;
            }
            ResolvedWorkflow resolved;
            resolved.workflowId = sanitizeWorkflowId(key);
            resolved.blueprintJson = *executable;
            resolved.displayTitle = row.getValueOfDisplayTitle();
            if (resolved.displayTitle.empty()) {
                resolved.displayTitle = row.getValueOfBusinessName();
            }
            resolved.matchedKey = key;
            resolved.fromDatabase = true;
            co_return std::optional<ResolvedWorkflow>{std::move(resolved)};
        }
    } catch (const std::exception &e) {
        LOG_WARN << "[ussd] registry lookup failed for '" << key << "': " << e.what();
    }
    co_return std::optional<ResolvedWorkflow>{};
}

std::optional<ResolvedWorkflow> UssdWorkflowResolver::lookupFile(const std::string &key) {
    const std::string stem = sanitizeFileStem(key);
    if (stem.empty()) {
        return std::nullopt;
    }
    return lookupFileStem(stem);
}

std::optional<ResolvedWorkflow> UssdWorkflowResolver::lookupFileStem(const std::string &stem) {
    const std::string safe = sanitizeFileStem(stem);
    if (safe.empty() || settings_.workflowDirectory.empty()) {
        return std::nullopt;
    }
    const std::string path = settings_.workflowDirectory + "/" + safe + ".json";
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }
    std::string text;
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        text = buffer.str();
    } catch (const std::exception &e) {
        LOG_WARN << "[ussd] cannot read blueprint file '" << path << "': " << e.what();
        return std::nullopt;
    }

    std::string workflowId = "wssd:" + safe;
    try {
        const auto document = nlohmann::json::parse(text);
        if (document.is_object() && document.contains("name") && document["name"].is_string()) {
            workflowId = document["name"].get<std::string>();
        }
    } catch (const std::exception &e) {
        LOG_WARN << "[ussd] blueprint file '" << path << "' is not valid JSON: " << e.what();
        return std::nullopt;
    }

    ResolvedWorkflow resolved;
    resolved.workflowId = workflowId;
    resolved.blueprintJson = text;
    resolved.matchedKey = safe;
    resolved.fromDatabase = false;
    return resolved;
}

}  // namespace wssd_api::sapo_host
