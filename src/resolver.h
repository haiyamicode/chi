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
    virtual WhereCondition *create_where_condition() = 0;

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
    ChiType *null_ = nullptr;
    ChiType *void_ref = nullptr;
    ChiType *byte_ = nullptr;
    ChiType *rune_ = nullptr;
    ChiType *str_lit = nullptr;
    ChiType *any = nullptr;
    ChiType *string = nullptr;
    ChiType *bool_ = nullptr;
    ChiType *array = nullptr;
    ChiType *optional = nullptr;
    ChiType *promise = nullptr;
    ChiType *undefined = nullptr;
    ChiType *zeroinit = nullptr;
    ChiType *never_ = nullptr;
    ChiType *lambda = nullptr;
    ChiType *span = nullptr;
    ChiType *unit = nullptr;
    ChiType *tuple = nullptr;
};

// Forward declaration for GenericResolver
class Resolver;

// Tracks a single generic instantiation and its type environment
struct TypeEnvEntry {
    string name;                           // e.g., "promise<Unit>", "Array<int>.add"
    map<ChiType *, ChiType *> subs;        // placeholder → concrete type
    ast::Node *node = nullptr;             // source node (FnDef or StructDecl)
    ChiType *generic_type = nullptr;       // the generic type being instantiated
    ChiType *subtype = nullptr;            // the Subtype node (for struct instantiations)
    bool from_method_sig = false;          // created by method signature substitution (defer resolution)
};

// Records all generic instantiations during resolution
struct GenericResolver {
    map<string, TypeEnvEntry> fn_envs;      // function instantiations
    map<string, TypeEnvEntry> struct_envs;  // struct instantiations

    void record_fn(const string &id, const string &name, ast::Node *node,
                   ChiType *generic_fn, map<ChiType *, ChiType *> subs);
    void record_struct(const string &id, const string &name, ChiType *generic,
                       ChiType *subtype, map<ChiType *, ChiType *> subs,
                       bool from_method_sig = false);
    void resolve_pending(Resolver *resolver);
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
    map<ChiType *, ChiType *> span_of = {};
    map<ChiType *, ChiType *> mut_span_of = {};
    map<string, ChiType *> fixed_array_of = {};
    map<ChiType *, ChiType *> pointer_of[(int)TypeKind::__COUNT] = {};
    optional<ErrorHandler> error_handler = {};
    uint32_t lang_flags = 0;
    map<string, ChiType *> composite_types = {};
    map<ChiType *, ChiType *> promise_of = {};
    map<ChiType *, ChiType *> shared_of = {};
    map<string, ChiType *> tuple_types = {};
    map<string, IntrinsicSymbol> intrinsic_symbols = {};
    ChiType *rt_array_type = nullptr;
    ChiType *rt_span_type = nullptr;
    ChiType *rt_promise_type = nullptr;
    ChiType *rt_shared_type = nullptr;
    ChiType *rt_result_type = nullptr;
    ChiType *rt_lambda_type = nullptr;
    ChiType *rt_string_type = nullptr;
    ChiType *rt_error_type = nullptr;
    ChiType *rt_unit_type = nullptr;
    ChiType *rt_empty_bind_type = nullptr;
    ast::Node *rt_enum_base = nullptr;
    ChiType *rt_sized_interface = nullptr;
    ChiType *rt_allow_unsized_interface = nullptr;
    ChiLifetime *static_lifetime = nullptr;

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
    bool is_unsafe_block = false; // True when inside an unsafe block or unsafe function
    map<string, ChiLifetime *> *fn_lifetime_params = nullptr; // Explicit lifetime params from function declaration

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

    ResolveScope set_is_unsafe_block(bool is_unsafe) const;
};

enum ResolveFlag : uint32_t { IS_FN_DECL_PROTO = 1 << 0, IS_FN_LAMBDA = 1 << 1 };

struct AwaitSite {
    ast::Node *await_expr = nullptr;
    ast::Node *resume_expr = nullptr;
};

