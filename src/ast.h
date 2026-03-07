
/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "sema.h"

namespace cx {

struct PackageConfig; // Forward declaration from package_config.h

namespace ast {
struct Node;
struct Package;
struct Scope;

MAKE_ENUM(NodeType, Error, Root, FnProto, FnDef, ParamDecl, Block, ReturnStmt, VarDecl, BinOpExpr,
          UnaryOpExpr, LiteralExpr, IfExpr, FnCallExpr, Primitive, Identifier, EmptyStmt,
          ConstructExpr, ParenExpr, StructDecl, DotExpr, SubtypeExpr, IndexExpr, SliceExpr,
          RangeExpr, TypedefDecl, TypeSigil, EnumVariant, CastExpr, ForStmt, WhileStmt, BranchStmt,
          TypeParam, LifetimeParam, PrefixExpr, ExternDecl, TryExpr, AwaitExpr, InferredType,
          ImportDecl, SizeofExpr, DeclAttribute, BindIdentifier, SwitchExpr, CaseExpr, ImportSymbol,
          ExportDecl, FieldInitExpr, EnumDecl, GeneratedFn, ThrowStmt, ImplementBlock,
          PackExpansion, DestructureDecl, DestructureField, UnitExpr);

MAKE_ENUM(ModuleKind, XS, XM);
MAKE_ENUM(ForLoopKind, Empty, Ternary, Range, Iter, IntRange);

enum FnParsingFlags : uint32_t {
    FN_BODY_REQUIRED = 1 << 0,
    FN_BODY_NONE = 1 << 1,
};

enum DeclFlag : uint32_t {
    DECL_NONE = 0,
    DECL_PRIVATE = 1 << 0,
    DECL_EXTERN = 1 << 1,
    DECL_IS_ENTRY = 1 << 2,
    DECL_PROTECTED = 1 << 3,
    DECL_MUTABLE = 1 << 4,
    DECL_STATIC = 1 << 5,
    DECL_ASYNC = 1 << 6,
    DECL_UNSAFE = 1 << 7,
    DECL_EXPORTED = 1 << 8,   // explicit `export` keyword on a declaration
};

struct Module {
    ModuleKind kind = ModuleKind::XS;
    Node *root = nullptr;
    Package *package = nullptr;
    string path = "";
    string id_path = "";
    string name = "";
    string filename = "";
    array<Node *> exports = {};
    array<Error> errors = {};
    cx::Scope *scope = nullptr;
    cx::Scope *import_scope = nullptr;
    optional<string> source = {};
    array<Module *> imports = {};
    array<Token *> tokens = {};
    array<Comment> comments = {};
    bool broken = false;

    Module() = default;
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;

    string full_path() const { return (fs::path(path) / filename).string(); }
    string display_path() const {
        auto fp = full_path();
        auto cwd = fs::current_path().string();
        if (fp.size() > cwd.size() + 1 && fp.substr(0, cwd.size()) == cwd && fp[cwd.size()] == '/') {
            return fp.substr(cwd.size() + 1);
        }
        return fp;
    }
    string global_id() const;

    static ModuleKind kind_from_extension(const string &ext) {
        if (ext == ".xs") {
            return ModuleKind::XS;
        } else if (ext == ".x") {
            return ModuleKind::XM;
        } else {
            panic("unknown module extension: {}", ext);
        }
        return ModuleKind::XS;
    }

    LANG_FLAG get_lang_flags() const {
        if (kind == ModuleKind::XM) {
            return LANG_FLAG_MANAGED;
        }
        return LANG_FLAG_SAFE;
    }
};

enum class PackageKind { BUILTIN, DEFAULT };

struct Package {
    array<box<Module>> modules = {};
    Node *entry_fn = nullptr;
    string src_path = "";
    string id_path = "";
    PackageKind kind = PackageKind::DEFAULT;
    string name = "";
    cx::PackageConfig *config = nullptr; // Package configuration (from package.jsonc)

    Module *add_module() { return modules.emplace(new Module())->get(); }
};

struct Root {
    array<Node *> top_level_decls = {};
};

struct DeclSpec {
    uint32_t flags = DECL_NONE;
    array<Node *> attributes = {};

