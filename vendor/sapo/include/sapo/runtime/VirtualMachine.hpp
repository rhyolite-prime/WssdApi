//
//  Sapo Engine — the embeddable facade (implementation_plan_2.md T4.1, T4.2).
//
//  `VirtualMachine` is what an embedder (the CLI, `sapo-server`, a Drogon
//  handler, a unit test) actually talks to:
//
//      VirtualMachine vm;
//      vm.addBlueprintFile("flow.json");
//      vm.start();                                  // validate + wire everything
//      auto outcome = vm.startSession("flow", {{"phone", "233…"}});
//      …
//      vm.resumeSession(outcome.session_id, {{"input", "1"}});
//
//  Sessions are addressed by id and live in the state store, not in this object:
//  `resumeSession` works after a process restart, and the object keeps no
//  per-session memory of its own. Suspension never parks a thread — timers and
//  event subscriptions do the waking (`tick()`/the scheduler thread).
//
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "runtime/Interpreter.hpp"
#include "runtime/StateStore.hpp"
#include "runtime/TaskServices.hpp"

namespace sapo::runtime {

    /// CLI/API-facing result: the report trimmed to what a caller branches on.
    struct ExecutionOutcome {
        std::string session_id;
        std::string execution_id;
        std::string workflow_id;
        std::string status;              // completed | terminated | awaiting_input | suspended | failed | cancelled
        bool ok{true};
        nlohmann::json output = nlohmann::json::object();
        nlohmann::json context = nlohmann::json::object();
        nlohmann::json prompt = nlohmann::json::object();
        std::string error;
        std::string error_code;
        std::string error_node;
        nlohmann::json error_data;
        std::string cursor;
        std::vector<std::string> warnings;
        size_t node_visits{0};
        int64_t elapsed_ms{0};

        [[nodiscard]] static ExecutionOutcome from(const ExecutionReport &report);
        [[nodiscard]] nlohmann::json toJson() const;
    };

    /// What `GET /sessions/{id}` exposes.
    struct SessionSnapshot {
        std::string session_id;
        std::string execution_id;
        std::string blueprint_id;
        std::string status;
        std::string cursor;
        nlohmann::json pending = nlohmann::json::object();
        nlohmann::json context = nlohmann::json::object();
        nlohmann::json output = nlohmann::json::object();   // `terminate.output`, once the session has one
        int64_t created_ms{0};
        int64_t updated_ms{0};
        size_t node_visits{0};
        std::string error;
        bool resumable{false};

        [[nodiscard]] static SessionSnapshot from(const SessionCheckpoint &checkpoint);
        [[nodiscard]] nlohmann::json toJson() const;
    };

    struct StartSessionOptions {
        std::string session_id;          // empty ⇒ generated
        std::string correlation_id;
        std::string start_node;          // entry override
        bool persist{true};
        size_t max_node_visits{0};       // 0 ⇒ service default
    };

    class VirtualMachine {
    public:
        explicit VirtualMachine(TaskServices services = TaskServices::defaults());
        ~VirtualMachine();

        VirtualMachine(VirtualMachine &&) noexcept;
        VirtualMachine &operator=(VirtualMachine &&) noexcept;
        VirtualMachine(const VirtualMachine &) = delete;
        VirtualMachine &operator=(const VirtualMachine &) = delete;

        [[nodiscard]] TaskServices &services() { return m_services; }
        [[nodiscard]] const TaskServices &services() const { return m_services; }
        [[nodiscard]] Interpreter &interpreter() { return *m_interpreter; }
        [[nodiscard]] WorkflowRegistry &workflows() { return *m_services.workflows; }
        [[nodiscard]] const WorkflowRegistry &workflows() const { return *m_services.workflows; }

        /// `sapo-config.json` (provider config + blueprint directories). Set before `start()`.
        void setConfigPath(std::string path) { m_config_path = std::move(path); }
        [[nodiscard]] const std::string &configPath() const { return m_config_path; }
        /// Where blueprints that start on their own (event triggers, schedules) live.
        void setWorkflowDirectory(std::string directory) { m_workflow_directory = std::move(directory); }

        /**
         * @brief Loads config and blueprints, runs the startup audit, installs the
         *        subflow/event/timer runners and (if a scheduler exists) starts it.
         * @return problems that must be fixed; empty means the engine is ready.
         */
        std::vector<std::string> start();
        void stop();
        [[nodiscard]] bool running() const { return m_started; }

        /// Parses and registers one blueprint. Returns the workflow id.
        std::string addBlueprintText(const std::string &text, const std::string &origin = "<inline>");
        std::string addBlueprintFile(const std::string &path);
        /// Registers every `*.json` under `directory`; problems are collected, not thrown.
        size_t addBlueprintDirectory(const std::string &directory);

        [[nodiscard]] std::vector<std::string> validateAll() const;

        ExecutionOutcome startSession(const std::string &workflow_id, const nlohmann::json &input = {},
                                      StartSessionOptions options = {});
        /// Runs a blueprint that is not in the registry (tests, `sapoc run file.json`).
        ExecutionOutcome runBlueprint(const nlohmann::json &blueprint, const nlohmann::json &input = {},
                                      StartSessionOptions options = {});
        ExecutionOutcome resumeSession(const std::string &session_id, const nlohmann::json &input = {});
        bool cancelSession(const std::string &session_id, const std::string &reason = "cancelled by caller");

        [[nodiscard]] std::optional<SessionSnapshot> session(const std::string &session_id) const;
        [[nodiscard]] std::vector<SessionSnapshot> sessions() const;
        [[nodiscard]] size_t suspendedSessionCount() const;

        /// Publishes into the event bus; workflow triggers and waiting sessions react.
        void publishEvent(const std::string &name, const nlohmann::json &payload = nlohmann::json::object());
        void publishEvent(Event event);

        /// Fires due timers and cron jobs (the CLI/tests use this instead of sleeping).
        size_t tick();
        size_t tickUntil(int64_t now_ms);
        /// Runs the scheduler on its own thread until `stop()`.
        void startBackgroundTick(std::chrono::milliseconds interval = std::chrono::milliseconds(250));

        [[nodiscard]] nlohmann::json metrics() const;
        [[nodiscard]] nlohmann::json traceFor(const std::string &execution_id) const;
        [[nodiscard]] nlohmann::json describe() const;

    private:
        void ensureStarted();
        [[nodiscard]] nlohmann::json capabilitiesSummary() const;
        [[nodiscard]] nlohmann::json dataSourceSummary() const;

        TaskServices m_services;
        std::unique_ptr<Interpreter> m_interpreter;
        std::string m_config_path;
        std::string m_workflow_directory;
        std::vector<std::string> m_problems;
        bool m_started{false};
        bool m_starting{false};
    };

} // namespace sapo::runtime
