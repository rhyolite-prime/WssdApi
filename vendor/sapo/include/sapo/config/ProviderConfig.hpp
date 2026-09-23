//
//  Sapo Engine — `sapo-config.json` provider configuration (implementation_plan_2.md T3.2).
//
//  A deployment supplies connector settings (endpoints, buckets, data sources,
//  tunables) without touching blueprints, and blueprints reference the values
//  through `config.*` / `secret.*` bindings. Secrets are never written to logs:
//  every secret value discovered here is registered with the logger for
//  redaction. Structural problems are reported by `validate()` at startup —
//  never swallowed.
//
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/Bindings.hpp"

namespace sapo::config {

    /// Parsed, read-only view of one `sapo-config.json`.
    class ProviderConfigStore {
    public:
        ProviderConfigStore() = default;
        explicit ProviderConfigStore(nlohmann::json document, std::string origin = "<memory>");

        /// Reads and parses a file. Throws `SapoError(ErrorCode::Parse)` when the
        /// file is missing, unparseable, or not an object.
        [[nodiscard]] static ProviderConfigStore load(const std::string &path);
        /// Walks up from `start_directory` looking for `sapo-config.json`.
        [[nodiscard]] static std::optional<ProviderConfigStore> discover(const std::string &start_directory);

        [[nodiscard]] const std::string &origin() const { return m_origin; }
        [[nodiscard]] const nlohmann::json &document() const { return *m_document; }
        [[nodiscard]] bool empty() const { return m_document->empty(); }

        // --- providers ------------------------------------------------------
        [[nodiscard]] bool hasProvider(const std::string &id) const;
        /// The provider's `config` object (settings), or null when absent.
        [[nodiscard]] nlohmann::json providerConfig(const std::string &id) const;
        /// `type` declared for a provider ("" when unknown).
        [[nodiscard]] std::string providerType(const std::string &id) const;
        [[nodiscard]] bool providerDeferred(const std::string &id) const;
        [[nodiscard]] std::vector<std::string> providerIds() const;

        // --- values ---------------------------------------------------------
        /// `config.<dotted.path>` lookup (also used by the binding provider).
        [[nodiscard]] std::optional<nlohmann::json> setting(const std::string &dotted_path) const;
        /// `secret.<NAME>`: the `secrets` map, else `SAPO_SECRET_<NAME>` in env.
        [[nodiscard]] std::optional<nlohmann::json> secret(const std::string &name) const;
        /// Tunables under `engine` (workers, log level, limits …).
        [[nodiscard]] nlohmann::json engine() const;

        /// Declarations for the data-source registry (`data_sources` array).
        [[nodiscard]] std::vector<nlohmann::json> dataSourceDeclarations() const;

        /// Secret *values* — handed to the logger so they are redacted.
        [[nodiscard]] std::vector<std::string> redactList() const;

        /// Startup validation: returns human-readable problems (empty ⇒ ok).
        [[nodiscard]] std::vector<std::string> validate() const;

        /// `config.*` + `secret.*` bindings for the expression evaluator.
        [[nodiscard]] runtime::BindingProviderPtr bindingProvider() const;

    private:
        std::shared_ptr<const nlohmann::json> m_document{std::make_shared<const nlohmann::json>(nlohmann::json::object())};
        std::string m_origin{"<memory>"};
    };

    /// Resolves `{"$secret": "NAME"}` / `{"$env": "NAME"}` indirections inside a
    /// config document. Exposed for tests and for plugins reading their own block.
    [[nodiscard]] nlohmann::json expandIndirections(const nlohmann::json &value, const ProviderConfigStore &store,
                                                    std::vector<std::string> &problems, const std::string &path = "");

} // namespace sapo::config
