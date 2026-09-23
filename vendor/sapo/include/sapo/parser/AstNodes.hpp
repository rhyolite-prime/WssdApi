//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//
//  Sapo DSL — AST node definitions (grammar v1, implementation_plan_2.md T1.1/T1.4).
//
//  Every field is *consumed* by a task: nothing is parsed-then-ignored any
//  more. Expressions are compiled once here (at parse time) into
//  `sapo::expr::Expression` handles, so the interpreter never recompiles.
//
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/expressions/Sel.hpp"

namespace sapo::parser {

    using Expression = sapo::expr::Expression;
    /// Map of string → compiled expression.
    using ExpressionObject = std::map<std::string, Expression>;

    enum class TaskType {
        Noop,
        Transform,
        Query,
        Command,
        Event,
        Loop,
        LoopControl,
        Choice,
        Condition,
        Wait,
        Schedule,
        Subflow,
        Script,
        Parallel,
        Terminate,
        Action,
        Try
    };

    [[nodiscard]] const char *toString(TaskType type);

    /// Retry policy attached to any node (`retry` block in the DSL).
    struct RetryPolicy {
        int max_attempts{1};
        int64_t backoff_ms{250};
        double multiplier{2.0};
        double jitter{0.2};
        int64_t max_backoff_ms{15000};
        /// Error codes / HTTP status codes that are retryable (empty ⇒ transport
        /// errors and 5xx/429 only).
        std::vector<std::string> retry_on;
    };

    /// Base class for all AST nodes.
    class AstNode {
    public:
        virtual ~AstNode() = default;
        [[nodiscard]] virtual TaskType getType() const = 0;

        std::string id;                             // always present (synthesized if omitted)
        std::optional<std::string> next;            // explicit sequential link
        std::optional<std::string> on_error;        // node-local error handler
        std::optional<RetryPolicy> retry;           // node-local retry policy
        std::optional<std::string> label;           // human annotation from the editor
        bool enabled{true};                         // disabled nodes are skipped
    };

    using NodePtr = std::shared_ptr<AstNode>;
    using WorkflowAST = std::vector<NodePtr>;

