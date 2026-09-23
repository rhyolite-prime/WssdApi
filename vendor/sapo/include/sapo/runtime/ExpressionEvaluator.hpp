//
//  ExpressionEvaluator v2 (implementation_plan_2.md T1.3).
//
//  Thin, stable facade over the SEL engine so tasks never talk to the parser
//  directly:
//    • `resolveValue`  — string templates ("https://x/$post_id", "${a + b}")
//    • `evaluate…`     — forced expression semantics (conditions, predicates)
//    • strict mode     — unresolved *references* throw with the variable name
//    • `env.*` / `secret.*` / `config.*` bindings through `IBindingProvider`
//
//  The public shape deliberately mirrors v1 (static methods, `RuntimeContext`
//  argument) so task code did not churn during the rewrite.
//
#pragma once

#include <map>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "parser/AstNodes.hpp"
#include "runtime/Bindings.hpp"
#include "runtime/Context.hpp"
#include "runtime/expressions/Sel.hpp"

namespace sapo::runtime {

    /// Everything an expression is allowed to look at.
    struct EvaluationScope {
        EvaluationScope() = default;
        EvaluationScope(const RuntimeContext &context) : context(&context) {}

        const RuntimeContext *context{nullptr};
        const IBindingProvider *bindings{nullptr};
        nlohmann::json locals = nlohmann::json::object();   // loop items, $input, …
        sapo::expr::EvalOptions options{};
        std::string node_id;                                  // for error messages

        [[nodiscard]] EvaluationScope withLocals(const nlohmann::json &extra) const {
            EvaluationScope copy = *this;
            copy.locals = extra.is_object() ? extra : nlohmann::json::object();
            return copy;
        }
        [[nodiscard]] EvaluationScope forNode(std::string id) const {
            EvaluationScope copy = *this;
            copy.node_id = std::move(id);
            return copy;
        }
        [[nodiscard]] EvaluationScope relaxed() const {
            EvaluationScope copy = *this;
            copy.options.strict = false;
            return copy;
        }
    };

    /// Bridges `EvaluationScope` → SEL identifier lookup.
    class ContextResolver final : public sapo::expr::IResolver {
    public:
        explicit ContextResolver(const EvaluationScope &scope) : m_scope(scope) {}

        [[nodiscard]] bool resolve(const std::string &name, nlohmann::json &out) const override;

    private:
        const EvaluationScope &m_scope;
    };

    class ExpressionEvaluator {
    public:
        /// Process-wide default for strict reference checking (grammar v1: on).
        static void setStrictByDefault(bool strict);
        [[nodiscard]] static bool strictByDefault();

        /**
         * @brief Resolves a single string that may contain `$var` references or
         *        `${expression}` interpolations. A template that is exactly one
         *        interpolation keeps the resolved JSON type.
         */
        static nlohmann::json resolveValue(const std::string &value, const RuntimeContext &ctx);
        static nlohmann::json resolveValue(const std::string &value, const EvaluationScope &scope);

        /// Resolves an entire string-map into a JSON object.
        static nlohmann::json resolveMap(const std::map<std::string, std::string> &map, const RuntimeContext &ctx);
        static nlohmann::json resolveMap(const parser::ExpressionObject &map, const EvaluationScope &scope);
        static nlohmann::json resolveMap(const nlohmann::json &object, const EvaluationScope &scope);

        /// Forces expression semantics (used by condition/choice/script/predicates).
        static nlohmann::json evaluate(const std::string &expression, const EvaluationScope &scope);
        static nlohmann::json evaluate(const std::string &expression, const RuntimeContext &ctx, bool strict = true);
        static bool evaluateBool(const std::string &expression, const EvaluationScope &scope);
        static bool evaluateBool(const std::string &expression, const RuntimeContext &ctx, bool strict = true);

        /// Compiled-expression overloads (no re-parse at evaluation time).
        static nlohmann::json evaluate(const parser::Expression &expression, const EvaluationScope &scope);
        static nlohmann::json resolve(const parser::Expression &expression, const EvaluationScope &scope);
        static bool evaluateBool(const parser::Expression &expression, const EvaluationScope &scope);
    };

} // namespace sapo::runtime
