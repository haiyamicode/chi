/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "analyzer.h"
#include "util.h"

using namespace cx;

static ast::Node *resolve_scanned_decl(ast::Node *decl, cx::Scope *scope) {
    if (!decl || !scope || decl->type != ast::NodeType::VarDecl || decl->resolved_type) {
        return decl;
    }

    auto replacement = scope->find_one(decl->name);
    if (!replacement || replacement == decl || !replacement->resolved_type) {
        return decl;
    }

    return replacement;
}

static bool cursor_in_node(ast::Node *node, Pos cursor_pos) {
    if (!node || !node->start_token || !node->end_token) {
        return false;
    }
    return cursor_pos.is_in_range(node->start_token->pos, node->end_token->pos) ||
           cursor_pos.add_offset(-1).is_in_range(node->start_token->pos, node->end_token->pos);
}

static cx::Scope *find_stmt_scope(ast::Node *node, Pos cursor_pos, cx::Scope *fallback) {
    if (!node) {
        return fallback;
    }

    switch (node->type) {
    case ast::NodeType::Block: {
        auto &block = node->data.block;
        auto scope = block.scope ? block.scope : fallback;
        for (auto stmt : block.statements) {
            if (cursor_in_node(stmt, cursor_pos)) {
                return find_stmt_scope(stmt, cursor_pos, scope);
            }
        }
        if (cursor_in_node(block.return_expr, cursor_pos)) {
            return find_stmt_scope(block.return_expr, cursor_pos, scope);
        }
        return scope;
    }
    case ast::NodeType::IfExpr: {
        auto &if_expr = node->data.if_expr;
        if (if_expr.binding_decl && cursor_in_node(if_expr.binding_decl, cursor_pos) &&
            if_expr.then_block && if_expr.then_block->type == ast::NodeType::Block &&
            if_expr.then_block->data.block.scope) {
            return if_expr.then_block->data.block.scope;
        }
        if (cursor_in_node(if_expr.then_block, cursor_pos)) {
            return find_stmt_scope(if_expr.then_block, cursor_pos, fallback);
        }
        if (cursor_in_node(if_expr.else_node, cursor_pos)) {
            return find_stmt_scope(if_expr.else_node, cursor_pos, fallback);
        }
        return fallback;
    }
    case ast::NodeType::CaseExpr:
        return find_stmt_scope(node->data.case_expr.body, cursor_pos, fallback);
    case ast::NodeType::SwitchExpr: {
        for (auto c : node->data.switch_expr.cases) {
            if (cursor_in_node(c, cursor_pos)) {
                return find_stmt_scope(c, cursor_pos, fallback);
            }
        }
        return fallback;
    }
    case ast::NodeType::TryExpr:
        if (cursor_in_node(node->data.try_expr.catch_block, cursor_pos)) {
            return find_stmt_scope(node->data.try_expr.catch_block, cursor_pos, fallback);
        }
        return fallback;
    default:
        return fallback;
    }
}

Analyzer::Analyzer() { m_ctx.flags = FLAG_SAVE_TOKENS; }

ast::Module *Analyzer::process_source(ast::Package *package, io::Buffer *src,
                                      const string &file_name) {
    return m_ctx.process_source(package, src, file_name);
}

ast::Module *Analyzer::process_file(ast::Package *package, const string &file_name) {
    auto src = io::Buffer::from_file(file_name);
    return process_source(package, &src, file_name);
}

ast::Module *Analyzer::format_file(ast::Package *package, const string &file_name) {
    auto src = io::Buffer::from_file(file_name);

    // Create module
    auto module = m_ctx.module_from_path(package, file_name);
    if (!module) {
        return nullptr;
    }

    // Tokenize
    Tokenization tokenization;
    Lexer lexer(&src, &tokenization);
    lexer.tokenize();

    module->comments = std::move(tokenization.comments);

    if (tokenization.error) {
        module->errors.add({*tokenization.error, tokenization.error_pos});
        return module;
    }

    // Parse with basic scope setup but skip semantic analysis
    auto resolver = m_ctx.create_resolver();
    ScopeResolver scope_resolver(&resolver);
    module->scope = scope_resolver.get_scope();
    module->import_scope = m_ctx.create_scope(module->scope);

    ParseContext pc;
    pc.resolver = &scope_resolver;
    pc.module = module;
    pc.allocator = &m_ctx;
    pc.format_mode = true;
    pc.add_token_results(tokenization.tokens);

    // Collect syntax errors only (resolution errors are skipped via format_mode)
    pc.error_handler = [&](const Error &error) { module->errors.add(error); };

    Parser parser(&pc);
    parser.parse();

    return module;
}