template <typename F>
static bool visit_async_children(ast::Node *node, bool include_try_catch, F &&visit) {
    if (!node) {
        return false;
    }

    auto visit_one = [&](ast::Node *child) -> bool {
        return child && visit(child);
    };
    auto visit_many = [&](auto &nodes) -> bool {
        for (auto child : nodes) {
            if (visit_one(child)) {
                return true;
            }
        }
        return false;
    };

    switch (node->type) {
    case ast::NodeType::TryExpr:
        if (visit_one(node->data.try_expr.expr)) {
            return true;
        }
        return include_try_catch && visit_one(node->data.try_expr.catch_block);
    case ast::NodeType::DestructureDecl:
        return visit_one(node->data.destructure_decl.expr);
    case ast::NodeType::VarDecl:
        return visit_one(node->data.var_decl.expr);
    case ast::NodeType::ReturnStmt:
        return visit_one(node->data.return_stmt.expr);
    case ast::NodeType::ThrowStmt:
        return visit_one(node->data.throw_stmt.expr);
    case ast::NodeType::BinOpExpr:
        return visit_one(node->data.bin_op_expr.op1) || visit_one(node->data.bin_op_expr.op2);
    case ast::NodeType::FnCallExpr:
        if (visit_one(node->data.fn_call_expr.fn_ref_expr)) {
            return true;
        }
        return visit_many(node->data.fn_call_expr.args);
    case ast::NodeType::UnaryOpExpr:
        return visit_one(node->data.unary_op_expr.op1);
    case ast::NodeType::Block:
        if (visit_many(node->data.block.statements)) {
            return true;
        }
        return visit_one(node->data.block.return_expr);
    case ast::NodeType::IfExpr:
        return visit_one(node->data.if_expr.condition) ||
               visit_one(node->data.if_expr.then_block) ||
               visit_one(node->data.if_expr.else_node);
    case ast::NodeType::SwitchExpr:
        return visit_one(node->data.switch_expr.expr) || visit_many(node->data.switch_expr.cases);
    case ast::NodeType::CaseExpr:
        return visit_many(node->data.case_expr.clauses) ||
               visit_one(node->data.case_expr.destructure_pattern) ||
               visit_one(node->data.case_expr.body);
    case ast::NodeType::ConstructExpr:
        if (visit_one(node->data.construct_expr.type) ||
            visit_many(node->data.construct_expr.items) ||
            visit_many(node->data.construct_expr.field_inits)) {
            return true;
        }
        return visit_one(node->data.construct_expr.spread_expr);
    case ast::NodeType::FieldInitExpr:
        return visit_one(node->data.field_init_expr.value);
    case ast::NodeType::TupleExpr:
        return visit_many(node->data.tuple_expr.items);
    case ast::NodeType::DotExpr:
        return visit_one(node->data.dot_expr.expr);
    case ast::NodeType::IndexExpr:
        return visit_one(node->data.index_expr.expr) ||
               visit_one(node->data.index_expr.subscript);
    case ast::NodeType::SliceExpr:
        return visit_one(node->data.slice_expr.expr) || visit_one(node->data.slice_expr.start) ||
               visit_one(node->data.slice_expr.end);
    case ast::NodeType::RangeExpr:
        return visit_one(node->data.range_expr.start) || visit_one(node->data.range_expr.end);
    case ast::NodeType::CastExpr:
        return visit_one(node->data.cast_expr.expr);
    case ast::NodeType::PrefixExpr:
        return visit_one(node->data.prefix_expr.expr);
    case ast::NodeType::PackExpansion:
    case ast::NodeType::ParenExpr:
        return visit_one(node->data.child_expr);
    case ast::NodeType::ForStmt:
        return visit_one(node->data.for_stmt.init) || visit_one(node->data.for_stmt.condition) ||
               visit_one(node->data.for_stmt.post) || visit_one(node->data.for_stmt.bind) ||
               visit_one(node->data.for_stmt.index_bind) ||
               visit_one(node->data.for_stmt.expr) || visit_one(node->data.for_stmt.body);
    case ast::NodeType::WhileStmt:
        return visit_one(node->data.while_stmt.condition) ||
               visit_one(node->data.while_stmt.body);
    default:
        return false;
    }
}

class Resolver {
    ResolveContext *m_ctx = nullptr;
    ast::Module *m_module = nullptr;
    ast::Node *m_subtype_origin = nullptr; // propagated origin during resolve_subtype
    ChiType *m_resolving_subtype = nullptr; // the subtype currently being resolved (for deferral)

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
    bool builtin_satisfies_intrinsic(ChiType *type, IntrinsicSymbol symbol);

