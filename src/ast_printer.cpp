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
    case SigilKind::MutRef:
        return "&mut";
    case SigilKind::Move:
        return "&move";
    case SigilKind::FixedArray:
    case SigilKind::Span:
        return ""; // handled specially in TypeSigil printer
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

static bool decl_emits_trailing_newline(Node *node) {
    if (!node)
        return false;
    switch (node->type) {
    case NodeType::FnDef:
        return node->data.fn_def.body != nullptr;
    case NodeType::StructDecl:
    case NodeType::ImplementBlock:
    case NodeType::EnumDecl:
    case NodeType::ExternDecl:
        return true;
    default:
        return false;
    }
}

static bool is_value_context(Node *node) {
    if (!node || !node->parent)
        return false;

    auto *parent = node->parent;
    switch (parent->type) {
    case NodeType::ReturnStmt:
    case NodeType::AwaitExpr:
    case NodeType::ThrowStmt:
    case NodeType::ParenExpr:
    case NodeType::TupleExpr:
    case NodeType::CastExpr:
    case NodeType::PrefixExpr:
    case NodeType::UnaryOpExpr:
    case NodeType::BinOpExpr:
    case NodeType::FieldInitExpr:
    case NodeType::FnCallExpr:
    case NodeType::ConstructExpr:
    case NodeType::SubtypeExpr:
    case NodeType::IndexExpr:
    case NodeType::SliceExpr:
    case NodeType::RangeExpr:
    case NodeType::SwitchExpr:
        return true;

    case NodeType::CaseExpr:
        return is_value_context(parent);

    case NodeType::IfExpr:
        if (parent->data.if_expr.then_block == node || parent->data.if_expr.else_node == node)
            return is_value_context(parent);
        return true;

    case NodeType::Block:
        return parent->data.block.return_expr == node || parent->data.block.is_arrow;

    default:
        return false;
    }
}

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
        Node *prev_decl = nullptr;
        for (auto decl : m_root->data.root.top_level_decls) {
            if (prev_decl) {
                long prev_end = -1;
                if (prev_decl->end_token)
                    prev_end = prev_decl->end_token->pos.line;
                else if (prev_decl->token)
                    prev_end = prev_decl->token->pos.line;
                else if (prev_decl->start_token)
                    prev_end = prev_decl->start_token->pos.line;

                long next_start = -1;
                if (m_comments && m_comment_idx < m_comments->len) {
                    auto &comment = m_comments->at(m_comment_idx);
                    auto *ft = first_token(decl);
                    long node_line = ft ? ft->pos.line : -1;
                    if (node_line >= 0 && comment.pos.line < node_line) {
                        next_start = comment.pos.line;
                    }
                }
                if (next_start < 0) {
                    auto *ft = first_token(decl);
                    if (ft)
                        next_start = ft->pos.line;
                }

                if (prev_end >= 0 && next_start >= 0) {
                    auto source_blank_lines = std::max<long>(0, next_start - prev_end - 1);
                    auto printer_blank_lines = decl_emits_trailing_newline(prev_decl) ? 1L : 0L;
                    for (long i = printer_blank_lines; i < source_blank_lines; i++) {
                        emit("\n");
                    }
                }
            }
            auto *ft = first_token(decl);
            if (ft)
                flush_comments_before(ft->pos);
            print_node(decl);
            if (decl->type == NodeType::VarDecl) {
                emit(";");
            }
            bool is_last_decl =
                m_root->data.root.top_level_decls.len > 0 &&
                decl == m_root->data.root.top_level_decls[m_root->data.root.top_level_decls.len - 1];
            if (!(is_last_decl && decl_emits_trailing_newline(decl))) {
                emit("\n");
            }
            prev_decl = decl;
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
        // Arrow lambdas: omit 'func' keyword — (n) => n * 2
        if (data.fn_kind == FnKind::Lambda && data.body && data.body->data.block.is_arrow) {
            m_suppress_func_keyword = true;
        }
        print_node(data.fn_proto);
        m_suppress_func_keyword = false;
        if (data.body) {
            if (!data.body->data.block.is_arrow) {
                emit(" ");
            }
            auto prev_fn_ret = m_fn_return_type;
            auto prev_strip_arrow_return = m_strip_arrow_return;
            m_fn_return_type = data.fn_proto ? data.fn_proto->data.fn_proto.return_type : nullptr;
            m_strip_arrow_return = data.fn_kind == FnKind::Lambda && data.body->data.block.is_arrow;
            print_node(data.body);
            m_strip_arrow_return = prev_strip_arrow_return;
            m_fn_return_type = prev_fn_ret;
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
            if (data.lifetime_params.len > 0) {
                emit("<");
                for (int i = 0; i < data.lifetime_params.len; i++) {
                    if (i > 0)
                        emit(", ");
                    auto *lt = data.lifetime_params[i];
                    emit("'{}", lt->name);
                    auto &bound = lt->data.lifetime_param.bound;
                    if (!bound.empty())
                        emit(": '{}", bound);
                }
                emit(">");
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
        // Print lifetime and type parameters if present
        if (data.lifetime_params.len > 0 || data.type_params.len > 0) {
            emit("<");
            for (int i = 0; i < data.lifetime_params.len; i++) {
                if (i > 0)
                    emit(", ");
                auto *lt = data.lifetime_params[i];
                emit("'{}", lt->name);
                auto &bound = lt->data.lifetime_param.bound;
                if (!bound.empty())
                    emit(": '{}", bound);
            }
            if (data.lifetime_params.len > 0 && data.type_params.len > 0)
                emit(", ");
            emit_wrapped_list(&data.type_params, "", "", ", ");
            emit(">");
        }
        // For params, handle varargs specially
        if (data.is_vararg) {
            // Calculate total length including varargs
            int base_len = m_current_column + 1; // "("
            for (int i = 0; i < data.params.len; i++) {
                auto param_str = format_node_to_string(data.params.at(i));
                base_len += param_str.size();
                if (i < data.params.len - 1 || data.params.len > 0) {
                    base_len += 2; // ", "
                }
            }
            base_len += 3; // "..."
            base_len += 1; // ")"
            bool should_wrap = base_len >= m_max_line_length;

            emit("(");
            if (should_wrap) {
                emit("\n");
                m_indent++;
                for (int i = 0; i < data.params.len; i++) {
                    print_indent(m_indent);
                    print_node(data.params.at(i));
                    emit(",\n");
                }
                print_indent(m_indent);
                emit("...\n");
                m_indent--;
                print_indent(m_indent);
            } else {
                print_node_list(&data.params);
                if (data.params.len > 0) {
                    emit(", ");
                }
                emit("...");
            }
            emit(")");
        } else {
            bool wrapped = emit_wrapped_list(&data.params, "(", ")", ", ");
        }
        if (data.return_type) {
            emit(" ");
            bool wrap = data.return_type->type == NodeType::FnProto;
            if (wrap) emit("(");
            print_node(data.return_type);
            if (wrap) emit(")");
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
        if (data.is_variadic) emit("...");
        emit("{}", node->name);
        if (!data.lifetime_bound.empty()) {
            emit(": '{}", data.lifetime_bound);
        } else if (data.type_bounds.len > 0) {
            emit(": ");
            for (size_t i = 0; i < data.type_bounds.len; i++) {
                if (i > 0)
                    emit(" + ");
                print_node(data.type_bounds[i]);
            }
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
        bool arrow_value_context = data.is_arrow && (m_strip_arrow_return || is_value_context(node));
        if (data.is_arrow) {
            emit(" => ");
        }
        if (data.is_unsafe) {
            emit("unsafe ");
        }
        bool is_empty_block = data.has_braces && !data.statements.len && !data.return_expr;
        if (data.has_braces) {
            if (is_empty_block) {
                emit("{{}}");
                break;
            }
            m_indent++;
            emit("{{\n");
        }

        Node *prev_stmt = nullptr;
        for (auto stmt : data.statements) {
            // Arrow lambda bodies: print expression directly without 'return' keyword
            if (arrow_value_context && stmt->type == NodeType::ReturnStmt &&
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
                } else if (stmt->type == NodeType::IfExpr || stmt->type == NodeType::Block ||
                           stmt->type == NodeType::SwitchExpr) {
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
    case NodeType::DestructureDecl: {
        auto &data = node->data.destructure_decl;
        switch (data.kind) {
        case ast::VarKind::Mutable:
            emit("var ");
            break;
        case ast::VarKind::Immutable:
            emit("let ");
            break;
        case ast::VarKind::Constant:
            emit("const ");
            break;
        }
        print_destructure_pattern(node);
        emit(" = ");
        print_node(data.expr);
        break;
    }
    case NodeType::VarDecl: {
        auto &data = node->data.var_decl;
        // Interface embed: ...InterfaceName (no var keyword, no field name)
        if (data.is_embed && !data.is_field) {
            emit("...");
            if (data.type)
                print_node(data.type);
            break;
        }
        if (data.is_field) {
            if (data.decl_spec) {
                print_declspec(data.decl_spec);
            }
        } else {
            if (data.decl_spec) {
                print_declspec(data.decl_spec);
            }
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
        // Collapse construct expressions:
        // - `var x: Type = {items}` → `var x = Type{items}` (move annotation to construct)
        // - `var x: Type = Type{items}` → `var x = Type{items}` (strip redundant annotation)
        // - Array types → `var x: Array<T> = [items]`
        auto *expr = data.expr;
        if (!data.is_field && expr && expr->type == NodeType::ConstructExpr &&
            !expr->data.construct_expr.is_new && !expr->data.construct_expr.is_array_literal) {
            auto &cdata = expr->data.construct_expr;
            auto *ctype = cdata.type;
            auto *vtype = data.type;
            Node *etype = ctype ? ctype : vtype;

            // Check if effective type is Array
            bool is_array = false;
            if (etype) {
                if (etype->type == NodeType::SubtypeExpr && etype->data.subtype_expr.type &&
                    etype->data.subtype_expr.type->name == "Array") {
                    is_array = true;
                } else if (etype->type == NodeType::Identifier && etype->name == "Array") {
                    is_array = true;
                }
            }

            // Check if effective type is Unit
            bool is_unit = false;
            if (etype && !cdata.items.len && !cdata.field_inits.len && !cdata.spread_expr) {
                if (etype->type == NodeType::Identifier && etype->name == "Unit") {
                    is_unit = true;
                }
            }

            if (is_unit) {
                emit(" = ()");
            } else if (is_array) {
                // Array: var x: Array<T> = [items]
                if (etype) {
                    emit(": ");
                    print_node(etype);
                }
                emit(" = ");
                emit_wrapped_list(&cdata.items, "[", "]", ", ");
            } else {
                // Non-array: collapse to var x = Type{items}
                bool keep_annotation = vtype && ctype && !types_match(vtype, ctype);
                if (keep_annotation) {
                    emit(": ");
                    print_node(vtype);
                }
                emit(" = ");
                if (etype)
                    print_node(etype);
                // Use wrapping for long construct expressions
                if (cdata.items.len && !cdata.field_inits.len && !cdata.spread_expr) {
                    emit_wrapped_list(&cdata.items, "{", "}", ", ");
                } else {
                    emit_construct_body(cdata);
                }
            }
        } else {
            // Non-construct expression or array literal — print normally
            if (data.type) {
                emit(": ");
                print_node(data.type);
            }
            if (expr) {
                emit(" = ");
                print_node(expr);
            }
        }
        break;
    }
    case NodeType::StructDecl: {
        auto &data = node->data.struct_decl;
        if (data.decl_spec) {
            print_declspec(data.decl_spec);
        }
        emit("{} ", node->token->str);
        if (!node->name.empty()) {
            emit("{}", node->name);
        }
        if (data.lifetime_params.len > 0 || data.type_params.len > 0) {
            emit("<");
            for (int i = 0; i < data.lifetime_params.len; i++) {
                if (i > 0)
                    emit(", ");
                auto *lt = data.lifetime_params[i];
                emit("'{}", lt->name);
                auto &bound = lt->data.lifetime_param.bound;
                if (!bound.empty())
                    emit(": '{}", bound);
            }
            if (data.lifetime_params.len > 0 && data.type_params.len > 0)
                emit(", ");
            emit_wrapped_list(&data.type_params, "", "", ", ");
            emit(">");
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
                    if (prev_type == NodeType::FnDef && prev_member->data.fn_def.body)
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
                    // FnDef with body prints \n itself; bodyless (interface) needs \n
                    if (!member->data.fn_def.body) {
                        flush_trailing_comment(member);
                        emit("\n");
                    }
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
    case NodeType::ImplementBlock: {
        auto &data = node->data.implement_block;
        emit("impl ");
        for (size_t i = 0; i < data.interface_types.len; i++) {
            if (i > 0)
                emit(", ");
            print_node(data.interface_types[i]);
        }
        if (data.where_clauses.len > 0) {
            if (data.interface_types.len > 0)
                emit(" ");
            emit("where ");
            for (size_t i = 0; i < data.where_clauses.len; i++) {
                if (i > 0) {
                    // Same param as previous: use +
                    if (data.where_clauses[i].param_name->str ==
                        data.where_clauses[i - 1].param_name->str) {
                        emit(" + ");
                    } else {
                        emit(", ");
                        emit(data.where_clauses[i].param_name->str);
                        emit(": ");
                    }
                } else {
                    emit(data.where_clauses[i].param_name->str);
                    emit(": ");
                }
                print_node(data.where_clauses[i].bound_type);
            }
        }
        emit(" {{");
        if (data.members.len) {
            emit("\n");
            m_indent++;
            Node *prev_member = nullptr;
            for (auto member : data.members) {
                if (prev_member && has_blank_line_between(prev_member, member)) {
                    emit("\n");
                }
                auto *ft = first_token(member);
                if (ft)
                    flush_comments_before(ft->pos);
                print_indent(m_indent);
                print_node(member);
                if (member->type == NodeType::FnDef && !member->data.fn_def.body) {
                    flush_trailing_comment(member);
                    emit("\n");
                }
                prev_member = member;
            }
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
        emit("{}{}", data.is_optional_chain ? "?." : ".", data.field->str);
        break;
    }
    case NodeType::ConstructExpr: {
        auto &data = node->data.construct_expr;
        if (data.is_array_literal) {
            emit_wrapped_list(&data.items, "[", "]", ", ");
            break;
        }
        // Collapse Unit{} or {} (with Unit context) to ()
        bool is_unit_construct = !data.items.len && !data.field_inits.len && !data.spread_expr;
        if (is_unit_construct) {
            if (data.type && data.type->type == NodeType::Identifier &&
                data.type->name == "Unit") {
                emit("()");
                break;
            }
            if (!data.type && node->resolved_type &&
                node->resolved_type->kind == cx::TypeKind::Unit) {
                emit("()");
                break;
            }
        }

        // Consume and reset flag so it only affects THIS construct, not nested ones
        bool suppress = m_suppress_construct_type;
        m_suppress_construct_type = false;
        if (data.is_new) {
            emit("new ");
        }
        if (data.type && !suppress) {
            print_node(data.type);
        }
        // For construct expressions, always try wrapping
        if (data.items.len || data.field_inits.len || data.spread_expr) {
            // If only items (no field_inits, no spread), use emit_wrapped_list
            if (data.items.len && !data.field_inits.len && !data.spread_expr) {
                emit_wrapped_list(&data.items, "{", "}", ", ");
            } else {
                emit_construct_body(data);
            }
        } else {
            emit("{{}}");
        }
        break;
    }
    case NodeType::FieldInitExpr: {
        auto &data = node->data.field_init_expr;
        // Collapse key: key → :key
        if (data.value && data.value->type == NodeType::Identifier &&
            data.value->name == data.field->str) {
            emit(":{}", data.field->str);
        } else {
            emit("{}: ", data.field->str);
            print_node(data.value);
        }
        break;
    }
    case NodeType::BinOpExpr: {
        auto &data = node->data.bin_op_expr;
        print_node(data.op1);
        // ?? coalesce is stored as QUES since lexer no longer merges ??
        if (data.op_type == TokenType::QUES)
            emit(" ?? ");
        else
            emit(" {} ", get_token_symbol(data.op_type));
        // Strip redundant type from construct in assignments (same pattern as ReturnStmt)
        if (data.op_type == TokenType::ASS && data.op2 &&
            data.op2->type == NodeType::ConstructExpr && data.op2->data.construct_expr.type &&
            !data.op2->data.construct_expr.is_new &&
            !data.op2->data.construct_expr.is_array_literal) {
            auto *var = data.op1->get_decl();
            Node *target_type = nullptr;
            if (var && var->type == NodeType::VarDecl) {
                target_type = var->data.var_decl.type;
                if (!target_type && var->data.var_decl.expr &&
                    var->data.var_decl.expr->type == NodeType::ConstructExpr)
                    target_type = var->data.var_decl.expr->data.construct_expr.type;
            }
            if (target_type && types_match(target_type, data.op2->data.construct_expr.type)) {
                m_suppress_construct_type = true;
            }
        }
        print_node(data.op2);
        m_suppress_construct_type = false;
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
            // Strip redundant type from construct only when it matches the function return type
            if (m_fn_return_type && data.expr->type == NodeType::ConstructExpr &&
                data.expr->data.construct_expr.type && !data.expr->data.construct_expr.is_new &&
                types_match(m_fn_return_type, data.expr->data.construct_expr.type)) {
                m_suppress_construct_type = true;
            }
            print_node(data.expr);
            m_suppress_construct_type = false;
        }
        break;
    }
    case NodeType::ThrowStmt: {
        emit("throw ");
        print_node(node->data.throw_stmt.expr);
        break;
    }
    case NodeType::ParenExpr: {
        auto &child = node->data.child_expr;
        emit("(");
        print_node(child);
        emit(")");
        break;
    }
    case NodeType::UnitExpr: {
        emit("()");
        break;
    }
    case NodeType::TupleExpr: {
        auto &data = node->data.tuple_expr;
        emit("(");
        for (int i = 0; i < data.items.len; i++) {
            print_node(data.items[i]);
            if (i < data.items.len - 1) emit(", ");
        }
        emit(")");
        break;
    }
    case NodeType::IfExpr: {
        auto &data = node->data.if_expr;
        emit("if ");
        if (data.binding_decl) {
            auto kind = data.binding_decl->type == NodeType::DestructureDecl
                            ? data.binding_decl->data.destructure_decl.kind
                            : data.binding_decl->data.var_decl.kind;
            emit(kind == VarKind::Mutable ? "var " : "let ");
            if (data.binding_decl->type == NodeType::DestructureDecl) {
                if (data.binding_clause) {
                    print_node(data.binding_clause);
                }
                print_destructure_pattern(data.binding_decl);
            } else {
                emit(data.binding_decl->name);
            }
            emit(" = ");
        }
        auto *cond = data.condition;
        if (cond && cond->type == NodeType::ParenExpr)
            cond = cond->data.child_expr;
        print_node(cond);

        // Collapse single-expression blocks: { expr } → => expr
        auto can_collapse = [](Node *block) {
            return block && block->type == NodeType::Block && block->data.block.has_braces &&
                   block->data.block.statements.len == 0 &&
                   block->data.block.return_expr != nullptr;
        };
        auto already_arrow = [](Node *block) {
            return block && block->type == NodeType::Block && block->data.block.is_arrow;
        };

        bool collapse_then = can_collapse(data.then_block);
        if (collapse_then) {
            data.then_block->data.block.is_arrow = true;
            data.then_block->data.block.has_braces = false;
        } else if (!already_arrow(data.then_block)) {
            emit(" ");
        }
        print_node(data.then_block);
        if (collapse_then) {
            data.then_block->data.block.is_arrow = false;
            data.then_block->data.block.has_braces = true;
        }

        if (data.else_node) {
            emit(" else");
            if (data.else_node->type == NodeType::IfExpr) {
                emit(" ");
                print_node(data.else_node);
            } else {
                bool collapse_else = can_collapse(data.else_node);
                if (collapse_else) {
                    data.else_node->data.block.is_arrow = true;
                    data.else_node->data.block.has_braces = false;
                } else if (!already_arrow(data.else_node)) {
                    emit(" ");
                }
                print_node(data.else_node);
                if (collapse_else) {
                    data.else_node->data.block.is_arrow = false;
                    data.else_node->data.block.has_braces = true;
                }
            }
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
            emit_wrapped_list(&data.type_args, "", "", ", ");
            emit(">");
        }
        emit_wrapped_list(&data.args, "(", ")", ", ");
        break;
    }
    case NodeType::SubtypeExpr: {
        auto &data = node->data.subtype_expr;
        print_node(data.type);
        emit("<");
        for (int i = 0; i < data.lifetime_args.len; i++) {
            if (i > 0)
                emit(", ");
            auto *lt = data.lifetime_args[i];
            emit("'{}", lt->name);
            auto &bound = lt->data.lifetime_param.bound;
            if (!bound.empty())
                emit(": '{}", bound);
        }
        if (data.lifetime_args.len > 0 && data.args.len > 0)
            emit(", ");
        emit_wrapped_list(&data.args, "", "", ", ");
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
    case NodeType::SliceExpr: {
        auto &data = node->data.slice_expr;
        print_node(data.expr);
        emit("[");
        if (data.start) {
            print_node(data.start);
        }
        emit("..");
        if (data.end) {
            print_node(data.end);
        }
        emit("]");
        break;
    }
    case NodeType::RangeExpr: {
        auto &data = node->data.range_expr;
        if (data.start) {
            print_node(data.start);
        }
        emit("..");
        if (data.end) {
            print_node(data.end);
        }
        break;
    }
    case NodeType::TypeSigil: {
        auto &data = node->data.sigil_type;
        if (data.sigil == SigilKind::FixedArray) {
            emit("[{}]", data.fixed_size);
            print_node(data.type);
            break;
        }
        if (data.sigil == SigilKind::Span) {
            emit(data.is_mut ? "[]mut " : "[]");
            print_node(data.type);
            break;
        }
        bool has_lifetime = !data.lifetime.empty();
        bool is_mut = data.sigil == SigilKind::MutRef;
        bool is_move = data.sigil == SigilKind::Move;
        if (has_lifetime && is_mut) {
            emit("&(mut, '{})", data.lifetime);
        } else if (has_lifetime) {
            emit("&'{}", data.lifetime);
        } else {
            emit("{}", get_sigil_symbol(data.sigil));
        }
        if (is_mut || is_move || has_lifetime) {
            emit(" ");
        }
        // Wrap inner type in parens when stacking different sigils for clarity
        auto inner_sigil = data.type->type == NodeType::TypeSigil ? data.type->data.sigil_type.sigil
                                                                  : SigilKind::None;
        bool needs_parens =
            inner_sigil != SigilKind::None && inner_sigil != data.sigil &&
            !(data.sigil == SigilKind::Optional && inner_sigil == SigilKind::Pointer) &&
            inner_sigil != SigilKind::FixedArray &&
            inner_sigil != SigilKind::Span;
        if (needs_parens)
            emit("(");
        print_node(data.type);
        if (needs_parens)
            emit(")");
        break;
    }
    case NodeType::TypedefDecl: {
        auto &data = node->data.typedef_decl;
        emit("typedef ");
        emit(data.identifier->str);
        if (data.type_params.len) {
            emit("<");
            emit_wrapped_list(&data.type_params, "", "", ", ");
            emit(">");
        }
        emit(" = ");
        print_node(data.type);
        emit(";");
        break;
    }
    case NodeType::EnumVariant: {
        auto &data = node->data.enum_variant;
        emit("{}", node->name);
        if (data.is_tuple_form) {
            emit("(");
            for (int i = 0; i < data.tuple_types.len; i++) {
                print_node(data.tuple_types[i]);
                if (i < data.tuple_types.len - 1) {
                    emit(", ");
                }
            }
            emit(")");
        } else if (data.struct_body) {
            emit(" ");
            print_struct_members(data.struct_body->data.struct_decl);
        }
        if (data.value) {
            emit(" = ");
            print_node(data.value);
        }
        break;
    }
    case NodeType::UnaryOpExpr: {
        auto &data = node->data.unary_op_expr;
        if (!data.is_suffix) {
            if (data.op_type == TokenType::MUTREF) {
                emit("&mut ");
            } else if (data.op_type == TokenType::MOVEREF) {
                emit("&move ");
            } else if (data.op_type == TokenType::KW_MOVE) {
                emit("move ");
            } else if (data.op_type == TokenType::KW_DELETE) {
                emit("delete ");
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
        if (data.catch_block) {
            if (data.catch_expr) {
                emit(" catch ");
                print_node(data.catch_expr);
                if (data.catch_err_var) {
                    emit(" as ");
                    emit(data.catch_err_var->name);
                }
                emit(" ");
            } else {
                emit(" catch ");
            }
            print_node(data.catch_block);
        } else if (data.catch_expr) {
            emit(" catch ");
            print_node(data.catch_expr);
        }
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
            // Collapse `for var i = START; i < END; i++` into `for i in START..END`
            bool collapsed = false;
            if (data.init && data.condition && data.post && data.init->type == NodeType::VarDecl &&
                data.init->data.var_decl.type == nullptr && data.init->data.var_decl.expr &&
                data.condition->type == NodeType::BinOpExpr &&
                data.condition->data.bin_op_expr.op_type == TokenType::LT &&
                data.condition->data.bin_op_expr.op1->type == NodeType::Identifier &&
                data.condition->data.bin_op_expr.op1->name == data.init->name &&
                data.post->type == NodeType::UnaryOpExpr &&
                data.post->data.unary_op_expr.op_type == TokenType::INC &&
                data.post->data.unary_op_expr.op1->type == NodeType::Identifier &&
                data.post->data.unary_op_expr.op1->name == data.init->name) {
                emit("{} in ", data.init->name);
                print_node(data.init->data.var_decl.expr);
                emit("..");
                print_node(data.condition->data.bin_op_expr.op2);
                emit(" ");
                collapsed = true;
            }
            if (!collapsed) {
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
        }
        if (data.kind == ForLoopKind::Range) {
            if (data.bind_sigil != SigilKind::None) {
                emit("{}", get_sigil_symbol(data.bind_sigil));
                if (data.bind_sigil == SigilKind::MutRef || data.bind_sigil == SigilKind::Move) {
                    emit(" ");
                }
            }
            if (data.bind) {
                print_node(data.bind);
                if (data.index_bind) {
                    emit(", ");
                    print_node(data.index_bind);
                }
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
            auto *cond = data.condition;
            if (cond->type == NodeType::ParenExpr)
                cond = cond->data.child_expr;
            print_node(cond);
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
        if (data.decl_spec) {
            print_declspec(data.decl_spec);
        }
        emit("extern {} ", data.type->to_string());
        emit("{{\n");
        m_indent++;

        // Print imports
        for (auto import_node : data.imports) {
            auto *ft = first_token(import_node);
            if (ft)
                flush_comments_before(ft->pos);
            print_indent(m_indent);
            print_node(import_node);
            emit("\n");
        }

        // Print exports
        for (auto export_node : data.exports) {
            auto *ft = first_token(export_node);
            if (ft)
                flush_comments_before(ft->pos);
            print_indent(m_indent);
            print_node(export_node);
            emit("\n");
        }

        // Print inline function declarations
        bool extern_exported = data.decl_spec && data.decl_spec->has_flag(DECL_EXPORTED);
        for (auto member : data.members) {
            auto *ft = first_token(member);
            if (ft)
                flush_comments_before(ft->pos);
            // Suppress per-member export when the extern block itself is exported
            auto *ds = member->get_declspec();
            if (extern_exported && ds) {
                ds->flags &= ~DECL_EXPORTED;
            }
            print_indent(m_indent);
            print_node(member);
            emit("\n");
            if (extern_exported && ds) {
                ds->flags |= DECL_EXPORTED;
            }
        }
        m_indent--;
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
        if (data.alias) {
            // Canonical form: import "./module" as mod;
            emit(data.path->to_string());
            emit(" as {}", data.alias->to_string());
        } else if (data.symbols.len) {
            // Canonical form: import {X, Y} from "./module";
            // Use wrapped list for long imports
            emit_wrapped_list(&data.symbols, "{", "}", ", ");
            emit(" from ");
            emit(data.path->to_string());
        } else {
            // Fallback (shouldn't happen in well-formed code)
            emit(data.path->to_string());
        }
        emit(";");
        break;
    }
    case NodeType::ExportDecl: {
        auto &data = node->data.export_decl;
        emit("export ");
        if (data.match_all) {
            // Canonical form: export * from "./module";
            emit("* from ");
            emit(data.path->to_string());
        } else if (data.symbols.len) {
            // Canonical form: export {X, Y} from "./module";
            // Use wrapped list for long exports
            emit_wrapped_list(&data.symbols, "{", "}", ", ");
            emit(" from ");
            emit(data.path->to_string());
        } else {
            // Fallback (shouldn't happen in well-formed code)
            emit(data.path->to_string());
        }
        emit(";");
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
        auto *expr = data.expr;
        if (expr && expr->type == NodeType::ParenExpr)
            expr = expr->data.child_expr;
        print_node(expr);
        if (data.is_type_switch)
            emit(".(type)");
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
                if (i == 0 && data.destructure_pattern) {
                    print_destructure_pattern(data.destructure_pattern);
                }
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
        if (data.type_params.len) {
            emit("<");
            emit_wrapped_list(&data.type_params, "", "", ", ");
            emit(">");
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
            bool is_last = (i == data.variants.len - 1);
            if (is_last && data.base_struct) {
                emit(";");
            } else if (!is_last) {
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
    case NodeType::PackExpansion: {
        auto &data = node->data.pack_expansion;
        emit("...");
        print_node(data.expr);
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

void AstPrinter::print_destructure_pattern(Node *node) {
    auto &data = node->data.destructure_decl;
    if (data.is_tuple) {
        emit("(");
        for (size_t i = 0; i < data.fields.len; i++) {
            if (i > 0)
                emit(", ");
            auto &field_data = data.fields[i]->data.destructure_field;
            if (field_data.is_rest)
                emit("...");
            else if (field_data.sigil == SigilKind::MutRef)
                emit("&mut ");
            else if (field_data.sigil == SigilKind::Reference)
                emit("&");
            emit("{}", field_data.binding_name->str);
        }
        emit(")");
    } else if (data.is_array) {
        emit("[");
        for (size_t i = 0; i < data.fields.len; i++) {
            if (i > 0)
                emit(", ");
            auto &field_data = data.fields[i]->data.destructure_field;
            if (field_data.is_rest)
                emit("...");
            else if (field_data.sigil == SigilKind::MutRef)
                emit("&mut ");
            else if (field_data.sigil == SigilKind::Reference)
                emit("&");
            emit("{}", field_data.binding_name->str);
        }
        emit("]");
    } else {
        emit("{{");
        for (size_t i = 0; i < data.fields.len; i++) {
            if (i > 0)
                emit(", ");
            auto &field_data = data.fields[i]->data.destructure_field;
            if (field_data.sigil == SigilKind::MutRef)
                emit("&mut ");
            else if (field_data.sigil == SigilKind::Reference)
                emit("&");
            emit("{}", field_data.field_name->str);
            if (field_data.nested) {
                emit(": ");
                print_destructure_pattern(field_data.nested);
            } else if (field_data.binding_name != field_data.field_name) {
                emit(": {}", field_data.binding_name->str);
            }
        }
        emit("}}");
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
                if (prev_type == NodeType::FnDef && prev_member->data.fn_def.body)
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
    if (declspec->has_flag(DECL_EXPORTED)) {
        emit("export ");
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
    if (declspec->has_flag(DECL_UNSAFE)) {
        emit("unsafe ");
    }
}

string AstPrinter::format_node_to_string(Node *node) {
    if (!node)
        return "";
    // Create a temporary printer to format this node to a string
    AstPrinter temp_printer(node, nullptr);
    return temp_printer.format_to_string();
}

bool AstPrinter::emit_wrapped_list(array<Node *> *items, const char *open, const char *close,
                                   const char *separator, int extra_indent) {
    if (!items || items->len == 0) {
        emit("{}{}", open, close);
        return false;
    }

    // Calculate the total length if printed on one line
    int total_length = m_current_column;
    total_length += strlen(open);
    for (int i = 0; i < items->len; i++) {
        auto item_str = format_node_to_string(items->at(i));
        total_length += item_str.size();
        if (i < items->len - 1) {
            total_length += strlen(separator);
        }
    }
    total_length += strlen(close);

    // If it fits on one line, emit inline
    if (total_length < m_max_line_length) {
        emit("{}", open);
        for (int i = 0; i < items->len; i++) {
            print_node(items->at(i));
            if (i < items->len - 1) {
                emit("{}", separator);
            }
        }
        emit("{}", close);
        return false;
    }

    // Otherwise, wrap with proper indentation
    emit("{}\n", open);
    m_indent += extra_indent;
    for (int i = 0; i < items->len; i++) {
        print_indent(m_indent);
        print_node(items->at(i));
        if (i < items->len - 1) {
            emit(",");
        }
        emit("\n");
    }
    m_indent -= extra_indent;
    print_indent(m_indent);
    emit("{}", close);
    return true;
}

void AstPrinter::emit_construct_body(ConstructExpr &data) {
    // Build a list of all elements for length calculation
    // Format: {spread, items..., field_inits...}
    auto format_element = [&](int idx, bool wrapped) {
        int pos = 0;
        if (data.spread_expr) {
            if (idx == pos) {
                emit("...");
                print_node(data.spread_expr);
                return;
            }
            pos++;
        }
        if (idx < pos + data.items.len) {
            print_node(data.items[idx - pos]);
            return;
        }
        pos += data.items.len;
        print_node(data.field_inits[idx - pos]);
    };

    int total_elements = (data.spread_expr ? 1 : 0) + data.items.len + data.field_inits.len;

    // Calculate total length if printed on one line
    int total_length = m_current_column + 2; // for { and }
    for (int i = 0; i < total_elements; i++) {
        // Format each element to measure its length
        int pos = 0;
        string elem_str;
        if (data.spread_expr && i == 0) {
            elem_str = "..." + format_node_to_string(data.spread_expr);
            pos = 1;
        } else {
            int adj = i - (data.spread_expr ? 1 : 0);
            if (adj < data.items.len) {
                elem_str = format_node_to_string(data.items[adj]);
            } else {
                elem_str = format_node_to_string(data.field_inits[adj - data.items.len]);
            }
        }
        total_length += elem_str.size();
        if (i < total_elements - 1)
            total_length += 2; // ", "
    }

    if (total_length < m_max_line_length && total_elements <= 2) {
        // Inline
        emit("{{");
        for (int i = 0; i < total_elements; i++) {
            format_element(i, false);
            if (i < total_elements - 1)
                emit(", ");
        }
        emit("}}");
    } else {
        // Wrapped
        emit("{{\n");
        m_indent++;
        for (int i = 0; i < total_elements; i++) {
            print_indent(m_indent);
            format_element(i, true);
            if (i < total_elements - 1)
                emit(",");
            emit("\n");
        }
        m_indent--;
        print_indent(m_indent);
        emit("}}");
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

bool AstPrinter::types_match(Node *a, Node *b) {
    if (!a || !b)
        return false;
    if (a->type != b->type)
        return false;
    switch (a->type) {
    case NodeType::Identifier:
        return a->name == b->name;
    case NodeType::Primitive:
        return a->name == b->name;
    case NodeType::SubtypeExpr: {
        if (!types_match(a->data.subtype_expr.type, b->data.subtype_expr.type))
            return false;
        if (a->data.subtype_expr.args.len != b->data.subtype_expr.args.len)
            return false;
        for (int i = 0; i < a->data.subtype_expr.args.len; i++) {
            if (!types_match(a->data.subtype_expr.args.at(i), b->data.subtype_expr.args.at(i)))
                return false;
        }
        return true;
    }
    case NodeType::TypeSigil:
        if (a->data.sigil_type.sigil == SigilKind::FixedArray ||
            b->data.sigil_type.sigil == SigilKind::FixedArray) {
            return a->data.sigil_type.sigil == b->data.sigil_type.sigil &&
                   a->data.sigil_type.fixed_size == b->data.sigil_type.fixed_size &&
                   types_match(a->data.sigil_type.type, b->data.sigil_type.type);
        }
        return a->data.sigil_type.sigil == b->data.sigil_type.sigil &&
               types_match(a->data.sigil_type.type, b->data.sigil_type.type);
    case NodeType::DotExpr:
        return a->data.dot_expr.field->str == b->data.dot_expr.field->str &&
               types_match(a->data.dot_expr.expr, b->data.dot_expr.expr);
    default:
        return false;
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