    Visibility get_visibility() const {
        if (flags & DECL_PRIVATE) {
            return Visibility::Private;
        }
        if (flags & DECL_PROTECTED) {
            return Visibility::Protected;
        }
        return Visibility::Public;
    }

    bool is_exported() const { return (flags & DECL_EXPORTED) != 0; }
    bool is_mutable() const { return has_flag(DECL_MUTABLE); }
    bool has_flag(DeclFlag flag) const { return (flags & flag) != 0; }
    bool is_extern() const { return has_flag(DECL_EXTERN); }
    bool is_static() const { return has_flag(DECL_STATIC); }
    bool is_async() const { return has_flag(DECL_ASYNC); }
    bool is_unsafe() const { return has_flag(DECL_UNSAFE); }
};

struct FnProto {
    array<Node *> params = {};
    Node *return_type = nullptr;
    Node *fn_def_node = nullptr;
    array<Node *> type_params = {};
    array<Node *> lifetime_params = {};
    bool is_vararg = false;
    bool is_type_expr = false;
    array<ChiLifetime *> resolved_param_lifetimes = {};
    ChiLifetime *resolved_return_lifetime = nullptr;
};

MAKE_ENUM(FnKind, TopLevel, Method, Constructor, Destructor, Lambda);
MAKE_ENUM(CaptureMode, ByRef, ByValue);

struct FnCapture {
    Node *decl = nullptr;
    CaptureMode mode = CaptureMode::ByRef;
};

struct FnDef {
    Node *fn_proto = nullptr;
    Node *body = nullptr;
    FnKind fn_kind = FnKind::TopLevel;
    DeclSpec *decl_spec = nullptr;
    array<FnCapture> captures = {};
    map<Node *, int32_t> capture_map = {};
    array<string> value_captures = {}; // parsed [ident, ...] names, consumed during resolution
    bool is_generated = false;
    bool has_try = false;
    bool has_cleanup = false;
    array<Node *> variants = {};
    map<Node *, array<Node *>> ref_edges = {}; // escape analysis: dependency graph
    map<Node *, Node *> sink_edges = {};       // move: a → b means a's ownership transferred to b
    map<Node *, size_t> edge_offsets = {};     // per-variable: index where current edges start
    map<Node *, long> terminal_last_use = {};  // per-terminal: offset of last reference
    array<Node *> terminals = {};              // nodes whose lifetimes extend beyond the function
    int32_t next_decl_order = 0;               // counter for assigning decl_order to locals

    // Register a terminal node (return statement, this with field assignments, etc.)
    void add_terminal(Node *terminal) {
        if (!terminal)
            return;
        for (size_t i = 0; i < terminals.len; i++) {
            if (terminals[i] == terminal)
                return;
        }
        terminals.add(terminal);
    }

    // Direct reference: A points to B's memory (from & operator)
    void add_ref_edge(Node *from, Node *to) {
        if (!from || !to || from == to)
            return;
        ref_edges[from].add(to);
    }

    // Move/sink: A's ownership was transferred to B (A is dead after this)
    void add_sink_edge(Node *from, Node *to) {
        if (!from || !to)
            return;
        sink_edges[from] = to;
    }

    bool is_sunk(Node *node) { return sink_edges.has_key(node); }

    // Get the offset where current (non-stale) edges start for a variable
    size_t current_edge_offset(Node *node) {
        auto *offset = edge_offsets.get(node);
        return offset ? *offset : 0;
    }

    // Mark current edges as stale (called before adding new edges on reassignment)
    void bump_edge_offset(Node *node) {
        auto *edges = ref_edges.get(node);
        edge_offsets[node] = edges ? edges->len : 0;
    }