    bool is_addressable(ast::Node *node);

    bool is_ref_mutable(ast::Node *node, ResolveScope &scope);

    bool is_struct_access_mutable(ChiType *type, ResolveScope *scope = nullptr);

    void check_cast(ast::Node *value, ChiType *from_type, ChiType *to_type);

    ChiType *resolve_value(ast::Node *node, ResolveScope &scope);

    string resolve_term_string(ast::Node *term);

    ChiStructMember *resolve_struct_member(ChiType *struct_type, ast::Node *node,
                                           ResolveScope &scope);

    IntrinsicSymbol resolve_intrinsic_symbol(ast::Node *node);

    void resolve_struct_embed(ChiType *struct_type, ast::Node *base_node,
                              ResolveScope &parent_scope);

    void resolve_vtable(ChiType *base_type, ChiType *derived_type, ast::Node *base_node,
                        bool from_embedding = false);

    ChiType *resolve_fn_call(ast::Node *node, ResolveScope &scope, ChiTypeFn *fn, NodeList *args,
                             ast::Node *fn_decl = nullptr);

    array<IntrinsicSymbol> interface_get_intrinsics(ChiType *interface_type);

    bool interface_satisfies_trait(ChiType *interface_type, ChiType *required_trait);
    bool struct_satisfies_interface(ChiType *struct_type, ChiType *iface_type);
    bool check_trait_bound(ChiType *type_arg, ChiType *trait_type);
    bool is_constructor_interface_compatible(ChiType *type, ChiType *iface_type);
    bool check_where_condition(WhereCondition *cond, ChiTypeSubtype *subtype_data);
    WhereCondition *build_where_condition(ast::ImplementBlockData &impl_data,
                                          ChiTypeStruct *struct_, ResolveScope &scope);

    void type_placeholders_sub_each(TypeList *input, ChiTypeSubtype *subs, TypeList *output);

    bool should_resolve_fn_body(ResolveScope &scope);

    ChiType *resolve_comparator(ChiType *type, ResolveScope &scope);

    ChiType *resolve(ast::Node *node, ResolveScope &scope, uint32_t flags = 0);

    ChiType *_resolve(ast::Node *node, ResolveScope &scope, uint32_t flags = 0);

    template <typename... Args>
    void error(ast::Node *node, const char *format, const Args &...args) {
        error_with_notes(node, {}, format, args...);
    }

    template <typename... Args>
    void error_with_notes(ast::Node *node, array<Note> notes, const char *format,
                          const Args &...args) {
        auto message = fmt::format(format, args...);
        if (auto fn = m_ctx->error_handler) {
            Error err = {message, *node->token};
            err.notes = std::move(notes);
            (*fn)(std::move(err));
            return;
        }

        auto pos = node->token->pos;
        print("{}:{}:{}: error: {}\n", m_module->display_path(), pos.line_number(), pos.col_number(),
              message);
        for (auto &note : notes) {
            print("{}:{}:{}: note: {}\n", m_module->display_path(), note.pos.line_number(),
                  note.pos.col_number(), note.message);
        }
        exit(1);
    }

  public:
    Resolver(ResolveContext *ctx);

    ResolveContext *get_context() { return m_ctx; }
    GenericResolver *get_generics() { return &m_ctx->generics; }

    bool is_borrowing_type(ChiType *type);
    bool type_needs_destruction(ChiType *type);
    bool is_non_copyable(ChiType *type);

    // Active where-clause trait bounds for placeholders (scoped, not mutated)
    map<ChiType *, array<ChiType *>> m_where_traits;

    // Returns all traits for a placeholder: declared traits + active where-clause bounds
    array<ChiType *> get_placeholder_traits(ChiType *ph);
    bool should_destroy(ast::Node *node, ChiType *type_override = nullptr);
    ast::Node *get_moved_expr(ast::Node *expr);
    bool use_implicit_owning_coercion(ChiType *from_type, ChiType *to_type);

    void context_init_primitives();
    void context_init_builtins(ast::Module *builtin_module);

    ast::Node *get_builtin(const string &name);

    SystemTypes *get_system_types() { return &m_ctx->system_types; }

