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

static const jit_nuint ARRAY_DATA_FIELD_OFFSET = 0;
static const auto ARRAY_SIZE_FIELD_OFFSET = jit_type_get_size(jit_type_nint);

static jit_type_t realloc_params[] = {jit_type_nint, jit_type_nuint};
static auto realloc_signature = jit_type_create_signature(jit_abi_cdecl, jit_type_nint, realloc_params, 2, 1);

void* sys_realloc(void* dest, size_t size) {
    return realloc(dest, size);
}

static jit_type_t free_params[] = {jit_type_nint};
static auto free_signature = jit_type_create_signature(jit_abi_cdecl, jit_type_void, free_params, 1, 1);

void sys_printf(const char* format, int value) {
    print(format, value);
}

Function::Function(jit_type_t signature, CompileContext* _ctx, ast::Node* _node) : jit_function(
        _ctx->jit_ctx, signature), ctx(_ctx), node(_node) {
    set_recompilable();
    if (node) {
        set_qualified_name(node->data.fn_def.container, node->name);
    }
    is_returning = this->new_value(jit_type_sys_bool);
}

void Function::build() {
    Compiler compiler(ctx);
    compiler.compile_fn_body(this);
}

void Function::set_qualified_name(ast::Node* container, const string& name) {
    if (container) {
        qualified_name = container->name + "::" + name;
    } else {
        qualified_name = name;
    }
}

jit_value Function::insn_call(Function* fn_ref, jit_value_t* args, long num_args) {
    return jit_function::insn_call(fn_ref->get_jit_name(), fn_ref->raw(), fn_ref->signature(), args, (uint32_t)num_args);
}

Compiler::Compiler(CompileContext* ctx) {
    m_ctx = ctx;
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
    auto fn = new_fn(build_jit_type(node), node);
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
            if (fn.container) {
                params.add(jit_type_create_pointer(to_jit_type(fn.container), 1));
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
            for (auto& field: struct_.fields) {
                fields.add(to_jit_type(field->type));
            }
            return jit_type_create_struct(fields.items, uint32_t(fields.size), 1);
        }
        case TypeId::String: {
            return string_type;
        }
        case TypeId::Pointer: {
            return jit_type_create_pointer(to_jit_type(type->data.pointer.elem), 1);
        }
        case TypeId::Array: {
            return to_jit_type(type->data.array.internal);
        }
        default:
            panic("unhandled");
            return {};
    }
}

inline jit_type_t Compiler::build_jit_type(cx::ast::Node* node) {
    return to_jit_type(node_get_type(node));
}

void Compiler::compile_fn_body(Function* fn) {
    static auto printf_signature = build_jit_type(m_ctx->resolver.get_builtin("printf"));
    if (!fn->node) {
        return;
    }
    auto& fn_def = fn->node->data.fn_def;
    if (fn_def.builtin_id != ast::BuiltinId::Invalid) {
        if (fn_def.builtin_id == ast::BuiltinId::Printf) {
            jit_value_t args[2];
            args[0] = fn->get_param(0).raw();
            args[1] = fn->get_param(1).raw();
            fn->insn_call_native("printf", (void*) sys_printf, printf_signature, args, 2);
        }
    } else {
        auto& proto = fn_def.fn_proto->data.fn_proto;
        int skip = fn_def.is_instance_method() ? 1 : 0;
        for (uint32_t i = 0; i < proto.params.size; i++) {
            add_value(proto.params[i], fn->get_param(uint32_t(i + skip)));
        }
        auto fn_type = node_get_type(fn->node);
        auto& fn_end = fn->push_return_scope()->emplace_back();
        auto s2 = fn->return_labels.size();
        auto size = fn->return_labels.back().size();
        fn->return_value = fn->new_value(to_jit_type(fn_type->data.fn.return_type));
        compile_block(fn, fn->node, fn_def.body);
        fn->insn_label(fn_end.label);
        fn->pop_return_scope();
        fn->insn_return(fn->return_value);
    }
    if (m_ctx->settings.enable_asm_print) {
        jit_dump_function(stdout, fn->raw(), fn->get_jit_name());
    }
}

