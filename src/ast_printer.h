/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "ast.h"
#include <string_view>

using namespace cx::ast;

namespace cx {
class AstPrinter {
    Node *m_root = nullptr;
    bool m_use_resolved_info = false;
    int m_indent = 0;
    bool m_suppress_func_keyword = false;
    bool m_suppress_construct_type = false;
    bool m_strip_arrow_return = false;
    Node *m_fn_return_type = nullptr;
    array<Comment> *m_comments = nullptr;
    size_t m_comment_idx = 0;
    fmt::memory_buffer *m_buffer = nullptr;
    int m_current_column = 0;
    int m_max_line_length = 100;

    void emit(std::string_view text) {
        if (m_buffer) {
            fmt::format_to(std::back_inserter(*m_buffer), "{}", text);
        } else {
            fmt::print("{}", text);
        }
        for (char c : text) {
            if (c == '\n') {
                m_current_column = 0;
            } else {
                m_current_column++;
            }
        }
    }

    template <typename... Args> void emit_fmt(fmt::format_string<Args...> fmt, Args &&...args) {
        emit(fmt::format(fmt, std::forward<Args>(args)...));
    }

  public:
    AstPrinter(Node *root, array<Comment> *comments = nullptr, bool use_resolved_info = false) {
        m_root = root;
        m_comments = comments;
        m_use_resolved_info = use_resolved_info;
    }

    void print_ast();

    // Format the AST and return the result as a string.
    string format_to_string();

    void print_node(Node *root);

    void print_indent(int level);

    void print_node_list(array<Node *> *list);

    void print_declspec(DeclSpec *declspec);

    void print_struct_members(StructDecl &data);

    void print_destructure_pattern(Node *node);

    // Returns true if there's at least one blank line between two nodes in the original source.
    // Used to preserve user-intentional blank lines during formatting.
    bool has_blank_line_between(Node *prev, Node *next);

    // Get the first token of a node (start_token or token).
    Token *first_token(Node *node);

    // Get the last token of a node (end_token or token).
    Token *last_token(Node *node);

    // Emit all comments whose source offset is before the given position.
    // Own-line comments get indent + newline; inline comments get printed with spaces.
    void flush_comments_before(Pos before_pos);

    // Emit trailing comments on the same line as the given node's last token.
    void flush_trailing_comment(Node *node);

    // Structural comparison of two type nodes (for safe construct collapsing).
    bool types_match(Node *a, Node *b);

    // Helper to format a node to a string (for length calculation).
    string format_node_to_string(Node *node);

    // Emit a list of nodes with automatic wrapping if it exceeds max_line_length.
    // Returns true if wrapped, false if emitted on single line.
    bool emit_wrapped_list(array<Node *> *items, const char *open, const char *close,
                           const char *separator, int extra_indent = 1);

    // Emit construct body (spread + items + field_inits) with wrapping support.
    void emit_construct_body(ConstructExpr &data);

    bool should_arrow_body_use_block_form(Node *value);
    bool should_suppress_construct_type_in_value_context(Node *value);
    void print_arrow_body_value(Node *value);

    bool should_semantically_shorthand_construct_type(Node *node);
    bool should_semantically_collapse_case_clause(Node *node);
};

void print_ast(Node *root);
} // namespace cx