    ChiType *get_system_type(TypeKind kind);

    ChiType *node_get_type(ast::Node *node);

    string format_type(ChiType *type, bool for_display = false);
    string format_type_list(TypeList *types, bool for_display = false);

    // Convenience wrappers: display = human-readable for errors, id = unique internal key
    string format_type_display(ChiType *type) { return format_type(type, true); }
    string format_type_id(ChiType *type) { return format_type(type, false); }

    string format_type_data(TypeKind kind, ChiType::Data *data, bool for_display = false);

    string resolve_global_id(ast::Node *node);

    bool has_interface_impl(ChiTypeStruct *struct_type, string interface_id);

    string resolve_qualified_name(ast::Node *node);

    ChiType *to_value_type(ChiType *type);

    ChiType *get_subtype(ChiType *generic, TypeList *type_args);
    ChiType *get_enum_subtype(ChiType *generic, TypeList *type_args);
    ast::Node *get_fn_variant(ChiType *generic_fn, TypeList *type_args, ast::Node *fn_node);
    ChiType *resolve_fn_subtype(ChiType *subtype);

    bool is_struct_type(ChiType *type);

    ChiType *eval_struct_type(ChiType *type, ast::Node *origin = nullptr);

    ChiTypeStruct *resolve_struct_type(ChiType *type);

    void copy_struct_members(ChiType *from, ChiType *to, ChiStructMember *parent_member = nullptr);

    ChiType *get_pointer_type(ChiType *elem, TypeKind kind = TypeKind::Pointer);

    ChiType *get_array_type(ChiType *elem);
    ChiType *get_span_type(ChiType *elem, bool is_mut = false);
    ChiType *get_fixed_array_type(ChiType *elem, uint32_t size);

    ChiType *get_promise_type(ChiType *value);
    ChiType *get_shared_type(ChiType *value);

    ChiTypeSubtype *get_promise_subtype(ChiType *type);
    bool is_promise_type(ChiType *type);
    ChiType *get_promise_value_type(ChiType *type);

    ChiType *get_fn_type(ChiType *ret, TypeList *params, bool is_variadic,
                         ChiType *container = nullptr, bool is_extern = false,
                         TypeList *type_params = nullptr);

    bool fn_type_has_placeholder(ChiType *fn_type);

    ChiType *get_lambda_for_fn(ChiType *fn);
    bool finalize_lambda_type_recursive(ChiType *type);
    void finalize_placeholder_lambda_params(ChiType *fn_type);

    ChiType *get_result_type(ChiType *value, ChiType *err);
    bool is_result_type(ChiType *type);
    bool contains_await(ast::Node *node);
    ast::Node *find_await_expr(ast::Node *node);
    AwaitSite find_await_site(ast::Node *node);

    ChiType *get_wrapped_type(ChiType *elem, TypeKind kind);

    ChiType *get_tuple_type(TypeList &elements);

    ChiType *resolve_subtype(ChiType *subtype, ast::Node *origin = nullptr);

    ast::Node *get_dummy_var(const string &name, ast::Node *expr = nullptr);

    // If expr is not addressable and produces a destructible type, materializes
    // a synthetic __tmp VarDecl in scope.block to own it. Returns the temp or nullptr.
    ast::Node *ensure_temp_owner(ast::Node *expr, ChiType *expr_type, ResolveScope &scope,
                                 bool force_addressable = false);

    ast::Node *create_narrowed_var(ast::Node *identifier, ast::Node *parent_stmt,
                                   ResolveScope &scope, ChiType *narrowed_type = nullptr);

    void resolve_destructure(ast::Node *pattern, ChiType *source_type, ResolveScope &scope,
                             ast::Node *borrow_source = nullptr);

    void resolve_destructure_fields(ast::Node *parent, array<ast::Node *> &fields,
                                    ChiType *source_type, ResolveScope &scope,
                                    array<ast::Node *> &generated_vars,
                                    ast::Node *borrow_source = nullptr);

    void resolve_array_destructure(ast::Node *parent, array<ast::Node *> &fields,
                                   ChiType *source_type, ResolveScope &scope,
                                   array<ast::Node *> &generated_vars,
                                   ast::Node *borrow_source = nullptr);

