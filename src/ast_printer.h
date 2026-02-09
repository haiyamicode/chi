/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "ast.h"

using namespace cx::ast;

namespace cx {
class AstPrinter {
    Node *m_root = nullptr;
    int m_indent = 0;
    bool m_suppress_func_keyword = false;
    bool m_suppress_construct_type = false;
    Node *m_fn_return_type = nullptr;
    array<Comment> *m_comments = nullptr;
    size_t m_comment_idx = 0;
    fmt::memory_buffer *m_buffer = nullptr;

    // Shadows global fmt::print — routes output to buffer when set, stdout otherwise.
    template <typename... Args> void emit(fmt::format_string<Args...> fmt, Args &&...args) {
        if (m_buffer) {
            fmt::format_to(std::back_inserter(*m_buffer), fmt, std::forward<Args>(args)...);
        } else {
            fmt::print(fmt, std::forward<Args>(args)...);
        }
    }

  public:
    AstPrinter(Node *root, array<Comment> *comments = nullptr) {
        m_root = root;
        m_comments = comments;
    }

    void print_ast();

    // Format the AST and return the result as a string.
    string format_to_string();

    void print_node(Node *root);

    void print_indent(int level);

    void print_node_list(array<Node *> *list);

    void print_declspec(DeclSpec *declspec);

    void print_struct_members(StructDecl &data);

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
};

void print_ast(Node *root);
} // namespace cx
