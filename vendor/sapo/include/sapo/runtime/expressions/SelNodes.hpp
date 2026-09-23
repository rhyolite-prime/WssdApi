//
//  SEL internals: AST node layout (shared by the parser and evaluator only).
//
#pragma once

#include "runtime/expressions/Sel.hpp"

namespace sapo::expr {

    enum class NodeKind {
        Literal,
        Variable,
        Member,
        Index,
        Call,
        MethodCall,
        Unary,
        Binary,
        Ternary,
        Coalesce,
        ArrayLiteral,
        ObjectLiteral
    };

    struct Node {
        NodeKind kind{NodeKind::Literal};
        std::string name;             // variable / field / function / operator
        json literal;                 // Literal payload
        NodePtr left, right, extra;   // operand slots
        std::vector<NodePtr> args;    // call arguments / array items / object values
        std::vector<std::string> keys; // object literal keys
        int index{0};                 // static index
        bool has_static_index{false};
        bool optional{false};         // `?.` access (missing → null, never an error)
        bool explicit_dollar{false};  // `$name` (must resolve) vs bare `name`
    };

} // namespace sapo::expr
