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

struct ModulePathInfo {
    string path = "";
    bool is_directory = false;
    string entry_path = "";
    string package_id_path = "";
};

struct Context {
    virtual ast::Node *create_node(ast::NodeType type) = 0;
    virtual ast::DeclSpec *create_decl_spec() = 0;
    virtual ChiType *create_type(TypeKind kind) = 0;
    virtual Scope *create_scope(Scope *parent) = 0;
    virtual Token *create_token() = 0;
    virtual ChiStructMember *create_struct_member() = 0;
    virtual InterfaceImpl *create_interface_impl() = 0;
    virtual ChiEnumVariant *create_enum_member() = 0;

    virtual ast::Module *module_from_path(ast::Package *package, const string &path,
                                          bool import = false) = 0;
    virtual ast::Module *process_source(ast::Package *package, io::Buffer *src,
                                        const string &file_name) = 0;
    virtual optional<ModulePathInfo> find_module_path(const string &path,
                                                      const string &base_path = "") = 0;
    virtual string get_stdlib_path(string path) = 0;
    virtual ast::Package *get_or_create_package(const string &id_path) = 0;
};

struct SystemTypes {
    ChiType *int_ = nullptr;
    ChiType *int32 = nullptr;
    ChiType *uint32 = nullptr;
    ChiType *int64 = nullptr;
    ChiType *uint64 = nullptr;
    ChiType *uint8 = nullptr;
    ChiType *float_ = nullptr;
    ChiType *float64 = nullptr;
    ChiType *void_ = nullptr;
    ChiType *void_ptr = nullptr;
    ChiType *null_ptr = nullptr;
    ChiType *void_ref = nullptr;
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
    ChiType *promise = nullptr;
    ChiType *undefined = nullptr;
    ChiType *lambda = nullptr;
};

// Forward declaration for GenericResolver
class Resolver;

// Tracks a single generic instantiation and its type environment
struct TypeEnvEntry {
    string name;                           // e.g., "promise<Unit>", "Array<int>.add"
    map<ChiType *, ChiType *> subs;        // placeholder → concrete type
    ast::Node *node = nullptr;             // source node (FnDef or StructDecl)
    ChiType *generic_type = nullptr;       // the generic type being instantiated
};

// Records all generic instantiations during resolution
struct GenericResolver {
    map<string, TypeEnvEntry> fn_envs;      // function instantiations
    map<string, TypeEnvEntry> struct_envs;  // struct instantiations

    void record_fn(const string &id, const string &name, ast::Node *node,
                   ChiType *generic_fn, map<ChiType *, ChiType *> subs);
    void record_struct(const string &id, const string &name, ChiType *generic,
                       map<ChiType *, ChiType *> subs);
    void dump(Resolver *resolver);
};

struct ResolveContext {
    ResolveContext(const ResolveContext &) = delete;
    ResolveContext &operator=(const ResolveContext &) = delete;

    Context *allocator = nullptr;
    GenericResolver generics;
    array<ast::Node *> builtins = {};
    SystemTypes system_types = {};
    array<ast::Node *> internal_methods = {};
    map<ChiType *, ChiType *> array_of = {};
    map<ChiType *, ChiType *> pointer_of[(int)TypeKind::__COUNT] = {};
    optional<ErrorHandler> error_handler = {};
    map<string, ChiType *> composite_types = {};
    map<ChiType *, ChiType *> promise_of = {};
    map<string, IntrinsicSymbol> intrinsic_symbols = {};
    ChiType *rt_array_type = nullptr;
    ChiType *rt_promise_type = nullptr;
    ChiType *rt_lambda_type = nullptr;
    ChiType *rt_string_type = nullptr;
    ChiType *rt_empty_bind_type = nullptr;
    ast::Node *rt_enum_base = nullptr;

    explicit ResolveContext(Context *allocator) { this->allocator = allocator; }
};

struct ResolveScope {
    bool skip_fn_bodies = false;
    ast::Node *parent_fn_node = nullptr;
    ChiType *parent_fn = nullptr;
    ChiType *parent_struct = nullptr;
    ChiType *value_type = nullptr;
    int64_t next_enum_value = 0;
    ast::Node *parent_loop = nullptr;
    bool is_escaping = false;
    ast::Module *module = nullptr;
    ast::Node *move_outlet = nullptr;
    ast::Block *block = nullptr;
    bool is_lhs = false;
    ChiType *parent_type_symbol = nullptr;
    bool is_fn_call = false; // True when resolving function reference for call

    ast::FnDef *parent_fn_def() {
        assert(parent_fn_node);
        assert(parent_fn_node->type == ast::NodeType::FnDef);
        return &parent_fn_node->data.fn_def;
    }

    ResolveScope set_parent_fn(ChiType *fn) const;