    void resolve_tuple_destructure(ast::Node *parent, array<ast::Node *> &fields,
                                   ChiType *source_type, ResolveScope &scope,
                                   array<ast::Node *> &generated_vars,
                                   ast::Node *borrow_source = nullptr);

    bool always_terminates(ast::Node *node);

    void collect_narrowables(ast::Node *expr, bool when_truthy, array<ast::Node *> &out);

    optional<ConstantValue> resolve_constant_value(ast::Node *node);

    void resolve(ast::Package *package);

    void resolve(ast::Module *module);

    void add_cleanup_var(ast::Block *block, ast::Node *var);
    void add_fn_body_param_cleanups(ast::Node *fn_node, ast::Node *body);

    template <typename PlaceholderHandler, typename RecursiveCallHandler>
    ChiType *recursive_type_replace(ChiType *type, ChiTypeSubtype *subs,
                                    PlaceholderHandler handle_placeholder,
                                    RecursiveCallHandler make_recursive_call);

    ChiType *type_placeholders_sub(ChiType *type, ChiTypeSubtype *subs);
    ChiType *type_placeholders_sub_map(ChiType *type, map<ChiType *, ChiType *> *subs);
    ChiType *wrap_placeholders_with_infer(ChiType *type);
    ChiType *wrap_placeholders_with_infer(ChiType *type, map<ChiType *, ChiType *> &infer_map);
    ChiType *resolve_concrete_subtypes(ChiType *type, ast::Node *origin = nullptr);
    ChiType *finalize_fn_type(ChiType *type, ast::Node *origin = nullptr);
    ChiType *finalize_fn_decl_type(ast::Node *decl, ast::Node *origin = nullptr);
    ChiType *finalize_member_fn_type(ChiStructMember *member, ast::Node *origin = nullptr);

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
    void add_call_borrow_edges(ast::FnDef &fn_def, ast::FnCallExpr &call, ast::Node *target);
    void add_borrow_source_edges(ast::FnDef &fn_def, ast::Node *expr, ast::Node *target,
                                 bool is_ref = false);
    void resolve_fn_lifetimes(ast::Node *fn_node);

    // Move tracking: record a sink edge if the expression transfers ownership
    void track_move_sink(ast::Node *parent_fn_node, ast::Node *expr, ChiType *expr_type,
                         ast::Node *dest, ChiType *dest_type);

    void check_lifetime_constraints(ast::FnDef *fn_def);
    void check_lifetime_constraints(ast::FnDef *fn_def, ast::FlowState &flow);

    bool compare_impl_type(ChiType *base, ChiType *impl);

    ChiType *get_enum_type(ChiType *type);
    ChiType *get_enum_root(ChiType *type);
    ChiType *resolve_enum_value_parent_type(ChiType *enum_type);
    void update_enum_value_member(ChiType *enum_value_type, ChiEnumVariant *member);
    bool is_enum_value_placeholder(ChiType *enum_type);
    bool enum_needs_destruction(ChiTypeEnum *enum_type);
    void record_specialized_fn_env(ast::Node *node, map<ChiType *, ChiType *> *base_subs = nullptr);
    void ensure_enum_subtype_final_type(ChiType *generic, ChiType *subtype);
    ChiEnumVariant *find_expected_enum_variant(const string &name, ChiType *expected_type);
    ChiStructMember *get_struct_member(ChiType *struct_type, const string &field_name);
    array<ChiStructMember *> get_enum_payload_fields(ChiType *type);
    ChiStructMember *get_struct_member_access(ast::Node *node, ChiType *struct_type,
                                              const string &field_name, bool is_internal,
                                              bool is_write, ResolveScope *scope = nullptr,
                                              ChiType *access_type = nullptr);
    bool is_friend_struct(ChiType *a, ChiType *b);

    struct OperatorMethodCall {
        ast::Node *call_node;
        ChiType *return_type;
    };
    optional<OperatorMethodCall> try_resolve_operator_method(IntrinsicSymbol symbol, ChiType *t1,
                                                             ChiType *t2, ast::Node *op1,
                                                             ast::Node *op2, ast::Node *node,
                                                             ResolveScope &scope);

    ChiType *try_auto_deref(ast::Node *node, ChiType *stype, const string &field_name,
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
