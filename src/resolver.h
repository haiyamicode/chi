/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "ast.h"
#include "sema.h"

namespace cx {
    class Resolver {
        array<ast::Node> m_primitives;
    public:
        Resolver();

        void add_primitive(const string& name);

        void init_primitives();

        ast::Node* get_primitive(const string& name);
    };

    class ModuleResolver {
        Resolver* m_resolver;
        array<Scope> m_scopes;
        Scope* m_current_scope = nullptr;

    public:
        ModuleResolver(Resolver* resolver);

        Scope* get_scope() { return m_current_scope; }

        bool declare_symbol(const string& name, ast::Node* node);

        ast::Node* find_symbol(const string& name);

        Scope* push_scope();

        void pop_scope();
    };
}