    // By-value copy: A inherits B's leaf terminals (the actual memory targets)
    // Traverses B's edges to find leaves, then creates edges from A to each leaf.
    void copy_ref_edges(Node *to, Node *from) {
        if (!to || !from || to == from)
            return;
        auto *deps = ref_edges.get(from);
        if (!deps || deps->len == 0) {
            // from itself is a leaf terminal
            add_ref_edge(to, from);
            return;
        }
        // Follow edges to find leaf terminals (nodes with no outgoing edges)
        array<Node *> stack;
        for (size_t i = 0; i < deps->len; i++)
            stack.add(deps->items[i]);
        map<Node *, bool> visited;
        while (stack.len > 0) {
            auto *node = stack.last();
            stack.len--;
            if (visited.has_key(node))
                continue;
            visited[node] = true;
            auto *next = ref_edges.get(node);
            if (next && next->len > 0) {
                for (size_t i = 0; i < next->len; i++)
                    stack.add(next->items[i]);
            } else {
                // Leaf terminal — add direct edge
                add_ref_edge(to, node);
            }
        }
    }

    bool is_static() { return decl_spec && decl_spec->is_static(); }
    bool is_async() { return decl_spec && decl_spec->is_async(); }

    bool is_instance_method() {
        switch (fn_kind) {
        case FnKind::Constructor:
        case FnKind::Destructor:
            return true;
        case FnKind::Method:
            return !is_static();
        default:
            return false;
        }
    }

