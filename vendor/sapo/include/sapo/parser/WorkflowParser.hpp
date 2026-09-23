//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//
//  Sapo DSL blueprint parser (grammar v1).
//
//  Responsibilities beyond "JSON → AST":
//    • canonical field names, with documented legacy aliases
//    • compile every expression once, so syntax errors are parse-time errors
//    • synthesize node ids, capture workflow metadata (`name`/`version`/…)
//    • hand the validated graph to `BlueprintValidator` (ids, jump targets,
//      reachability, cycles) and reject unknown capability commands
//
#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "parser/AstNodes.hpp"

namespace sapo::capabilities {
    class CapabilityRegistry;
}

namespace sapo::parser {

    struct ParseOptions {
        /// When set, `command`/`action` targets are checked against the registry
        /// so an unknown capability is a parse-time error (never a silent no-op).
        const sapo::capabilities::CapabilityRegistry *capabilities{nullptr};
        /// Reject nodes that are unreachable / have dangling jump targets.
        bool validate{true};
        /// Fail instead of warn on unknown node fields (helps catch typos).
        bool strict_fields{false};
        /// Maximum nodes accepted (defence against absurd blueprints).
        size_t max_nodes{10000};
    };

    class WorkflowParser {
    public:
        /// Legacy entry point: JSON array of nodes (or object root) → AST.
        static WorkflowAST parse(const std::string &json_content);

        /// Full entry point: metadata + nodes + validated index.
        static ParsedWorkflow parseWorkflow(const std::string &json_content, const ParseOptions &options = {});
        static ParsedWorkflow parseWorkflowJson(const nlohmann::json &document, const ParseOptions &options = {});

        /// Parses a single node object (used by plugin manifests / tests).
        static NodePtr parseSingleNode(const nlohmann::json &node_json);
    };

} // namespace sapo::parser
