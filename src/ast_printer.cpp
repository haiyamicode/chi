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
    if (!node) return;
    // Flush any inline comments before this node's position.
    // Own-line comments are handled by callers before indentation.
    auto *_ft = first_token(node);
    if (_ft) flush_comments_before(_ft->pos);
    switch (node->type) {
    case NodeType::Root: {
        for (auto decl : m_root->data.root.top_level_decls) {
            auto *ft = first_token(decl);
            if (ft) flush_comments_before(ft->pos);
            print_node(decl);
            if (decl->type == NodeType::VarDecl) {
                print(";");
            }
            print("\n");
        }
        // Flush any trailing comments at end of file
        if (m_comments) {
            while (m_comment_idx < m_comments->len) {
                auto &comment = m_comments->at(m_comment_idx);
                print("{}\n", comment.text);
                m_comment_idx++;
            }
        }
        break;
    }
    case NodeType::FnDef: {
        auto &data = node->data.fn_def;
        print_declspec(data.decl_spec);
        // Value captures go between 'func' and params: func [x, y](...)
        if (data.value_captures.len > 0) {
            m_suppress_func_keyword = true;
            print("func [");
            for (size_t i = 0; i < data.value_captures.len; i++) {
                print("{}", data.value_captures.at(i));
                if (i < data.value_captures.len - 1) {
                    print(", ");
                }
            }
            print("] ");
        }
        print_node(data.fn_proto);
        m_suppress_func_keyword = false;
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
            if (!m_suppress_func_keyword) {
                print("func");
            }
            return;
        }
        if (m_suppress_func_keyword) {
            if (!node->name.empty()) {
                print("{}", node->name);
            }
        } else {
            print("func {}", node->name);
        }
        // Print type parameters if present
        if (data.type_params.len > 0) {
            print("<");
            print_node_list(&data.type_params);
            print(">");
        }
        print("(");
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
    case NodeType::Primitive: {
        print("{}", node->name);
        break;
    }
    case NodeType::TypeParam: {
        auto &data = node->data.type_param;
        print("{}", node->name);
        if (data.type_bound) {
            print(": ");
            print_node(data.type_bound);
        }
        if (data.default_type) {
            print(" = ");
            print_node(data.default_type);
        }
        break;
    }
    case NodeType::ParamDecl: {
        auto &data = node->data.param_decl;
        if (data.is_variadic) {
            print("...");
        }
        print(node->name);
        if (data.type) {
            print(": ");
            print_node(data.type);
        }
        if (data.default_value) {
            print(" = ");
            print_node(data.default_value);
        }
        break;
    }
    case NodeType::Block: {
        auto &data = node->data.block;
        auto is_inline = !data.statements.len && !data.has_braces;
        if (data.is_arrow) {
            print(" => ");
        }
        if (data.has_braces) {
            m_indent++;
            print("{{\n");
        }

        Node *prev_stmt = nullptr;
        for (auto stmt : data.statements) {
            // Arrow lambda bodies: print expression directly without 'return' keyword
            if (data.is_arrow && stmt->type == NodeType::ReturnStmt && stmt->data.return_stmt.expr) {
                print_node(stmt->data.return_stmt.expr);
            } else {
                // Preserve blank lines from original source
                if (prev_stmt && has_blank_line_between(prev_stmt, stmt)) {
                    print("\n");
                }
                auto *ft = first_token(stmt);
                if (ft) flush_comments_before(ft->pos);
                print_indent(m_indent);
                print_node(stmt);
                if (stmt->type == NodeType::ForStmt || stmt->type == NodeType::WhileStmt) {
                    // These handlers print their own \n
                } else if (stmt->type == NodeType::IfStmt || stmt->type == NodeType::Block) {
                    print("\n");
                } else {
                    print(";");
                    flush_trailing_comment(stmt);
                    print("\n");
                }
            }
            prev_stmt = stmt;
        }

        if (data.return_expr) {
            // Preserve blank line before return expression
            if (prev_stmt && has_blank_line_between(prev_stmt, data.return_expr)) {
                print("\n");
            }
            if (!is_inline) {
                auto *ft = first_token(data.return_expr);
                if (ft) flush_comments_before(ft->pos);
                print_indent(m_indent);
            }
            print_node(data.return_expr);
            if (!is_inline) {
                print("\n");
            }
        }

        if (data.has_braces) {
            // Flush any remaining comments inside the block before the closing brace
            if (node->end_token) {
                flush_comments_before(node->end_token->pos);
            }
            m_indent--;
            print_indent(m_indent);
            print("}}");
        }
        break;
    }
    case NodeType::VarDecl: {
        auto &data = node->data.var_decl;
        if (!data.is_field) {
            switch (data.kind) {
            case ast::VarKind::Mutable:
                print("var");
                break;
            case ast::VarKind::Immutable:
                print("let");
                break;
            case ast::VarKind::Constant:
                print("const");
                break;
            }
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
            print(" implements ");
            print_node_list(&data.implements);
        }
        print(" {{");

        if (data.members.len) {
            print("\n");
            m_indent++;
            Node *prev_member = nullptr;
            NodeType prev_type = NodeType::Error;
            size_t i = 0;
            for (auto member : data.members) {
                // Blank line: type change, between methods, or user's blank lines
                if (prev_member) {
                    bool want_blank = false;
                    if (prev_type != member->type) want_blank = true;
                    if (prev_type == NodeType::FnDef) want_blank = true;
                    if (has_blank_line_between(prev_member, member)) want_blank = true;
                    if (want_blank) print("\n");
                }
                prev_type = member->type;
                auto *ft = first_token(member);
                if (ft) flush_comments_before(ft->pos);
                print_indent(m_indent);
                print_node(member);
                if (member->type == NodeType::VarDecl) {
                    print(";");
                    flush_trailing_comment(member);
                    print("\n");
                } else if (member->type == NodeType::FnDef) {
                    // FnDef handler already prints \n after body
                } else if (member->type == NodeType::EnumVariant) {
                    if (i != data.members.len - 1) {
                        print(",");
                    }
                    flush_trailing_comment(member);
                    print("\n");
                }
                prev_member = member;
                i++;
            }
            m_indent--;
        } else if (node->end_token && m_comments && m_comment_idx < m_comments->len
                   && m_comments->at(m_comment_idx).pos < node->end_token->pos) {
            // Empty struct with inner comments
            print("\n");
            m_indent++;
            flush_comments_before(node->end_token->pos);
            m_indent--;
            print_indent(m_indent);
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
            }
            print_node(data.type);
        }
        print("{{");
        // Print positional items first, then named field inits
        print_node_list(&data.items);
        if (data.items.len && data.field_inits.len) {
            print(", ");
        }
        print_node_list(&data.field_inits);
        print("}}");
        break;
    }
    case NodeType::FieldInitExpr: {
        auto &data = node->data.field_init_expr;
        print(".{} = ", data.field->str);
        print_node(data.value);
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
        // No \n here — Block handler owns it, preventing double \n in else-if chains
        break;
    }
    case NodeType::FnCallExpr: {
        auto &data = node->data.fn_call_expr;
        print_node(data.fn_ref_expr);
        // Print type parameters if present
        if (data.type_args.len > 0) {
            print("<");
            print_node_list(&data.type_args);
            print(">");
        }
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
        if (data.sigil == SigilKind::MutRef) {
            if (data.has_wrapping) {
                print("<");
            } else {
                print(" ");
            }
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
        if (data.struct_body) {
            print(" ");
            print_struct_members(data.struct_body->data.struct_decl);
        }
        break;
    }
    case NodeType::UnaryOpExpr: {
        auto &data = node->data.unary_op_expr;
        if (!data.is_suffix) {
            if (data.op_type == TokenType::MUTREF) {
                print("&mut ");
            } else {
                print("{}", get_token_symbol(data.op_type));
            }
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
    case NodeType::AwaitExpr: {
        auto &data = node->data.await_expr;
        print("await ");
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
            if (data.bind_sigil != SigilKind::None) {
                print("{}", get_sigil_symbol(data.bind_sigil));
                print(" ");
            }
            if (data.bind) {
                print_node(data.bind);
                print(" ");
            }
            print("in ");
            print_node(data.expr);
            print(" ");
        }
        print_node(data.body);
        print("\n");
        break;
    }
    case NodeType::WhileStmt: {
        auto &data = node->data.while_stmt;
        print("while");
        if (data.condition) {
            print(" ");
            print_node(data.condition);
        }
        print(" ");
        print_node(data.body);
        print("\n");
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
        if (node->token) {
            print("{}", node->token->to_string());
        }
        break;
    }
    case NodeType::EmptyStmt: {
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
        m_indent++;
        for (int i = 0; i < data.cases.len; i++) {
            auto case_node = data.cases.at(i);
            auto *ft = first_token(case_node);
            if (ft) flush_comments_before(ft->pos);
            print_indent(m_indent);
            print_node(case_node);
            if (i < data.cases.len - 1) {
                print(",");
            }
            // Flush trailing comment — use body if present (arrow block end)
            auto *trail_node = case_node->data.case_expr.body ? case_node->data.case_expr.body : case_node;
            flush_trailing_comment(trail_node);
            print("\n");
        }
        // Flush trailing comments before closing brace
        if (node->end_token) {
            flush_comments_before(node->end_token->pos);
        }
        m_indent--;
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
    case NodeType::EnumDecl: {
        auto &data = node->data.enum_decl;
        print_declspec(data.decl_spec);
        print("enum ");
        if (!node->name.empty()) {
            print("{}", node->name);
        }
        if (data.discriminator_field || data.discriminator_type) {
            print(" (");
            if (data.discriminator_field) {
                print("{}: ", data.discriminator_field->get_name());
            }
            if (data.discriminator_type) {
                print_node(data.discriminator_type);
            }
            print(")");
        }
        print(" {{\n");
        m_indent++;
        for (int i = 0; i < data.variants.len; i++) {
            auto variant = data.variants.at(i);
            auto *ft = first_token(variant);
            if (ft) flush_comments_before(ft->pos);
            print_indent(m_indent);
            print_node(variant);
            // Variants with struct bodies use ';' separator
            if (variant->data.enum_variant.struct_body) {
                print(";");
            } else if (i < data.variants.len - 1) {
                print(",");
            }
            flush_trailing_comment(variant);
            print("\n");
        }
        // Flush inner comments in empty enum
        if (data.variants.len == 0 && !data.base_struct
            && node->end_token && m_comments && m_comment_idx < m_comments->len
            && m_comments->at(m_comment_idx).pos < node->end_token->pos) {
            flush_comments_before(node->end_token->pos);
        }
        // Print base struct members if present
        if (data.base_struct) {
            print("\n");
            print_indent(m_indent);
            print("struct ");
            print_struct_members(data.base_struct->data.struct_decl);
            print("\n");
        }
        m_indent--;
        print("}}\n");
        break;
    }
    default:
        break;
    }
}

void AstPrinter::print_indent(int level) {
    for (int i = 0; i < level; i++) {
        print("    ");
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

void AstPrinter::print_struct_members(StructDecl &data) {
    print("{{");
    if (data.members.len) {
        print("\n");
        m_indent++;
        Node *prev_member = nullptr;
        NodeType prev_type = NodeType::Error;
        for (auto member : data.members) {
            if (prev_member) {
                bool want_blank = false;
                if (prev_type != member->type) want_blank = true;
                if (prev_type == NodeType::FnDef) want_blank = true;
                if (has_blank_line_between(prev_member, member)) want_blank = true;
                if (want_blank) print("\n");
            }
            prev_type = member->type;
            auto *ft = first_token(member);
            if (ft) flush_comments_before(ft->pos);
            print_indent(m_indent);
            print_node(member);
            if (member->type == NodeType::VarDecl) {
                print(";");
                flush_trailing_comment(member);
                print("\n");
            } else if (member->type == NodeType::FnDef) {
                // FnDef handler already prints \n after body
            }
            prev_member = member;
        }
        m_indent--;
        print_indent(m_indent);
    }
    print("}}");
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
    if (declspec->has_flag(DECL_PROTECTED)) {
        print("protected ");
    }
    if (declspec->has_flag(DECL_STATIC)) {
        print("static ");
    }
    if (declspec->has_flag(DECL_MUTABLE)) {
        print("mut ");
    }
    if (declspec->has_flag(DECL_ASYNC)) {
        print("async ");
    }
}

bool AstPrinter::has_blank_line_between(Node *prev, Node *next) {
    long prev_end = -1;
    if (prev->end_token)
        prev_end = prev->end_token->pos.line;
    else if (prev->token)
        prev_end = prev->token->pos.line;
    else if (prev->start_token)
        prev_end = prev->start_token->pos.line;

    // Use first comment's position if there are comments before the next node
    long next_start = -1;
    if (m_comments && m_comment_idx < m_comments->len) {
        auto &comment = m_comments->at(m_comment_idx);
        auto *ft = first_token(next);
        long node_line = ft ? ft->pos.line : -1;
        if (node_line >= 0 && comment.pos.line < node_line) {
            next_start = comment.pos.line;
        }
    }
    if (next_start < 0) {
        if (next->start_token)
            next_start = next->start_token->pos.line;
        else if (next->token)
            next_start = next->token->pos.line;
    }

    if (prev_end < 0 || next_start < 0)
        return false;
    return next_start - prev_end > 1;
}

Token *AstPrinter::first_token(Node *node) {
    if (node->start_token)
        return node->start_token;
    return node->token;
}

Token *AstPrinter::last_token(Node *node) {
    if (node->end_token)
        return node->end_token;
    return node->token;
}

void AstPrinter::flush_comments_before(Pos before_pos) {
    if (!m_comments) return;
    long last_comment_line = -1;
    while (m_comment_idx < m_comments->len) {
        auto &comment = m_comments->at(m_comment_idx);
        if (!(comment.pos < before_pos)) break;

        if (comment.pos.line == before_pos.line) {
            // Inline comment (same line as the next token) — print inline
            print("{} ", comment.text);
        } else {
            // Own-line comment — print on its own line with indent
            if (last_comment_line >= 0 && comment.pos.line - last_comment_line > 1) {
                print("\n");
            }
            print_indent(m_indent);
            print("{}\n", comment.text);
            last_comment_line = comment.pos.line;
        }
        m_comment_idx++;
    }
    // Preserve blank line between last own-line comment and the node
    if (last_comment_line >= 0 && before_pos.line - last_comment_line > 1) {
        print("\n");
    }
}

void AstPrinter::flush_trailing_comment(Node *node) {
    if (!m_comments || !node) return;
    auto *lt = last_token(node);
    if (!lt) return;
    long on_line = lt->pos.line;
    while (m_comment_idx < m_comments->len) {
        auto &comment = m_comments->at(m_comment_idx);
        if (comment.pos.line != on_line) break;
        print(" {}", comment.text);
        m_comment_idx++;
    }
}
