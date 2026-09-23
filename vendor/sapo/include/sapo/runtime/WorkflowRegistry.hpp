//
//  Sapo Engine — workflow registry (implementation_plan_2.md T2.5).
//
//  Blueprints are loaded once, validated, and addressable by id so that
//    • `subflow` nodes can start them (with input/output binding and a depth cap),
//    • `schedule` jobs and event triggers can start them, and
//    • the service can run many workflows from one process.
//
//  The registry deliberately does *not* know how to execute: a `Runner` callback
//  is installed by the VM, which keeps the dependency graph acyclic.
//
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "parser/WorkflowParser.hpp"
#include "runtime/Clock.hpp"
#include "runtime/EventBus.hpp"

namespace sapo::runtime {

    struct WorkflowEntry {
        std::string id;
        parser::ParsedWorkflow workflow;
        std::string origin;             // file path or "<inline>"
        int64_t loaded_ms{0};
    };

    struct ChildRunRequest {
        std::string workflow_id;
        nlohmann::json input = nlohmann::json::object();
        std::string parent_session;
        std::string correlation_id;
        size_t depth{0};
        bool wait_for_completion{true};
        std::string node_id;            // the subflow node (error context)
        std::string output_key;         // where the parent stores the result
        nlohmann::json return_map = nlohmann::json::object(); // {parent_key: child_key}
    };

    struct ChildRunResult {
        std::string session_id;
        std::string status;             // completed | failed | waiting | …
        nlohmann::json output = nlohmann::json::object();
        std::string error;
    };

    class WorkflowRegistry {
    public:
        using Runner = std::function<ChildRunResult(const ChildRunRequest &)>;

        explicit WorkflowRegistry(parser::ParseOptions options = {}, ClockPtr clock = defaultClock());

        /// Hosts (and `sapoc --strict`) may tighten parsing after construction.
        void setParseOptions(parser::ParseOptions options) { m_options = options; }
        [[nodiscard]] const parser::ParseOptions &parseOptions() const { return m_options; }

        /// Registers an already-parsed workflow. Id defaults to `metadata.name`.
        std::string add(parser::ParsedWorkflow workflow, std::string id = {}, std::string origin = {});
        std::string loadJsonText(const std::string &text, std::string id = {}, std::string origin = "<inline>");
        std::string loadFile(const std::string &path);
        /// Loads every `*.json` in a directory. Returns the number loaded and
        /// appends per-file problems (a broken file never hides the others).
        size_t loadDirectory(const std::string &directory, std::vector<std::string> &problems);

        bool remove(const std::string &id);
        void clear();

        [[nodiscard]] const parser::ParsedWorkflow *find(const std::string &id) const;
        [[nodiscard]] const WorkflowEntry *entry(const std::string &id) const;
        [[nodiscard]] std::vector<std::string> ids() const;
        [[nodiscard]] size_t size() const { return m_entries.size(); }
        [[nodiscard]] bool empty() const { return m_entries.empty(); }

        void setRunner(Runner runner) { m_runner = std::move(runner); }
        [[nodiscard]] bool hasRunner() const { return static_cast<bool>(m_runner); }

        /// Workflows whose root `trigger.event` matches `event`.
        [[nodiscard]] std::vector<std::string> workflowsTriggeredBy(const std::string &event) const;

        /// Subscribes every declared trigger event on the bus. `start` is invoked
        /// with (workflow id, event) — the VM uses it to spawn a session.
        /// Returns subscription ids so a caller can undo them.
        std::vector<EventBus::SubscriptionId> installEventTriggers(
            EventBus &bus, const std::function<void(const std::string &, const Event &)> &start);

        /// Subflow dispatch: unknown id / missing runner / recursion limit are all
        /// reported as `SapoError` (they must never start the wrong blueprint).
        [[nodiscard]] ChildRunResult runChild(const ChildRunRequest &request) const;

        void setMaxDepth(size_t depth) { m_max_depth = depth == 0 ? 1 : depth; }
        [[nodiscard]] size_t maxDepth() const { return m_max_depth; }

        /// Reports references to workflows that are not registered (subflow ids).
        [[nodiscard]] std::vector<std::string> unresolvedReferences() const;

    private:
        std::map<std::string, WorkflowEntry> m_entries;
        parser::ParseOptions m_options;
        ClockPtr m_clock;
        Runner m_runner;
        size_t m_max_depth{8};
    };

} // namespace sapo::runtime
