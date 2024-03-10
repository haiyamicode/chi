
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
          TypeSigil, EnumMember, CastExpr, ForStmt, BranchStmt, TypeParam, PrefixExpr, ExternDecl,
          TryExpr, InferredType, ImportDecl, SizeofExpr, DeclAttribute, BindIdentifier,
          ImportSymbol);

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
    string name = "";
    string filename = "";
    array<Node *> exports = {};
    array<Error> errors = {};
    cx::Scope *scope = nullptr;
    optional<string> source = {};
    array<Module *> imports = {};

    string full_path() const { return (fs::path(path) / filename).string(); }

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
    string root_src_dir = "";
    string root_src_path = "";
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

    bool has_try_or_cleanup() { return has_try || cleanup_vars.size; }
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
    ConstantValue resolved_value = {};
    DeclSpec *decl_spec = {};
    bool is_generated = false;
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

// composite literal
struct ConstructExpr {
    bool is_new = false;
    array<Node *> items = {};
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
    bool resolve_variant = false;
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
    array<Node *> symbols = {};
    ast::Module *resolved_module = nullptr;
};

struct ImportSymbol {
    Token *name = nullptr;
    Token *alias = nullptr;
    Node *import = nullptr;
    Node *resolved_decl = nullptr;
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
        TypeParam type_param;
        PrefixExpr prefix_expr;
        ExternDecl extern_decl;
        ImportDecl import_decl;
        ImportSymbol import_symbol;
        DeclAttribute decl_attribute;

        NodeData() {}

        ~NodeData() {}
    } data;

    Node(const Node &) = delete;

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
            _AST_CASE_INITIALIZE_FIELD(decl_attribute, DeclAttribute)
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
            _AST_CASE_DESTROY_FIELD(decl_attribute, DeclAttribute)
        default:
            break;
        }
    }

    bool is_heap_allocated() { return escape.escaped; }

    Node *get_decl(ChiTypeSubtype *variant_input = nullptr) {
        switch (type) {
        case NodeType::VarDecl:
            return this;
        case NodeType::Identifier:
            return data.identifier.decl->get_decl();
        case NodeType::DotExpr: {
            if (data.dot_expr.resolve_variant) {
                auto variant_member = data.dot_expr.resolved_member->variants[variant_input];
                assert(variant_member);
                return variant_member->node;
            }
            auto decl = data.dot_expr.resolved_decl;
            if (decl) {
                return decl;
            }
            return nullptr;
        }
        case NodeType::ImportSymbol:
            return data.import_symbol.resolved_decl;
        case NodeType::FnDef:
            return this;
        default:
            panic("node type {} does not have decl", PRINT_ENUM(type));
        }
        return nullptr;
    }

    DeclSpec &get_declspec() {
        switch (type) {
        case NodeType::FnDef:
            return *data.fn_def.decl_spec;
        case NodeType::VarDecl:
            return *data.var_decl.decl_spec;
        case NodeType::StructDecl:
            return *data.struct_decl.decl_spec;
        default:
            panic("node type {} does not have declspec", PRINT_ENUM(type));
        };
        return *data.fn_def.decl_spec;
    }

    bool can_escape() {
        if (type == NodeType::Identifier) {
            return data.identifier.kind == IdentifierKind::Value;
        }
        return true;
    }
};
} // namespace ast

typedef array<ast::Node *> NodeList;
} // namespace cx