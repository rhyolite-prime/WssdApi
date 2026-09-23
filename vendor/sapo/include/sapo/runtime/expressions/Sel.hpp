//
//  Sapo Expression Language (SEL) — public surface.
//  implementation_plan_2.md T1.2
//
//  A small, typed, JSON-native expression language that replaces the old
//  "strip the $, hand it to exprtk, fall back to interpolation" heuristic.
//
//  Highlights
//    • literals: numbers, 'strings', "strings", true/false/null, [arrays], {objects}
//    • variables: `$user.id`, `$result.items[0].id`, bare names (`user.id`)
//    • operators: arithmetic, comparison, logical, `??`, ternary, string `+`
//    • function library: len/upper/lower/contains/coalesce/to_number/... plus
//      lambda-ish `map/filter/any/all/count` taking an inline predicate expr
//    • structured `{left, operator, right}` expressions are accepted too
//
//  Compilation happens once (at blueprint parse time) and results are memoised
//  in a process-wide cache, so evaluation never recompiles.
//
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/SapoError.hpp"

namespace sapo::expr {

    using json = nlohmann::json;

    struct Node;
    using NodePtr = std::shared_ptr<const Node>;

    /**
     * @brief Identifier lookup contract. Implemented over `RuntimeContext`
     *        (+ loop locals + `env.`/`secret.`/`config.` bindings) by
     *        `ExpressionEvaluator`.
     */
    class IResolver {
    public:
        virtual ~IResolver() = default;
        /// @return true when `name` is a known identifier.
        virtual bool resolve(const std::string &name, json &out) const = 0;
    };

    struct EvalOptions {
        /// Throw (with the offending name) instead of substituting null.
        bool strict = true;
        /// Tolerate `1 + "abc"`-style coercion failures (produce null).
        bool lenient_types = false;
    };

    /// A compiled, immutable SEL program.
    class Program {
    public:
        explicit Program(NodePtr root, std::string source)
            : m_root(std::move(root)), m_source(std::move(source)) {}

        [[nodiscard]] const NodePtr &root() const { return m_root; }
        [[nodiscard]] const std::string &source() const { return m_source; }

    private:
        NodePtr m_root;
        std::string m_source;
    };

    using ProgramPtr = std::shared_ptr<const Program>;

    /** Per-evaluation mutable state (locals for iterators/predicates). */
    struct EvalState {
        const IResolver *resolver{nullptr};
        EvalOptions options{};
        std::map<std::string, json> locals;
        int recursion{0};

        void setLocal(std::string name, json value) { locals[std::move(name)] = std::move(value); }
        void clearLocal(const std::string &name) { locals.erase(name); }
    };

    /// Evaluates a compiled program against a resolver.
    [[nodiscard]] json evaluate(const Program &program, EvalState &state);

    /// Parses + compiles SEL source. Throws `SapoError(ErrorCode::Expression)`
    /// on syntax errors — callers surface this as a blueprint parse error.
    [[nodiscard]] ProgramPtr compile(const std::string &source);

    /// True when the text contains no `$` interpolation markers and is not
    /// itself an expression (fast path for plain literals).
    [[nodiscard]] bool isPlainLiteral(const std::string &source);

    /// True when the text needs *no* templating at all but is a math/logic
    /// expression (i.e. should be compiled as a program).
    [[nodiscard]] bool looksLikeExpression(const std::string &source);

    // -------------------------------------------------------------------------
    // String templates: "Total: $order.total (status ${status == 'paid' ? 'ok' : 'no'})"
    // -------------------------------------------------------------------------
    class Template {
    public:
        struct Segment {
            std::string literal;      // when program == nullptr
            ProgramPtr program;       // interpolation slot
        };

        Template(std::string source, std::vector<Segment> segments)
            : m_source(std::move(source)), m_segments(std::move(segments)) {}