    bool has_try_or_cleanup() { return has_try || has_cleanup; }
};

struct ParamDecl {
    Node *type = nullptr;
    bool is_variadic = false;
    Node *default_value = nullptr;
    ChiLifetime *borrow_lifetime = nullptr; // for borrowing value params (e.g. func() types)
};

struct TypeParam {
    long index = 0;
    array<Node *> type_bounds = {};
    Node *default_type = nullptr;
    Node *source_decl = nullptr; // The struct/function that owns this type parameter
    string lifetime_bound;       // "a" for T: 'a (lifetime bound)
};

struct LifetimeParam {
    long index = 0;
    string bound; // "b" for 'a: 'b (outlives bound)
    Node *source_decl = nullptr;
};

struct Block {
    array<Node *> statements = {};
    array<Node *> implicit_vars = {};
    array<Node *> stmt_temp_vars = {}; // arg temporaries, compiled after push_scope
    array<Node *> cleanup_vars = {};
    cx::Scope *scope = nullptr;
    bool is_arrow = false;
    Node *return_expr = nullptr;
    bool has_braces = false;
    bool is_unsafe = false;
};

struct ReturnStmt {
    Node *expr = nullptr;
};

struct ThrowStmt {
    Node *expr = nullptr;
};

enum class VarKind {
    Mutable,   // var - can be reassigned
    Immutable, // let - cannot be reassigned (runtime value OK)
    Constant   // const - compile-time constant (must be evaluable at compile time)
};

struct VarDecl {
    Token *identifier = nullptr;
    Node *type = nullptr;
    Node *expr = nullptr;
    VarKind kind = VarKind::Mutable;
    bool is_field = false;
    bool is_embed = false;
    ChiStructMember *resolved_field = nullptr;
    optional<ConstantValue> resolved_value = std::nullopt;
    DeclSpec *decl_spec = {};
    bool is_generated = false;
    Node *initialized_at = nullptr;
    Node *narrowed_from = nullptr;
};

struct BinOpExpr {
    TokenType op_type = TokenType::ERROR;
    Node *op1 = nullptr;
    Node *op2 = nullptr;
    Node *resolved_call = nullptr;
    bool is_initializing = false; // true when this assignment is the first write to the target
};

struct UnaryOpExpr {
    TokenType op_type = TokenType::ERROR;
    Node *op1 = nullptr;
    bool is_suffix = false;
    Node *resolved_call = nullptr; // for Unwrap/UnwrapMut operator methods
};

struct TryExpr {
    Node *expr = nullptr;
    Node *catch_expr = nullptr;    // catch type (FileError) — null for catch-all
    Node *catch_block = nullptr;   // the { ... } block — null means no catch (Result mode)
    Node *catch_err_var = nullptr; // implicit VarDecl for err binding
};

struct AwaitExpr {
    Node *expr = nullptr;
};

struct FnCallExpr {
    Node *fn_ref_expr = nullptr;
    array<Node *> args = {};
    array<Node *> type_args = {}; // Explicit type parameters
    bool is_builtin = false;
    Node *generated_fn = nullptr;
    array<Node *> post_narrow_vars = {}; // narrowed vars emitted after call
};

struct IfExpr {
    Node *condition = nullptr;
    Node *then_block = nullptr;
    Node *else_node = nullptr; // can be null, block node, or another if node
    array<Node *> post_narrow_vars = {}; // narrowed vars emitted after guard clause
};

struct StructDecl {
    array<Node *> members = {};
    ContainerKind kind = ContainerKind::Struct;
    array<Node *> type_params = {};
    DeclSpec *decl_spec = {};
};

struct WhereClause {
    Token *param_name = nullptr; // type parameter name token (e.g., T)
    Node *bound_type = nullptr;  // trait type expression (e.g., Show)
};

struct ImplementBlockData {
    array<Node *> interface_types = {};
    array<Node *> members = {};
    array<WhereClause> where_clauses = {}; // Non-empty = conditional where-block
};

struct ExternDecl {
    Token *type = nullptr;
    DeclSpec *decl_spec = nullptr;
    array<Node *> members = {};
    array<Node *> imports = {}; // Import declarations (ImportDecl nodes) for C headers
    array<Node *> exports = {}; // Export declarations (ExportDecl nodes) for C headers
};

struct FieldInitExpr {
    Token *token = nullptr;
    Token *field = nullptr;
    Node *value = nullptr;
    ChiStructMember *resolved_field = nullptr;
    void *compiled_field_address = nullptr;
};

MAKE_ENUM(SigilKind, None, Pointer, Reference, Optional, MutRef, Move, FixedArray, Span)

struct DestructureField {
    Token *field_name = nullptr;   // struct field to extract
    Token *binding_name = nullptr; // local variable name (default = field_name)
    Node *nested = nullptr;        // for nested: points to DestructureDecl
    ChiStructMember *resolved_field = nullptr;
    SigilKind sigil = SigilKind::None; // &field or &mut field
};

struct DestructureDecl {
    array<Node *> fields = {}; // DestructureField nodes
    Node *expr = nullptr;      // RHS expression
    VarKind kind = VarKind::Mutable;
    array<Node *> generated_vars = {};                // resolver creates VarDecl nodes here
    Node *temp_var = nullptr;                         // temp to hold RHS value
    bool is_array = false;                            // array destructuring: var [a, b] = arr
    ChiStructMember *resolved_index_method = nullptr; // index_mut for array destructure
};

// composite literal
struct ConstructExpr {
    bool is_new = false;
    bool is_array_literal = false;
    array<Node *> items = {};
    array<Node *> field_inits = {};
    Node *type = nullptr;
    Node *spread_expr = nullptr; // ...expr spread source
};

struct TypedefDecl {
    Node *type = nullptr;
    Token *identifier = nullptr;
    array<Node *> type_params = {};
};

struct DotExpr {
    Node *expr = nullptr;
    Token *field = nullptr;
    ChiStructMember *resolved_struct_member = nullptr;
    int64_t resolved_value = 0;
    Node *resolved_decl = nullptr;
    bool should_resolve_variant = false;
    bool is_optional_chain = false;
    DotKind resolved_dot_kind = DotKind::Field;
    int resolved_index = -1;
    Node *narrowed_var = nullptr;
};

struct SubtypeExpr {
    Node *type = nullptr;
    array<Node *> args = {};
};

struct IndexExpr {
    Node *expr = nullptr;
    Node *subscript = nullptr;
    ChiStructMember *resolved_method = nullptr;
};

struct SliceExpr {
    Node *expr = nullptr;
    Node *start = nullptr;
    Node *end = nullptr;
    ChiStructMember *resolved_method = nullptr;
};

struct RangeExpr {
    Node *start = nullptr;
    Node *end = nullptr;
};

MAKE_ENUM(IdentifierKind, Value, TypeName, This, ThisType)

struct Identifier {
    IdentifierKind kind = IdentifierKind::Value;
    Node *decl = nullptr;
};

struct VarIdentifier {
    Node *size_expr = nullptr;
};

struct CastExpr {
    Node *dest_type = nullptr;
    Node *expr = nullptr;
};

MAKE_ENUM(CSizeClass, Default, Long, LongLong, Short)

struct SigilExpr {
    SigilKind sigil = SigilKind::None;
    Node *expr = nullptr;
};

struct ForStmt {
    ForLoopKind kind = ForLoopKind::Empty;
    Node *init = nullptr;
    Node *condition = nullptr;
    Node *post = nullptr;
    Node *body = nullptr;
    Node *bind = nullptr;
    Node *index_bind = nullptr;
    Node *expr = nullptr;
    SigilKind bind_sigil = SigilKind::None;
};

struct WhileStmt {
    Node *condition = nullptr;
    Node *body = nullptr;
};

struct SwitchExpr {
    Node *expr = nullptr;
    array<Node *> cases = {};
    bool is_type_switch = false;
};

struct CaseExpr {
    array<Node *> clauses = {};
    Node *body = nullptr;
    bool is_else = false;
};

struct TypeSigil {
    Node *type = nullptr;
    SigilKind sigil = SigilKind::None;
    Node *etype = nullptr;
    string lifetime;            // e.g. "this" from &'this int
    uint32_t fixed_size = 0;    // for SigilKind::FixedArray
    bool is_mut = false;        // for SigilKind::Span — []mut T
};

struct EnumVariant {
    Node *parent = nullptr;
    Token *name = nullptr;
    Node *value = nullptr;
    int64_t resolved_value = -1;
    Node *struct_body = nullptr;
    ChiType *resolved_type = nullptr;
    ChiEnumVariant *resolved_enum_variant = nullptr;
    ResolveStatus resolve_status = ResolveStatus::None;
};

struct PrefixExpr {
    Token *prefix = nullptr;
    Node *expr = nullptr;
};

struct PackExpansion {
    Node *expr = nullptr; // The expression being expanded (e.g., args)
};

struct CapturePath {
    Node *function = nullptr;   // The function in the capture chain
    int32_t capture_index = -1; // Index of this variable in this function's captures
};

struct EscapeAnalysis {
    bool escaped = false;
    bool moved = false;
    array<CapturePath> capture_path = {}; // Path from original declaration to current context

