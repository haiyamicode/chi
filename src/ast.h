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

        MAKE_ENUM(NodeType,
                  Error,
                  Root,
                  FnProto,
                  FnDef,
                  ParamDecl,
                  Block,
                  ReturnStmt,
                  VarDecl,
                  BinOpExpr,
                  UnaryOpExpr,
                  LiteralExpr,
                  IfStmt,
                  FnCallExpr,
                  Primitive,
                  Identifier,
                  EmptyStmt,
                  ParenExpr,
                  StructDecl,
                  ComplitExpr,
                  DotExpr,
                  SubtypeExpr,
                  IndexExpr,
                  TypedefDecl,
                  TypeSigil,
                  EnumMember,
                  VarIdentifier,
                  CastExpr,
                  ForStmt,
                  BranchStmt
        );

        MAKE_ENUM(ModuleKind, CX, CHX, HEADER)

        struct Module {
            ModuleKind kind;
            Node* root;
            Package* package;
            string path;
            array<Node*> imports;
        };

        struct Package {
            array<Module> modules;
            Node* entry_fn = nullptr;
            string root_src_dir;
            string root_src_path;
        };

        struct Root {
            array<Node*> top_level_decls;
        };

        struct FnProto {
            array<Node*> params;
            Node* return_type;
            Node* fn_def_node;
        };

        MAKE_ENUM(FnKind, TopLevel, InstanceMethod, StaticMethod, Constructor, Destructor);
        MAKE_ENUM(BuiltinId, Invalid, Printf, ArrayAdd)

        struct FnDef {
            Node* fn_proto;
            Node* body;
            BuiltinId builtin_id;
            FnKind fn_kind;
            Node* container;

            bool is_instance_method() {
                return fn_kind != FnKind::StaticMethod && fn_kind != FnKind::TopLevel;
            }
        };

        struct ParamDecl {
            Node* type;
        };

        struct Block {
            array<Node*> statements;
        };

        struct ReturnStmt {
            Node* expr = nullptr;
        };

        struct VarDecl {
            Node* identifier;
            Node* type;
            Node* expr;
            bool is_embed;
            ChiStructField* resolved_field = nullptr;
        };

        struct BinOpExpr {
            TokenType op_type;
            Node* op1;
            Node* op2;
        };

        struct UnaryOpExpr {
            TokenType op_type;
            Node* op1;
            bool is_suffix;
        };

        struct FnCallExpr {
            Node* fn_ref_expr;
            array<Node*> args;
            bool is_builtin;
        };

        struct IfStmt {
            Node* condition;
            Node* then_block;
            Node* else_node; // can be null, block node, or another if node
        };

        struct StructDecl {
            array<Node*> members;
            ContainerKind kind;
        };

        // composite literal
        struct ComplitExpr {
            array<Node*> items;
        };

        struct TypedefDecl {
            Node* type;
            Node* identifier;
        };

        struct DotExpr {
            Node* expr;
            Token* field;
            ChiStructMember* resolved_member;
            int64_t resolved_value;
        };

        struct SubtypeExpr {
            Node* type;
            array<Node*> args;
        };

        struct IndexExpr {
            Node* expr;
            Node* subscript;
        };

        MAKE_ENUM(IdentifierKind,
                  Value,
                  TypeName,
                  This
        )

        struct Identifier {
            IdentifierKind kind;
            Node* decl;
        };

        struct VarIdentifier {
            Node* size_expr;
        };

        struct CastExpr {
            Node* dest_type;
            Node* expr;
        };

        struct ForStmt {
            Node* init;
            Node* condition;
            Node* post;
            Node* body;
        };

        MAKE_ENUM(CSizeClass,
                  Default,
                  Long,
                  LongLong,
                  Short
        );

        MAKE_ENUM(SigilKind, Pointer)

        struct TypeSigil {
            Node* type;
            SigilKind sigil;
        };

        struct EnumMember {
            Node* value;
            int64_t resolved_value;
        };

        struct Node {
            NodeType type;
            Token* token;
            Module* module;
            string name;
            ChiType* resolved_type = nullptr;

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
                Node* child_expr;
                Identifier identifier;
                IfStmt if_stmt;
                StructDecl struct_decl;
                ComplitExpr complit_expr;
                DotExpr dot_expr;
                SubtypeExpr subtype_expr;
                IndexExpr index_expr;
                TypedefDecl typedef_decl;
                TypeSigil type_sigil;
                EnumMember enum_member;
                VarIdentifier var_identifier;
                CastExpr cast_expr;
                ForStmt for_stmt;

                NodeData() {}

                ~NodeData() {}
            } data;

            Node(const Node&) = delete;

            explicit Node(NodeType type) {
                this->type = type;
                memset(&data, 0, sizeof(data));
            }

#define AST_CASE_DESTROY_FIELD(field, type) case NodeType::type: data.field.~type(); break;

            ~Node() {
                switch (type) {
                    AST_CASE_DESTROY_FIELD(root, Root)
                    AST_CASE_DESTROY_FIELD(fn_proto, FnProto)
                    AST_CASE_DESTROY_FIELD(block, Block)
                    AST_CASE_DESTROY_FIELD(fn_call_expr, FnCallExpr)
                    AST_CASE_DESTROY_FIELD(struct_decl, StructDecl)
                    AST_CASE_DESTROY_FIELD(typedef_decl, TypedefDecl)
                    AST_CASE_DESTROY_FIELD(enum_member, EnumMember)
                    default:
                        break;
                }
            }

        };
    }

    typedef array<ast::Node*> NodeList;
}