ast::Module *Analyzer::format_source(ast::Package *package, io::Buffer *src,
                                     const string &file_name) {
    auto module = m_ctx.module_from_path(package, file_name);
    if (!module) {
        return nullptr;
    }

    Tokenization tokenization;
    Lexer lexer(src, &tokenization);
    lexer.tokenize();

    module->comments = std::move(tokenization.comments);

    if (tokenization.error) {
        module->errors.add({*tokenization.error, tokenization.error_pos});
        return module;
    }

    auto resolver = m_ctx.create_resolver();
    ScopeResolver scope_resolver(&resolver);
    module->scope = scope_resolver.get_scope();
    module->import_scope = m_ctx.create_scope(module->scope);

    ParseContext pc;
    pc.resolver = &scope_resolver;
    pc.module = module;
    pc.allocator = &m_ctx;
    pc.format_mode = true;
    pc.add_token_results(tokenization.tokens);

    pc.error_handler = [&](const Error &error) { module->errors.add(error); };

    Parser parser(&pc);
    parser.parse();

    return module;
}

void Analyzer::build_runtime() {
    auto resolver = m_ctx.create_resolver();
    resolver.context_init_primitives();

    auto rt_file_path = m_ctx.init_rt_stdlib();
    auto rt_source = io::Buffer::from_file(rt_file_path);
    auto module = process_source(m_ctx.rt_package, &rt_source, rt_file_path);
    resolver.context_init_builtins(module);
}

ast::Module *Analyzer::build_runtime_from_source(io::Buffer *src) {
    auto resolver = m_ctx.create_resolver();
    resolver.context_init_primitives();

    auto rt_file_path = m_ctx.init_rt_stdlib();
    auto module = process_source(m_ctx.rt_package, src, rt_file_path);
    resolver.context_init_builtins(module);
    return module;
}

ast::Module *Analyzer::analyze_package_file(ast::Package *package, const string &file_name) {
    build_runtime();
    return process_file(package, file_name);
}

ast::Module *Analyzer::analyze_file(const string &entry_file_name) {
    return analyze_package_file(m_ctx.add_package("."), entry_file_name);
}

