//
//  Sapo Engine — control-flow vocabulary (implementation_plan_2.md T1.5).
//
//  The interpreter's only signalling mechanism. Every node execution returns a
//  `ControlSignal`; there are no magic variable names (`__SYS_JUMP_TO__` and
//  friends are gone), no exceptions for normal routing, and no thread-blocking
//  "wait" that keeps a worker parked.
//
#pragma once

#include <optional>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

namespace sapo::runtime {

    /// Continue with the node's normal successor.
    struct Continue {};

    /// Jump to a named node (still inside the current body frame when possible).
    struct JumpTo {
        std::string target;
    };

    /// Pause this session and persist it. The three flavours (prompt, timer,
    /// event) share one signal so the VM's suspend path is a single code path.
    struct SuspendRequest {
        std::string session_id;
        std::string node_id;
        std::string reason{"input"};            // input | timer | event
        nlohmann::json prompt;                  // {message, interaction_type, input_validation}
        std::string input_variable{"input"};    // where the reply is written
        std::optional<int64_t> resume_at_ms;    // timer deadline
        std::optional<int64_t> timeout_ms;
        std::string resume_node;                // node to run when woken
        std::optional<std::string> event_name;  // awaited event (on_event / trigger)
        std::optional<std::string> handler_node; // jump target when the event arrives
        nlohmann::json data = nlohmann::json::object();
    };

    /// Stop the workflow here; `payload` is what the caller gets back.
    struct Terminate {
        std::string status{"success"};          // success | failed | cancelled
        nlohmann::json payload = nlohmann::json::object();
        std::optional<std::string> error_code;
        std::optional<std::string> message;
    };

    /// Unwind to the end of the enclosing (or named) loop.
    struct LoopBreak {
        std::optional<std::string> loop;
    };

    /// Finish the current iteration of the enclosing (or named) loop.
    struct LoopContinue {
        std::optional<std::string> loop;
    };

    using ControlSignal = std::variant<Continue, JumpTo, SuspendRequest, Terminate, LoopBreak, LoopContinue>;

    namespace signal {

        template<typename T>
        [[nodiscard]] inline bool is(const ControlSignal &signal) {
            return std::holds_alternative<T>(signal);
        }

        template<typename T>
        [[nodiscard]] inline const T *get(const ControlSignal &signal) {
            return std::get_if<T>(&signal);
        }

        [[nodiscard]] inline std::string describe(const ControlSignal &signal) {
            if (is<Continue>(signal)) return "continue";
            if (const auto *jump = get<JumpTo>(signal)) return "jump:" + jump->target;
            if (is<SuspendRequest>(signal)) return "suspend";
            if (const auto *terminate = get<Terminate>(signal)) return "terminate:" + terminate->status;
            if (is<LoopBreak>(signal)) return "break";
            if (is<LoopContinue>(signal)) return "continue-loop";
            return "unknown";
        }

    } // namespace signal

} // namespace sapo::runtime