    bool is_capture() { return capture_path.len > 0; }

    // Get the original declaring function (root of the capture chain)
    Node *get_original_function() {
        return capture_path.len > 0 ? capture_path[0].function : nullptr;
    }

    // Get the immediate capturing function (last in the chain)
    Node *get_immediate_capturing_function() {
        return capture_path.len > 0 ? capture_path[capture_path.len - 1].function : nullptr;
    }

    // Get the capture depth (how many function levels deep)
    int get_capture_depth() { return capture_path.len; }

    // Get the capture index for the immediate capturing function (most commonly needed)
    int32_t get_immediate_capture_index() {
        return capture_path.len > 0 ? capture_path[capture_path.len - 1].capture_index : -1;
    }
};

struct SizeofExpr {
    Node *type = nullptr;
    Node *expr = nullptr;
};

struct ImportDecl {
    Token *path = nullptr;
    Token *alias = nullptr;
    Token *match_all = nullptr;
    array<Node *> symbols = {};
    ast::Module *resolved_module = nullptr;
    DeclSpec *decl_spec = {};
};

typedef ImportDecl ExportDecl;

struct ImportSymbol {
    Token *name = nullptr;
    Token *alias = nullptr;
    Node *import = nullptr;
    Node *resolved_decl = nullptr;

    string output_name() { return alias ? alias->get_name() : name->get_name(); }
};

struct DeclAttribute {
    Node *term = nullptr;
};

struct ExprInfo {
    Node *decl = nullptr;
    ChiStructMember *member = nullptr;
};

struct EnumDecl {
    DeclSpec *decl_spec = {};
    array<Node *> variants = {};
    Node *base_struct = nullptr;
    Token *discriminator_field = nullptr;
    Node *discriminator_type = nullptr;
    array<Node *> type_params = {};

