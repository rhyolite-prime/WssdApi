//
//  Sapo Engine — workflow interpreter (implementation_plan_2.md T1.5, T2.1,
//  T2.2, T2.5, T2.6, T4.1).
//
//  Design:
//    • Nodes return a typed `ControlSignal` (continue / jump / suspend /
//      terminate / break / continue). No magic context keys, no exceptions for
//      normal routing.
//    • Control-flow constructs run through a *frame stack* (loop, try, if-branch,
//      parallel branch). Frames are plain serialisable data, so a session can be
//      checkpointed in the middle of the third iteration of a nested loop and
//      resumed later — in this process or another one.
//    • Failures are `SapoError`s materialised as `$error`, routed to a node's
//      `on_error`, to the nearest `try` frame, or out to the caller; `retry`
//      applies exponential backoff with jitter first.
//    • Suspension never parks a thread: the session goes to the state store and a
//      timer / event subscription wakes it.
//
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "parser/AstNodes.hpp"
#include "runtime/ControlFlow.hpp"
#include "runtime/StateStore.hpp"
#include "runtime/TaskServices.hpp"
#include "tasks/Task.hpp"

namespace sapo::runtime {

    struct RunOptions {
        std::string session_id;            // empty ⇒ generated
        std::string execution_id;          // empty ⇒ generated
        std::string start_node;            // entry override (scheduled node, resume)
        nlohmann::json input = nlohmann::json::object();
        /// Write checkpoints (suspend, terminate, failure) to the state store.
        bool persist{true};
        /// 0 ⇒ `services.limits.max_node_visits`.
        size_t max_node_visits{0};
        std::string correlation_id;
        size_t depth{0};
    };

    struct ExecutionReport {
        std::string session_id;
        std::string execution_id;
        std::string blueprint_id;
        /// completed | terminated | awaiting_input | suspended | failed | cancelled
        std::string status{"completed"};
        std::string cursor;                        // node to run next when suspended
        nlohmann::json output = nlohmann::json::object();
        nlohmann::json prompt = nlohmann::json::object();
        std::string error_code;
        std::string error_message;
        std::string error_node;
        nlohmann::json error_data = nlohmann::json();
        std::string correlation_id;

        std::vector<std::string> visited;
        std::vector<std::string> warnings;
        size_t node_visits{0};
        int64_t elapsed_ms{0};
        /// Final session context. `output` is this, replaced by the payload of an
        /// explicit `terminate` node.
        nlohmann::json context = nlohmann::json::object();

        [[nodiscard]] bool succeeded() const { return status == "completed" || status == "terminated"; }
        [[nodiscard]] bool suspended() const { return status == "awaiting_input" || status == "suspended"; }
        [[nodiscard]] bool failed() const { return status == "failed"; }
        [[nodiscard]] nlohmann::json toJson() const;
    };

    /// Per-execution state (one per run/resume of a session).
    struct RunState {
        std::shared_ptr<RuntimeContext> context;
        std::vector<RuntimeContext::FrameState> frames;
        std::string cursor;
        std::string current_node_id;
        std::string session_id;
        std::string execution_id;
        std::string workflow_id;
        size_t depth{0};
        size_t visits{0};
        size_t budget{100000};
        std::vector<std::string> visited;
        std::vector<std::string> warnings;
        nlohmann::json locals = nlohmann::json::object();
        /// name → {provider, scope, config} for `query` nodes.
        nlohmann::json data_sources = nlohmann::json::object();
        std::string status{"running"};
        std::string terminate_status{"success"};
        nlohmann::json terminate_payload = nlohmann::json::object();
        nlohmann::json suspend_request = nlohmann::json::object();
        std::string suspend_cursor;
        std::string error;
        std::string error_code;
        std::string error_node;
        nlohmann::json error_data = nlohmann::json();
        int64_t started_ms{0};
        bool finished{false};
        /// Parallel branch: finishing the outermost frame ends this drive().
        bool branch_mode{false};
        bool branch_finished{false};
        /// Error escaping a `finally` block; re-raised once the frame unwinds.
        nlohmann::json pending_error = nlohmann::json();
        /// Node the session was suspended on; its per-activation locals are
        /// cleared right after it re-executes on resume.
        std::string resume_node_id;
    };

    class Interpreter {
    public:
        explicit Interpreter(TaskServices services, const tasks::TaskRegistry *tasks = nullptr);

        [[nodiscard]] TaskServices &services() { return m_services; }
        [[nodiscard]] const TaskServices &services() const { return m_services; }
        void setTaskRegistry(const tasks::TaskRegistry *registry) { m_tasks = registry; }

        /// Runs a workflow from `options.start_node` (or its entry node).
        ExecutionReport run(const parser::ParsedWorkflow &workflow, const nlohmann::json &input,
                            RunOptions options = {});

