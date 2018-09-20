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
    struct Allocator {
        virtual ast::Node* create_node(ast::NodeType type) = 0;

        virtual ChiType* create_type(TypeId type_id) = 0;
    };

    class Resolver {
        Allocator* m_allocator;
        map<ast::Node*, ChiType*> m_types;
        ast::Module* m_module = nullptr;
        ChiTypeFn* m_current_fn = nullptr;

        array<ast::Node*> m_builtins;

        ChiType* create_type(TypeId type_id);

        ast::Node* create_node(ast::NodeType type);

        void add_primitive(const string& name, ChiType* type);

        void add_builtin(const string& name, ChiType* type);

        void init_primitives();

        void init_builtins();

        bool can_assign(ChiType* from_type, ChiType* to_type);

        void check_assignment(ast::Node* node, ChiType* from_type, ChiType* to_type);

        void resolve(ast::Module* module);

        ChiType* resolve(ast::Node* node);

        ChiType* _resolve(ast::Node* node);

        string to_string(ChiType* type);

        template<typename... Args>
        void error(ast::Node* node, const char* format, const Args& ...args) {
            auto pos = node->token->pos;
            print("{}:{}:{}: error: {}\n", m_module->path, pos.line + 1,
                  pos.col + 1, fmt::format(format, args...));
            exit(0);
        }

    public:
        Resolver(Allocator* ctx);

        ast::Node* get_builtin(const string& name);

        ChiType* get_node_type(ast::Node* node);

        void resolve(ast::Package* package);
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
