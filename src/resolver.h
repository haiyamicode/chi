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

    struct ResolveContext {
        Allocator* allocator;
        array<ast::Node*> builtins;
        ResolveContext(Allocator* allocator) { this->allocator = allocator; }
    };

    struct ResolveScope {
        ChiType* parent_fn = nullptr;
        ChiType* parent_struct = nullptr;
        ChiType* value_type = nullptr;

        ResolveScope set_parent_fn(ChiType* fn) const;

        ResolveScope set_parent_struct(ChiType* struct_) const;

        ResolveScope set_value_type(ChiType* value_type) const;
    };

    class Resolver {
        ResolveContext* m_ctx;

        ast::Module* m_module = nullptr;

        ChiType* create_type(TypeId type_id);

        ChiType* create_pointer_type(ChiType* base, bool is_ref);

        ast::Node* create_node(ast::NodeType type);

        void add_primitive(const string& name, ChiType* type);

        void add_builtin(const string& name, ChiType* type);

        void create_primitives();

        void create_builtins();

        bool can_assign(ChiType* from_type, ChiType* to_type);

        void check_assignment(ast::Node* node, ChiType* from_type, ChiType* to_type);

        ChiType* to_value_type(ChiType* type);

        void resolve_struct_member(ChiType* struct_type, ast::Node* node, const ResolveScope& scope);

        void resolve_fn_call(ast::Node* node, const ResolveScope& scope, ChiTypeFn* fn, NodeList* args);

        ChiStructMember* get_struct_member(ChiType* struct_type, const string& field_name);

        bool should_resolve_fn_body(const ResolveScope& scope);

        void resolve(ast::Module* module);

        ChiType* resolve(ast::Node* node, const ResolveScope& scope);

        ChiType* _resolve(ast::Node* node, const ResolveScope& scope);

        string to_string(ChiType* type);

        template<typename... Args>
        void error(ast::Node* node, const char* format, const Args& ...args) {
            auto pos = node->token->pos;
            print("{}:{}:{}: error: {}\n", m_module->path, pos.line + 1,
                  pos.col + 1, fmt::format(format, args...));
            exit(0);
        }

    public:
        Resolver(ResolveContext* ctx);

        ResolveContext* get_context() { return m_ctx; }

        void context_init_builtins();

        ast::Node* get_builtin(const string& name);

        ChiType* node_get_type(ast::Node* node);

        void resolve(ast::Package* package);
    };

    class ScopeResolver {
        Resolver* m_resolver;
        array<Scope> m_scopes;
        Scope* m_current_scope = nullptr;

    public:
        ScopeResolver(Resolver* resolver);

        Scope* get_scope() { return m_current_scope; }

        bool declare_symbol(const string& name, ast::Node* node);

        ast::Node* find_symbol(const string& name);

        Scope* push_scope(ast::Node* owner);

        void pop_scope();
    };
}