        [[nodiscard]] const std::vector<Segment> &segments() const { return m_segments; }
        [[nodiscard]] bool isLiteral() const { return m_segments.size() == 1 && !m_segments.front().program; }
        /// Single interpolation covering the whole string → keep the JSON type.
        [[nodiscard]] bool isSingleInterpolation() const {
            return m_segments.size() == 1 && m_segments.front().program != nullptr;
        }
        /// True only when the template is the untouched source text (no `$$`
        /// or `\$` escapes were consumed) — callers can then skip rendering.
        [[nodiscard]] bool isPlainLiteral() const {
            return isLiteral() && m_segments.front().literal == m_source;
        }
        [[nodiscard]] const std::string &source() const { return m_source; }

        /// Renders against `state`. Type-preserving when the template is a
        /// single interpolation, string concatenation otherwise.
        [[nodiscard]] json render(EvalState &state) const;

    private:
        std::string m_source;
        std::vector<Segment> m_segments;
    };

    using TemplatePtr = std::shared_ptr<const Template>;

    [[nodiscard]] TemplatePtr compileTemplate(const std::string &source);

    // -------------------------------------------------------------------------
    // Compiled handle stored on AST nodes
    // -------------------------------------------------------------------------

    /**
     * @brief A compiled SEL expression (or string template) plus its source.
     *
     * Implicitly constructible from a string so hand-built AST nodes in tests
     * and translators keep working: `node->expression = "amount > 100";`.
     */
    class Expression {
    public:
        Expression() = default;

        /// Compiles `source` as a *template* — the shape used for URLs,
        /// headers, prompts, bodies. Lazily promotes to a program when the
        /// whole string is one interpolation-free expression.
        Expression(std::string source);

        [[nodiscard]] bool empty() const { return m_source.empty(); }
        [[nodiscard]] const std::string &source() const { return m_source; }

        /// Resolves string templates (typed result for single interpolation).
        [[nodiscard]] json resolve(const IResolver &resolver, const EvalOptions &options = {}) const;

        /// Forces expression semantics ("amount >= 100" style predicates).
        [[nodiscard]] json evaluate(const IResolver &resolver, const EvalOptions &options = {}) const;

        /// Convenience: expression semantics coerced through SEL truthiness.
        [[nodiscard]] bool evaluateBool(const IResolver &resolver, const EvalOptions &options = {}) const;

        /// Accepts `"str"` or `{left, operator, right}` shapes from the DSL.
        [[nodiscard]] static Expression fromJson(const json &value);

        /// Condition/predicate fields: the source must compile as a program
        /// (syntax errors therefore surface at blueprint parse time).
        [[nodiscard]] static Expression fromExpression(std::string source);

        /// Data literal carried by a structured expression object (numbers,
        /// booleans, nested objects) — never treated as SEL source text.
        [[nodiscard]] static Expression fromLiteral(json value);

        /// Rebinds the handle (used by the parser when a node field is filled in
        /// after construction).
        void setJson(const json &value);

        /// Template resolution with a supplied fallback for empty handles.
        [[nodiscard]] json resolveJson(const IResolver &resolver, const EvalOptions &options,
                                      const json &fallback) const;

        /// Structured `{left, operator, right}` → canonical SEL text.
        [[nodiscard]] static std::string normalizeSource(const json &value);

    private:
        std::string m_source;
        ProgramPtr m_program;
        TemplatePtr m_template;
    };

    /// Time source used by `now()` / `format_date()`. Hosts install their own
    /// (the VM forwards `IClock`, tests use a manual clock).
    void setClockProvider(std::function<int64_t()> provider);
    [[nodiscard]] int64_t nowMillis();

    /// Process-wide compile cache (AST nodes hold `Expression`, which is enough
    /// for per-blueprint caching; this de-dupes across blueprint reloads).
    class ExpressionCache {
    public:
        static ExpressionCache &instance();

        [[nodiscard]] ProgramPtr getOrCompile(const std::string &source);
        [[nodiscard]] TemplatePtr getOrCompileTemplate(const std::string &source);
        void clear();
        [[nodiscard]] size_t programs() const;
        [[nodiscard]] size_t templates() const;

    private:
        static constexpr size_t kSoftLimit = 4096;

        mutable std::mutex m_mutex;
        std::map<std::string, ProgramPtr> m_programs;
        std::map<std::string, TemplatePtr> m_templates;
    };

} // namespace sapo::expr