ScanResult Analyzer::scan(ast::Module *module, Pos cursor_pos) {
    ScanResult result;
    result.module = module;
    result.pos = cursor_pos;
    if (!module || !module->root) return result;
    auto resolver = m_ctx.create_resolver();
    ScopeResolver scope_resolver(&resolver);

    for (auto decl : module->root->data.root.top_level_decls) {
        if (scan(decl, cursor_pos, &result)) {
            break;
        }
    }

    // find token matching the cursor
    for (auto token : module->tokens) {
        if (token->pos.offset <= cursor_pos.offset &&
            token->pos.offset + (long)token->to_string().size() > cursor_pos.offset) {
            result.token = token;
            break;
        }
    }

    // get info from token
    if (result.token && result.scope) {
        if (result.token->is_identifier()) {
            auto token_node = result.token->node;
            // get associated decl from identifier
            if (token_node) {
                if (token_node->type == ast::NodeType::ConstructExpr) {
                    // Click on construct type name -> jump to new() or struct
                    auto *resolved = token_node->resolved_type;
                    if (resolved) {
                        auto *struct_type = resolver.resolve_struct_type(resolved);
                        if (struct_type) {
                            auto *ctor = struct_type->get_constructor();
                            if (ctor && ctor->node) {
                                result.decl = ctor->node;
                            } else if (struct_type->node) {
                                result.decl = struct_type->node;
                            }
                        }
                    }
                } else if (token_node->type == ast::NodeType::FieldInitExpr) {
                    // Field name in construct expression -> jump to struct member
                    auto &fi = token_node->data.field_init_expr;
                    if (fi.resolved_field && fi.resolved_field->node) {
                        result.decl = fi.resolved_field->node;
                    }
                } else if (token_node->type == ast::NodeType::Identifier) {
                    if (token_node->data.identifier.decl) {
                        result.decl = token_node->data.identifier.decl;
                    }
                    // else: leave decl null so scope fallback can resolve it
                } else {
                    result.decl = token_node;
                }
                result.decl = resolve_scanned_decl(result.decl, result.scope);
            }

            // Check if token is a field name in a construct expression
            if (!result.decl && result.construct_expr) {
                auto &cdata = result.construct_expr->data.construct_expr;
                for (auto fi : cdata.field_inits) {
                    if (fi->data.field_init_expr.field == result.token &&
                        fi->data.field_init_expr.resolved_field &&
                        fi->data.field_init_expr.resolved_field->node) {
                        result.decl = fi->data.field_init_expr.resolved_field->node;
                        break;
                    }
                }
            }

            // resolve from DotExpr first (e.g. math.sqrtf -> sqrtf's FnDef)
            // This must come before scope lookup to avoid resolving to a
            // same-named symbol in an outer scope (e.g. stdio.printf vs builtin printf)
            if (!result.decl && result.dot_expr) {
                auto &dot = result.dot_expr->data.dot_expr;
                if (dot.resolved_decl) {
                    result.decl = dot.resolved_decl;
                } else if (dot.resolved_struct_member && dot.resolved_struct_member->node) {
                    result.decl = dot.resolved_struct_member->node;
                }
            }

            // resort to symbol lookup
            if (!result.decl) {
                auto name = result.token->str;
                auto decl = scope_resolver.find_symbol(name, result.scope);
                if (decl) {
                    result.decl = decl;
                }
            }
            result.decl = resolve_scanned_decl(result.decl, result.scope);
        }
    }

    return result;
}

