
/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "sema.h"

namespace cx {
namespace ast {
struct Node;
struct Package;
struct Scope;

MAKE_ENUM(NodeType, Error, Root, FnProto, FnDef, ParamDecl, Block, ReturnStmt, VarDecl, BinOpExpr,
          UnaryOpExpr, LiteralExpr, IfStmt, FnCallExpr, Primitive, Identifier, EmptyStmt,
          ConstructExpr, ParenExpr, StructDecl, DotExpr, SubtypeExpr, IndexExpr, TypedefDecl,
          TypeSigil, EnumMember, CastExpr, ForStmt, WhileStmt, BranchStmt, TypeParam, PrefixExpr,
          ExternDecl, TryExpr, InferredType, ImportDecl, SizeofExpr, DeclAttribute, BindIdentifier,
          SwitchExpr, CaseExpr, ImportSymbol, ExportDecl, FieldInitExpr);

MAKE_ENUM(ModuleKind, XC, XM);
MAKE_ENUM(ForLoopKind, Empty, Ternary, Range);

enum FnParsingFlags : uint32_t {
    FN_BODY_REQUIRED = 1 << 0,
    FN_BODY_NONE = 1 << 1,
};

enum DeclFlag : uint32_t {
    DECL_NONE = 0,
    DECL_PRIVATE = 1 << 0,
    DECL_EXTERN = 1 << 1,
    DECL_IS_ENTRY = 1 << 2,
};

struct Module {
    ModuleKind kind = ModuleKind::XC;
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
    bool broken = false;

    Module() = default;
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;

    string full_path() const { return (fs::path(path) / filename).string(); }
    string global_id() const;

    static ModuleKind kind_from_extension(const string &ext) {
        if (ext == ".xc") {
            return ModuleKind::XC;
        } else if (ext == ".x") {
            return ModuleKind::XM;
        } else {
            panic("unknown module extension: {}", ext);
        }
        return ModuleKind::XC;
    }

    LANG_FLAG get_lang_flags() const {
        if (kind == ModuleKind::XM) {
            return LANG_FLAG_MANAGED;
        }
        return LANG_FLAG_NONE;
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
        return Visibility::Public;
    }

    bool is_exported() const { return get_visibility() == Visibility::Public; }
    bool has_flag(DeclFlag flag) const { return (flags & flag) != 0; }
    bool is_extern() const { return has_flag(DECL_EXTERN); }
};

struct FnProto {
    array<Node *> params = {};
    Node *return_type = nullptr;
    Node *fn_def_node = nullptr;
    bool is_vararg = false;
    bool is_type_expr = false;
};

MAKE_ENUM(FnKind, TopLevel, InstanceMethod, StaticMethod, Constructor, Destructor, Lambda);

struct FnDef {
    Node *fn_proto = nullptr;
    Node *body = nullptr;
    FnKind fn_kind = FnKind::TopLevel;
    DeclSpec *decl_spec = nullptr;
    array<Node *> captures = {};
    map<Node *, int32_t> capture_map = {};
    bool is_generated = false;
    array<Node *> cleanup_vars = {};
    bool has_try = false;

    bool is_instance_method() {
        return fn_kind != FnKind::StaticMethod && fn_kind != FnKind::TopLevel;
    }

