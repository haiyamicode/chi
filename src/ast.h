/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "lexer.h"

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
                  LiteralExpr,
                  FnCallExpr,
                  IfBoolExpr,
                  Identifier,
                  EmptyStmt,
                  ParenExpr
        );

        struct Module {
            Node* root;
            Package* package;
            string path;
            array<Node*> imports;
        };

        struct Package {
            array<Module> modules;
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

        struct FnDef {
            Node* fn_proto;
            Node* body;
        };

        struct ParamDecl {
            Node* type;
        };

        struct Block {
            array<Node*> statements;
        };

        struct ReturnStmt {
            Node* expr = NULL;
        };

        struct VarDecl {
            Node* type = NULL;
            Node* expr = NULL;
        };

        struct BinOpExpr {
            TokenType op_type;
            Node* op1;
            Node* op2;
        };

        struct FnCallExpr {
            Node* fn_ref_expr;
            array<Node*> args;
            bool is_builtin;
        };

        struct IfBoolExpr {
            Node* condition;
            Node* then_block;
            Node* else_node; // can be null, block node, or another if node
        };

        MAKE_ENUM(IdentifierKind,
                  Value,
                  TypeName
        )

        struct Identifier {
            IdentifierKind kind;
            bool is_builtin;
        };

        struct Node {
            NodeType type;
            Token* token;
            Module* module;
            string name;

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
                Node* child_expr;
                Identifier identifier;

                NodeData() {}

                ~NodeData() {}
            } data;

            Node(NodeType type) {
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
                    default:
                        break;
                }
            }

        };
    }
}