    ResolveScope set_parent_struct(ChiType *struct_) const;

    ResolveScope set_parent_type_symbol(ChiType *symbol) const;

    ResolveScope set_value_type(ChiType *value_type) const;

    ResolveScope set_parent_loop(ast::Node *loop) const;

    ResolveScope set_is_escaping(bool is_escaping) const;

    ResolveScope set_parent_fn_node(ast::Node *fn) const;

    ResolveScope set_module(ast::Module *module) const;

    ResolveScope set_move_outlet(ast::Node *outlet) const;

    ResolveScope set_block(ast::Block *block) const;

    ResolveScope set_is_lhs(bool is_lhs) const;

    ResolveScope set_is_fn_call(bool is_fn_call) const;
};

enum ResolveFlag : uint32_t { IS_FN_DECL_PROTO = 1 << 0, IS_FN_LAMBDA = 1 << 1 };

class Resolver {
    ResolveContext *m_ctx = nullptr;
    ast::Module *m_module = nullptr;

    Context *get_allocator() { return m_ctx->allocator; }

    ChiType *create_type(TypeKind kind);

    ChiType *create_type_symbol(optional<string> name, ChiType *type);

    ChiType *create_pointer_type(ChiType *elem, TypeKind kind);

    ChiType *create_int_type(int bit_count, bool is_unsigned);

    ChiType *create_float_type(int bit_count);

    ast::Node *create_node(ast::NodeType type);

    ast::Node *add_primitive(const string &name, ChiType *type);

    bool can_assign(ChiType *from_type, ChiType *to_type, bool is_explicit = false);
    bool can_assign_fn(ChiType *from_fn, ChiType *to_fn, bool is_explicit = false);

    bool is_same_type(ChiType *a, ChiType *b);

    TypeKind get_sigil_type_kind(ast::SigilKind sigil);

    void check_assignment(ast::Node *value, ChiType *from_type, ChiType *to_type,
                          bool is_explicit = false);

    void check_binary_op(ast::Node *node, TokenType op_type, ChiType *type);

    bool is_addressable(ast::Node *node);

    bool is_ref_mutable(ast::Node *node, ResolveScope &scope);

    bool is_struct_access_mutable(ChiType *type);

    void check_cast(ast::Node *value, ChiType *from_type, ChiType *to_type);

    ChiType *resolve_value(ast::Node *node, ResolveScope &scope);

    string resolve_term_string(ast::Node *term);

    ChiStructMember *resolve_struct_member(ChiType *struct_type, ast::Node *node,
                                           ResolveScope &scope);

    IntrinsicSymbol resolve_intrinsic_symbol(ast::Node *node);

    void resolve_struct_embed(ChiType *struct_type, ast::Node *base_node,
                              ResolveScope &parent_scope);

    void resolve_vtable(ChiType *base_type, ChiType *derived_type, ast::Node *base_node);

    ChiType *resolve_fn_call(ast::Node *node, ResolveScope &scope, ChiTypeFn *fn, NodeList *args,
                             ast::Node *fn_decl = nullptr);

    array<IntrinsicSymbol> interface_get_intrinsics(ChiType *interface_type);

    bool interface_satisfies_trait(ChiType *interface_type, ChiType *required_trait);

    void type_placeholders_sub_each(TypeList *input, ChiTypeSubtype *subs, TypeList *output);

    bool should_resolve_fn_body(ResolveScope &scope);

    ChiType *resolve_comparator(ChiType *type, ResolveScope &scope);

    ChiType *resolve(ast::Node *node, ResolveScope &scope, uint32_t flags = 0);

    ChiType *_resolve(ast::Node *node, ResolveScope &scope, uint32_t flags = 0);

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
    GenericResolver *get_generics() { return &m_ctx->generics; }

    bool type_needs_destruction(ChiType *type);
    bool should_destroy(ast::Node *node, ChiType *type_override = nullptr);

    void context_init_primitives();
    void context_init_builtins(ast::Module *builtin_module);

    ast::Node *get_builtin(const string &name);

    SystemTypes *get_system_types() { return &m_ctx->system_types; }

    ChiType *get_system_type(TypeKind kind);

    ChiType *node_get_type(ast::Node *node);

    string format_type(ChiType *type, bool for_display = false);
    string format_type_list(TypeList *types, bool for_display = false);

    // Convenience wrappers for common use cases
    string to_display_string(ChiType *type) { return format_type(type, true); }
    string to_unique_id(ChiType *type) { return format_type(type, false); }

    string format_type_data(TypeKind kind, ChiType::Data *data, bool for_display = false);

    string resolve_global_id(ast::Node *node);

    bool has_interface_impl(ChiTypeStruct *struct_type, string interface_id);

    string resolve_qualified_name(ast::Node *node);