    string get_discriminator_field() {
        return discriminator_field ? discriminator_field->get_name() : "__value";
    }
};

struct GeneratedFn {
    Node *original_fn = nullptr;
    ChiType *fn_subtype = nullptr;
    Node *fn_proto = nullptr;
};

struct Node {
    NodeType type = NodeType::Error;
    Token *token = nullptr;
    Module *module = nullptr;
    string name = "";
    ChiType *resolved_type = nullptr;
    ChiType *orig_type = nullptr;
    EscapeAnalysis escape = {};
    Node *parent_fn = nullptr;
    Node *root_node = nullptr;
    uint32_t id = 0;
    int index = 0;
    Node *parent = nullptr;
    Token *start_token = nullptr;
    Token *end_token = nullptr;
    string global_id = "";
    IntrinsicSymbol symbol = IntrinsicSymbol::None;
    int32_t decl_order = -1; // declaration order within function (-1 = not a local)
    Node *resolved_outlet = nullptr; // synthetic __tmp that owns this expr's result (for cleanup)

    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;

    union NodeData {
        Root root;
        FnProto fn_proto;
        FnDef fn_def;
        ParamDecl param_decl;
        Block block;
        ReturnStmt return_stmt;
        VarDecl var_decl;
        FnCallExpr fn_call_expr;
        BinOpExpr bin_op_expr;
        UnaryOpExpr unary_op_expr;
        TryExpr try_expr;
        AwaitExpr await_expr;
        Node *child_expr;
        Identifier identifier;
        IfExpr if_expr;
        StructDecl struct_decl;
        ConstructExpr construct_expr;
        DotExpr dot_expr;
        SubtypeExpr subtype_expr;
        IndexExpr index_expr;
        SliceExpr slice_expr;
        RangeExpr range_expr;
        TypedefDecl typedef_decl;
        TypeSigil sigil_type;
        EnumVariant enum_variant;
        VarIdentifier var_identifier;
        CastExpr cast_expr;
        ForStmt for_stmt;
        WhileStmt while_stmt;
        TypeParam type_param;
        LifetimeParam lifetime_param;
        PrefixExpr prefix_expr;
        PackExpansion pack_expansion;
        ExternDecl extern_decl;
        ImportDecl import_decl;
        ExportDecl export_decl;
        ImportSymbol import_symbol;
        DeclAttribute decl_attribute;
        SwitchExpr switch_expr;
        CaseExpr case_expr;
        FieldInitExpr field_init_expr;
        EnumDecl enum_decl;
        GeneratedFn generated_fn;
        ThrowStmt throw_stmt;
        ImplementBlockData implement_block;
        DestructureDecl destructure_decl;
        DestructureField destructure_field;

        NodeData() {}

        ~NodeData() {}
    } data;

#define _AST_CASE_INITIALIZE_FIELD(field, type)                                                    \
    case NodeType::type:                                                                           \
        new (&data.field) type();                                                                  \
        break;

    explicit Node(NodeType type) {
        this->type = type;
        memset(&data, 0, sizeof(data));

        switch (type) {
            _AST_CASE_INITIALIZE_FIELD(root, Root)
            _AST_CASE_INITIALIZE_FIELD(fn_proto, FnProto)
            _AST_CASE_INITIALIZE_FIELD(fn_def, FnDef)
            _AST_CASE_INITIALIZE_FIELD(block, Block)
            _AST_CASE_INITIALIZE_FIELD(fn_call_expr, FnCallExpr)
            _AST_CASE_INITIALIZE_FIELD(struct_decl, StructDecl)
            _AST_CASE_INITIALIZE_FIELD(typedef_decl, TypedefDecl)
            _AST_CASE_INITIALIZE_FIELD(enum_variant, EnumVariant)
            _AST_CASE_INITIALIZE_FIELD(construct_expr, ConstructExpr)
            _AST_CASE_INITIALIZE_FIELD(subtype_expr, SubtypeExpr)
            _AST_CASE_INITIALIZE_FIELD(extern_decl, ExternDecl)
            _AST_CASE_INITIALIZE_FIELD(import_decl, ImportDecl)
            _AST_CASE_INITIALIZE_FIELD(export_decl, ExportDecl)
            _AST_CASE_INITIALIZE_FIELD(decl_attribute, DeclAttribute)
            _AST_CASE_INITIALIZE_FIELD(switch_expr, SwitchExpr)
            _AST_CASE_INITIALIZE_FIELD(case_expr, CaseExpr)
            _AST_CASE_INITIALIZE_FIELD(field_init_expr, FieldInitExpr)
            _AST_CASE_INITIALIZE_FIELD(enum_decl, EnumDecl)
            _AST_CASE_INITIALIZE_FIELD(generated_fn, GeneratedFn)
            _AST_CASE_INITIALIZE_FIELD(lifetime_param, LifetimeParam)
            _AST_CASE_INITIALIZE_FIELD(destructure_decl, DestructureDecl)
        default:
            break;
        }
    }

#define _AST_CASE_DESTROY_FIELD(field, type)                                                       \
    case NodeType::type:                                                                           \
        data.field.~type();                                                                        \
        break;

