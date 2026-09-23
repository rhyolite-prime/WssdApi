//
//  Sapo Engine — task interface (implementation_plan_2.md T1.5, rewritten for plan v2).
//
//  A task is a *synchronous, stateless* unit of work: it receives the AST node,
//  the session context and the injected services, does its thing, and returns a
//  `ControlSignal`. No `std::future`, no thread parked per node (the worker pool
//  is used only where a blueprint genuinely asks for concurrency), and no magic
//  `__SYS_*` context keys — routing is expressed by the signal type.
//
//  Control-flow constructs (loop / try / parallel / condition bodies) are driven
//  by the interpreter's frame stack; their task classes expose the evaluation
//  helpers they own (items to iterate, branch selection, merge policy) rather
//  than mutating control state behind the VM's back.
//
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "parser/AstNodes.hpp"
#include "runtime/Context.hpp"
#include "runtime/ControlFlow.hpp"
#include "runtime/ExpressionEvaluator.hpp"
#include "runtime/TaskServices.hpp"

namespace sapo::tasks {

    using runtime::ControlSignal;
    using runtime::Continue;
    using runtime::JumpTo;
    using runtime::LoopBreak;
    using runtime::LoopContinue;
    using runtime::SuspendRequest;
    using runtime::Terminate;

    /// Everything one node activation can see.
    struct ExecutionContext {
        ExecutionContext(const parser::NodePtr &node, runtime::RuntimeContext &context,
                         const runtime::TaskServices &services, std::string session_id = {},
                         std::string execution_id = {}, size_t depth = 0)
            : node(node), context(context), services(services), session_id(std::move(session_id)),
              execution_id(std::move(execution_id)), depth(depth) {}

        parser::NodePtr node;
        runtime::RuntimeContext &context;
        const runtime::TaskServices &services;
        std::string session_id;
        std::string execution_id;
        std::string workflow_id;  // registry key of the running blueprint
        size_t depth{0};

        /// Loop item/index, `$error`, resumed input… (shadows nothing in context).
        nlohmann::json locals = nlohmann::json::object();
        /// Data sources available to `query` nodes: {name: {provider, scope, config}}.
        /// Built once per workflow by the interpreter (blueprint + sapo-config.json).
        nlohmann::json data_sources = nlohmann::json::object();
        /// Current iteration for frame-owning nodes (0-based, -1 outside a frame).
        int64_t iteration{-1};

        [[nodiscard]] const std::string &nodeId() const { return node->id; }
        [[nodiscard]] parser::TaskType type() const { return node->getType(); }

        /// Typed view of the node; throws (internal error) if the AST lies.
        template<typename Node>
        [[nodiscard]] const Node &as() const {
            const Node *typed = dynamic_cast<const Node *>(node.get());
            if (typed == nullptr) {
                throw runtime::SapoError(runtime::ErrorCode::Internal,
                                         "interpreter dispatched a " + std::string(parser::toString(node->getType())) +
                                             " node to the wrong task",
                                         nlohmann::json::object(), node->id);
            }
            return *typed;
        }

        [[nodiscard]] runtime::EvaluationScope scope() const;

        /// Expression → value (strict: unresolved roots raise SapoError).
        [[nodiscard]] nlohmann::json eval(const parser::Expression &expression) const;
        /// Empty expressions evaluate to `fallback` instead of throwing.
        [[nodiscard]] nlohmann::json evalOr(const parser::Expression &expression, const nlohmann::json &fallback) const;
        [[nodiscard]] bool evalBool(const parser::Expression &expression) const;
        /// Optional expressions: null signal when absent.
        [[nodiscard]] bool evalBoolOr(const std::optional<parser::Expression> &expression, bool fallback) const;

        /// Recursive template resolution for `inputs`/`payload`/`body`-shaped JSON:
        /// strings are resolved as templates, containers are walked, everything
        /// else passes through.
        [[nodiscard]] nlohmann::json resolve(const nlohmann::json &template_value) const;
        /// Same, but `strict=false` (missing variables become null, not errors).
        [[nodiscard]] nlohmann::json resolveLenient(const nlohmann::json &template_value) const;

        /// Context write that is recorded in the fork/merge journal.
        void write(const std::string &key, const nlohmann::json &value) const;

        [[nodiscard]] nlohmann::json read(const std::string &dotted_path) const;
        [[nodiscard]] bool has(const std::string &dotted_path) const;

        void log_debug(const std::string &message, const nlohmann::json &fields = {}) const;
        void log_info(const std::string &message, const nlohmann::json &fields = {}) const;
        void log_warn(const std::string &message, const nlohmann::json &fields = {}) const;
    };

    class ITask {
    public:
        virtual ~ITask() = default;

        /// The node type this task executes (registry key).
        [[nodiscard]] virtual parser::TaskType handles() const = 0;

        /// Performs the node's work. Default: nothing to do, continue.
        [[nodiscard]] virtual ControlSignal execute(ExecutionContext &execution) const {
            (void) execution;
            return Continue{};
        }
    };

    using TaskPtr = std::shared_ptr<const ITask>;

    /// TaskType → task. Static and shared; the interpreter never owns behavior.
    class TaskRegistry {
    public:
        void add(parser::TaskType type, TaskPtr task);
        [[nodiscard]] const ITask *find(parser::TaskType type) const;
        [[nodiscard]] bool handles(parser::TaskType type) const;
        [[nodiscard]] std::vector<std::string> handledTypes() const;
        /// Built-in tasks for every grammar-v1 node type.
        static TaskRegistry &defaults();

    private:
        std::vector<std::pair<parser::TaskType, TaskPtr>> m_tasks;
    };

    /// Resolves a template (string | object | array) against the scope.
    [[nodiscard]] nlohmann::json resolveObjectTemplate(const nlohmann::json &object,
                                                        const ExecutionContext &execution, bool strict = true);

    /// Pulls a value out of a task's response envelope for `outputs` mapping
    /// (`{"user": "$.data.user"}` style paths, plain keys, or SEL).
    [[nodiscard]] nlohmann::json extractField(const nlohmann::json &source, const std::string &reference,
                                              const ExecutionContext &execution);

} // namespace sapo::tasks