static ast::Node *find_dot_expr(ast::Node *node, Pos cursor_pos) {
    if (!node)
        return nullptr;
    if (node->type == ast::NodeType::DotExpr && node->token) {
        // the DotExpr's token is the '.' itself; cursor is right after it
        auto dot_offset = node->token->pos.offset;
        if (cursor_pos.offset == dot_offset + 1 ||
            (node->data.dot_expr.field && cursor_pos.offset > dot_offset &&
             cursor_pos.offset <= dot_offset + 1 + node->data.dot_expr.field->to_string().size())) {
            // check children first for nested dots (e.g. a.b.c)
            auto inner = find_dot_expr(node->data.dot_expr.expr, cursor_pos);
            return inner ? inner : node;
        }
        // cursor doesn't match this dot, but may match a nested dot in the sub-expression
        return find_dot_expr(node->data.dot_expr.expr, cursor_pos);
    }

    // recurse into child expressions
    switch (node->type) {
    case ast::NodeType::VarDecl:
        return find_dot_expr(node->data.var_decl.expr, cursor_pos);
    case ast::NodeType::BinOpExpr: {
        auto r = find_dot_expr(node->data.bin_op_expr.op1, cursor_pos);
        return r ? r : find_dot_expr(node->data.bin_op_expr.op2, cursor_pos);
    }
    case ast::NodeType::UnaryOpExpr:
        return find_dot_expr(node->data.unary_op_expr.op1, cursor_pos);
    case ast::NodeType::ReturnStmt:
        return find_dot_expr(node->data.return_stmt.expr, cursor_pos);
    case ast::NodeType::ThrowStmt:
        return find_dot_expr(node->data.throw_stmt.expr, cursor_pos);
    case ast::NodeType::FnCallExpr: {
        auto r = find_dot_expr(node->data.fn_call_expr.fn_ref_expr, cursor_pos);
        if (r)
            return r;
        for (auto arg : node->data.fn_call_expr.args) {
            r = find_dot_expr(arg, cursor_pos);
            if (r)
                return r;
        }
        return nullptr;
    }
    case ast::NodeType::IndexExpr: {
        auto r = find_dot_expr(node->data.index_expr.expr, cursor_pos);
        return r ? r : find_dot_expr(node->data.index_expr.subscript, cursor_pos);
    }
    case ast::NodeType::CastExpr:
        return find_dot_expr(node->data.cast_expr.expr, cursor_pos);
    case ast::NodeType::TryExpr: {
        auto r = find_dot_expr(node->data.try_expr.expr, cursor_pos);
        if (!r && node->data.try_expr.catch_expr)
            r = find_dot_expr(node->data.try_expr.catch_expr, cursor_pos);
        if (!r && node->data.try_expr.catch_block)
            r = find_dot_expr(node->data.try_expr.catch_block, cursor_pos);
        return r;
    }
    case ast::NodeType::AwaitExpr:
        return find_dot_expr(node->data.await_expr.expr, cursor_pos);
    case ast::NodeType::PrefixExpr:
        return find_dot_expr(node->data.prefix_expr.expr, cursor_pos);
    case ast::NodeType::IfExpr: {
        auto r = find_dot_expr(node->data.if_expr.binding_decl, cursor_pos);
        if (r)
            return r;
        r = find_dot_expr(node->data.if_expr.condition, cursor_pos);
        if (r)
            return r;
        r = find_dot_expr(node->data.if_expr.then_block, cursor_pos);
        return r ? r : find_dot_expr(node->data.if_expr.else_node, cursor_pos);
    }
    case ast::NodeType::ConstructExpr: {
        for (auto item : node->data.construct_expr.field_inits) {
            auto r = find_dot_expr(item, cursor_pos);
            if (r)
                return r;
        }
        return nullptr;
    }
    case ast::NodeType::FieldInitExpr:
        return find_dot_expr(node->data.field_init_expr.value, cursor_pos);
    case ast::NodeType::SwitchExpr: {
        auto r = find_dot_expr(node->data.switch_expr.expr, cursor_pos);
        if (r)
            return r;
        for (auto c : node->data.switch_expr.cases) {
            r = find_dot_expr(c, cursor_pos);
            if (r)
                return r;
        }
        return nullptr;
    }
    case ast::NodeType::CaseExpr:
        return find_dot_expr(node->data.case_expr.body, cursor_pos);
    case ast::NodeType::Block: {
        for (auto stmt : node->data.block.statements) {
            auto r = find_dot_expr(stmt, cursor_pos);
            if (r)
                return r;
        }
        return nullptr;
    }
    default:
        return nullptr;
    }
}

