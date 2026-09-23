//
//  Sapo Engine — data-source provider SPI (implementation_plan_2.md T2.8).
//
//  `query` nodes read through this seam instead of talking to a database
//  directly. Providers return rows; filtering and paging are applied by the
//  registry so every provider behaves the same way. Providers that are
//  intentionally not implemented (redis, postgresql) are *registered as
//  deferred* — they fail loudly at dispatch instead of returning an empty set.
//
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/Context.hpp"

namespace sapo::http {
    class IHttpTransport;
}

namespace sapo::data {

    /// One entry of a node's `data_sources` (or `sapo-config.json`).
    struct DataSourceConfig {
        std::string name;
        std::string scope{"internal"};   // internal | external
        std::string provider;            // context | mock | http | redis | postgresql
        nlohmann::json config = nlohmann::json::object();
        std::string origin;              // "blueprint" | "config-file" (error messages)
    };

    struct DataQuery {
        nlohmann::json filter;                        // object matcher, array of matchers, or null
        std::optional<int> limit;
        std::optional<int> offset;
        std::string statement;                        // provider-specific query text (SQL etc.)
        nlohmann::json parameters = nlohmann::json::object();
    };

    struct DataPage {
        nlohmann::json rows = nlohmann::json::array();
        bool pre_filtered{false};    // provider already applied `query.filter`
        bool pre_paged{false};       // provider already applied limit/offset
        size_t total_matches{0};     // provider hint; 0 ⇒ computed by the registry
        bool truncated{false};       // rows were dropped by offset/limit
        nlohmann::json meta = nlohmann::json::object();
    };

    using RowMatcher = std::function<bool(const nlohmann::json &row)>;

    class IDataSourceProvider {
    public:
        virtual ~IDataSourceProvider() = default;

        [[nodiscard]] virtual std::string providerId() const = 0;
        [[nodiscard]] virtual std::string description() const { return {}; }
        /// Deferred providers are known but not implemented: dispatch throws.
        [[nodiscard]] virtual bool deferred() const { return false; }

        /// Configuration problems for this provider (empty ⇒ ok). Checked at startup
        /// so a misconfigured source is not discovered on the first query.
        [[nodiscard]] virtual std::vector<std::string> validate(const DataSourceConfig &config) const;

        /// Fetch rows. `matcher` may be used for pushdown; the registry applies it
        /// afterwards when the provider did not.
        [[nodiscard]] virtual DataPage query(const DataSourceConfig &config, const DataQuery &query,
                                             sapo::runtime::RuntimeContext &context, const RowMatcher &matcher) const;

        /// Convenience for providers without pushdown support.
        [[nodiscard]] DataPage query(const DataSourceConfig &config, const DataQuery &query,
                                     sapo::runtime::RuntimeContext &context) const {
            return this->query(config, query, context, nullptr);
        }
    };

    using ProviderPtr = std::shared_ptr<IDataSourceProvider>;

    class DataSourceRegistry {
    public:
        void add(ProviderPtr provider);
        void clear();
        [[nodiscard]] bool has(const std::string &provider_id) const;
        [[nodiscard]] const IDataSourceProvider *find(const std::string &provider_id) const;
        [[nodiscard]] std::vector<std::string> providerIds() const;
        [[nodiscard]] std::vector<std::string> deferredProviders() const;

        /// Dispatch + shared post-processing (matcher, offset, limit).
        [[nodiscard]] DataPage run(const DataSourceConfig &config, const DataQuery &query,
                                   sapo::runtime::RuntimeContext &context, const RowMatcher &matcher = nullptr) const;

        /// Startup validation of declared sources (plan: misconfiguration is a
        /// parse/startup error, not a runtime surprise).
        [[nodiscard]] std::vector<std::string> validate(const std::vector<DataSourceConfig> &sources) const;

        [[nodiscard]] static DataSourceRegistry &global();
        /// Context + mock + http, plus redis/postgresql registered as deferred.
        static std::shared_ptr<DataSourceRegistry> withBuiltIns(const std::shared_ptr<sapo::http::IHttpTransport> &transport);

    private:
        std::vector<ProviderPtr> m_providers;
    };

    // ---------------------------------------------------------------------
    /// Generic filter semantics shared by providers and by the query task:
    /// `{"field": value}` equality, `{"field": {"$gt": 3, "$in": […]}}`,
    /// `$regex`, `$exists`, `$ne`, arrays of matchers (AND).
    [[nodiscard]] bool matchesFilter(const nlohmann::json &row, const nlohmann::json &filter);

    /// Matcher object usable as a `RowMatcher`.
    [[nodiscard]] RowMatcher makeRowMatcher(nlohmann::json filter);

} // namespace sapo::data