        /// Continues a suspended session with `input` (a prompt reply, an event
        /// payload, or null for a timer tick).
        /// @param timed_out set by a prompt-timeout tick: the suspended node is
        ///        failed with a `Timeout` error instead of being handed an answer.
        ExecutionReport resume(const parser::ParsedWorkflow &workflow, SessionCheckpoint checkpoint,
                               const nlohmann::json &input, bool timed_out = false);

        /// A failed `resume` for a session that is no longer suspendable.
        ExecutionReport resumeSession(const std::string &session_id, const nlohmann::json &input);
        bool cancelSession(const std::string &session_id, const std::string &reason = "cancelled by caller");

        /**
         * @brief Wires subflows, event triggers, scheduled jobs and wait timers into
         *        the services. Call once after construction (it needs the registry).
         */
        void installRunners();

        /// Convenience used by the CLI/tests: parse + register + run a file.
        ExecutionReport runFile(const std::string &path, const nlohmann::json &input, RunOptions options = {});

    private:
        struct CapturedError {
            std::string code;
            std::string message;
            std::string node;
            nlohmann::json data = nlohmann::json();

            [[nodiscard]] nlohmann::json toJson() const;
            [[nodiscard]] static CapturedError from(const SapoError &error);
            [[nodiscard]] static CapturedError fromJson(const nlohmann::json &value);
        };

        [[nodiscard]] const tasks::ITask *taskFor(parser::TaskType type, const std::string &node_id) const;

        /// One node activation: task dispatch + retry + error capture.
        [[nodiscard]] ControlSignal activateNode(RunState &state, const parser::NodePtr &node);

        void drive(const parser::ParsedWorkflow &workflow, RunState &state);
        void advanceAfterNode(const parser::ParsedWorkflow &workflow, RunState &state);
        /// Frame bookkeeping. `closeFrame` unwinds outwards, so a body ending
        /// inside an outer loop continues that loop instead of stopping.
        void closeFrame(const parser::ParsedWorkflow &workflow, RunState &state);
        void advanceTryPhase(const parser::ParsedWorkflow &workflow, RunState &state);
        void startNextIteration(const parser::ParsedWorkflow &workflow, RunState &state);
        bool nextIterationFor(const parser::ParsedWorkflow &workflow, RunState &state, const std::string &loop_id);
        bool finishLoop(const parser::ParsedWorkflow &workflow, RunState &state, const std::string &loop_id);
        void eraseIterationLocals(RunState &state, const nlohmann::json &frame_state);
        void resolveJump(RunState &state, const std::string &target);
        void pushFrame(RunState &state, const std::string &kind, const std::string &node_id,
                       const std::vector<std::string> &ids, const std::string &resume_target,
                       const nlohmann::json &frame_state);

        ControlSignal stepLoop(const parser::ParsedWorkflow &workflow, RunState &state,
                               const parser::LoopNode &node);
        ControlSignal stepTry(const parser::ParsedWorkflow &workflow, RunState &state, const parser::TryNode &node);
        ControlSignal stepCondition(const parser::ParsedWorkflow &workflow, RunState &state,
                                   const parser::ConditionNode &node);
        ControlSignal runParallel(const parser::ParsedWorkflow &workflow, RunState &state,
                                  const parser::ParallelNode &node);
        /// Nearest handler wins: `on_error`, then the innermost frame that can
        /// recover, otherwise the session fails. Returns false if unhandled.
        bool handleError(const parser::ParsedWorkflow &workflow, RunState &state, const CapturedError &error);

        void bindIteration(RunState &state, const RuntimeContext::FrameState &frame);
        [[nodiscard]] tasks::ExecutionContext makeExecution(RunState &state, const parser::NodePtr &node);
        [[nodiscard]] nlohmann::json buildDataSources(const parser::ParsedWorkflow &workflow) const;

        void finishRun(const parser::ParsedWorkflow &workflow, RunState &state, const RunOptions &options,
                       ExecutionReport &report);
        [[nodiscard]] SessionCheckpoint checkpointFor(const parser::ParsedWorkflow &workflow, const RunState &state,
                                                       const ExecutionReport &report) const;
        void armWakeup(const RunState &state, const SessionCheckpoint &checkpoint);
        void disarmWakeup(const std::string &session_id);

        void startScheduledJob(const ScheduledJob &job, int64_t fired_at_ms);
        void resumeFromTimer(const Timer &timer);
        void startTriggeredWorkflow(const std::string &workflow_id, const Event &event);

        TaskServices m_services;
        const tasks::TaskRegistry *m_tasks{nullptr};
        // Recursive: an event callback resumes a session, and the resume path
        // re-enters the interpreter to disarm that same subscription.
        std::recursive_mutex m_mutex;
        std::map<std::string, std::vector<EventBus::SubscriptionId>> m_event_subscriptions;
        EventBus::SubscriptionId m_trigger_subscription{0};
        bool m_runners_installed{false};
    };

} // namespace sapo::runtime
