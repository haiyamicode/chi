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

    struct SystemTypes {
        ChiType* int_;
        ChiType* int64;
        ChiType* float_;
        ChiType* double_;
        ChiType* void_;
        ChiType* char_;
        ChiType* str_lit;
        ChiType* any;
        ChiType* string;
        ChiType* bool_;
    };

    struct ResolveContext {
        Allocator* allocator;
        SystemTypes system_types;
        array<ast::Node*> builtins;
        map<ChiType*, ChiType*> array_types;
        map<ChiType*, ChiType*> pointer_types;

        ResolveContext(Allocator* allocator) { this->allocator = allocator; }
    };

    struct ResolveScope {
        bool skip_fn_bodies = false;
        ChiType* parent_fn = nullptr;
        ChiType* parent_struct = nullptr;
        ChiType* value_type = nullptr;
        int64_t next_enum_value = 0;
        ast::Node* parent_loop = nullptr;

        ResolveScope set_parent_fn(ChiType* fn) const;

        ResolveScope set_parent_struct(ChiType* struct_) const;

        ResolveScope set_value_type(ChiType* value_type) const;

        ResolveScope set_parent_loop(ast::Node* loop) const;
    };

    class Resolver {
        ResolveContext* m_ctx;

        ast::Module* m_module = nullptr;

        ChiType* create_type(TypeId type_id);

        ChiType* create_type_symbol(optional<string> name, ChiType* type);

        ChiType* create_pointer_type(ChiType* elem);

        ChiType* create_int_type(int bit_count, bool is_unsigned);

        ChiType* create_float_type(int bit_count);

        ast::Node* create_node(ast::NodeType type);

        ast::Node* add_primitive(const string& name, ChiType* type);

        void add_builtin_fn(const string& name, ChiType* type, ast::BuiltinId builtin_id);

        void create_primitives();

        void create_builtins();

        bool can_assign(ChiType* from_type, ChiType* to_type);

        bool is_same_type(ChiType* a, ChiType* b);

        void check_assignment(ast::Node* value, ChiType* from_type, ChiType* to_type);

        void check_binary_op(ast::Node* node, TokenType op_type, ChiType* type);

        bool is_addressable(ast::Node* node);

        void check_cast(ast::Node* value, ChiType* from_type, ChiType* to_type);

        ChiType* resolve_value(ast::Node* node, ResolveScope& scope);

        ChiStructMember* resolve_struct_member(ChiType* struct_type, ast::Node* node, ResolveScope& scope);

        void resolve_struct_embed(ChiType* struct_type, ast::Node* node, ResolveScope& parent_scope);

        void resolve_vtable(ChiType* base_type, ChiType* derived_type, ast::Node* embed_node);

        void resolve_fn_call(ast::Node* node, ResolveScope& scope, ChiTypeFn* fn, NodeList* args);

        void type_placeholders_sub_each(TypeList* input, ChiTypeSubtype* subs, TypeList* output);

        ChiStructMember* get_struct_member(ChiType* struct_type, const string& field_name);

        bool should_resolve_fn_body(ResolveScope& scope);

        void resolve(ast::Module* module);

        ChiType* resolve(ast::Node* node, ResolveScope& scope);

        ChiType* _resolve(ast::Node* node, ResolveScope& scope);

        ConstantValue resolve_constant_value(ast::Node* node);

        SystemTypes* get_system_types() { return &m_ctx->system_types; }

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

        string to_string(ChiType* type);

        ChiType* to_value_type(ChiType* type);

        ChiType* get_subtype(ChiType* generic, TypeList* type_args);

        ChiType* get_pointer_type(ChiType* elem);

        ChiType* get_array_type(ChiType* elem);

        ChiType* resolve_subtype(ChiType* subtype);

        void resolve(ast::Package* package);

        bool type_is_int(ChiType* type) {
            return type->id == TypeId::Int || type->id == TypeId::Bool || type->id == TypeId::Pointer;
        }

        ChiType* type_placeholders_sub(ChiType* type, ChiTypeSubtype* subs);
    };

    class ScopeResolver {
        Resolver* m_resolver;
        array<box<Scope>> m_scopes;
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
