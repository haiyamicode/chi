/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "jit.h"

using namespace cx;
using namespace cx::jit;

static auto string_type = jit_type_create_pointer(jit_type_sys_char, 1);

Function::Function(jit_context& context, jit_type_t signature, Compiler* compiler, ast::Node* node) : jit_function(
        context, signature) {
    this->compiler = compiler;
    this->node = node;
    set_recompilable();
}

void Function::build() {
    compiler->compile_fn_body(this);
}

Compiler::Compiler(CompileContext* compile_ctx, ResolveContext* resolve_ctx) : m_resolver(resolve_ctx) {
    m_ctx = compile_ctx;
}

void Compiler::compile(ast::Module* module) {
    auto& root = module->root->data.root;
    for (auto decl: root.top_level_decls) {
        if (decl->type == ast::NodeType::FnDef) {
            compile_fn(decl);
        }
    }
}

void Compiler::compile_fn(ast::Node* fn) {
    auto& data = fn->data.fn_def;
    m_ctx->functions.emplace(fn, new Function(get_jit_context(), build_jit_type(fn), this, fn));
}

jit_type_t Compiler::to_jit_type(ChiType* type) {
    switch (type->id) {
        case TypeId::Bool:
            return jit_type_sbyte;
        case TypeId::Int:
            return jit_type_int;
        case TypeId::Void:
            return jit_type_void;
        case TypeId::Fn: {
            array<jit_type_t> params;
            auto& fn = type->data.fn;
            for (auto param: fn.params) {
                params.add(to_jit_type(param));
            }
            auto return_type = to_jit_type(fn.return_type);
            return jit_type_create_signature
                    (jit_abi_cdecl, return_type, params.items, uint32_t(params.size), 1);
        }
        case TypeId::String: {
            return string_type;
        }
        default:
            panic("unhandled");
            return {};
    }
}

inline jit_type_t Compiler::build_jit_type(cx::ast::Node* node) {
    return to_jit_type(get_node_type(node));
}

void sys_printf(const char* format, int value) {
    print(format, value);
}

void Compiler::compile_fn_body(jit::Function* fn) {
    static auto printf_signature = build_jit_type(m_resolver.get_builtin("printf"));
    auto& fn_def = fn->node->data.fn_def;
    if (fn_def.is_builtin) {
        if (fn->node->name == "printf") {
            jit_value_t args[2];
            args[0] = fn->get_param(0).raw();
            args[1] = fn->get_param(1).raw();
            fn->insn_call_native(fn->node->name.c_str(), (void*) sys_printf, printf_signature, args, 2);
        }
    } else {
        auto& proto = fn_def.fn_proto->data.fn_proto;
        for (uint32_t i = 0; i < proto.params.size; i++) {
            add_value(proto.params[i], fn->get_param(uint32_t(i)));
        }
        compile_block(fn, fn_def.body);
    }
}

void Compiler::compile_stmt(jit::Function* fn, ast::Node* stmt) {
    switch (stmt->type) {
        case ast::NodeType::VarDecl: {
            auto& data = stmt->data.var_decl;
            auto var = fn->new_value(build_jit_type(stmt));
            add_value(stmt, var);
            if (data.expr) {
                fn->store(var, compile_expr_value(fn, data.expr));
            }
            break;
        }
        case ast::NodeType::ReturnStmt: {
            auto& data = stmt->data.return_stmt;
            fn->insn_return(compile_expr_value(fn, data.expr));
            break;
        }
        case ast::NodeType::IfStmt: {
            auto& data = stmt->data.if_stmt;
            auto cond = compile_expr_value(fn, data.condition);
            jit_label if_end;
            fn->insn_branch_if_not(cond, if_end);
            compile_block(fn, data.then_block);
            fn->insn_label(if_end);
            break;
        }
        default:
            compile_expr_value(fn, stmt);
    }
}

jit::Function* Compiler::compile_expr_fn_ref(jit::Function* fn, ast::Node* expr) {
    if (expr->type == ast::NodeType::Identifier) {
        auto& data = expr->data.identifier;
        auto type = get_node_type(expr);
        if (type->id == TypeId::Fn) {
            return m_ctx->functions[data.decl].get();
        } else {
            panic("unhandled");
        }
    }
    unreachable();
    return nullptr;
}

jit_value Compiler::compile_expr_value(jit::Function* fn, ast::Node* expr) {
    switch (expr->type) {
        case ast::NodeType::FnCallExpr: {
            auto& data = expr->data.fn_call_expr;
            auto fn_ref = compile_expr_fn_ref(fn, data.fn_ref_expr);
            array<jit_value_t> args;
            for (auto& arg: data.args) {
                auto value = compile_expr_value(fn, arg);
                args.add(value.raw());
            }
            return fn->insn_call(fn_ref->node->name.c_str(), fn_ref->raw(), fn_ref->signature(), args.items,
                                 uint32_t(args.size));
        }
        case ast::NodeType::Identifier: {
            auto& data = expr->data.identifier;
            return m_ctx->values[data.decl];
        }
        case ast::NodeType::LiteralExpr: {
            auto token = expr->token;
            switch (token->type) {
                case TokenType::INT:
                    return fn->new_constant(jit_int(token->val.i));
                case TokenType::STRING: {
                    return create_string_const(fn, token->str);
                }
                default:
                    panic("unhandled");
            }
        }
        case ast::NodeType::ParenExpr: {
            auto& child = expr->data.child_expr;
            return compile_expr_value(fn, child);
        }
        case ast::NodeType::BinOpExpr: {
            auto& data = expr->data.bin_op_expr;
            auto op1 = compile_expr_value(fn, data.op1);
            auto op2 = compile_expr_value(fn, data.op2);
            switch (data.op_type) {
                case TokenType::ASS:
                    fn->store(op1, op2);
                    return op1;
                case TokenType::LT:
                    return fn->insn_lt(op1, op2);
                case TokenType::ADD:
                    return fn->insn_add(op1, op2);
                case TokenType::MUL:
                    return fn->insn_mul(op1, op2);
                case TokenType::SUB:
                    return fn->insn_sub(op1, op2);
                default:
                    panic("unhandled");
            }
        }
        default:
            unreachable();
            return fn->new_constant(jit_int(0));
    }
}

jit_value Compiler::create_string_const(jit::Function* fn, const string& str) {
    return jit_value_create_nint_constant(fn->raw(), string_type, jit_nint(str.c_str()));
}

void Compiler::compile_block(jit::Function* fn, ast::Node* node) {
    assert(node->type == ast::NodeType::Block);
    for (auto stmt: node->data.block.statements) {
        compile_stmt(fn, stmt);
    }
}
