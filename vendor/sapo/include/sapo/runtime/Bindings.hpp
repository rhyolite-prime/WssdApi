//
//  Sapo Engine — out-of-band bindings (`env.*`, `secret.*`, `config.*`).
//  implementation_plan_2.md T1.3 / T3.2
//
#pragma once

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace sapo::runtime {

    /**
     * @brief Resolves namespaced references used inside DSL strings.
     *
     * `ns` is the first path segment after stripping `$` (`env`, `secret`,
     * `config`, …) and `key` the remainder (`API_TOKEN`, `smtp.password`, …).
     */
    class IBindingProvider {
    public:
        virtual ~IBindingProvider() = default;
        [[nodiscard]] virtual bool lookup(std::string_view ns, std::string_view key, nlohmann::json &out) const = 0;
        /// Names this provider answers for (used by the validator / docs).
        [[nodiscard]] virtual std::vector<std::string> namespaces() const = 0;
    };

    using BindingProviderPtr = std::shared_ptr<IBindingProvider>;

    /// Reads `env.VAR_NAME` from the process environment. Nothing else.
    class EnvironmentBindingProvider final : public IBindingProvider {
    public:
        [[nodiscard]] bool lookup(std::string_view ns, std::string_view key, nlohmann::json &out) const override {
            if (ns != "env") return false;
            const std::string name(key);
            if (const char *value = std::getenv(name.c_str()); value != nullptr) {
                out = std::string(value);
                return true;
            }
            return false;
        }

        [[nodiscard]] std::vector<std::string> namespaces() const override { return {"env"}; }
    };

    /// Chains providers; first match wins.
    class CompositeBindingProvider final : public IBindingProvider {
    public:
        void add(BindingProviderPtr provider) {
            if (provider) m_providers.push_back(std::move(provider));
        }

        [[nodiscard]] bool lookup(std::string_view ns, std::string_view key, nlohmann::json &out) const override {
            for (const auto &provider : m_providers) {
                if (provider->lookup(ns, key, out)) return true;
            }
            return false;
        }

        [[nodiscard]] std::vector<std::string> namespaces() const override {
            std::vector<std::string> out;
            for (const auto &provider : m_providers) {
                for (auto &ns : provider->namespaces()) out.push_back(std::move(ns));
            }
            return out;
        }

    private:
        std::vector<BindingProviderPtr> m_providers;
    };

    /**
     * @brief Default provider: process environment only. Hosts layer a
     *        `ProviderConfigStore` on top for `secret.*` / `config.*`.
     */
    [[nodiscard]] inline BindingProviderPtr defaultBindingProvider() {
        static BindingProviderPtr provider = std::make_shared<EnvironmentBindingProvider>();
        return provider;
    }

} // namespace sapo::runtime
