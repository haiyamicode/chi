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

MAKE_ENUM(NodeType, Error, Root, FnProto, FnDef, ParamDecl, Block, ReturnStmt, VarDecl, BinOpExpr,
          UnaryOpExpr, LiteralExpr, IfStmt, FnCallExpr, Primitive, Identifier, EmptyStmt,
          ConstructExpr, ParenExpr, StructDecl, DotExpr, SubtypeExpr, IndexExpr, TypedefDecl,
          TypeSigil, EnumMember, CastExpr, ForStmt, BranchStmt, TypeParam, PrefixExpr, ExternDecl);

MAKE_ENUM(ModuleKind, CX, CHX, HEADER)
MAKE_ENUM(FnBodyMode, Optional, Required, None);

enum DeclFlag : uint32_t {
    DECL_NONE = 0,
    DECL_EXPORTED = 1 << 0,
    DECL_PRIVATE = 1 << 1,
    DECL_EXTERN = 1 << 2,
};

struct Module {
    ModuleKind kind;
    Node *root;
    Package *package;
    string path;
    array<Node *> imports = {};
    array<Node *> exports = {};
};

enum class PackageKind { BUILTIN, DEFAULT };

struct Package {
    array<Module> modules;
    Node *entry_fn = nullptr;
    string root_src_dir;
    string root_src_path;
    PackageKind kind = PackageKind::DEFAULT;
};

struct Root {
    array<Node *> top_level_decls;
};

struct DeclSpec {
    uint32_t flags = DECL_NONE;
};

struct FnProto {
    array<Node *> params;
    Node *return_type = nullptr;
    Node *fn_def_node;
};

MAKE_ENUM(FnKind, TopLevel, InstanceMethod, StaticMethod, Constructor, Destructor);

struct FnDef {
    Node *fn_proto;
    Node *body;
    FnKind fn_kind;
    DeclSpec decl_spec;

    bool is_instance_method() {
        return fn_kind != FnKind::StaticMethod && fn_kind != FnKind::TopLevel;
    }
};

struct ParamDecl {
    Node *type;
    bool is_variadic;
};

struct TypeParam {
    Node *type;
    long index;
};

struct Block {
    array<Node *> statements;
};

struct ReturnStmt {
    Node *expr = nullptr;
};

struct VarDecl {
    Token *identifier;
    Node *type;
    Node *expr;
    bool is_const;
    bool is_field;
    bool is_embed;
    ChiStructMember *resolved_field = nullptr;
    ConstantValue resolved_value;
};

struct BinOpExpr {
    TokenType op_type;
    Node *op1;
    Node *op2;
};

struct UnaryOpExpr {
    TokenType op_type;
    Node *op1;
    bool is_suffix;
};

struct FnCallExpr {
    Node *fn_ref_expr;
    array<Node *> args;
    bool is_builtin;
};

struct IfStmt {
    Node *condition;
    Node *then_block;
    Node *else_node; // can be null, block node, or another if node
};

struct StructDecl {
    array<Node *> members;
    ContainerKind kind;
    array<Node *> type_params;
    array<Node *> implements;
    DeclSpec decl_spec;
};

struct ExternDecl {
    Token *type;
    array<Node *> members;
};

// composite literal
struct ConstructExpr {
    bool is_new;
    array<Node *> items;
    Node *type;
};

struct TypedefDecl {
    Node *type;
    Token *identifier;
};

struct DotExpr {
    Node *expr;
    Token *field;
    ChiStructMember *resolved_member;
    int64_t resolved_value;
};

struct SubtypeExpr {
    Node *type;
    array<Node *> args;
};

struct IndexExpr {
    Node *expr;
    Node *subscript;
};

MAKE_ENUM(IdentifierKind, Value, TypeName, This)

struct Identifier {
    IdentifierKind kind;
    Node *decl;
};

struct VarIdentifier {
    Node *size_expr;
};

struct CastExpr {
    Node *dest_type;
    Node *expr;
};

struct ForStmt {
    Node *init;
    Node *condition;
    Node *post;
    Node *body;
};

MAKE_ENUM(CSizeClass, Default, Long, LongLong, Short);

MAKE_ENUM(SigilKind, Pointer, Reference, Optional, Box)

struct TypeSigil {
    Node *type;
    SigilKind sigil;
    Node *etype;
};

struct EnumMember {
    Node *value;
    int64_t resolved_value;
};

struct PrefixExpr {
    Token *prefix;
    Node *expr;
};

struct Node {
    NodeType type;
    Token *token;
    Module *module;
    string name;
    ChiType *resolved_type = nullptr;
    ChiType *orig_type = nullptr;

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

        NodeData() {}

        ~NodeData() {}
    } data;

    Node(const Node &) = delete;

    explicit Node(NodeType type) {
        this->type = type;
        memset(&data, 0, sizeof(data));
    }

#define _AST_CASE_DESTROY_FIELD(field, type)                                                       \
    case NodeType::type:                                                                           \
        data.field.~type();                                                                        \
        break;

    ~Node() {
        switch (type) {
            _AST_CASE_DESTROY_FIELD(root, Root)
            _AST_CASE_DESTROY_FIELD(fn_proto, FnProto)
            _AST_CASE_DESTROY_FIELD(block, Block)
            _AST_CASE_DESTROY_FIELD(fn_call_expr, FnCallExpr)
            _AST_CASE_DESTROY_FIELD(struct_decl, StructDecl)
            _AST_CASE_DESTROY_FIELD(typedef_decl, TypedefDecl)
            _AST_CASE_DESTROY_FIELD(enum_member, EnumMember)
            _AST_CASE_DESTROY_FIELD(construct_expr, ConstructExpr)
            _AST_CASE_DESTROY_FIELD(subtype_expr, SubtypeExpr)
            _AST_CASE_DESTROY_FIELD(extern_decl, ExternDecl)
        default:
            break;
        }
    }
};
} // namespace ast

typedef array<ast::Node *> NodeList;
} // namespace cx