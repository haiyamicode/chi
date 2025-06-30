/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "ast_printer.h"

using namespace cx;
using namespace cx::ast;

string get_sigil_symbol(SigilKind sigil) {
    switch (sigil) {
    case SigilKind::Pointer:
        return "*";
    case SigilKind::Optional:
        return "?";
    case SigilKind::Reference:
        return "&";
    case SigilKind::Box:
        return "^";
    case SigilKind::MutRef:
        return "&mut";
    default:
        panic("unknown sigil {}", PRINT_ENUM(sigil));
        return "";
    }
}

void cx::print_ast(Node *root) {
    AstPrinter printer(root);
    return printer.print_ast();
}

void AstPrinter::print_ast() { print_node(m_root); }

void AstPrinter::print_node(Node *node) {
    switch (node->type) {
    case NodeType::Root: {
        for (auto decl : m_root->data.root.top_level_decls) {
            print_node(decl);
            if (decl->type == NodeType::VarDecl) {
                print(";");
            }
            print("\n");
        }
        break;
    }
    case NodeType::FnDef: {
        auto &data = node->data.fn_def;
        print_declspec(data.decl_spec);
        print_node(data.fn_proto);
        if (data.body) {
            print(" ");
            print_node(data.body);
            if (data.fn_kind != FnKind::Lambda) {
                print("\n");
            }
        } else {
            print(";");
        }
        break;
    }
    case NodeType::FnProto: {
        auto &data = node->data.fn_proto;
        if (data.is_type_expr && !data.params.len && !data.return_type) {
            print("func");
            return;
        }
        print("func {}(", node->name);
        print_node_list(&data.params);
        print(")");
        if (data.return_type) {
            print(" ");
            print_node(data.return_type);
        }
        break;
    }
    case NodeType::Identifier: {
        print("{}", node->name);
        break;
    }
    case NodeType::TypeParam: {
        auto &data = node->data.type_param;
        print("{}", node->name);
        if (data.bound) {
            print(" : ");
            print_node(data.bound);
        }
        break;
    }
    case NodeType::ParamDecl: {
        auto &data = node->data.param_decl;
        if (data.is_variadic) {
            print("...");
        }
        print(node->name);
        print(": ");
        print_node(data.type);
        break;
    }
    case NodeType::Block: {
        auto &data = node->data.block;
        if (data.is_arrow) {
            print(" => ");
        }
        if (data.has_braces) {
            m_indent++;
            print("{{\n");
        }
        for (auto stmt : data.statements) {
            print_indent(m_indent);
            print_node(stmt);
            if (stmt->type != NodeType::IfStmt && stmt->type != NodeType::ForStmt) {
                print(";\n");
            }
        }

        if (data.return_expr) {
            if (data.statements.len) {
                print_node_list(&data.statements);
            }
            print_indent(m_indent);
            print_node(data.return_expr);
            print("\n");
        }

        if (data.has_braces) {
            m_indent--;
            print_indent(m_indent);
            print("}}");
        }
        break;
    }
    case NodeType::VarDecl: {
        auto &data = node->data.var_decl;
        if (!data.is_field) {
            print(data.is_const ? "const" : "var");
            print(" ");
        }
        if (data.is_embed) {
            print("...");
        }
        print(node->name);
        if (data.type) {
            print(": ");
            print_node(data.type);
        }
        if (data.expr) {
            print(" = ");
            print_node(data.expr);
        }
        break;
    }
    case NodeType::StructDecl: {
        auto &data = node->data.struct_decl;
        print("{} ", node->token->str);
        if (!node->name.empty()) {
            print("{}", node->name);
        }
        if (data.type_params.len) {
            print("<");
            print_node_list(&data.type_params);
            print(">");
        }
        if (data.implements.len) {
            print(": ");
            print_node_list(&data.implements);
        }
        print(" {{");

        NodeType member_type = NodeType::Error;
        if (data.members.len) {
            print("\n");
            m_indent++;
            size_t i = 0;
            for (auto member : data.members) {
                if (member_type != NodeType::Error && member_type != member->type) {
                    print("\n");
                }
                member_type = member->type;
                print_indent(m_indent);
                print_node(member);
                if (member->type == NodeType::VarDecl) {
                    print(";\n");
                } else if (member->type == NodeType::FnDef) {
                    print("\n");
                } else if (member->type == NodeType::EnumVariant) {
                    if (i != data.members.len - 1) {
                        print(",\n");
                    } else {
                        print("\n");
                    }
                }
                i++;
            }
            m_indent--;
        }
        print("}}");
        print("\n");
        break;
    }
    case NodeType::DotExpr: {
        auto &data = node->data.dot_expr;
        print_node(data.expr);
        print(".{}", data.field->str);
        break;
    }
    case NodeType::ConstructExpr: {
        auto &data = node->data.construct_expr;
        if (data.type) {
            if (data.is_new) {
                print("new ");
            } else {
                print(".");
            }
            print_node(data.type);
        }
        print("{{");
        print_node_list(&data.items);
        print("}}");
        break;
    }
    case NodeType::BinOpExpr: {
        auto &data = node->data.bin_op_expr;
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
        auto &data = node->data.return_stmt;
        print("return");
        if (data.expr) {
            print(" ");
            print_node(data.expr);
        }
        break;
    }
    case NodeType::ParenExpr: {
        auto &child = node->data.child_expr;
        print("(");
        print_node(child);
        print(")");
        break;
    }
    case NodeType::IfStmt: {
        auto &data = node->data.if_stmt;
        print("if ");
        print_node(data.condition);
        print(" ");
        print_node(data.then_block);
        if (data.else_node) {
            print(" else ");
            print_node(data.else_node);
        }
        print("\n");
        break;
    }
    case NodeType::FnCallExpr: {
        auto &data = node->data.fn_call_expr;
        print_node(data.fn_ref_expr);
        print("(");
        print_node_list(&data.args);
        print(")");
        break;
    }
    case NodeType::SubtypeExpr: {
        auto &data = node->data.subtype_expr;
        print_node(data.type);
        print("<");
        print_node_list(&data.args);
        print(">");
        break;
    }
    case NodeType::IndexExpr: {
        auto &data = node->data.index_expr;
        print_node(data.expr);
        print("[");
        print_node(data.subscript);
        print("]");
        break;
    }
    case NodeType::TypeSigil: {
        auto &data = node->data.sigil_type;
        print("{}", get_sigil_symbol(data.sigil));
        if (data.has_wrapping && data.sigil == SigilKind::MutRef) {
            print("<");
        }
        print_node(data.type);
        if (data.has_wrapping && data.sigil == SigilKind::MutRef) {
            print(">");
        }
        break;
    }
    case NodeType::TypedefDecl: {
        auto &data = node->data.typedef_decl;
        print("typedef ");
        print(data.identifier->str);
        print(" = ");
        print_node(data.type);
        break;
    }
    case NodeType::EnumVariant: {
        auto &data = node->data.enum_variant;
        print("{}", node->name);
        if (data.value) {
            print(" = ");
            print_node(data.value);
        }
        break;
    }
    case NodeType::UnaryOpExpr: {
        auto &data = node->data.unary_op_expr;
        if (!data.is_suffix) {
            print("{}", get_token_symbol(data.op_type));
            print_node(data.op1);
        } else {
            print_node(data.op1);
            print("{}", get_token_symbol(data.op_type));
        }
        break;
    }
    case NodeType::TryExpr: {
        auto &data = node->data.try_expr;
        print("try ");
        print_node(data.expr);
        break;
    }
    case NodeType::CastExpr: {
        auto &data = node->data.cast_expr;
        print_node(data.expr);
        print(" as ");
        print_node(data.dest_type);
        break;
    }
    case NodeType::BindIdentifier: {
        print("{}", node->token->to_string());
        break;
    }
    case NodeType::ForStmt: {
        auto &data = node->data.for_stmt;
        if (node->index > 0) {
            print("\n");
            print_indent(m_indent);
        }
        print("for ");
        if (data.kind == ForLoopKind::Ternary) {
            if (data.init) {
                print_node(data.init);
            }
            print(";");
            if (data.condition) {
                print(" ");
                print_node(data.condition);
            }
            print(";");
            if (data.post) {
                print(" ");
                print_node(data.post);
            }
            print(" ");
        }
        if (data.kind == ForLoopKind::Range) {
            print_node(data.expr);
            if (data.bind) {
                print("=> ");
                print_node(data.bind);
            }
            print(" ");
        }
        print_node(data.body);
        print("\n");
        if (!node->is_last_stmt()) {
            print("\n");
        }
        break;
    }
    case NodeType::BranchStmt: {
        print("{}", node->token->to_string());
        break;
    }
    case NodeType::ExternDecl: {
        auto &data = node->data.extern_decl;
        print("extern {} ", data.type->to_string());
        print("{{\n");
        for (auto member : data.members) {
            print_indent(m_indent + 1);
            print_node(member);
            print("\n");
        }
        print("}}\n");
        break;
    }
    case NodeType::Error: {
        print("{}", node->token->to_string());
        break;
    }
    case NodeType::ImportDecl: {
        auto &data = node->data.import_decl;
        print("import ");
        print(data.path->to_string());
        if (data.alias) {
            print(" as {}", data.alias->to_string());
        }
        if (data.symbols.len) {
            print(" {{");
            print_node_list(&data.symbols);
            print("}}");
        }
        print(";\n");
        break;
    }
    case NodeType::ExportDecl: {
        auto &data = node->data.import_decl;
        print("export ");
        print(data.path->to_string());
        if (data.alias) {
            print(" as {}", data.alias->to_string());
        }
        if (data.match_all) {
            print(" *");
        } else {
            if (data.symbols.len) {
                print(" {{");
                print_node_list(&data.symbols);
                print("}}");
            }
        }
        print(";\n");
        break;
    }
    case NodeType::ImportSymbol: {
        auto &data = node->data.import_symbol;
        print("{}", data.name->to_string());
        if (data.alias) {
            print(" as {}", data.alias->to_string());
        }
        break;
    }
    case NodeType::PrefixExpr: {
        auto &data = node->data.prefix_expr;
        print("{} ", data.prefix->str);
        print_node(data.expr);
        break;
    }
    case NodeType::SwitchExpr: {
        auto &data = node->data.switch_expr;
        print("switch ");
        print_node(data.expr);
        print(" {{\n");
        for (int i = 0; i < data.cases.len; i++) {
            auto case_node = data.cases.at(i);
            print_indent(m_indent + 1);
            print_node(case_node);
            if (i < data.cases.len - 1) {
                print(",");
            }
            print("\n");
        }
        print_indent(m_indent);
        print("}}");
        break;
    }

    case NodeType::CaseExpr: {
        auto &data = node->data.case_expr;
        if (data.is_else) {
            print("else");
        } else {
            for (int i = 0; i < data.clauses.len; i++) {
                auto clause = data.clauses.at(i);
                print_node(clause);
                if (i < data.clauses.len - 1) {
                    print(", ");
                }
            }
        }

        print_node(data.body);
        break;
    }
    default:
        print("\n");
        panic("unhandled node {}", PRINT_ENUM(node->type));
    }
}

void AstPrinter::print_indent(int level) {
    for (int i = 0; i < level; i++) {
        print("  ");
    }
}

void AstPrinter::print_node_list(array<Node *> *list) {
    for (int i = 0; i < list->len; i++) {
        print_node(list->at(i));
        if (i < list->len - 1) {
            print(", ");
        }
    }
}

void AstPrinter::print_declspec(DeclSpec *declspec) {
    for (auto attr : declspec->attributes) {
        print("@[");
        print_node(attr->data.decl_attribute.term);
        print("]\n");
        print_indent(m_indent);
    }
    if (declspec->has_flag(DECL_PRIVATE)) {
        print("private ");
    }
}
