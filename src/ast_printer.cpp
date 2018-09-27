/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "ast_printer.h"

using namespace cx;
using namespace cx::ast;

void cx::print_ast(Node* root) {
    AstPrinter printer(root);
    return printer.print_ast();
}

void AstPrinter::print_ast() {
    print_node(m_root);
}

void AstPrinter::print_node(Node* node) {
    switch (node->type) {
        case NodeType::Root: {
            for (auto decl: m_root->data.root.top_level_decls) {
                print_node(decl);
                print("\n");
            }
            break;
        }
        case NodeType::FnDef: {
            auto& data = node->data.fn_def;
            print_node(data.fn_proto);
            print(" ");
            print_node(data.body);
            break;
        }
        case NodeType::FnProto: {
            auto& data = node->data.fn_proto;
            print("[@func] ");
            print_node(data.return_type);
            print(" {}(", node->name);
            print_node_list(&data.params);
            print(")");
            break;
        }
        case NodeType::Identifier: {
            print("{}", node->name);
            break;
        }
        case NodeType::ParamDecl: {
            auto& data = node->data.param_decl;
            print_node(data.type);
            print(" ");
            print(node->name);
            break;
        }
        case NodeType::Block: {
            auto& data = node->data.block;
            print("{{\n");
            m_indent++;
            for (auto stmt: data.statements) {
                print_indent(m_indent);
                print_node(stmt);
                if (stmt->type != NodeType::IfStmt) {
                    print(";\n");
                }
            }
            m_indent--;
            print_indent(m_indent);
            print("}}\n");
            break;
        }
        case NodeType::VarDecl: {
            auto& data = node->data.var_decl;
            print("[@var] ");
            print_node(data.type);
            print(" {}", node->name);
            if (data.expr) {
                print(" = ");
                print_node(data.expr);
            }
            break;
        }
        case NodeType::StructDecl: {
            auto& data = node->data.struct_decl;
            print("struct {} ", node->name);
            print("{{\n");
            m_indent++;
            for (auto member: data.members) {
                print_indent(m_indent);
                if (member->type == NodeType::FnDef) {
                    auto kind = member->data.fn_def.fn_kind;
                    print("[@{}] ", PRINT_ENUM(kind));
                }
                print_node(member);
                if (member->type == NodeType::VarDecl) {
                    print(";\n");
                }
            }
            m_indent--;
            print("}}\n");
            break;
        }
        case NodeType::DotExpr: {
            auto& data = node->data.dot_expr;
            print_node(data.expr);
            print(".{}", data.field->str);
            break;
        }
        case NodeType::ComplitExpr: {
            auto& data = node->data.complit_expr;
            print("{{");
            print_node_list(&data.items);
            print("}}");
            break;
        }
        case NodeType::BinOpExpr: {
            auto& data = node->data.bin_op_expr;
            print_node(data.op1);
            print(" {} ", get_token_symbol(data.op_type));
            print_node(data.op2);
            break;
        }
        case NodeType::LiteralExpr: {
            print("{}", node->token->to_string());
            break;
        }
        case NodeType::ReturnStmt: {
            auto& data = node->data.return_stmt;
            print("return");
            if (data.expr) {
                print(" ");
                print_node(data.expr);
            }
            break;
        }
        case NodeType::ParenExpr: {
            auto& child = node->data.child_expr;
            print("(");
            print_node(child);
            print(")");
            break;
        }
        case NodeType::IfStmt: {
            auto& data = node->data.if_stmt;
            print("if ");
            print_node(data.condition);
            print(" ");
            print_node(data.then_block);
            break;
        }
        case NodeType::FnCallExpr: {
            auto& data = node->data.fn_call_expr;
            print_node(data.fn_ref_expr);
            print("(");
            print_node_list(&data.args);
            print(")");
            break;
        }
        default:
            print("\n");
            panic("unhandled {}", PRINT_ENUM(node->type));
    }
}

void AstPrinter::print_indent(int level) {
    for (int i = 0; i < level; i++) {
        print("  ");
    }
}

void AstPrinter::print_node_list(array<Node*>* list) {
    for (int i = 0; i < list->size; i++) {
        print_node(list->at(i));
        if (i < list->size - 1) {
            print(", ");
        }
    }
}