void Compiler::compile_stmt(Function* fn, ast::Node* stmt) {
    switch (stmt->type) {
        case ast::NodeType::VarDecl: {
            auto& data = stmt->data.var_decl;
            auto var = fn->new_value(build_jit_type(stmt));
            add_value(stmt, var);
            if (data.expr) {
                fn->store(var, compile_simple_value(fn, data.expr));
            }
            break;
        }
        case ast::NodeType::ReturnStmt: {
            auto& data = stmt->data.return_stmt;
            fn->store(fn->is_returning, fn->new_constant(jit_int(1)));
            fn->store(fn->return_value, compile_simple_value(fn, data.expr));
            fn->insn_branch(*fn->get_return_label());
            break;
        }
        case ast::NodeType::IfStmt: {
            auto& data = stmt->data.if_stmt;
            auto cond = compile_simple_value(fn, data.condition);
            jit_label if_end;
            fn->insn_branch_if_not(cond, if_end);
            compile_block(fn, stmt, data.then_block);
            fn->insn_label(if_end);
            break;
        }
        default:
            compile_simple_value(fn, stmt);
    }
}

jit_value Compiler::compile_simple_value(Function *fn, ast::Node *expr) {
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
                auto& dot_expr = fn_expr->data.dot_expr;
                auto method_node = dot_expr.resolved_member->node;
                auto builtin_id = method_node->data.fn_def.builtin_id;
                if (builtin_id == ast::BuiltinId::ArrayAdd) {
                    return compile_array_add(fn, dot_expr.expr, data.args[0]);
                }
                auto dot = compile_dot_expr(fn, fn_expr);
                args.add(dot.container.raw());
                fn_ref = get_fn(method_node);
            } else {
                panic("unhandled");
            }
            assert(fn_ref);
            for (auto& arg: data.args) {
                auto value = compile_simple_value(fn, arg);
                args.add(value.raw());
            }
            return fn->insn_call(fn_ref, args.items, args.size);
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
        case ast::NodeType::ParenExpr: {
            auto& child = expr->data.child_expr;
            return compile_simple_value(fn, child);
        }
        case ast::NodeType::BinOpExpr: {
            auto& data = expr->data.bin_op_expr;
            auto op2 = compile_simple_value(fn, data.op2);
            if (data.op_type == TokenType::ASS) {
                auto lhs_type = data.op1->type;
                if (lhs_type == ast::NodeType::DotExpr || lhs_type == ast::NodeType::IndexExpr) {
                    auto ref = compile_value_ref(fn, data.op1);
                    fn->insn_store_relative(ref.address, 0, op2);
                    return op2;
                } else {
                    auto op1 = compile_simple_value(fn, data.op1);
                    fn->store(op1, op2);
                    return op2;
                }
            }
            auto op1 = compile_simple_value(fn, data.op1);
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
            auto ctn_type = node_get_type(expr);
            auto temp = fn->new_value(to_jit_type(ctn_type));
            auto this_ = fn->insn_address_of(temp).raw();
            compile_construction(fn, this_, ctn_type, expr);
            return temp;
        }
        case ast::NodeType::DotExpr:
        case ast::NodeType::IndexExpr: {
            auto ref = compile_value_ref(fn, expr);
            return fn->insn_load_relative(ref.address, 0, ref.type);
        }
        default:
            panic("unhandled {}", PRINT_ENUM(expr->type));
            break;
    }
    return fn->new_constant(jit_int(0));
}

ValueRef Compiler::compile_value_ref(Function *fn, ast::Node *expr) {
    switch (expr->type) {
    case ast::NodeType::DotExpr: {
        auto dot = compile_dot_expr(fn, expr);
        auto address = fn->insn_add_relative(dot.container, dot.field->offset);
        return {address, dot.field->type};
    }
    case ast::NodeType::IndexExpr: {
        auto& data = expr->data.index_expr;
        auto arr = compile_array_ref(fn, data.expr);
        auto index = compile_simple_value(fn, data.subscript);
        auto address = fn->insn_load_elem_address(arr.data, index, arr.elem_type);
        return {address, arr.elem_type};
    }
    case ast::NodeType::Identifier: {
        auto value = compile_simple_value(fn, expr);
        return {fn->insn_address_of(value), build_jit_type(expr)};
    }
    default:
        unreachable();
    }
}

jit_value Compiler::create_string_const(Function* fn, const string& str) {
    return jit_value_create_nint_constant(fn->raw(), string_type, jit_nint(str.c_str()));
}