    bool has_try_or_cleanup() { return has_try || cleanup_vars.len; }
};

struct ParamDecl {
    Node *type = nullptr;
    bool is_variadic = false;
};

struct TypeParam {
    Node *type = nullptr;
    long index = 0;
    Node *bound = nullptr;
};

struct Block {
    array<Node *> statements = {};
    array<Node *> implicit_vars = {};
    cx::Scope *scope = nullptr;
    bool is_arrow = false;
    Node *return_expr = nullptr;
    bool has_braces = false;
};

struct ReturnStmt {
    Node *expr = nullptr;
};

struct VarDecl {
    Token *identifier = nullptr;
    Node *type = nullptr;
    Node *expr = nullptr;
    bool is_const = false;
    bool is_field = false;
    bool is_embed = false;
    ChiStructMember *resolved_field = nullptr;
    optional<ConstantValue> resolved_value = std::nullopt;
    DeclSpec *decl_spec = {};
    bool is_generated = false;
    Node *initialized_at = nullptr;
};

struct BinOpExpr {
    TokenType op_type = TokenType::ERROR;
    Node *op1 = nullptr;
    Node *op2 = nullptr;
};

struct UnaryOpExpr {
    TokenType op_type = TokenType::ERROR;
    Node *op1 = nullptr;
    bool is_suffix = false;
};

struct TryExpr {
    Node *expr = nullptr;
    Node *catch_expr = nullptr;
};

struct FnCallExpr {
    Node *fn_ref_expr = nullptr;
    array<Node *> args = {};
    bool is_builtin = false;
};

struct IfStmt {
    Node *condition = nullptr;
    Node *then_block = nullptr;
    Node *else_node = nullptr; // can be null, block node, or another if node
};

struct StructDecl {
    array<Node *> members = {};
    ContainerKind kind = ContainerKind::Struct;
    array<Node *> type_params = {};
    array<Node *> implements = {};
    DeclSpec *decl_spec = {};
};

struct ExternDecl {
    Token *type = nullptr;
    array<Node *> members = {};
};

struct FieldInitExpr {
    Token *token = nullptr;
    Token *field = nullptr;
    Node *value = nullptr;
    ChiStructMember *resolved_field = nullptr;
};

// composite literal
struct ConstructExpr {
    bool is_new = false;
    array<Node *> items = {};
    array<Node *> field_inits = {};
    Node *type = nullptr;
    Node *resolved_outlet = nullptr;
};

struct TypedefDecl {
    Node *type = nullptr;
    Token *identifier = nullptr;
};

struct DotExpr {
    Node *expr = nullptr;
    Token *field = nullptr;
    ChiStructMember *resolved_member = nullptr;
    int64_t resolved_value = 0;
    Node *resolved_decl = nullptr;
    bool should_resolve_variant = false;
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

MAKE_ENUM(IdentifierKind, Value, TypeName, This)

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

MAKE_ENUM(SigilKind, None, Pointer, Reference, Optional, Box)

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
    Node *expr = nullptr;
    bool is_ref = false;
};

struct WhileStmt {
    Node *condition = nullptr;
    Node *body = nullptr;
};

struct SwitchExpr {
    Node *expr = nullptr;
    array<Node *> cases = {};
    Node *resolved_outlet = nullptr;
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
};

struct EnumMember {
    Node *value = nullptr;
    int64_t resolved_value = 0;
};

struct PrefixExpr {
    Token *prefix = nullptr;
    Node *expr = nullptr;
};

struct EscapeAnalysis {
    bool escaped = false;
    int32_t local_index = -1;
    bool moved = false;

