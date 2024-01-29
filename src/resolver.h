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
    virtual ast::Node *create_node(ast::NodeType type) = 0;
    virtual ChiType *create_type(TypeKind kind) = 0;
    virtual Scope *create_scope(Scope *parent) = 0;
    virtual Token *create_token() = 0;
};

struct SystemTypes {
    ChiType *int_ = nullptr;
    ChiType *int64 = nullptr;
    ChiType *uint8 = nullptr;
    ChiType *float_ = nullptr;
    ChiType *double_ = nullptr;
    ChiType *void_ = nullptr;
    ChiType *char_ = nullptr;
    ChiType *str_lit = nullptr;
    ChiType *any = nullptr;
    ChiType *string = nullptr;
    ChiType *bool_ = nullptr;
    ChiType *array = nullptr;
    ChiType *optional = nullptr;
    ChiType *box = nullptr;
    ChiType *result = nullptr;
    ChiType *error = nullptr;
};

struct ResolveContext {
    Allocator *allocator = nullptr;
    array<ast::Node *> builtins = {};
    SystemTypes system_types = {};
    array<ast::Node *> internal_methods = {};
    map<ChiType *, ChiType *> array_of = {};
    map<ChiType *, ChiType *> pointer_of[(int)TypeKind::__COUNT] = {};
    optional<ErrorHandler> error_handler = {};
    map<string, ChiType *> composite_types = {};

    ResolveContext(Allocator *allocator) { this->allocator = allocator; }
};

struct ResolveScope {
    bool skip_fn_bodies = false;
    ChiType *parent_fn = nullptr;
    ChiType *parent_struct = nullptr;
    ChiType *value_type = nullptr;
    int64_t next_enum_value = 0;
    ast::Node *parent_loop = nullptr;
    bool is_escaping = false;

    ResolveScope set_parent_fn(ChiType *fn) const;

    ResolveScope set_parent_struct(ChiType *struct_) const;

    ResolveScope set_value_type(ChiType *value_type) const;

    ResolveScope set_parent_loop(ast::Node *loop) const;

    ResolveScope set_is_escaping(bool is_escaping) const;
};

class Resolver {
    ResolveContext *m_ctx = nullptr;
    map<ast::Node *, ChiType *> m_tmods = {};
    ast::Module *m_module = nullptr;

    void set_tmod(ast::Node *iden, ChiType *type) { m_tmods[iden->data.identifier.decl] = type; }

    void unset_tmod(ast::Node *iden) { m_tmods.unset(iden->data.identifier.decl); }

    LANG_FLAG get_lang_flags() { return m_module->get_lang_flags(); }

    bool is_managed() { return has_lang_flag(get_lang_flags(), LANG_FLAG_MANAGED); }

    ChiType *create_type(TypeKind kind);

    ChiType *create_type_symbol(optional<string> name, ChiType *type);

    ChiType *create_pointer_type(ChiType *elem, TypeKind kind);

    ChiType *create_int_type(int bit_count, bool is_unsigned);

    ChiType *create_float_type(int bit_count);

    ast::Node *create_node(ast::NodeType type);

    ast::Node *add_primitive(const string &name, ChiType *type);

    bool can_assign(ChiType *from_type, ChiType *to_type);

    bool is_same_type(ChiType *a, ChiType *b);

    TypeKind get_sigil_type_kind(ast::SigilKind sigil);

    void check_assignment(ast::Node *value, ChiType *from_type, ChiType *to_type);

    void check_binary_op(ast::Node *node, TokenType op_type, ChiType *type);

    bool is_addressable(ast::Node *node);

    void check_cast(ast::Node *value, ChiType *from_type, ChiType *to_type);

    ChiType *resolve_value(ast::Node *node, ResolveScope &scope);

    ChiStructMember *resolve_struct_member(ChiType *struct_type, ast::Node *node,
                                           ResolveScope &scope);

    void resolve_struct_embed(ChiType *struct_type, ast::Node *base_node,
                              ResolveScope &parent_scope);

    void resolve_vtable(ChiType *base_type, ChiType *derived_type, ast::Node *base_node);

    void resolve_fn_call(ast::Node *node, ResolveScope &scope, ChiTypeFn *fn, NodeList *args);

    void type_placeholders_sub_each(TypeList *input, ChiTypeSubtype *subs, TypeList *output);

    ChiStructMember *get_struct_member(ChiType *struct_type, const string &field_name);

    bool should_resolve_fn_body(ResolveScope &scope);

    void resolve(ast::Module *module);

    ChiType *resolve(ast::Node *node, ResolveScope &scope);

    ChiType *_resolve(ast::Node *node, ResolveScope &scope);

    template <typename... Args>
    void error(ast::Node *node, const char *format, const Args &...args) {
        auto message = fmt::format(format, args...);
        if (auto fn = m_ctx->error_handler) {
            (*fn)({message, *node->token});
            return;
        }

        auto pos = node->token->pos;
        print("{}:{}:{}: error: {}\n", m_module->full_path(), pos.line_number(), pos.col_number(),
              message);
        exit(1);
    }

  public:
    Resolver(ResolveContext *ctx);

    ResolveContext *get_context() { return m_ctx; }

    void context_init_primitives();
    void context_init_builtins(ast::Module *builtin_module);

    ast::Node *get_builtin(const string &name);

    SystemTypes *get_system_types() { return &m_ctx->system_types; }

    ChiType *get_system_type(TypeKind kind);

    ChiType *node_get_type(ast::Node *node);

    string to_string(ChiType *type);

    ChiType *to_value_type(ChiType *type);

    ChiType *get_subtype(ChiType *generic, TypeList *type_args);

    ChiType *get_pointer_type(ChiType *elem, TypeKind kind = TypeKind::Pointer);

    ChiType *get_array_type(ChiType *elem);

    ChiType *get_result_type(ChiType *value, ChiType *err);

    ChiType *get_wrapped_type(ChiType *elem, TypeKind kind);

    ChiType *resolve_subtype(ChiType *subtype);

    ConstantValue resolve_constant_value(ast::Node *node);

    void resolve(ast::Package *package);

    bool type_is_int(ChiType *type) {
        return type->kind == TypeKind::Int || type->kind == TypeKind::Bool ||
               type->kind == TypeKind::Pointer || type->kind == TypeKind::Reference;
    }

    ChiType *type_placeholders_sub(ChiType *type, ChiTypeSubtype *subs);

    ast::Node *find_root_decl(ast::Node *node);
};

class ScopeResolver {
    Resolver *m_resolver = nullptr;
    Scope *m_current_scope = nullptr;

  public:
    ScopeResolver(Resolver *resolver);

    Scope *get_scope() { return m_current_scope; }

    bool declare_symbol(const string &name, ast::Node *node);

    ast::Node *find_symbol(const string &name);

    Scope *push_scope(ast::Node *owner);

    void pop_scope();

    bool is_top_level() { return m_current_scope->parent == nullptr; }
};
} // namespace cx
