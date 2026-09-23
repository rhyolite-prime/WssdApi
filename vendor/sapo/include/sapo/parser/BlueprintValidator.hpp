//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//
//  Static blueprint validation (implementation_plan_2.md T1.4).
//
//  The parser checks that a node is *well-formed*; the validator checks that the
//  graph is *coherent*: unique ids, every jump target exists, nothing is
//  unreachable, cycles are reported, and structural rules (a `break` outside a
//  loop, a schedule pointing at itself, a subflow that recurses) are refused.
//  Running this before execution is what turns "engine threw at node 47" into a
//  message that names the node and the field.
//
#pragma once

#include <string>
#include <vector>

#include "parser/AstNodes.hpp"

namespace sapo::parser {

    struct ParseOptions;

    struct ValidationIssue {
        enum class Level { Warning, Error };

        Level level{Level::Error};
        std::string node_id;
        std::string message;

        [[nodiscard]] bool is_error() const { return level == Level::Error; }
        [[nodiscard]] std::string describe() const {
            return std::string(level == Level::Error ? "ERROR" : "WARNING") +
                   (node_id.empty() ? std::string(" — ") : " [" + node_id + "] ") + message;
        }
    };

    class BlueprintValidator {
    public:
        /// Collects every issue without throwing (useful for `sapoc validate`).
        [[nodiscard]] static std::vector<ValidationIssue> collect(const ParsedWorkflow &workflow,
                                                                   const ParseOptions &options);

        /// Validates and appends human-readable lines to `warnings`. Throws
        /// `SapoError(ErrorCode::Parse)` on the first Error-level issue.
        static void validate(ParsedWorkflow &workflow, const ParseOptions &options, std::vector<std::string> &warnings);

        /// Every node id this node can transfer control to (bodies, jumps,
        /// handlers, branches, subflow bodies). Used by the validator, the VM and
        /// the editor tooling.
        [[nodiscard]] static std::vector<std::string> outgoingTargets(const AstNode &node);

        /// Ids reachable from `entry_id` following `outgoingTargets`.
        [[nodiscard]] static std::vector<std::string> reachableFrom(const ParsedWorkflow &workflow,
                                                                     const std::string &entry_id);
    };

} // namespace sapo::parser