// Find the innermost FnCallExpr whose argument list contains the cursor.
// Sets result->fn_call and result->active_param.
static bool find_fn_call(ast::Node *node, Pos cursor_pos, ScanResult *result) {
    if (!node)
        return false;

    if (node->type == ast::NodeType::FnCallExpr) {
        auto &call = node->data.fn_call_expr;
        auto fn_ref = call.fn_ref_expr;
        auto fn_ref_end =
            fn_ref ? (fn_ref->end_token ? fn_ref->end_token : fn_ref->token) : nullptr;
        if (fn_ref_end && node->end_token) {
            auto open_offset = fn_ref_end->pos.offset + (long)fn_ref_end->to_string().size();
            auto close_offset = node->end_token->pos.offset;

            if (cursor_pos.offset > open_offset && cursor_pos.offset <= close_offset) {
                // Check children first for nested calls
                for (auto arg : call.args) {
                    if (find_fn_call(arg, cursor_pos, result))
                        return true;
                }
                // This is the innermost call
                result->fn_call = node;
                result->active_param = 0;
                for (int i = 0; i < call.args.len; i++) {
                    auto arg = call.args[i];
                    auto arg_start = arg->start_token ? arg->start_token : arg->token;
                    if (arg_start && cursor_pos.offset >= arg_start->pos.offset) {
                        result->active_param = i;
                    }
                }
                // Past all args (trailing comma) → next param
                if (call.args.len > 0) {
                    auto last = call.args[call.args.len - 1];
                    auto last_end = last->end_token ? last->end_token : last->token;
                    if (last_end && cursor_pos.offset >
                                        last_end->pos.offset + (long)last_end->to_string().size()) {
                        result->active_param = call.args.len;
                    }
                }
                return true;
            }
        }
    }

    // Recurse into children
    switch (node->type) {
    case ast::NodeType::VarDecl:
        return find_fn_call(node->data.var_decl.expr, cursor_pos, result);
    case ast::NodeType::BinOpExpr:
        return find_fn_call(node->data.bin_op_expr.op1, cursor_pos, result) ||
               find_fn_call(node->data.bin_op_expr.op2, cursor_pos, result);
    case ast::NodeType::UnaryOpExpr:
        return find_fn_call(node->data.unary_op_expr.op1, cursor_pos, result);
    case ast::NodeType::ReturnStmt:
        return find_fn_call(node->data.return_stmt.expr, cursor_pos, result);
    case ast::NodeType::ThrowStmt:
        return find_fn_call(node->data.throw_stmt.expr, cursor_pos, result);
    case ast::NodeType::FnCallExpr: {
        auto &call = node->data.fn_call_expr;
        if (find_fn_call(call.fn_ref_expr, cursor_pos, result))
            return true;
        for (auto arg : call.args) {
            if (find_fn_call(arg, cursor_pos, result))
                return true;
        }
        return false;
    }
    case ast::NodeType::IndexExpr:
        return find_fn_call(node->data.index_expr.expr, cursor_pos, result) ||
               find_fn_call(node->data.index_expr.subscript, cursor_pos, result);
    case ast::NodeType::CastExpr:
        return find_fn_call(node->data.cast_expr.expr, cursor_pos, result);
    case ast::NodeType::DotExpr:
        return find_fn_call(node->data.dot_expr.expr, cursor_pos, result);
    case ast::NodeType::PrefixExpr:
        return find_fn_call(node->data.prefix_expr.expr, cursor_pos, result);
    case ast::NodeType::IfExpr:
        return find_fn_call(node->data.if_expr.binding_decl, cursor_pos, result) ||
               find_fn_call(node->data.if_expr.condition, cursor_pos, result) ||
               find_fn_call(node->data.if_expr.then_block, cursor_pos, result) ||
               find_fn_call(node->data.if_expr.else_node, cursor_pos, result);
    case ast::NodeType::ConstructExpr:
        for (auto item : node->data.construct_expr.field_inits) {
            if (find_fn_call(item, cursor_pos, result))
                return true;
        }
        return false;
    case ast::NodeType::FieldInitExpr:
        return find_fn_call(node->data.field_init_expr.value, cursor_pos, result);
    case ast::NodeType::Block:
        for (auto stmt : node->data.block.statements) {
            if (find_fn_call(stmt, cursor_pos, result))
                return true;
        }
        return false;
    case ast::NodeType::TryExpr:
        return find_fn_call(node->data.try_expr.expr, cursor_pos, result);
    case ast::NodeType::SwitchExpr: {
        if (find_fn_call(node->data.switch_expr.expr, cursor_pos, result))
            return true;
        for (auto c : node->data.switch_expr.cases) {
            if (find_fn_call(c, cursor_pos, result))
                return true;
        }
        return false;
    }
    case ast::NodeType::CaseExpr:
        return find_fn_call(node->data.case_expr.body, cursor_pos, result);
    default:
        return false;
    }
}