    ~Node() {
        switch (type) {
            _AST_CASE_DESTROY_FIELD(root, Root)
            _AST_CASE_DESTROY_FIELD(fn_proto, FnProto)
            _AST_CASE_DESTROY_FIELD(fn_def, FnDef)
            _AST_CASE_DESTROY_FIELD(block, Block)
            _AST_CASE_DESTROY_FIELD(fn_call_expr, FnCallExpr)
            _AST_CASE_DESTROY_FIELD(struct_decl, StructDecl)
            _AST_CASE_DESTROY_FIELD(typedef_decl, TypedefDecl)
            _AST_CASE_DESTROY_FIELD(enum_variant, EnumVariant)
            _AST_CASE_DESTROY_FIELD(construct_expr, ConstructExpr)
            _AST_CASE_DESTROY_FIELD(subtype_expr, SubtypeExpr)
            _AST_CASE_DESTROY_FIELD(extern_decl, ExternDecl)
            _AST_CASE_DESTROY_FIELD(import_decl, ImportDecl)
            _AST_CASE_DESTROY_FIELD(export_decl, ExportDecl)
            _AST_CASE_DESTROY_FIELD(decl_attribute, DeclAttribute)
            _AST_CASE_DESTROY_FIELD(switch_expr, SwitchExpr)
            _AST_CASE_DESTROY_FIELD(case_expr, CaseExpr)
            _AST_CASE_DESTROY_FIELD(field_init_expr, FieldInitExpr)
            _AST_CASE_DESTROY_FIELD(enum_decl, EnumDecl)
            _AST_CASE_DESTROY_FIELD(generated_fn, GeneratedFn)
            _AST_CASE_DESTROY_FIELD(destructure_decl, DestructureDecl)
        default:
            memset(&data, 0, sizeof(data));
            break;
        }
    }

#define _AST_CASE_CLONE_FIELD(field, type)                                                         \
    case NodeType::type:                                                                           \
        b->data.field = data.field;                                                                \
        break;

    void clone(ast::Node *b) {
        b->type = type;
        b->token = token;
        b->module = module;
        b->name = name;
        b->resolved_type = resolved_type;
        b->orig_type = orig_type;
        b->escape = escape;
        b->parent_fn = parent_fn;
        b->index = index;
        b->parent = parent;

        switch (type) {
            _AST_CASE_CLONE_FIELD(root, Root)
            _AST_CASE_CLONE_FIELD(fn_proto, FnProto)
            _AST_CASE_CLONE_FIELD(fn_def, FnDef)
            _AST_CASE_CLONE_FIELD(block, Block)
            _AST_CASE_CLONE_FIELD(fn_call_expr, FnCallExpr)
            _AST_CASE_CLONE_FIELD(struct_decl, StructDecl)
            _AST_CASE_CLONE_FIELD(typedef_decl, TypedefDecl)
            _AST_CASE_CLONE_FIELD(enum_variant, EnumVariant)
            _AST_CASE_CLONE_FIELD(construct_expr, ConstructExpr)
            _AST_CASE_CLONE_FIELD(subtype_expr, SubtypeExpr)
            _AST_CASE_CLONE_FIELD(extern_decl, ExternDecl)
            _AST_CASE_CLONE_FIELD(import_decl, ImportDecl)
            _AST_CASE_CLONE_FIELD(decl_attribute, DeclAttribute)
            _AST_CASE_CLONE_FIELD(switch_expr, SwitchExpr)
            _AST_CASE_CLONE_FIELD(case_expr, CaseExpr)
            _AST_CASE_CLONE_FIELD(field_init_expr, FieldInitExpr)
            _AST_CASE_CLONE_FIELD(enum_decl, EnumDecl)
            _AST_CASE_CLONE_FIELD(generated_fn, GeneratedFn)
            _AST_CASE_CLONE_FIELD(destructure_decl, DestructureDecl)
        default:
            memcpy(&b->data, &data, sizeof(data));
            break;
        }
    }

