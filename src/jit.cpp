/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include <jit/jit-dump.h>

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
        } else if (decl->type == ast::NodeType::StructDecl) {
            compile_struct(decl);
        }
    }
}

void Compiler::compile_fn(ast::Node* node) {
    auto fn = new Function(get_jit_context(), build_jit_type(node), this, node);
    m_ctx->functions.emplace(node, fn);
}

jit_type_t Compiler::to_jit_type(ChiType* type) {
    auto cached = m_ctx->types.get(type);
    if (cached) {
        return *cached;
    }
    auto result = _to_jit_type(type);
    m_ctx->types[type] = result;
    return result;
}

jit_type_t Compiler::_to_jit_type(ChiType* type) {
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
            if (fn.struct_) {
                params.add(jit_type_create_pointer(to_jit_type(fn.struct_), 1));
            }
            for (auto param: fn.params) {
                params.add(to_jit_type(param));
            }
            auto return_type = to_jit_type(fn.return_type);
            return jit_type_create_signature
                    (jit_abi_cdecl, return_type, params.items, uint32_t(params.size), 1);
        }
        case TypeId::Struct: {
            auto& struct_ = type->data.struct_;
            array<jit_type_t> fields;
            for (auto field: struct_.fields) {
                fields.add(to_jit_type(field.type));
            }
            return jit_type_create_struct(fields.items, uint32_t(fields.size), 1);
        }
        case TypeId::String: {
            return string_type;
        }
        case TypeId::Pointer: {
            return jit_type_create_pointer(to_jit_type(type->data.pointer.base), 1);
        }
        default:
            panic("unhandled");
            return {};
    }
}

inline jit_type_t Compiler::build_jit_type(cx::ast::Node* node) {
    return to_jit_type(node_get_type(node));
}

void sys_printf(const char* format, int value) {
    print(format, value);
}