void Compiler::compile_block(Function* fn, ast::Node* parent, ast::Node* block) {
    assert(block->type == ast::NodeType::Block);
    array<ast::Node*> vars; // vars to destroy
    switch (parent->type) {
        case ast::NodeType::FnDef: {
            auto& fn_proto = parent->data.fn_def.fn_proto;
            for (auto param: fn_proto->data.fn_proto.params) {
                if (should_destroy(param)) {
                    vars.add(param);
                }
            }
            break;
        }
        default:
            break;
    }

    auto ret_scope = fn->push_return_scope();
    ret_scope->emplace_back();
    for (auto stmt: block->data.block.statements) {
        if (stmt->type == ast::NodeType::VarDecl) {
            if (should_destroy(stmt)) {
                vars.add(stmt);
                ret_scope->emplace_back().var = stmt;
            }
        }
        compile_stmt(fn, stmt);
    }
    // call destructors
    fn->store(fn->is_returning, fn->new_constant(jit_int(0)));
    if (vars.size) {
        for (long i = vars.size - 1; i >= 0; i--) {
            auto var = vars[i];
            if (var == ret_scope->back().var) {
                fn->insn_label(ret_scope->back().label);
                ret_scope->pop_back();
            }
            auto address = fn->insn_address_of(m_ctx->values[var]);
            compile_var_destroy(fn, var, address);
        }
    }
    fn->insn_label(ret_scope->back().label);
    ret_scope->pop_back();
    assert(ret_scope->empty());
    fn->pop_return_scope();
    fn->insn_branch_if(fn->is_returning, *fn->get_return_label());
}

void Compiler::compile_struct(ast::Node* node) {
    auto type = node_get_type(node);
    auto struct_type = type->data.type_name.giving_type;
    auto& struct_ = struct_type->data.struct_;
    auto struct_jit = to_jit_type(struct_type);
    auto this_ = jit_type_create_pointer(struct_jit, 1);
    auto& dflt = m_ctx->defaults[node];

    // default constructor
    auto cons_signature = jit_type_create_signature
            (jit_abi_cdecl, jit_type_void, &this_, uint32_t(1), 1);
    auto constructor = new_fn(cons_signature, nullptr);
    dflt.constructor.reset(constructor);
    constructor->set_qualified_name(node, "_new");

    auto& data = node->data.struct_decl;
    auto cons_this = constructor->get_param(0);
    for (auto member: data.members) {
        if (member->type == ast::NodeType::FnDef) {
            compile_fn(member);
        } else if (member->type == ast::NodeType::VarDecl) {
            auto& var = member->data.var_decl;
            if (var.expr) {
                auto field = var.resolved_field;
                assert(field);
                auto offset = jit_type_get_offset(struct_jit, (uint32_t)field->index);
                constructor->insn_store_relative(cons_this, offset, compile_simple_value(constructor, var.expr));
            }
        }
    }

    // default destructor
    auto des_signature = jit_type_create_signature
            (jit_abi_cdecl, jit_type_void, &this_, uint32_t(1), 1);
    auto destructor = new_fn(des_signature, nullptr);
    dflt.destructor.reset(destructor);
    destructor->set_qualified_name(node, "_delete");

    auto des_this = destructor->get_param(0);
    for (long i=data.members.size-1; i>=0; i--) {
        auto member = data.members[i];
        if (member->type == ast::NodeType::VarDecl) {
            if (should_destroy(member)) {
                auto field = member->data.var_decl.resolved_field;
                auto offset = jit_type_get_offset(struct_jit, (uint32_t) field->index);
                auto address = destructor->insn_add_relative(des_this.raw(), offset);
                compile_var_destroy(destructor, member, address);
            }
        }
    }

    if (m_ctx->settings.enable_asm_print) {
        jit_dump_function(stdout, constructor->raw(), constructor->get_jit_name());
        jit_dump_function(stdout, destructor->raw(), destructor->get_jit_name());
    }
}

DotValue Compiler::compile_dot_expr(Function* fn, ast::Node* expr) {
    auto& data = expr->data.dot_expr;
    auto member_type = node_get_type(expr);
    auto ctn_type = node_get_type(data.expr);
    jit_value container;
    if (ctn_type->id == TypeId::Pointer) {
        container = compile_simple_value(fn, data.expr);
        ctn_type = ctn_type->data.pointer.elem;
    } else {
        container = compile_value_ref(fn, data.expr).address;
    }
    DotValue dot = {container};
    auto member = data.resolved_member;
    assert(member);
    if (member->field) {
        auto offset = jit_type_get_offset(to_jit_type(ctn_type), (uint32_t) member->field->index);
        dot.field.emplace();
        dot.field->offset = offset;
        dot.field->type = to_jit_type(member_type);
    }
    return dot;
}