    // --- 1. TRANSFORM NODE -------------------------------------------------
    class TransformNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Transform; }

        std::string operation;          // assign | filter | map | project | merge | group | sort
        Expression input;               // expression yielding the source value
        /// Projection / mapping template: either an object of
        /// `{"field": "<expr>"}` entries or a string predicate (`filter`).
        nlohmann::json mapping = nlohmann::json::object();
        Expression predicate;           // optional extra filter predicate
        std::string output;             // context key written with the result
        std::string item_variable{"item"};
        std::string index_variable{"index"};
        bool output_as_array{false};    // merge/project produce arrays when set
    };

    // --- 2. QUERY NODE -----------------------------------------------------
    class QueryNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Query; }

        // Canonical grammar v1 fields.
        std::string source;             // data-source id (BNF: "source")
        nlohmann::json filter = nlohmann::json();  // object matcher or expression string
        std::optional<int> limit;
        std::optional<int> offset;
        std::string output;             // context key to write rows into
        // Legacy aliases (kept working; `data_source_id` etc. map onto the above).
        std::string statement;          // provider-specific query text
        nlohmann::json parameters = nlohmann::json::object();
        std::string output_context_key; // legacy output target
    };

    // --- 3. COMMAND NODE ---------------------------------------------------
    class CommandNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Command; }

        std::string command;            // dot-notation target ("http.post", "user.create")
        std::optional<std::vector<std::string>> output_paths; // keys pulled from a JSON reply
        std::string output;             // context key for the whole response envelope
        nlohmann::json outputs = nlohmann::json::object();    // {"key": "<expr>"} written individually

        struct HttpAuthConfig {
            std::string type{"basic"};
            Expression username;
            Expression password;
            Expression token;           // for type = "bearer"
        };

        struct HttpRequestConfig {
            Expression url;
            std::optional<HttpAuthConfig> auth;
            nlohmann::json headers = nlohmann::json::object();
            nlohmann::json query = nlohmann::json::object();
            nlohmann::json body = nlohmann::json();      // object/array/string template
            std::optional<int> timeout;                  // milliseconds
            bool follow_redirects{true};
            std::optional<std::string> content_type;
        };

        std::optional<HttpRequestConfig> http_request;
        nlohmann::json inputs = nlohmann::json::object(); // payload for non-http capabilities
    };

    // --- 4. EVENT EMIT NODE ------------------------------------------------
    class EventEmitNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Event; }

        std::string name;               // routing key on the event bus
        nlohmann::json payload = nlohmann::json::object();
        bool publish_now{true};         // false ⇒ enqueue for the next tick
    };

    // --- 5. WAIT NODE ------------------------------------------------------
    class WaitNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Wait; }

        std::optional<std::string> duration;    // "30s" | "10m" | "2h" | ISO-8601 duration
        Expression until;                       // condition or absolute timestamp
        std::optional<int64_t> poll_interval_ms; // `until` polling cadence when inline
        bool durable{true};                     // suspend + timer instead of sleeping
    };

    // --- 6. SCHEDULE NODE --------------------------------------------------
    class ScheduleNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Schedule; }

        std::string cron;                        // 5/6 field cron, @daily, "in 5 minutes"
        std::optional<std::string> timezone;     // "UTC" | "+03:00"
        std::string body;                        // workflow id to start (or node id in this workflow)
        std::optional<std::string> workflow;     // explicit workflow id
        std::optional<std::string> job_id;       // stable identity for the job registry
        nlohmann::json input = nlohmann::json::object(); // initial context for the workflow
        bool enabled{true};
    };

    // --- 7. SUBFLOW NODE ---------------------------------------------------
    class SubflowNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Subflow; }

        std::string workflow;
        bool wait_for_completion{true};
        nlohmann::json inputs = nlohmann::json::object();
        std::optional<std::string> output;       // context key receiving the child result
        nlohmann::json return_map = nlohmann::json::object(); // {"parent_key": "child_key"}
        int max_depth{8};
    };

    // --- 8. SCRIPT NODE ----------------------------------------------------
    class ScriptNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Script; }

        std::string language{"sel"};             // "sel" (default) | "expr" (exprtk fast path)
        Expression code;
        std::optional<std::string> output;
        std::map<std::string, std::string> bindings;  // extra local names
    };

    // --- 9. TERMINATE NODE -------------------------------------------------
    class TerminateNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Terminate; }

        std::string status{"success"};            // success | failed | cancelled
        std::optional<std::string> error_code;
        std::optional<Expression> message;
        nlohmann::json output = nlohmann::json::object(); // final session payload
    };

    // --- 10. NOOP NODE -----------------------------------------------------
    class NoopNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Noop; }

        nlohmann::json meta = nlohmann::json::object();
        nlohmann::json assign = nlohmann::json::object(); // optional literal context writes
    };

    // --- 11. CONDITION NODE (`if` sugar) -----------------------------------
    class ConditionNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Condition; }

        Expression expression;
        std::string on_true;
        std::optional<std::string> on_false;
        /// Inline bodies (`then` / `else` holding node arrays). They are hoisted
        /// into the flat node list and referenced by id, exactly like `loop.body`.
        std::vector<std::string> then_body;
        std::vector<std::string> else_body;
    };

    // --- 12. CHOICE NODE ---------------------------------------------------
    class ChoiceNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Choice; }

        Expression expression;                    // selector, e.g. `$payment_method`
        std::map<std::string, std::string> cases; // value → node id
        std::optional<std::string> default_target;
    };

    // --- 13. PARALLEL NODE -------------------------------------------------
    class ParallelNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Parallel; }

        /// One branch of the parallel section. `child_tasks` entries and
        /// `tasks` ids both land here: a branch is an ordered list of node ids
        /// executed on the worker pool, isolated except for the merged write-set.
        struct Branch {
            std::vector<std::string> node_ids;
            std::optional<std::string> label;
        };

        std::vector<Branch> branches;
        bool fail_fast{true};
        std::string merge_policy{"last_writer_wins"}; // last_writer_wins | skip_conflicts | fail_conflicts
        int max_concurrency{0};                      // 0 ⇒ unbounded (worker pool applies its own cap)
    };

    // --- 14. ACTION NODE ---------------------------------------------------
    class ActionNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Action; }

        struct SystemEvent {
            std::string event_name;
            std::optional<Expression> trigger_condition;
            std::optional<std::string> handler;   // node id to jump to on match
        };

        struct DataSource {
            std::string name;
            std::string scope{"internal"};        // internal | external
            std::string provider;
            nlohmann::json config = nlohmann::json::object();
        };

        struct TaskReference {
            std::string task_id;
            std::optional<Expression> execute_condition;
        };

        struct PromptConfig {
            Expression message;
            std::string interaction_type{"input"}; // display | input | menu
            std::optional<Expression> input_validation;
            std::optional<int> timeout_ms;
            std::optional<std::string> output;      // where the user's reply is stored
            nlohmann::json options = nlohmann::json::object(); // dynamic menu source/label/value/pagination
        };

        std::string capability;                     // optional plugin call before suspending
        std::optional<PromptConfig> prompt_config;
        std::optional<SystemEvent> on_event;
        std::vector<DataSource> data_sources;
        nlohmann::json inputs = nlohmann::json::object();
        nlohmann::json outputs = nlohmann::json::object();
        std::optional<std::string> input_variable;  // where the raw reply lands (default "input")
        std::vector<TaskReference> next_tasks;
        bool await_input{true};                     // false ⇒ capability-only action, never suspends
    };

    // --- 15. LOOP NODE -----------------------------------------------------
    class LoopNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Loop; }

        Expression collection;                      // expression yielding an array (or number)
        Expression count;                           // `count: 5` numeric loop
        Expression condition;                       // `while: "$retry < 3"` style guard
        std::string iterator{"item"};               // `$item` bound per pass
        std::string index{"index"};
        std::vector<std::string> body;              // ordered node ids making up the body
        int max_iterations{1000};
        std::string on_item_error{"fail"};          // fail | continue | retry
    };

    // --- 16. LOOP CONTROL NODE (break / continue) --------------------------
    class LoopControlNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::LoopControl; }

        std::string action{"continue"};             // break | continue
        std::optional<Expression> when;             // only act when true
        std::optional<std::string> loop;            // target loop id (default: innermost)
    };

    // --- 17. TRY NODE (try / catch / finally) ------------------------------
    class TryNode : public AstNode {
    public:
        [[nodiscard]] TaskType getType() const override { return TaskType::Try; }

        std::vector<std::string> body;
        std::vector<std::string> catch_body;
        std::vector<std::string> finally_body;
        std::string error_variable{"error"};        // `$error` in the catch scope
        /// Optional per-error-class routing: {"HTTP_STATUS_ERROR": "node_id"}
        std::map<std::string, std::string> catch_when;
    };

    // ----------------------------------------------------------------------
    /// Workflow metadata wrapper (root object form).
    struct WorkflowMetadata {
        std::string name{"sapo.workflow"};
        std::string version{"1.0"};
        std::string description;
        /// Event that starts this workflow when registered in the registry.
        std::optional<std::string> trigger_event;
        nlohmann::json trigger_input = nlohmann::json::object();
        nlohmann::json defaults = nlohmann::json::object();                  // initial context values
        nlohmann::json raw = nlohmann::json::object();
    };

    /// Full parse result: metadata + nodes + the (validated) node index.
    struct ParsedWorkflow {
        WorkflowAST nodes;
        /// First top-level node. `next` edges are materialised at parse time, so
        /// this plus a node id is all a checkpoint needs to resume a session.
        std::string entry_id;
        WorkflowMetadata metadata;
        std::vector<std::string> warnings;
        std::map<std::string, NodePtr> index;       // id → node

        [[nodiscard]] const NodePtr *find(const std::string &node_id) const {
            auto it = index.find(node_id);
            return it == index.end() ? nullptr : &it->second;
        }
    };

} // namespace sapo::parser