static ast::Node *find_construct_expr(ast::Node *node, Pos cursor_pos) {
    if (!node)
        return nullptr;
    switch (node->type) {
    case ast::NodeType::ConstructExpr: {
        // Check if cursor is inside the construct's braces (not on the type name)
        auto end = node->end_token;
        auto *type_node = node->data.construct_expr.type;
        // Cursor must be after the type (i.e., inside the braces)
        long body_start_offset = node->start_token ? node->start_token->pos.offset : -1;
        if (type_node) {
            auto type_end = type_node->end_token ? type_node->end_token : type_node->token;
            if (type_end)
                body_start_offset =
                    type_end->pos.offset + (long)type_end->to_string().size();
        }
        if (body_start_offset >= 0 && end &&
            cursor_pos.offset > body_start_offset &&
            cursor_pos.offset <= end->pos.offset) {
            // Check inside field init values first (nested constructs)
            for (auto item : node->data.construct_expr.field_inits) {
                auto r = find_construct_expr(item->data.field_init_expr.value, cursor_pos);
                if (r)
                    return r;
            }
            for (auto item : node->data.construct_expr.items) {
                auto r = find_construct_expr(item, cursor_pos);
                if (r)
                    return r;
            }
            return node;
        }
        return nullptr;
    }
    case ast::NodeType::VarDecl:
        return find_construct_expr(node->data.var_decl.expr, cursor_pos);
    case ast::NodeType::BinOpExpr: {
        auto r = find_construct_expr(node->data.bin_op_expr.op1, cursor_pos);
        return r ? r : find_construct_expr(node->data.bin_op_expr.op2, cursor_pos);
    }
    case ast::NodeType::ReturnStmt:
        return find_construct_expr(node->data.return_stmt.expr, cursor_pos);
    case ast::NodeType::FnCallExpr: {
        for (auto arg : node->data.fn_call_expr.args) {
            auto r = find_construct_expr(arg, cursor_pos);
            if (r)
                return r;
        }
        return nullptr;
    }
    default:
        return nullptr;
    }
}

bool Analyzer::scan(ast::Node *node, Pos cursor_pos, ScanResult *result) {
    if (!node)
        return false;
    switch (node->type) {
    case ast::NodeType::FnDef: {
        auto &fn = node->data.fn_def;
        if (fn.body && fn.body->type == ast::NodeType::Block) {
            auto &block = fn.body->data.block;
            auto start = fn.body->start_token;
            auto end = fn.body->end_token;

            if (start && end && cursor_pos.is_in_range(start->pos, end->pos)) {
                result->fn = node;
                result->block = fn.body;
                result->scope = block.scope;

                // scan statements
                auto check_stmt = [&](ast::Node *stmt) -> bool {
                    if (!stmt)
                        return false;
                    if (stmt->start_token && stmt->end_token) {
                        auto start_pos = stmt->start_token->pos;
                        auto end_pos = stmt->end_token->pos;

                        if (cursor_pos.is_in_range(start_pos, end_pos) ||
                            cursor_pos.add_offset(-1).is_in_range(start_pos, end_pos)) {
                            auto dot = find_dot_expr(stmt, cursor_pos);
                            if (dot) {
                                result->dot_expr = dot;
                                result->is_dot = true;
                                return true;
                            }
                            find_fn_call(stmt, cursor_pos, result);
                            auto ce = find_construct_expr(stmt, cursor_pos);
                            if (ce)
                                result->construct_expr = ce;
                            result->stmt = stmt;
                            result->scope = find_stmt_scope(stmt, cursor_pos, result->scope);
                            return true;
                        }
                    }
                    // statement missing tokens — still try to find a DotExpr
                    auto dot = find_dot_expr(stmt, cursor_pos);
                    if (dot) {
                        result->dot_expr = dot;
                        result->is_dot = true;
                        result->scope = find_stmt_scope(stmt, cursor_pos, result->scope);
                        return true;
                    }
                    return false;
                };

                for (auto stmt : block.statements) {
                    if (check_stmt(stmt))
                        return true;
                }
                // also check return_expr (incomplete expressions land here)
                if (check_stmt(block.return_expr))
                    return true;
            }
        }
        break;
    }
    case ast::NodeType::StructDecl: {
        auto &struct_decl = node->data.struct_decl;
        for (auto member : struct_decl.members) {
            if (member->type == ast::NodeType::FnDef) {
                if (scan(member, cursor_pos, result)) {
                    return true;
                }
            } else if (member->type == ast::NodeType::ImplementBlock) {
                for (auto impl_member : member->data.implement_block.members) {
                    if (impl_member->type == ast::NodeType::FnDef) {
                        if (scan(impl_member, cursor_pos, result)) {
                            return true;
                        }
                    }
                }
            }
        }
        break;
    }
    case ast::NodeType::EnumDecl: {
        auto &enum_decl = node->data.enum_decl;
        if (enum_decl.base_struct) {
            if (scan(enum_decl.base_struct, cursor_pos, result)) {
                return true;
            }
        }
        break;
    }
    default:
        break;
    }
    return false;
}
