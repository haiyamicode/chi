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
    array<Comment> *m_comments = nullptr;
    size_t m_comment_idx = 0;

  public:
    AstPrinter(Node *root, array<Comment> *comments = nullptr) {
        m_root = root;
        m_comments = comments;
    }

    void print_ast();

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
};

void print_ast(Node *root);
} // namespace cx