    ChiType *to_value_type(ChiType *type);

    ChiType *get_subtype(ChiType *generic, TypeList *type_args);
    ast::Node *get_fn_variant(ChiType *generic_fn, TypeList *type_args, ast::Node *fn_node);
    ChiType *resolve_fn_subtype(ChiType *subtype);

    bool is_struct_type(ChiType *type);

    ChiType *eval_struct_type(ChiType *type);

    ChiTypeStruct *resolve_struct_type(ChiType *type);

    void copy_struct_members(ChiType *from, ChiType *to, ChiStructMember *parent_member = nullptr);

    ChiType *get_pointer_type(ChiType *elem, TypeKind kind = TypeKind::Pointer);

    ChiType *get_array_type(ChiType *elem);

    ChiType *get_promise_type(ChiType *value);

    bool is_promise_type(ChiType *type);

    ChiType *get_promise_value_type(ChiType *type);

    ChiType *get_fn_type(ChiType *ret, TypeList *params, bool is_variadic,
                         ChiType *container = nullptr, bool is_extern = false,
                         TypeList *type_params = nullptr);

    ChiType *get_lambda_for_fn(ChiType *fn);
    bool finalize_lambda_type_recursive(ChiType *type);
    void finalize_placeholder_lambda_params(ChiType *fn_type);

    ChiType *get_result_type(ChiType *value, ChiType *err);

    ChiType *get_wrapped_type(ChiType *elem, TypeKind kind);

    ChiType *resolve_subtype(ChiType *subtype);

    ast::Node *get_dummy_var(const string &name, ast::Node *expr = nullptr);

    optional<ConstantValue> resolve_constant_value(ast::Node *node);

    void resolve(ast::Package *package);

    void resolve(ast::Module *module);

    template <typename PlaceholderHandler, typename RecursiveCallHandler>
    ChiType *recursive_type_replace(ChiType *type, ChiTypeSubtype *subs,
                                    PlaceholderHandler handle_placeholder,
                                    RecursiveCallHandler make_recursive_call);

    ChiType *type_placeholders_sub(ChiType *type, ChiTypeSubtype *subs);
    ChiType *type_placeholders_sub_map(ChiType *type, map<ChiType *, ChiType *> *subs);

    void type_placeholders_sub_each_selective(TypeList *list, ChiTypeSubtype *subs,
                                              TypeList *output, ast::Node *source_filter);
    ChiType *type_placeholders_sub_selective(ChiType *type, ChiTypeSubtype *subs,
                                             ast::Node *source_filter);

    ChiType *substitute_this_type(ChiType *type, ChiType *replacement);

    // Type inference using visitor pattern
    bool infer_type_arguments(ChiTypeFn *fn, TypeList *arg_types,
                              map<ChiType *, ChiType *> *inferred_types);
    bool infer_from_return_type(ChiTypeFn *fn, ChiType *expected_type,
                                map<ChiType *, ChiType *> *inferred_types);

    template <typename PlaceholderHandler, typename RecursiveCallHandler>
    bool visit_type_recursive(ChiType *param_type, ChiType *arg_type,
                              PlaceholderHandler handle_placeholder,
                              RecursiveCallHandler make_recursive_call);

    ast::Node *find_root_decl(ast::Node *node);

    bool compare_impl_type(ChiType *base, ChiType *impl);

    ChiStructMember *get_struct_member(ChiType *struct_type, const string &field_name);
    ChiStructMember *get_struct_member_access(ast::Node *node, ChiType *struct_type,
                                              const string &field_name, bool is_internal,
                                              bool is_write);
    bool is_friend_struct(ChiType *a, ChiType *b);

    struct OperatorMethodCall {
        ast::Node *call_node;
        ChiType *return_type;
    };
    optional<OperatorMethodCall> try_resolve_operator_method(IntrinsicSymbol symbol, ChiType *t1,
                                                             ChiType *t2, ast::Node *op1,
                                                             ast::Node *op2, ast::Node *node,
                                                             ResolveScope &scope);

    // Static utility function to map operator TokenType to IntrinsicSymbol
    static IntrinsicSymbol get_operator_intrinsic_symbol(TokenType op_type);
};

class ScopeResolver {
    Resolver *m_resolver = nullptr;
    Scope *m_current_scope = nullptr;

  public:
    ScopeResolver(Resolver *resolver);

    Scope *get_scope() { return m_current_scope; }

    bool declare_symbol(const string &name, ast::Node *node);

    ast::Node *find_symbol(const string &name, Scope *current_scope = nullptr);

    array<ast::Node *> get_all_symbols(Scope *current_scope = nullptr);

    Scope *push_scope(ast::Node *owner);

    void pop_scope();

    bool is_top_level() { return m_current_scope->parent == nullptr; }
};
} // namespace cx