Function* Compiler::get_fn(ast::Node* node) {
    auto fn = m_ctx->functions.get(node);
    return fn ? fn->get() : nullptr;
}

void Compiler::compile_construction(Function* fn, jit_value_t dest, ChiType* struct_type, ast::Node* expr) {
    if (struct_type->id == TypeId::Array) {
        auto zero = fn->new_constant(jit_int(0));
        fn->insn_store_relative(dest, 0, zero);
        fn->insn_store_relative(dest, ARRAY_SIZE_FIELD_OFFSET, zero);
        return;
    }
    auto& struct_ = struct_type->data.struct_;
    auto& dflt = m_ctx->defaults[struct_.node];
    fn->insn_call(dflt.constructor.get(), &dest, 1);
    auto cons_member = struct_.find_member("new");
    if (cons_member) {
        auto constructor = get_fn(cons_member->node);
        assert(constructor);
        array<jit_value_t> args;
        args.add(dest);
        if (expr) {
            assert(expr->type == ast::NodeType::ComplitExpr);
            auto& data = expr->data.complit_expr;
            for (auto arg: data.items) {
                auto value = compile_simple_value(fn, arg);
                args.add(value.raw());
            }
        }
        fn->insn_call(constructor, args.items, args.size);
    }
}

Array Compiler::compile_array_ref(Function *fn, ast::Node *expr) {
    auto array_type = node_get_type(expr);
    auto array_ = compile_simple_value(fn, expr);
    auto this_ = fn->insn_address_of(array_);
    Array result;
    result.ptr = this_;
    result.data = fn->insn_load_relative(this_, ARRAY_DATA_FIELD_OFFSET, jit_type_nint);
    result.size = fn->insn_load_relative(this_, ARRAY_SIZE_FIELD_OFFSET, jit_type_nint);
    result.elem_type = to_jit_type(array_type->data.array.elem);
    result.elem_size = jit_type_get_size(result.elem_type);
    return result;
}

jit_value Compiler::compile_array_add(Function *fn, ast::Node *expr, ast::Node* value_arg) {
    auto arr = compile_array_ref(fn, expr);
    auto elem_size = fn->new_constant(arr.elem_size);
    auto inc = fn->new_constant(jit_int(1));
    auto old_size = arr.size;
    auto this_ = arr.ptr;
    auto new_size = fn->insn_add(old_size, inc);
    fn->insn_store_relative(this_, ARRAY_SIZE_FIELD_OFFSET, new_size);
    auto mem_size = fn->insn_mul(new_size, elem_size);
    jit_value_t ra_args[] = {arr.data.raw(), mem_size.raw()};
    auto new_data = fn->insn_call_native("realloc", (void*)sys_realloc, realloc_signature, ra_args, 2);
    fn->insn_store_relative(this_, ARRAY_DATA_FIELD_OFFSET, new_data);
    auto address = fn->insn_load_elem_address(new_data, old_size, arr.elem_type);
    auto value = compile_simple_value(fn, value_arg);
    fn->insn_store_relative(address, 0, value);
    return address;
}

void Compiler::compile_array_destroy(Function *fn, jit_value &arr) {
    auto data = fn->insn_load_relative(arr, ARRAY_DATA_FIELD_OFFSET, jit_type_nint);
    jit_value_t free_args[] = {data.raw()};
    fn->insn_call_native("free", (void *) free, free_signature, free_args, 1);
}

void Compiler::compile_var_destroy(Function *fn, ast::Node *var, jit_value& address) {
    auto type = node_get_type(var);
    switch (type->id) {
        case TypeId::Array:
            compile_array_destroy(fn, address);
            break;
        case TypeId::Struct: {
            auto& struct_ = type->data.struct_;
            auto& dflt = m_ctx->defaults[struct_.node];
            jit_value_t args[] = {address.raw()};
            fn->insn_call(dflt.destructor.get(), args, 1);
            if (auto destructor = struct_.find_member("delete")) {
                auto des_fn = get_fn(destructor->node);
                fn->insn_call(des_fn, args, 1);
            }
            break;
        }
        default:
            break;
    }
}

Function *Compiler::new_fn(jit_type_t signature, ast::Node* node) {
    return new Function(signature, get_context(), node);
}

bool Compiler::should_destroy(ast::Node *node) {
    switch (node_get_type(node)->id) {
        case TypeId::Array:
        case TypeId::Struct:
            return true;
        default:
            return false;
    }
}
