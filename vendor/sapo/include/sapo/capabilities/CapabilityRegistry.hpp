//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//
//  Capability providers (implementation_plan_2.md T3.1).
//
//  Everything a blueprint can call by dot-notation name — `crypto.hash`,
//  `data.uuid`, `log.info`, `notify.sms`, plugin capabilities — goes through
//  this seam. The registry is the single source of truth for "does this name
//  exist?", which is what makes the P0-2 fix stick: an unknown capability is
//  refused at parse time, and the engine has no shell fallback to reach.
//
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/Context.hpp"

namespace sapo::capabilities {

    /// What a provider is being asked to do.
    struct CapabilityCall {
        std::string name;                        // full dotted name, e.g. "crypto.sha256"
        nlohmann::json inputs = nlohmann::json::object();
        std::string node_id;                     // for error context
        sapo::runtime::RuntimeContext *context{nullptr}; // may be null (pure capabilities)
    };

    using json = nlohmann::json;

    /// Metadata that makes capabilities discoverable and *checkable*.
    struct CapabilityDescriptor {
        std::string name;                 // "crypto.sha256"
        std::string provider;             // provider id that answers it
        std::string description;
        std::string category;             // crypto | data | fs | notify | …
        json input_schema = json::object();   // JSON-Schema subset (see validateSchema)
        json output_schema = json::object();
        bool deferred{false};             // declared but not implemented → loud runtime error
        /// Optional example blueprint fragment, surfaced in generated docs.
        std::string example;
    };

    /// Result of a capability call. Providers throw `SapoError` for failures;
    /// `ok` is reserved for the structured error payloads the VM needs.
    struct CapabilityResult {
        json value = json();
        bool ok{true};
        std::string error_code;
        std::string error_message;

        [[nodiscard]] static CapabilityResult success(json value) {
            CapabilityResult result;
            result.value = std::move(value);
            return result;
        }
        [[nodiscard]] static CapabilityResult failure(std::string code, std::string message) {
            CapabilityResult result;
            result.ok = false;
            result.error_code = std::move(code);
            result.error_message = std::move(message);
            return result;
        }
    };

    /// Implementations must be thread-safe: parallel branches dispatch concurrently.
    class ICapabilityProvider {
    public:
        virtual ~ICapabilityProvider() = default;

        [[nodiscard]] virtual std::string providerId() const = 0;
        /// Namespace prefixes this provider answers, e.g. {"crypto", "data"}.
        [[nodiscard]] virtual std::vector<std::string> namespaces() const = 0;
        /// Every capability it can serve, with schemas.
        [[nodiscard]] virtual std::vector<CapabilityDescriptor> capabilities() const = 0;

        /// Default behaviour: prefix match on `namespaces()`.
        [[nodiscard]] virtual bool supports(const std::string &name) const;

        /// Execute the call. Throw `sapo::runtime::SapoError` on failure.
        [[nodiscard]] virtual CapabilityResult execute(const CapabilityCall &call) = 0;
    };

    using ProviderPtr = std::shared_ptr<ICapabilityProvider>;

    /// Registry + dispatcher.
    class CapabilityRegistry {
    public:
        void addProvider(ProviderPtr provider);
        void clear();

        [[nodiscard]] bool empty() const { return m_providers.empty(); }
        [[nodiscard]] const std::vector<ProviderPtr> &providers() const { return m_providers; }

        /// True if some provider answers `name` (and it is not a deferred stub).
        [[nodiscard]] bool has(const std::string &name) const;
        [[nodiscard]] bool knows(const std::string &name) const;  // true for deferred names too
        [[nodiscard]] const CapabilityDescriptor *describe(const std::string &name) const;
        [[nodiscard]] std::vector<CapabilityDescriptor> all() const;
        [[nodiscard]] std::vector<std::string> providerIds() const;
        /// Namespace segment of a dotted name (`crypto.sha256` → `crypto`).
        [[nodiscard]] static std::string describeNamespace(const std::string &name);

        /// Resolves the provider that must serve `name`: an exact advertised
        /// capability first, then dynamic providers by namespace prefix. Null when
        /// nothing answers — which is what makes unknown commands a hard error.
        [[nodiscard]] ICapabilityProvider *providerFor(const std::string &name) const;

        /// Dispatch, resolving the provider by longest namespace prefix.
        /// Unknown names throw `CapabilityNotFound`; deferred names throw
        /// `NotImplemented` — never a silent no-op (plan T3.3).
        [[nodiscard]] CapabilityResult dispatch(const CapabilityCall &call) const;

        /// Startup self-check: duplicate registrations, descriptors without
        /// schemas, providers advertising capabilities they do not answer.
        /// Returns human-readable problems (empty ⇒ healthy).
        [[nodiscard]] std::vector<std::string> audit() const;

        /// Process-wide default registry (opt-in; injectable everywhere).
        static CapabilityRegistry &global();

    private:
        std::vector<ProviderPtr> m_providers;
    };

    // ---------------------------------------------------------------------
    /// Tiny JSON-Schema subset used to validate capability inputs at parse
    /// time and at dispatch: `type`, `required`, `properties`, `enum`,
    /// `items`, `minLength`, `default`. Enough to reject misconfiguration
    /// before a workflow ever runs; not a full validator.
    struct SchemaIssue {
        std::string path;
        std::string message;
        [[nodiscard]] std::string describe() const { return (path.empty() ? "<root>" : path) + ": " + message; }
    };

    /// Validates `value` against `schema`, applying defaults into `value`.
    /// Returns issues (empty ⇒ valid). Unknown schema keywords are ignored.
    std::vector<SchemaIssue> validateSchema(const json &schema, json &value, const std::string &path = "");

    /// Convenience for providers: validate or throw.
    void validateOrThrow(const json &schema, json &value, const std::string &capability);

} // namespace sapo::capabilities
