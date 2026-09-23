//
//  Sapo Engine — built-in task implementations (grammar v1).
//
//  One header for the whole task pack: each class is small, and the interpreter
//  reaches them only through `TaskRegistry`. Task classes are stateless — all
//  per-run state lives in the session `RuntimeContext`, which is what makes a
//  suspended session serialisable.
//
#pragma once

#include <optional>
#include <string>

#include "tasks/Task.hpp"

namespace sapo::tasks {

    /// `noop` — context writes, annotations, explicit join points.
    class NoopTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Noop; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;
    };

    /// `transform` — real data shaping: assign | filter | map | project | merge |
    /// group | sort | flatten | reduce | set | copy (T2.7).
    class TransformTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Transform; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;

        [[nodiscard]] static nlohmann::json apply(const parser::TransformNode &node, const nlohmann::json &input,
                                                   ExecutionContext &execution);
    };

    /// `script` — SEL expression sequence (`expr` language runs through exprtk).
    class ScriptTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Script; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;
    };

    /// `event` — publishes on the session's event bus.
    class EventTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Event; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;
    };

    /// `terminate` — ends the workflow with a status + payload.
    class TerminateTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Terminate; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;
    };

    /// `break` / `continue` / `loop_control`.
    class LoopControlTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::LoopControl; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;
    };

    /// `wait` — computes the wake-up instant; the interpreter turns it into a
    /// durable suspend (never a sleeping thread).
    class WaitTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Wait; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;

        /// Absolute deadline (epoch ms) for a wait node, or nullopt for a
        /// condition-driven wait.
        [[nodiscard]] static std::optional<int64_t> deadlineFor(const parser::WaitNode &node,
                                                                 const runtime::TaskServices &services,
                                                                 int64_t now_ms);
    };

    /// `schedule` — registers a cron/relative job with the scheduler.
    class ScheduleTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Schedule; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;
    };

    /// `query` — reads through the data-source registry (T2.8).
    class QueryTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Query; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;
    };

    /// `command` — `http.*` over the injected transport; every other name goes to
    /// the capability registry. There is no shell path (P0-2).
    class CommandTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Command; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;

        /// Performs the HTTP call and returns the response envelope
        /// `{status, ok, body, headers, elapsed_ms, url}`. Throws SapoError on
        /// transport failure and on non-2xx (T2.6).
        [[nodiscard]] static nlohmann::json performHttp(const parser::CommandNode &node, ExecutionContext &execution);
        /// Builds the request from a resolved template (exposed for tests).
        [[nodiscard]] static sapo::http::Request buildRequest(const parser::CommandNode &node,
                                                               ExecutionContext &execution);
    };

    /// `action` — prompt (suspend), capability call, event wait, and/or the
    /// declarative `next_tasks` fan-out.
    class ActionTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Action; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;
    };

    /// `subflow` — starts a registered workflow through the registry.
    class SubflowTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Subflow; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;
    };

    /// Which branch a condition node takes, and what to run there.
    struct ConditionBranch {
        bool taken{false};
        std::vector<std::string> body;   // inline body ids (empty ⇒ jump form)
        std::string target;              // jump target id, may be empty
    };

    /// `condition` / `if` — picks the branch. The interpreter executes bodies.
    class ConditionTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Condition; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;

        [[nodiscard]] static bool take(const parser::ConditionNode &node, ExecutionContext &execution);
        [[nodiscard]] static ConditionBranch branch(const parser::ConditionNode &node, ExecutionContext &execution);
    };

    /// `choice` — maps a selector value onto a case target.
    class ChoiceTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Choice; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;

        /// Target node id: the matching case, the default, or "" (no match).
        [[nodiscard]] static std::string select(const parser::ChoiceNode &node, ExecutionContext &execution);
    };

    /// `loop` — supplies the iteration source; the interpreter owns the frame.
    class LoopTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Loop; }
        /// A loop is driven by the VM's frame stack; executing the node itself
        /// would double-iterate, so this asserts the guard once and continues.
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;

        /// Items to iterate: an array for `collection`, `[0…count-1]` for `count`,
        /// or null for a condition-driven loop.
        [[nodiscard]] static nlohmann::json items(const parser::LoopNode &node, ExecutionContext &execution);
        /// `while` guard for condition-driven loops (default true).
        [[nodiscard]] static bool guard(const parser::LoopNode &node, ExecutionContext &execution);
    };

    /// `try` — marker task: the interpreter runs body/catch/finally frames.
    class TryTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Try; }
    };

    /// `parallel` — the interpreter runs the branches on the worker pool and
    /// merges their write-sets; this task exposes the join policy.
    class ParallelTask final : public ITask {
    public:
        [[nodiscard]] parser::TaskType handles() const override { return parser::TaskType::Parallel; }
        [[nodiscard]] ControlSignal execute(ExecutionContext &execution) const override;

        [[nodiscard]] static runtime::RuntimeContext::MergePolicy mergePolicy(const std::string &name);
    };

} // namespace sapo::tasks
