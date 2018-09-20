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
        Node* m_root = nullptr;
        int m_indent = 0;

    public:
        AstPrinter(Node* root) { m_root = root; }

        void print_ast();

        void print_node(Node* root);

        void print_indent(int level);

        void print_node_list(array<Node*>* list);
    };

    void print_ast(Node* root);
}