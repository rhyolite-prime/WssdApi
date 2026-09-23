//
// SapoEngineService.h — process-wide owner of the Sapo VirtualMachine.
//
// One engine serves every USSD turn in the process: sessions live in the
// state store (Redis/File), not in this object, so concurrent turns for
// different subscribers never share mutable state. Blueprint registration
// is the only mutating path and is serialized on registryMutex_; steady
// state turns only read the registry.
//
// Threading contract: every method that touches the engine below the
// `// --- blocking` line performs socket/file I/O and MUST be invoked off
// Drogon's IO threads (see BlockingRunner, used by UssdSessionOrchestrator).
//

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/VirtualMachine.hpp"

#include "SapoSettings.h"

namespace wssd_api::sapo_host {

class SapoEngineService {
  public:
    static SapoEngineService &instance() {
        static SapoEngineService service;
        return service;
    }

    SapoEngineService(const SapoEngineService &) = delete;
    SapoEngineService &operator=(const SapoEngineService &) = delete;

    /// Builds TaskServices (Drogon log sink, Drogon HTTP transport, state
    /// store, provider config) and the VirtualMachine. Idempotent: a second
    /// call is a no-op that returns true. Returns false on fatal setup error.
    bool configure(const SapoSettings &settings);

    /// Loads blueprints, runs the startup audit and starts the background
    /// scheduler tick. Returns the *fatal* problems (empty = ready); engine
    /// warnings (validator WARNINGs such as control-flow cycle reports) are
    /// logged and tolerated, matching the engine itself, which marks the VM
    /// started regardless. Callers log every returned entry as an error.
    std::vector<std::string> start();

    /// Stops the scheduler tick and the engine. Safe to call repeatedly.
    void stop();

    [[nodiscard]] bool running() const {
        return started_.load();
    }

    [[nodiscard]] const SapoSettings &settings() const {
        return settings_;
    }

    /// Start-problem severity: VirtualMachine::start() returns validator
    /// warnings (cycle reports, unused-field notes) in the same vector as
    /// fatal errors, distinguished only by a "WARNING" tag in the text.
    /// Inline so unit tests can cover the contract without linking the engine.
    static bool isWarningProblem(const std::string &problem) {
        return problem.find("WARNING") != std::string::npos;
    }

    [[nodiscard]] bool hasWorkflow(const std::string &workflowId) const;

    /// Registers `blueprintJson` under `workflowId` (the blueprint's own
    /// `name` is rewritten so registry ids stay deterministic). An empty
    /// blueprint only verifies the id is already registered (startup-loaded
    /// files). Re-registration is content-hashed: unchanged blueprints are
    /// a cheap no-op, changed ones replace the previous entry.
    /// Returns the workflow id, or "" with `error` populated.
    std::string ensureBlueprint(const std::string &workflowId,
                                const std::string &blueprintJson,
                                std::string &error);

    // --- blocking: call off the IO threads --------------------------------

    sapo::runtime::ExecutionOutcome startUssdSession(const std::string &workflowId,
                                                     const nlohmann::json &input,
                                                     const std::string &sapoSessionId,
                                                     const std::string &correlationId);

    sapo::runtime::ExecutionOutcome resumeUssdSession(const std::string &sapoSessionId,
                                                      const nlohmann::json &input);

    /// One USSD turn: resumes the Sapo session when it is resumable,
    /// otherwise starts `workflowId` under the deterministic session id.
    /// A resume that fails (e.g. checkpoint expired between lookup and
    /// resume) is retried once as a fresh start instead of failing the
    /// subscriber. Never throws: engine exceptions become failed outcomes.
    sapo::runtime::ExecutionOutcome executeUssdTurn(const std::string &workflowId,
                                                     const std::string &sapoSessionId,
                                                     nlohmann::json baseInput,
                                                     const std::string &rawInput,
                                                     const std::string &dialCode,
                                                     const std::string &correlationId);

    [[nodiscard]] std::optional<sapo::runtime::SessionSnapshot> findSession(
        const std::string &sapoSessionId) const;

    bool cancelSession(const std::string &sapoSessionId, const std::string &reason);

    [[nodiscard]] nlohmann::json metrics() const;

  private:
    SapoEngineService() = default;
    ~SapoEngineService();

    static sapo::runtime::ExecutionOutcome failedOutcome(const std::string &workflowId,
                                                          const std::string &sapoSessionId,
                                                          const std::string &code,
                                                          const std::string &message,
                                                          const std::string &node);

    SapoSettings settings_;
    std::unique_ptr<sapo::runtime::VirtualMachine> vm_;
    std::unordered_map<std::string, std::size_t> blueprintHashes_;
    mutable std::mutex registryMutex_;
    std::atomic<bool> started_{false};
    bool configured_ = false;
};

}  // namespace wssd_api::sapo_host