void Compiler::compile_fn_body(jit::Function* fn) {
    static auto printf_signature = build_jit_type(m_resolver.get_builtin("printf"));
    if (!fn->node) {
        return;
    }
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
        int skip = fn_def.is_instance_method() ? 1 : 0;
        for (uint32_t i = 0; i < proto.params.size; i++) {
            add_value(proto.params[i], fn->get_param(uint32_t(i + skip)));
        }
        compile_block(fn, fn_def.body);
    }
    if (m_ctx->settings.enable_asm_print) {
        jit_dump_function(stdout, fn->raw(), fn->node->name.c_str());
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

jit_value Compiler::compile_expr_value(jit::Function* fn, ast::Node* expr) {
    switch (expr->type) {
        case ast::NodeType::FnCallExpr: {
            auto& data = expr->data.fn_call_expr;
            auto fn_expr = data.fn_ref_expr;
            array<jit_value_t> args;
            Function* fn_ref = nullptr;
            if (fn_expr->type == ast::NodeType::Identifier) {
                auto& iden = fn_expr->data.identifier;
                fn_ref = get_fn(iden.decl);
            } else if (fn_expr->type == ast::NodeType::DotExpr) {
                auto dot_expr = fn_expr->data.dot_expr;
                auto method = node_get_type(fn_expr)->meta.struct_member;
                auto this_ = fn->insn_address_of(compile_expr_value(fn, dot_expr.expr));
                args.add(this_.raw());
                fn_ref = get_fn(method->node);
            } else {
                panic("unhandled");
            }
            assert(fn_ref);
            for (auto& arg: data.args) {
                auto value = compile_expr_value(fn, arg);
                args.add(value.raw());
            }
            return fn->insn_call(fn_ref->node->name.c_str(), fn_ref->raw(), fn_ref->signature(), args.items,
                                 uint32_t(args.size));
        }
        case ast::NodeType::Identifier: {
            auto& data = expr->data.identifier;
            if (data.kind == ast::IdentifierKind::This) {
                return fn->get_param(0);
            }
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
            break;
        }
        case ast::NodeType::DotExpr: {
            auto dot = compile_dot_expr(fn, expr);
            return fn->insn_load_relative(dot.struct_ptr, dot.offset, dot.value_type);
        }
        case ast::NodeType::ParenExpr: {
            auto& child = expr->data.child_expr;
            return compile_expr_value(fn, child);
        }
        case ast::NodeType::BinOpExpr: {
            auto& data = expr->data.bin_op_expr;
            auto op2 = compile_expr_value(fn, data.op2);
            if (data.op_type == TokenType::ASS) {
                if (data.op1->type == ast::NodeType::DotExpr) {
                    auto dot = compile_dot_expr(fn, data.op1);
                    fn->insn_store_relative(dot.struct_ptr, dot.offset, op2);
                    return op2;
                } else {
                    auto op1 = compile_expr_value(fn, data.op1);
                    fn->store(op1, op2);
                    return op2;
                }
            }
            auto op1 = compile_expr_value(fn, data.op1);
            switch (data.op_type) {
                case TokenType::LT:
                    return fn->insn_lt(op1, op2);
                case TokenType::LE:
                    return fn->insn_le(op1, op2);
                case TokenType::EQ:
                    return fn->insn_eq(op1, op2);
                case TokenType::GT:
                    return fn->insn_gt(op1, op2);
                case TokenType::GE:
                    return fn->insn_ge(op1, op2);
                case TokenType::ADD:
                    return fn->insn_add(op1, op2);
                case TokenType::MUL:
                    return fn->insn_mul(op1, op2);
                case TokenType::SUB:
                    return fn->insn_sub(op1, op2);
                default:
                    panic("unhandled");
            }
            break;
        }
        case ast::NodeType::ComplitExpr: {
            auto& data = expr->data.complit_expr;
            auto struct_type = node_get_type(expr);
            auto& struct_ = struct_type->data.struct_;
            auto temp = fn->new_value(to_jit_type(struct_type));
            auto this_ = fn->insn_address_of(temp).raw();
            if (auto default_cons = get_fn(struct_.node)) {
                fn->insn_call("_new", default_cons->raw(), default_cons->signature(), &this_, 1, 1);
            }
            auto cons_member = struct_.members_table.get("new");
            if (cons_member) {
                auto cons_fn = get_fn(cons_member->node);
                assert(cons_fn);
                array<jit_value_t> args;
                args.add(this_);
                for (auto arg: data.items) {
                    auto value = compile_expr_value(fn, arg);
                    args.add(value.raw());
                }
                fn->insn_call("new", cons_fn->raw(), cons_fn->signature(), args.items,
                              (uint32_t) args.size);
            }
            return temp;
        }
        default:
            break;
    }
    unreachable();
    return fn->new_constant(jit_int(0));
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

void Compiler::compile_struct(ast::Node* node) {
    auto type = node_get_type(node);
    auto struct_type = type->data.type_name.giving_type;
    auto& struct_ = struct_type->data.struct_;
    auto struct_jit = to_jit_type(struct_type);
    auto this_ = jit_type_create_pointer(struct_jit, 1);
    auto cons_type = jit_type_create_signature
            (jit_abi_cdecl, jit_type_void, &this_, uint32_t(1), 1);
    auto cons_fn = new Function(get_jit_context(), cons_type, this, nullptr);
    m_ctx->functions.emplace(node, cons_fn);
    auto& data = node->data.struct_decl;
    auto cons_this = cons_fn->get_param(0);
    for (auto member: data.members) {
        if (member->type == ast::NodeType::FnDef) {
            compile_fn(member);
        } else {
            auto& var = member->data.var_decl;
            auto m = struct_.members_table.get(member->name);
            assert(m->field);
            if (var.expr) {
                auto offset = jit_type_get_offset(struct_jit, (uint32_t) m->field->index);
                cons_fn->insn_store_relative(cons_this, offset, compile_expr_value(cons_fn, var.expr));
            }
        }
    }
    if (m_ctx->settings.enable_asm_print) {
        jit_dump_function(stdout, cons_fn->raw(), "_new");
    }
}

DotField Compiler::compile_dot_expr(jit::Function* fn, ast::Node* node) {
    auto& data = node->data.dot_expr;
    auto member_type = node_get_type(node);
    auto ctn_type = node_get_type(data.expr);
    auto ctn_jit = to_jit_type(ctn_type);
    auto ctn_value = compile_expr_value(fn, data.expr);
    auto ctn_ptr = ctn_type->id == TypeId::Pointer ? ctn_value : fn->insn_address_of(ctn_value);
    auto member = member_type->meta.struct_member;
    if (member->field) {
        DotField result;
        auto offset = jit_type_get_offset(ctn_jit, (uint32_t) member->field->index);
        result.value_type = to_jit_type(member_type);
        result.offset = offset;
        result.field = member->field;
        result.struct_ptr = ctn_ptr;
        return result;
    } else {
        panic("unhandled");
    }
    return {};
}

jit::Function* Compiler::get_fn(ast::Node* node) {
    auto fn = m_ctx->functions.get(node);
    return fn ? fn->get() : nullptr;
}
