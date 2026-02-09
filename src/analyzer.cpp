/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "analyzer.h"
#include "util.h"

using namespace cx;

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
            token->pos.offset + token->to_string().size() > cursor_pos.offset) {
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
                if (token_node->type == ast::NodeType::Identifier &&
                    token_node->data.identifier.decl) {
                    result.decl = token_node->data.identifier.decl;
                } else {
                    result.decl = token_node;
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
        }
    }

    return result;
}

static ast::Node *find_dot_expr(ast::Node *node, Pos cursor_pos) {
    if (!node) return nullptr;
    if (node->type == ast::NodeType::DotExpr && node->token) {
        // the DotExpr's token is the '.' itself; cursor is right after it
        auto dot_offset = node->token->pos.offset;
        if (cursor_pos.offset == dot_offset + 1 ||
            (node->data.dot_expr.field &&
             cursor_pos.offset > dot_offset &&
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
    case ast::NodeType::FnCallExpr: {
        auto r = find_dot_expr(node->data.fn_call_expr.fn_ref_expr, cursor_pos);
        if (r) return r;
        for (auto arg : node->data.fn_call_expr.args) {
            r = find_dot_expr(arg, cursor_pos);
            if (r) return r;
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
        return r ? r : find_dot_expr(node->data.try_expr.catch_expr, cursor_pos);
    }
    case ast::NodeType::AwaitExpr:
        return find_dot_expr(node->data.await_expr.expr, cursor_pos);
    case ast::NodeType::PrefixExpr:
        return find_dot_expr(node->data.prefix_expr.expr, cursor_pos);
    case ast::NodeType::IfStmt: {
        auto r = find_dot_expr(node->data.if_stmt.condition, cursor_pos);
        if (r) return r;
        r = find_dot_expr(node->data.if_stmt.then_block, cursor_pos);
        return r ? r : find_dot_expr(node->data.if_stmt.else_node, cursor_pos);
    }
    case ast::NodeType::ConstructExpr: {
        for (auto item : node->data.construct_expr.field_inits) {
            auto r = find_dot_expr(item, cursor_pos);
            if (r) return r;
        }
        return nullptr;
    }
    case ast::NodeType::FieldInitExpr:
        return find_dot_expr(node->data.field_init_expr.value, cursor_pos);
    case ast::NodeType::SwitchExpr: {
        auto r = find_dot_expr(node->data.switch_expr.expr, cursor_pos);
        if (r) return r;
        for (auto c : node->data.switch_expr.cases) {
            r = find_dot_expr(c, cursor_pos);
            if (r) return r;
        }
        return nullptr;
    }
    case ast::NodeType::CaseExpr:
        return find_dot_expr(node->data.case_expr.body, cursor_pos);
    case ast::NodeType::Block: {
        for (auto stmt : node->data.block.statements) {
            auto r = find_dot_expr(stmt, cursor_pos);
            if (r) return r;
        }
        return nullptr;
    }
    default:
        return nullptr;
    }
}

bool Analyzer::scan(ast::Node *node, Pos cursor_pos, ScanResult *result) {
    if (!node) return false;
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
                    if (!stmt) return false;
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
                            result->stmt = stmt;
                            return true;
                        }
                    }
                    // statement missing tokens — still try to find a DotExpr
                    auto dot = find_dot_expr(stmt, cursor_pos);
                    if (dot) {
                        result->dot_expr = dot;
                        result->is_dot = true;
                        return true;
                    }
                    return false;
                };

                for (auto stmt : block.statements) {
                    if (check_stmt(stmt)) return true;
                }
                // also check return_expr (incomplete expressions land here)
                if (check_stmt(block.return_expr)) return true;
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
            }
        }
        break;
    }
    default:
        break;
    }
    return false;
}