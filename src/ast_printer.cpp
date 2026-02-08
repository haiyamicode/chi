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

string AstPrinter::format_to_string() {
    fmt::memory_buffer buf;
    m_buffer = &buf;
    print_ast();
    m_buffer = nullptr;
    return string(buf.data(), buf.size());
}

void AstPrinter::print_node(Node *node) {
    if (!node)
        return;
    // Flush any inline comments before this node's position.
    // Own-line comments are handled by callers before indentation.
    auto *_ft = first_token(node);
    if (_ft)
        flush_comments_before(_ft->pos);
    switch (node->type) {
    case NodeType::Root: {
        for (auto decl : m_root->data.root.top_level_decls) {
            auto *ft = first_token(decl);
            if (ft)
                flush_comments_before(ft->pos);
            print_node(decl);
            if (decl->type == NodeType::VarDecl) {
                emit(";");
            }
            emit("\n");
        }
        // Flush any trailing comments at end of file
        if (m_comments) {
            while (m_comment_idx < m_comments->len) {
                auto &comment = m_comments->at(m_comment_idx);
                emit("{}\n", comment.text);
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
            emit("func [");
            for (size_t i = 0; i < data.value_captures.len; i++) {
                emit("{}", data.value_captures.at(i));
                if (i < data.value_captures.len - 1) {
                    emit(", ");
                }
            }
            emit("] ");
        }
        print_node(data.fn_proto);
        m_suppress_func_keyword = false;
        if (data.body) {
            emit(" ");
            print_node(data.body);
            if (data.fn_kind != FnKind::Lambda) {
                emit("\n");
            }
        } else {
            emit(";");
        }
        break;
    }
    case NodeType::FnProto: {
        auto &data = node->data.fn_proto;
        if (data.is_type_expr && !data.params.len && !data.return_type) {
            if (!m_suppress_func_keyword) {
                emit("func");
            }
            return;
        }
        if (m_suppress_func_keyword) {
            if (!node->name.empty()) {
                emit("{}", node->name);
            }
        } else {
            emit("func {}", node->name);
        }
        // Print type parameters if present
        if (data.type_params.len > 0) {
            emit("<");
            print_node_list(&data.type_params);
            emit(">");
        }
        emit("(");
        print_node_list(&data.params);
        emit(")");
        if (data.return_type) {
            emit(" ");
            print_node(data.return_type);
        }
        break;
    }
    case NodeType::Identifier: {
        emit("{}", node->name);
        break;
    }
    case NodeType::Primitive: {
        emit("{}", node->name);
        break;
    }
    case NodeType::TypeParam: {
        auto &data = node->data.type_param;
        emit("{}", node->name);
        if (data.type_bound) {
            emit(": ");
            print_node(data.type_bound);
        }
        if (data.default_type) {
            emit(" = ");
            print_node(data.default_type);
        }
        break;
    }
    case NodeType::ParamDecl: {
        auto &data = node->data.param_decl;
        if (data.is_variadic) {
            emit("...");
        }
        emit(node->name);
        if (data.type) {
            emit(": ");
            print_node(data.type);
        }
        if (data.default_value) {
            emit(" = ");
            print_node(data.default_value);
        }
        break;
    }
    case NodeType::Block: {
        auto &data = node->data.block;
        auto is_inline = !data.statements.len && !data.has_braces;
        if (data.is_arrow) {
            emit(" => ");
        }
        if (data.has_braces) {
            m_indent++;
            emit("{{\n");
        }

        Node *prev_stmt = nullptr;
        for (auto stmt : data.statements) {
            // Arrow lambda bodies: print expression directly without 'return' keyword
            if (data.is_arrow && stmt->type == NodeType::ReturnStmt &&
                stmt->data.return_stmt.expr) {
                print_node(stmt->data.return_stmt.expr);
            } else {
                // Preserve blank lines from original source
                if (prev_stmt && has_blank_line_between(prev_stmt, stmt)) {
                    emit("\n");
                }
                auto *ft = first_token(stmt);
                if (ft)
                    flush_comments_before(ft->pos);
                print_indent(m_indent);
                print_node(stmt);
                if (stmt->type == NodeType::ForStmt || stmt->type == NodeType::WhileStmt) {
                    // These handlers print their own \n
                } else if (stmt->type == NodeType::IfStmt || stmt->type == NodeType::Block) {
                    emit("\n");
                } else {
                    emit(";");
                    flush_trailing_comment(stmt);
                    emit("\n");
                }
            }
            prev_stmt = stmt;
        }

        if (data.return_expr) {
            // Preserve blank line before return expression
            if (prev_stmt && has_blank_line_between(prev_stmt, data.return_expr)) {
                emit("\n");
            }
            if (!is_inline) {
                auto *ft = first_token(data.return_expr);
                if (ft)
                    flush_comments_before(ft->pos);
                print_indent(m_indent);
            }
            print_node(data.return_expr);
            if (!is_inline) {
                emit("\n");
            }
        }

        if (data.has_braces) {
            // Flush any remaining comments inside the block before the closing brace
            if (node->end_token) {
                flush_comments_before(node->end_token->pos);
            }
            m_indent--;
            print_indent(m_indent);
            emit("}}");
        }
        break;
    }
    case NodeType::VarDecl: {
        auto &data = node->data.var_decl;
        if (!data.is_field) {
            switch (data.kind) {
            case ast::VarKind::Mutable:
                emit("var");
                break;
            case ast::VarKind::Immutable:
                emit("let");
                break;
            case ast::VarKind::Constant:
                emit("const");
                break;
            }
            emit(" ");
        }
        if (data.is_embed) {
            emit("...");
        }
        emit(node->name);
        if (data.type) {
            emit(": ");
            print_node(data.type);
        }
        if (data.expr) {
            emit(" = ");
            print_node(data.expr);
        }
        break;
    }
    case NodeType::StructDecl: {
        auto &data = node->data.struct_decl;
        emit("{} ", node->token->str);
        if (!node->name.empty()) {
            emit("{}", node->name);
        }
        if (data.type_params.len) {
            emit("<");
            print_node_list(&data.type_params);
            emit(">");
        }
        if (data.implements.len) {
            emit(" implements ");
            print_node_list(&data.implements);
        }
        emit(" {{");

        if (data.members.len) {
            emit("\n");
            m_indent++;
            Node *prev_member = nullptr;
            NodeType prev_type = NodeType::Error;
            size_t i = 0;
            for (auto member : data.members) {
                // Blank line: type change, between methods, or user's blank lines
                if (prev_member) {
                    bool want_blank = false;
                    if (prev_type != member->type)
                        want_blank = true;
                    if (prev_type == NodeType::FnDef)
                        want_blank = true;
                    if (has_blank_line_between(prev_member, member))
                        want_blank = true;
                    if (want_blank)
                        emit("\n");
                }
                prev_type = member->type;
                auto *ft = first_token(member);
                if (ft)
                    flush_comments_before(ft->pos);
                print_indent(m_indent);
                print_node(member);
                if (member->type == NodeType::VarDecl) {
                    emit(";");
                    flush_trailing_comment(member);
                    emit("\n");
                } else if (member->type == NodeType::FnDef) {
                    // FnDef handler already prints \n after body
                } else if (member->type == NodeType::EnumVariant) {
                    if (i != data.members.len - 1) {
                        emit(",");
                    }
                    flush_trailing_comment(member);
                    emit("\n");
                }
                prev_member = member;
                i++;
            }
            m_indent--;
        } else if (node->end_token && m_comments && m_comment_idx < m_comments->len &&
                   m_comments->at(m_comment_idx).pos < node->end_token->pos) {
            // Empty struct with inner comments
            emit("\n");
            m_indent++;
            flush_comments_before(node->end_token->pos);
            m_indent--;
            print_indent(m_indent);
        }
        emit("}}");
        emit("\n");
        break;
    }
    case NodeType::DotExpr: {
        auto &data = node->data.dot_expr;
        print_node(data.expr);
        emit(".{}", data.field->str);
        break;
    }
    case NodeType::ConstructExpr: {
        auto &data = node->data.construct_expr;
        if (data.type) {
            if (data.is_new) {
                emit("new ");
            }
            print_node(data.type);
        }
        emit("{{");
        // Print positional items first, then named field inits
        print_node_list(&data.items);
        if (data.items.len && data.field_inits.len) {
            emit(", ");
        }
        print_node_list(&data.field_inits);
        emit("}}");
        break;
    }
    case NodeType::FieldInitExpr: {
        auto &data = node->data.field_init_expr;
        emit(".{} = ", data.field->str);
        print_node(data.value);
        break;
    }
    case NodeType::BinOpExpr: {
        auto &data = node->data.bin_op_expr;
        print_node(data.op1);
        emit(" {} ", get_token_symbol(data.op_type));
        print_node(data.op2);
        break;
    }
    case NodeType::LiteralExpr: {
        emit("{}", node->token->to_string());
        break;
    }
    case NodeType::ReturnStmt: {
        auto &data = node->data.return_stmt;
        emit("return");
        if (data.expr) {
            emit(" ");
            print_node(data.expr);
        }
        break;
    }
    case NodeType::ParenExpr: {
        auto &child = node->data.child_expr;
        emit("(");
        print_node(child);
        emit(")");
        break;
    }
    case NodeType::IfStmt: {
        auto &data = node->data.if_stmt;
        emit("if ");
        print_node(data.condition);
        emit(" ");
        print_node(data.then_block);
        if (data.else_node) {
            emit(" else ");
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
            emit("<");
            print_node_list(&data.type_args);
            emit(">");
        }
        emit("(");
        print_node_list(&data.args);
        emit(")");
        break;
    }
    case NodeType::SubtypeExpr: {
        auto &data = node->data.subtype_expr;
        print_node(data.type);
        emit("<");
        print_node_list(&data.args);
        emit(">");
        break;
    }
    case NodeType::IndexExpr: {
        auto &data = node->data.index_expr;
        print_node(data.expr);
        emit("[");
        print_node(data.subscript);
        emit("]");
        break;
    }
    case NodeType::TypeSigil: {
        auto &data = node->data.sigil_type;
        emit("{}", get_sigil_symbol(data.sigil));
        if (data.sigil == SigilKind::MutRef) {
            if (data.has_wrapping) {
                emit("<");
            } else {
                emit(" ");
            }
        }
        print_node(data.type);
        if (data.has_wrapping && data.sigil == SigilKind::MutRef) {
            emit(">");
        }
        break;
    }
    case NodeType::TypedefDecl: {
        auto &data = node->data.typedef_decl;
        emit("typedef ");
        emit(data.identifier->str);
        emit(" = ");
        print_node(data.type);
        break;
    }
    case NodeType::EnumVariant: {
        auto &data = node->data.enum_variant;
        emit("{}", node->name);
        if (data.value) {
            emit(" = ");
            print_node(data.value);
        }
        if (data.struct_body) {
            emit(" ");
            print_struct_members(data.struct_body->data.struct_decl);
        }
        break;
    }
    case NodeType::UnaryOpExpr: {
        auto &data = node->data.unary_op_expr;
        if (!data.is_suffix) {
            if (data.op_type == TokenType::MUTREF) {
                emit("&mut ");
            } else {
                emit("{}", get_token_symbol(data.op_type));
            }
            print_node(data.op1);
        } else {
            print_node(data.op1);
            emit("{}", get_token_symbol(data.op_type));
        }
        break;
    }
    case NodeType::TryExpr: {
        auto &data = node->data.try_expr;
        emit("try ");
        print_node(data.expr);
        break;
    }
    case NodeType::AwaitExpr: {
        auto &data = node->data.await_expr;
        emit("await ");
        print_node(data.expr);
        break;
    }
    case NodeType::CastExpr: {
        auto &data = node->data.cast_expr;
        print_node(data.expr);
        emit(" as ");
        print_node(data.dest_type);
        break;
    }
    case NodeType::BindIdentifier: {
        emit("{}", node->token->to_string());
        break;
    }
    case NodeType::ForStmt: {
        auto &data = node->data.for_stmt;
        emit("for ");
        if (data.kind == ForLoopKind::Ternary) {
            if (data.init) {
                print_node(data.init);
            }
            emit(";");
            if (data.condition) {
                emit(" ");
                print_node(data.condition);
            }
            emit(";");
            if (data.post) {
                emit(" ");
                print_node(data.post);
            }
            emit(" ");
        }
        if (data.kind == ForLoopKind::Range) {
            if (data.bind_sigil != SigilKind::None) {
                emit("{}", get_sigil_symbol(data.bind_sigil));
                emit(" ");
            }
            if (data.bind) {
                print_node(data.bind);
                emit(" ");
            }
            emit("in ");
            print_node(data.expr);
            emit(" ");
        }
        print_node(data.body);
        emit("\n");
        break;
    }
    case NodeType::WhileStmt: {
        auto &data = node->data.while_stmt;
        emit("while");
        if (data.condition) {
            emit(" ");
            print_node(data.condition);
        }
        emit(" ");
        print_node(data.body);
        emit("\n");
        break;
    }
    case NodeType::BranchStmt: {
        emit("{}", node->token->to_string());
        break;
    }
    case NodeType::ExternDecl: {
        auto &data = node->data.extern_decl;
        emit("extern {} ", data.type->to_string());
        emit("{{\n");
        for (auto member : data.members) {
            print_indent(m_indent + 1);
            print_node(member);
            emit("\n");
        }
        emit("}}\n");
        break;
    }
    case NodeType::Error: {
        if (node->token) {
            emit("{}", node->token->to_string());
        }
        break;
    }
    case NodeType::EmptyStmt: {
        break;
    }
    case NodeType::ImportDecl: {
        auto &data = node->data.import_decl;
        emit("import ");
        emit(data.path->to_string());
        if (data.alias) {
            emit(" as {}", data.alias->to_string());
        }
        if (data.symbols.len) {
            emit(" {{");
            print_node_list(&data.symbols);
            emit("}}");
        }
        emit(";\n");
        break;
    }
    case NodeType::ExportDecl: {
        auto &data = node->data.import_decl;
        emit("export ");
        emit(data.path->to_string());
        if (data.alias) {
            emit(" as {}", data.alias->to_string());
        }
        if (data.match_all) {
            emit(" *");
        } else {
            if (data.symbols.len) {
                emit(" {{");
                print_node_list(&data.symbols);
                emit("}}");
            }
        }
        emit(";\n");
        break;
    }
    case NodeType::ImportSymbol: {
        auto &data = node->data.import_symbol;
        emit("{}", data.name->to_string());
        if (data.alias) {
            emit(" as {}", data.alias->to_string());
        }
        break;
    }
    case NodeType::PrefixExpr: {
        auto &data = node->data.prefix_expr;
        emit("{} ", data.prefix->str);
        print_node(data.expr);
        break;
    }
    case NodeType::SwitchExpr: {
        auto &data = node->data.switch_expr;
        emit("switch ");
        print_node(data.expr);
        emit(" {{\n");
        m_indent++;
        for (int i = 0; i < data.cases.len; i++) {
            auto case_node = data.cases.at(i);
            auto *ft = first_token(case_node);
            if (ft)
                flush_comments_before(ft->pos);
            print_indent(m_indent);
            print_node(case_node);
            if (i < data.cases.len - 1) {
                emit(",");
            }
            // Flush trailing comment — use body if present (arrow block end)
            auto *trail_node =
                case_node->data.case_expr.body ? case_node->data.case_expr.body : case_node;
            flush_trailing_comment(trail_node);
            emit("\n");
        }
        // Flush trailing comments before closing brace
        if (node->end_token) {
            flush_comments_before(node->end_token->pos);
        }
        m_indent--;
        print_indent(m_indent);
        emit("}}");
        break;
    }

    case NodeType::CaseExpr: {
        auto &data = node->data.case_expr;
        if (data.is_else) {
            emit("else");
        } else {
            for (int i = 0; i < data.clauses.len; i++) {
                auto clause = data.clauses.at(i);
                print_node(clause);
                if (i < data.clauses.len - 1) {
                    emit(", ");
                }
            }
        }

        print_node(data.body);
        break;
    }
    case NodeType::EnumDecl: {
        auto &data = node->data.enum_decl;
        print_declspec(data.decl_spec);
        emit("enum ");
        if (!node->name.empty()) {
            emit("{}", node->name);
        }
        if (data.discriminator_field || data.discriminator_type) {
            emit(" (");
            if (data.discriminator_field) {
                emit("{}: ", data.discriminator_field->get_name());
            }
            if (data.discriminator_type) {
                print_node(data.discriminator_type);
            }
            emit(")");
        }
        emit(" {{\n");
        m_indent++;
        for (int i = 0; i < data.variants.len; i++) {
            auto variant = data.variants.at(i);
            auto *ft = first_token(variant);
            if (ft)
                flush_comments_before(ft->pos);
            print_indent(m_indent);
            print_node(variant);
            // Variants with struct bodies use ';' separator
            if (variant->data.enum_variant.struct_body) {
                emit(";");
            } else if (i < data.variants.len - 1) {
                emit(",");
            }
            flush_trailing_comment(variant);
            emit("\n");
        }
        // Flush inner comments in empty enum
        if (data.variants.len == 0 && !data.base_struct && node->end_token && m_comments &&
            m_comment_idx < m_comments->len &&
            m_comments->at(m_comment_idx).pos < node->end_token->pos) {
            flush_comments_before(node->end_token->pos);
        }
        // Print base struct members if present
        if (data.base_struct) {
            emit("\n");
            print_indent(m_indent);
            emit("struct ");
            print_struct_members(data.base_struct->data.struct_decl);
            emit("\n");
        }
        m_indent--;
        emit("}}\n");
        break;
    }
    default:
        break;
    }
}

void AstPrinter::print_indent(int level) {
    for (int i = 0; i < level; i++) {
        emit("    ");
    }
}

void AstPrinter::print_node_list(array<Node *> *list) {
    for (int i = 0; i < list->len; i++) {
        print_node(list->at(i));
        if (i < list->len - 1) {
            emit(", ");
        }
    }
}

void AstPrinter::print_struct_members(StructDecl &data) {
    emit("{{");
    if (data.members.len) {
        emit("\n");
        m_indent++;
        Node *prev_member = nullptr;
        NodeType prev_type = NodeType::Error;
        for (auto member : data.members) {
            if (prev_member) {
                bool want_blank = false;
                if (prev_type != member->type)
                    want_blank = true;
                if (prev_type == NodeType::FnDef)
                    want_blank = true;
                if (has_blank_line_between(prev_member, member))
                    want_blank = true;
                if (want_blank)
                    emit("\n");
            }
            prev_type = member->type;
            auto *ft = first_token(member);
            if (ft)
                flush_comments_before(ft->pos);
            print_indent(m_indent);
            print_node(member);
            if (member->type == NodeType::VarDecl) {
                emit(";");
                flush_trailing_comment(member);
                emit("\n");
            } else if (member->type == NodeType::FnDef) {
                // FnDef handler already prints \n after body
            }
            prev_member = member;
        }
        m_indent--;
        print_indent(m_indent);
    }
    emit("}}");
}

void AstPrinter::print_declspec(DeclSpec *declspec) {
    for (auto attr : declspec->attributes) {
        emit("@[");
        print_node(attr->data.decl_attribute.term);
        emit("]\n");
        print_indent(m_indent);
    }
    if (declspec->has_flag(DECL_PRIVATE)) {
        emit("private ");
    }
    if (declspec->has_flag(DECL_PROTECTED)) {
        emit("protected ");
    }
    if (declspec->has_flag(DECL_STATIC)) {
        emit("static ");
    }
    if (declspec->has_flag(DECL_MUTABLE)) {
        emit("mut ");
    }
    if (declspec->has_flag(DECL_ASYNC)) {
        emit("async ");
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
    if (!m_comments)
        return;
    long last_comment_line = -1;
    while (m_comment_idx < m_comments->len) {
        auto &comment = m_comments->at(m_comment_idx);
        if (!(comment.pos < before_pos))
            break;

        if (comment.pos.line == before_pos.line) {
            // Inline comment (same line as the next token) — print inline
            emit("{} ", comment.text);
        } else {
            // Own-line comment — print on its own line with indent
            if (last_comment_line >= 0 && comment.pos.line - last_comment_line > 1) {
                emit("\n");
            }
            print_indent(m_indent);
            emit("{}\n", comment.text);
            last_comment_line = comment.pos.line;
        }
        m_comment_idx++;
    }
    // Preserve blank line between last own-line comment and the node
    if (last_comment_line >= 0 && before_pos.line - last_comment_line > 1) {
        emit("\n");
    }
}

void AstPrinter::flush_trailing_comment(Node *node) {
    if (!m_comments || !node)
        return;
    auto *lt = last_token(node);
    if (!lt)
        return;
    long on_line = lt->pos.line;
    while (m_comment_idx < m_comments->len) {
        auto &comment = m_comments->at(m_comment_idx);
        if (comment.pos.line != on_line)
            break;
        emit(" {}", comment.text);
        m_comment_idx++;
    }
}