    bool is_heap_allocated() { return escape.escaped; }

    Node *get_decl(optional<TypeId> container_type_id = std::nullopt) {
        switch (type) {
        case NodeType::VarDecl:
            return this;
        case NodeType::Identifier:
            return data.identifier.decl ? data.identifier.decl->get_decl() : nullptr;
        case NodeType::DotExpr: {
            if (data.dot_expr.should_resolve_variant && container_type_id.has_value()) {
                auto parent_id = *container_type_id;
                auto member_name = data.dot_expr.field->get_name();
                auto base_member = data.dot_expr.resolved_struct_member->root_variant;
                if (!base_member) {
                    base_member = data.dot_expr.resolved_struct_member;
                }

                assert(base_member);
                auto variant_member = base_member->variants.get(parent_id);
                if (!variant_member) {
                    panic("variant member not found: {}", member_name);
                }
                return (*variant_member)->node;
            }
            auto decl = data.dot_expr.resolved_decl;
            if (decl) {
                return decl;
            }
            return nullptr;
        }
        case NodeType::ImportSymbol:
            return data.import_symbol.resolved_decl;
        case NodeType::ExportDecl:
            return this;
        case NodeType::FnDef:
            return this;
        case NodeType::UnaryOpExpr:
            return data.unary_op_expr.op1->get_decl();
        default:
            break;
        }
        return nullptr;
    }

    Node *get_root_node() { return root_node ? root_node : this; }

    bool is_mutable() {
        switch (type) {
        case NodeType::VarDecl:
            return data.var_decl.kind == VarKind::Mutable;
        default:
            return false;
        }
    }

    DeclSpec *get_declspec() {
        switch (type) {
        case NodeType::FnDef:
            return data.fn_def.decl_spec;
        case NodeType::VarDecl:
            return data.var_decl.decl_spec;
        case NodeType::StructDecl:
            return data.struct_decl.decl_spec;
        case NodeType::EnumDecl:
            return data.enum_decl.decl_spec;
        case NodeType::ExportDecl:
            return data.export_decl.decl_spec;
        case NodeType::FnProto:
            if (!data.fn_proto.fn_def_node) {
                return nullptr;
            }
            return data.fn_proto.fn_def_node->get_declspec();
        case NodeType::GeneratedFn:
            return data.generated_fn.original_fn->get_declspec();
        default:
            return nullptr;
        }
    }

    DeclSpec &declspec_ref() {
        auto ptr = get_declspec();
        if (!ptr) {
            panic("node type {} does not have declspec", PRINT_ENUM(type));
        }
        return *ptr;
    };

    DeclSpec declspec() {
        auto ptr = get_declspec();
        if (!ptr) {
            return {};
        }
        return *ptr;
    }

    bool can_escape() {
        if (type == NodeType::Identifier) {
            return data.identifier.kind == IdentifierKind::Value;
        }
        return true;
    }

    bool is_ancestor_of(Node *child) {
        for (auto *n = child; n; n = n->parent) {
            if (n == this)
                return true;
        }
        return false;
    }

    bool is_last_stmt() {
        return parent && parent->type == NodeType::Block &&
               index == parent->data.block.statements.len - 1;
    }
};
} // namespace ast

typedef array<ast::Node *> NodeList;
} // namespace cx