    bool is_capture() { return local_index >= 0; }
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

struct Node {
    NodeType type = NodeType::Error;
    Token *token = nullptr;
    Module *module = nullptr;
    string name = "";
    ChiType *resolved_type = nullptr;
    ChiType *orig_type = nullptr;
    EscapeAnalysis escape = {};
    Node *parent_fn = nullptr;
    uint32_t id = 0;
    int index = 0;
    Node *parent = nullptr;
    Token *start_token = nullptr;
    Token *end_token = nullptr;
    string global_id = "";
    IntrinsicSymbol symbol = IntrinsicSymbol::None;

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
        Node *child_expr;
        Identifier identifier;
        IfStmt if_stmt;
        StructDecl struct_decl;
        ConstructExpr construct_expr;
        DotExpr dot_expr;
        SubtypeExpr subtype_expr;
        IndexExpr index_expr;
        TypedefDecl typedef_decl;
        TypeSigil sigil_type;
        EnumMember enum_member;
        VarIdentifier var_identifier;
        CastExpr cast_expr;
        ForStmt for_stmt;
        WhileStmt while_stmt;
        TypeParam type_param;
        PrefixExpr prefix_expr;
        ExternDecl extern_decl;
        ImportDecl import_decl;
        ExportDecl export_decl;
        ImportSymbol import_symbol;
        DeclAttribute decl_attribute;
        SwitchExpr switch_expr;
        CaseExpr case_expr;
        FieldInitExpr field_init_expr;

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
            _AST_CASE_INITIALIZE_FIELD(enum_member, EnumMember)
            _AST_CASE_INITIALIZE_FIELD(construct_expr, ConstructExpr)
            _AST_CASE_INITIALIZE_FIELD(subtype_expr, SubtypeExpr)
            _AST_CASE_INITIALIZE_FIELD(extern_decl, ExternDecl)
            _AST_CASE_INITIALIZE_FIELD(import_decl, ImportDecl)
            _AST_CASE_INITIALIZE_FIELD(export_decl, ExportDecl)
            _AST_CASE_INITIALIZE_FIELD(decl_attribute, DeclAttribute)
            _AST_CASE_INITIALIZE_FIELD(switch_expr, SwitchExpr)
            _AST_CASE_INITIALIZE_FIELD(case_expr, CaseExpr)
            _AST_CASE_INITIALIZE_FIELD(field_init_expr, FieldInitExpr)
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
            _AST_CASE_DESTROY_FIELD(enum_member, EnumMember)
            _AST_CASE_DESTROY_FIELD(construct_expr, ConstructExpr)
            _AST_CASE_DESTROY_FIELD(subtype_expr, SubtypeExpr)
            _AST_CASE_DESTROY_FIELD(extern_decl, ExternDecl)
            _AST_CASE_DESTROY_FIELD(import_decl, ImportDecl)
            _AST_CASE_DESTROY_FIELD(export_decl, ExportDecl)
            _AST_CASE_DESTROY_FIELD(decl_attribute, DeclAttribute)
            _AST_CASE_DESTROY_FIELD(switch_expr, SwitchExpr)
            _AST_CASE_DESTROY_FIELD(case_expr, CaseExpr)
            _AST_CASE_DESTROY_FIELD(field_init_expr, FieldInitExpr)
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
            _AST_CASE_CLONE_FIELD(enum_member, EnumMember)
            _AST_CASE_CLONE_FIELD(construct_expr, ConstructExpr)
            _AST_CASE_CLONE_FIELD(subtype_expr, SubtypeExpr)
            _AST_CASE_CLONE_FIELD(extern_decl, ExternDecl)
            _AST_CASE_CLONE_FIELD(import_decl, ImportDecl)
            _AST_CASE_CLONE_FIELD(decl_attribute, DeclAttribute)
            _AST_CASE_CLONE_FIELD(switch_expr, SwitchExpr)
            _AST_CASE_CLONE_FIELD(case_expr, CaseExpr)
            _AST_CASE_CLONE_FIELD(field_init_expr, FieldInitExpr)
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
            return data.identifier.decl->get_decl();
        case NodeType::DotExpr: {
            if (data.dot_expr.should_resolve_variant && container_type_id.has_value()) {
                auto parent_id = *container_type_id;
                auto member_name = data.dot_expr.field->get_name();
                auto base_member = data.dot_expr.resolved_member->root_variant;
                if (!base_member) {
                    base_member = data.dot_expr.resolved_member;
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
            return data.unary_op_expr.op1;
        default:
            break;
        }
        return nullptr;
    }

    DeclSpec *get_declspec() {
        switch (type) {
        case NodeType::FnDef:
            return data.fn_def.decl_spec;
        case NodeType::VarDecl:
            return data.var_decl.decl_spec;
        case NodeType::StructDecl:
            return data.struct_decl.decl_spec;
        case NodeType::ExportDecl:
            return data.export_decl.decl_spec;
        case NodeType::FnProto:
            if (!data.fn_proto.fn_def_node) {
                return nullptr;
            }
            return data.fn_proto.fn_def_node->get_declspec();
        default:
            return nullptr;
        }
    }

    DeclSpec &declspec() {
        auto ptr = get_declspec();
        if (!ptr) {
            panic("node type {} does not have declspec", PRINT_ENUM(type));
        }
        return *ptr;
    };

    bool can_escape() {
        if (type == NodeType::Identifier) {
            return data.identifier.kind == IdentifierKind::Value;
        }
        return true;
    }

    bool is_last_stmt() {
        return parent && parent->type == NodeType::Block &&
               index == parent->data.block.statements.len - 1;
    }
};
} // namespace ast

typedef array<ast::Node *> NodeList;
} // namespace cx