/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include <jit/jit-dump.h>

#include "jit.h"
#include "internals.h"

using namespace cx;
using namespace cx::jit;

static auto PTR_SIZE = jit_type_get_size(jit_type_nint);

static const jit_nuint ARRAY_DATA_FIELD_OFFSET = 0;
static const auto ARRAY_SIZE_FIELD_OFFSET = PTR_SIZE;
const auto TRAIT_TYPE_FIELD_OFFSET = 0;
const auto TRAIT_DATA_FIELD_OFFSET = PTR_SIZE;
const auto STRING_DATA_FIELD_OFFSET = 0;
const auto STRING_SIZE_FIELD_OFFSET = PTR_SIZE;
const auto IMPL_VTABLE_FIELD_OFFSET = PTR_SIZE;
static const string OPTIONAL_UNWRAP_PANIC = "panic: unwrapping null optional value\n";

static auto str_lit_type = jit_type_create_pointer(jit_type_sys_char, 1);

static jit_type_t realloc_params[] = {jit_type_nint, jit_type_nuint};
static auto realloc_signature = jit_type_create_signature(jit_abi_cdecl, jit_type_nint, realloc_params, 2, 1);

void* sys_realloc(void* dest, size_t size) {
    return realloc(dest, size);
}

static jit_type_t malloc_params[] = {jit_type_nuint};
static auto malloc_signature = jit_type_create_signature(jit_abi_cdecl, jit_type_nint, malloc_params, 1, 1);

void* sys_malloc(size_t size) {
    return malloc(size);
}

static jit_type_t free_params[] = {jit_type_nint};
static auto free_signature = jit_type_create_signature(jit_abi_cdecl, jit_type_void, free_params, 1, 1);

static jit_type_t trait_internal_fields[] = {jit_type_nint, jit_type_nint};
static auto trait_internal_struct = jit_type_create_struct(trait_internal_fields, 2, 1);

static jit_type_t any_internal_fields[] = {jit_type_nint, jit_type_nint, jit_type_nint};
static auto any_internal_struct = jit_type_create_struct(any_internal_fields, 3, 1);

static jit_type_t array_internal_fields[] = {jit_type_nint, jit_type_uint, jit_type_uint};
static auto array_internal_struct = jit_type_create_struct(array_internal_fields, 3, 1);

static jit_type_t string_internal_fields[] = {jit_type_nint, jit_type_uint};
static auto string_internal_struct = jit_type_create_struct(string_internal_fields, 2, 1);

static jit_type_t string_set_data_params[] = {jit_type_nint, jit_type_nint};
static auto string_set_data_signature = jit_type_create_signature(jit_abi_cdecl, jit_type_nint, string_set_data_params,
                                                                  2, 1);

static jit_type_t string_concat_params[] = {jit_type_nint, string_internal_struct, string_internal_struct};
static auto string_concat_signature = jit_type_create_signature(jit_abi_cdecl,
                                                                jit_type_void, string_concat_params, 3, 1);

static jit_type_t printf_params[] = {string_internal_struct, array_internal_struct};
static auto printf_signature = jit_type_create_signature(jit_abi_cdecl,
                                                         jit_type_void, printf_params, 2, 1);

Function::Function(jit_type_t signature, CompileContext* _ctx, ast::Node* _node) : jit_function(
        _ctx->jit_ctx, signature), ctx(_ctx), node(_node) {
    if (node) {
        qualified_name = node->name;
    }
    if (_ctx->is_aot_enabled()) {
        asm_name = "_" + qualified_name;
    } else {
        set_recompilable();
    }
    is_returning = this->new_value(jit_type_sys_bool);
}

void Function::build() {
    Compiler compiler(ctx, this);
    compiler.compile_fn_body(this);
}

int64_t Function::add_fn_symbol(const string& name) {
    ctx->fn_symbols.add(name);
    return (int64_t) ctx->fn_symbols.size;
}

jit_value Function::insn_call(Function* fn_ref, jit_value_t* args, long num_args) {
    if (ctx->jit_ctx.is_aot_enabled()) {
        auto fid = add_fn_symbol(fn_ref->get_asm_name());
        return jit_function::insn_call_native(fn_ref->get_jit_name(), (void*) (-fid), fn_ref->signature(), args,
                                              (uint32_t) num_args);
    }
    return jit_function::insn_call(fn_ref->get_jit_name(), fn_ref->raw(), fn_ref->signature(), args,
                                   (uint32_t) num_args);
}

jit_value Function::insn_call_native(const char* name, void* native_func, jit_type_t signature,
                                     jit_value_t* args, unsigned int num_args, int flags) {
    if (ctx->jit_ctx.is_aot_enabled()) {
        return jit_function::insn_call_native(name, (void*) (-add_fn_symbol(name)),
                                              signature, args, num_args, flags);
    }
    return jit_function::insn_call_native(name, native_func, signature, args, num_args, flags);
}

jit_value Function::get_null_constant() {
    return new_constant(jit_nint(NULL));
}

Function* CompileContext::add_fn(ast::Node* node, Function* fn) {
    functions.emplace(fn)->get();
    function_table[node] = fn;
    return fn;
}

Compiler::Compiler(CompileContext* ctx, Function* fn) {
    m_ctx = ctx;
    m_fn = fn;
}

void Compiler::compile_module(ast::Module* module) {
    auto& root = module->root->data.root;
    for (auto decl: root.top_level_decls) {
        if (decl->type == ast::NodeType::FnDef) {
            compile_fn(decl);
        } else if (decl->type == ast::NodeType::StructDecl) {
            compile_struct(decl);
        }
    }
}

Function* Compiler::compile_fn(ast::Node* node) {
    if (auto fn = get_fn(node)) {
        return fn;
    }
    auto type = compile_type_of(node);
    return add_fn(type, node);
}

jit_type_t Compiler::to_jit_int_type(ChiType* type) {
    auto& data = type->data.int_;
    if (data.is_unsigned) {
        switch (data.bit_count) {
            case 8:
                return jit_type_ubyte;
            case 16:
                return jit_type_ushort;
            case 32:
                return jit_type_uint;
            case 64:
                return jit_type_ulong;
            default:
                break;
        }
    } else {
        switch (data.bit_count) {
            case 8:
                return jit_type_sbyte;
            case 16:
                return jit_type_short;
            case 32:
                return jit_type_int;
            case 64:
                return jit_type_long;
            default:
                break;
        }
    }
    return jit_type_int;
}

jit_type_t Compiler::compile_type(ChiType* type) {
    type = eval_type(type);
    auto cached = m_ctx->type_table.get(type->id);
    if (cached) {
        return *cached;
    }
    auto result = _compile_type(type);
    m_ctx->type_table[type->id] = result;
    return result;
}

jit_type_t Compiler::_compile_type(ChiType* type) {
    switch (type->kind) {
        case TypeKind::Bool:
            return jit_type_sbyte;
        case TypeKind::Int: {
            return to_jit_int_type(type);
        }
        case TypeKind::Void:
            return jit_type_void;
        case TypeKind::Fn: {
            array<jit_type_t> params;
            auto& fn = type->data.fn;
            if (fn.container) {
                params.add(jit_type_create_pointer(compile_type(fn.container), 1));
            }
            for (size_t i = 0; i < fn.params.size; i++) {
                auto param_type = fn.params[i];
                if (fn.is_variadic && i == fn.params.size - 1) {
                    param_type = get_resolver()->get_array_type(param_type);
                }
                params.add(compile_type(param_type));
            }
            auto return_type = compile_type(fn.return_type);
            return jit_type_create_signature
                    (jit_abi_cdecl, return_type, params.items, uint32_t(params.size), 1);
        }
        case TypeKind::Struct: {
            auto& struct_ = type->data.struct_;
            if (struct_.kind == ContainerKind::Trait) {
                return trait_internal_struct;
            }
            array<jit_type_t> fields;
            for (auto& field: struct_.fields) {
                fields.add(compile_type(field->resolved_type));
            }
            if (struct_.kind == ContainerKind::Union) {
                jit_type_create_union(fields.items, uint32_t(fields.size), 1);
            }
            return jit_type_create_struct(fields.items, uint32_t(fields.size), 1);
        }
        case TypeKind::Subtype: {
            return compile_type(eval_type(type));
        }
        case TypeKind::String: {
            return string_internal_struct;
        }
        case TypeKind::Pointer: {
            return jit_type_nint;
        }
        case TypeKind::Array: {
            return compile_type(type->data.array.internal);
        }
        case TypeKind::Any: {
            return any_internal_struct;
        }
        case TypeKind::Optional: {
            jit_type_t fields[] = {compile_type(type->get_elem()), jit_type_sbyte};
            return jit_type_create_struct(fields, 2, 1);
        }
        case TypeKind::Box: {
            static jit_type_t fields[] = {jit_type_nint};
            return jit_type_create_struct(fields, 1, 1);
        }
        default:
            panic("unhandled {}", PRINT_ENUM(type->kind));
            return {};
    }
}

inline jit_type_t Compiler::compile_type_of(cx::ast::Node* node) {
    return compile_type(get_chitype(node));
}

void Compiler::compile_fn_body(Function* fn) {
    if (!fn->node) {
        return;
    }
    auto& fn_def = fn->node->data.fn_def;
    if (fn_def.is_built_in()) {
        switch (fn_def.builtin_id) {
            case ast::BuiltinId::Printf: {
                jit_value_t args[] = {fn->get_param(0).raw(), fn->get_param(1).raw()};
                fn->insn_call_native("_cx_printf", (void*) cx_printf, printf_signature, args, 2);
                break;
            }
            case ast::BuiltinId::Debug: {
                static jit_type_t debug_params[] = {string_internal_struct};
                static auto debug_signature = jit_type_create_signature(jit_abi_cdecl,
                                                                        jit_type_void, debug_params, 1, 1);
                jit_value_t args[] = {fn->get_param(0).raw()};
                fn->insn_call_native("_cx_debug", (void*) cx_debug, debug_signature, args, 1);
                break;
            }
            case ast::BuiltinId::ArrayAdd: {
                auto array_type = get_chitype(fn->node)->data.fn.container;
                auto array_ = compile_wrapped_type(array_type);
                auto ret = compile_array_add(fn, fn->get_param(0), (jit_uint) array_.elem_size, fn->get_param(1));
                fn->insn_return(ret);
                break;
            }
            default:
                break;
        }
    } else {
        auto& proto = fn_def.fn_proto->data.fn_proto;
        int skip = fn_def.is_instance_method() ? 1 : 0;
        for (uint32_t i = 0; i < proto.params.size; i++) {
            add_value(proto.params[i], fn->get_param(uint32_t(i + skip)));
        }
        auto fn_type = get_chitype(fn->node);
        auto& fn_end = fn->push_return_scope()->emplace_back();
        fn->return_value = fn->new_value(compile_type(fn_type->data.fn.return_type));
        if (fn_def.body) {
            compile_block(fn, fn->node, fn_def.body);
        }
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
            auto var = fn->new_value(compile_type_of(stmt));
            add_value(stmt, var);
            if (data.expr) {
                fn->store(var, compile_assignment_value(fn, data.expr, stmt));
            }
            break;
        }
        case ast::NodeType::ReturnStmt: {
            auto& data = stmt->data.return_stmt;
            fn->store(fn->is_returning, fn->new_constant(1));
            if (data.expr) {
                fn->store(fn->return_value, compile_assignment_value(fn, data.expr, stmt));
            }
            fn->insn_branch(*fn->get_return_label());
            break;
        }
        case ast::NodeType::IfStmt: {
            auto& data = stmt->data.if_stmt;
            auto cond = compile_assignment_to_type(fn, data.condition, get_system_types()->bool_);
            jit_label else_block, if_end;
            fn->insn_branch_if_not(cond, else_block);
            compile_block(fn, stmt, data.then_block);
            fn->insn_branch(if_end);
            fn->insn_label(else_block);
            if (data.else_node) {
                if (data.else_node->type == ast::NodeType::Block) {
                    compile_block(fn, stmt, data.else_node);
                } else {
                    compile_stmt(fn, data.else_node);
                }
            }
            fn->insn_label(if_end);
            break;
        }
        case ast::NodeType::ForStmt: {
            auto& data = stmt->data.for_stmt;
            auto loop = fn->push_loop();
            if (data.init) {
                compile_stmt(fn, data.init);
            }
            fn->insn_label(loop->start);
            if (data.condition) {
                auto cond = compile_simple_value(fn, data.condition);
                fn->insn_branch_if_not(cond, loop->end);
            }
            compile_block(fn, stmt, data.body);
            if (data.post) {
                compile_stmt(fn, data.post);
            }
            fn->insn_branch(loop->start);
            fn->insn_label(loop->end);
            fn->pop_loop();
            break;
        }
        case ast::NodeType::BranchStmt: {
            auto loop = fn->get_loop();
            switch (stmt->token->type) {
                case TokenType::KW_BREAK:
                    fn->insn_branch(loop->end);
                    break;
                case TokenType::KW_CONTINUE:
                    fn->insn_branch(loop->start);
                    break;
                default:
                    unreachable();
            }
            break;
        }
        default:
            compile_simple_value(fn, stmt);
    }
}

jit_value Compiler::compile_simple_value(Function* fn, ast::Node* expr) {
    switch (expr->type) {
        case ast::NodeType::FnCallExpr: {
            auto& data = expr->data.fn_call_expr;
            auto fn_expr = data.fn_ref_expr;
            ast::Node* fn_ref = nullptr;
            array<jit_value_t> args;
            optional<jit_value> fn_ref_vtable;
            optional<jit_value> va_list_ref;
            if (fn_expr->type == ast::NodeType::Identifier) {
                auto& iden = fn_expr->data.identifier;
                fn_ref = iden.decl;
            } else if (fn_expr->type == ast::NodeType::DotExpr) {
                auto& dot_expr = fn_expr->data.dot_expr;
                auto dot = compile_dot_expr(fn, fn_expr);
                assert(dot.method);
                fn_ref = *dot.method;
                args.add(dot.ctn_address.raw());
                if (dot.vtable_fn) {
                    fn_ref_vtable = *dot.vtable_fn;
                }
            } else {
                panic("unhandled");
            }
            assert(fn_ref);
            auto fn_type = get_chitype(fn_ref);
            auto& fn_spec = fn_type->data.fn;
            auto param_last = fn_spec.params.size - (int) fn_spec.is_variadic;
            for (size_t i = 0; i < param_last; i++) {
                auto value = compile_assignment_to_type(fn, data.args[i], fn_spec.get_param_at(i));
                args.add(value.raw());
            }
            if (fn_spec.is_variadic) {
                auto elem_type = fn_spec.params.last();
                auto va_type = get_resolver()->get_array_type(elem_type);
                auto va_tmp = fn->new_value(compile_type(va_type));
                auto va_addr = fn->insn_address_of(va_tmp);
                compile_array_construction(fn, va_addr);
                auto elem_size = jit_type_get_size(compile_type(elem_type));
                for (size_t i = param_last; i < data.args.size; i++) {
                    auto value = compile_assignment_to_type(fn, data.args[i], elem_type);
                    compile_array_add(fn, va_addr, (jit_uint) elem_size, value);
                }
                args.add(va_tmp.raw());
                va_list_ref = va_addr;
            }
            if (fn_ref_vtable) {
                return fn->insn_call_indirect_vtable(*fn_ref_vtable, compile_type_of(fn_ref),
                                                     args.items, uint32_t(args.size));
            }
            auto fn_instance = get_fn(fn_ref);
            assert(fn_instance);
            auto value = fn->insn_call(fn_instance, args.items, args.size);
            if (va_list_ref) {
                compile_field_mem_free(fn, *va_list_ref, 0);
            }
            return value;
        }
        case ast::NodeType::Identifier: {
            auto& data = expr->data.identifier;
            if (data.kind == ast::IdentifierKind::This) {
                return fn->get_param(0);
            }
            if (data.decl->type == ast::NodeType::VarDecl) {
                auto& var = data.decl->data.var_decl;
                if (var.is_const) {
                    return compile_constant_value(fn, var.resolved_value, get_chitype(data.decl));
                }
            }
            auto& val = m_ctx->value_table[data.decl];
            if (expr->orig_type && expr->orig_type->kind == TypeKind::Optional) {
                auto addr = fn->insn_address_of(val);
                return fn->insn_load_relative(addr, 0, compile_type(expr->resolved_type));
            }
            return val;
        }
        case ast::NodeType::LiteralExpr: {
            auto token = expr->token;
            switch (token->type) {
                case TokenType::BOOL:
                    return fn->new_constant(jit_sbyte(token->val.b));
                case TokenType::INT:
                    return fn->new_constant(jit_int(token->val.i));
                case TokenType::STRING: {
                    return compile_string_alloc(fn, create_string_constant(fn, token->str));
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
            auto op2 = compile_assignment_value(fn, data.op2, data.op1);
            auto value_type = get_chitype(expr);
            if (is_assignment_op(data.op_type)) {
                auto ref = compile_value_ref(fn, data.op1);
                if (should_destroy(data.op1)) {
                    compile_destruction(fn, ref.address, data.op1);
                }
                if (data.op_type != TokenType::ASS) {
                    auto op = get_assignment_op(data.op_type);
                    auto op1 = fn->insn_load_relative(ref.address, 0, compile_type(value_type));
                    op2 = compile_arithmetic_op(fn, value_type, op, op1, op2);
                }
                fn->insn_store_relative(ref.address, 0, op2);
                return op2;
            }
            auto op1 = compile_simple_value(fn, data.op1);
            return compile_arithmetic_op(fn, value_type, data.op_type, op1, op2);
        }
        case ast::NodeType::UnaryOpExpr: {
            auto& data = expr->data.unary_op_expr;
            switch (data.op_type) {
                case TokenType::AND: {
                    auto temp = fn->new_value(jit_type_nint);
                    fn->store(temp, compile_value_ref(fn, data.op1).address);
                    return temp;
                }
                case TokenType::INC:
                case TokenType::DEC: {
                    auto ref = compile_value_ref(fn, data.op1);
                    auto value = fn->insn_load_relative(ref.address, 0, ref.type);
                    auto diff = data.op_type == TokenType::INC ? 1 : -1;
                    auto new_value = fn->insn_add(value, fn->new_constant(diff));
                    fn->insn_store_relative(ref.address, 0, new_value);
                    if (data.is_suffix) {
                        return value;
                    }
                    return new_value;
                }
                case TokenType::LNOT: {
                    if (data.is_suffix) {
                        auto ref = compile_value_ref(fn, data.op1);
                        auto opt = compile_wrapped_type(get_chitype(data.op1));
                        auto flag = fn->insn_load_relative(ref.address, opt.get_opt_flag_offset(), jit_type_sbyte);
                        jit_label if_ok;
                        fn->insn_branch_if(flag, if_ok);
                        compile_panic(fn, OPTIONAL_UNWRAP_PANIC);
                        fn->insn_label(if_ok);
                        return fn->insn_load_relative(ref.address, 0, opt.elem);
                    } else {
                        return fn->insn_to_not_bool(
                                compile_assignment_to_type(fn, data.op1, get_system_types()->bool_));
                    }
                }
                default:
                    break;
            }
            auto value = compile_simple_value(fn, data.op1);
            switch (data.op_type) {
                case TokenType::SUB:
                    return fn->insn_neg(value);
                case TokenType::ADD:
                    return value;
                case TokenType::NOT:
                    return fn->insn_not(value);
                case TokenType::MUL:
                    return fn->insn_load_relative(value, 0, compile_type_of(expr));
                default:
                    unreachable();
            }
            break;
        }
        case ast::NodeType::ConstructExpr: {
            auto& data = expr->data.construct_expr;
            auto ctn_type = get_chitype(expr);
            jit_value this_;
            jit_value value;
            if (data.is_new) {
                assert(ctn_type->is_raw_pointer());
                ctn_type = ctn_type->get_elem();
                jit_nuint size_value = jit_type_get_size(compile_type(ctn_type));
                this_ = compile_mem_alloc(fn, fn->new_constant(size_value));
                value = this_;
            } else {
                value = fn->new_value(compile_type(ctn_type));
                this_ = fn->insn_address_of(value);
            }
            compile_construction(fn, this_.raw(), ctn_type, expr);
            return value;
        }
        case ast::NodeType::DotExpr: {
            auto& data = expr->data.dot_expr;
            auto ctn_type = get_chitype(data.expr);
            if (ctn_type->kind == TypeKind::TypeSymbol) {
                auto enum_node = data.resolved_member->node;
                auto value = enum_node->data.enum_member.resolved_value;
                return fn->new_constant((jit_int) value);
            }
            // fallthrough
        }
        case ast::NodeType::IndexExpr: {
            auto ref = compile_value_ref(fn, expr);
            return fn->insn_load_relative(ref.address, 0, ref.type);
        }
        case ast::NodeType::CastExpr: {
            auto& data = expr->data.cast_expr;
            auto value = compile_simple_value(fn, data.expr);
            auto from_type = get_chitype(data.expr);
            auto to_type = get_chitype(expr);
            return compile_conversion(fn, value, from_type, to_type);
        }
        default:
            panic("unhandled {}", PRINT_ENUM(expr->type));
            break;
    }
    return jit_value();
}

jit_value Compiler::compile_assignment_value(Function* fn, ast::Node* expr, ast::Node* dest) {
    return compile_assignment_to_type(fn, expr, get_chitype(dest));
}

jit_value Compiler::compile_assignment_to_type(Function* fn, ast::Node* expr, ChiType* dest_type) {
    auto value = compile_simple_value(fn, expr);
    auto src_type = get_chitype(expr);
    if (expr->type == ast::NodeType::ConstructExpr && src_type == dest_type) {
        return value;
    }
    return compile_conversion(fn, value, src_type, dest_type);
}

jit_value Compiler::compile_conversion(Function* fn, const jit_value& value,
                                       ChiType* from_type, ChiType* to_type) {
    switch (to_type->kind) {
        case TypeKind::Struct: {
            if (ChiTypeStruct::is_trait(to_type)) {
                assert(from_type->is_raw_pointer());
                auto& struct_ = from_type->get_elem()->data.struct_;
                auto impl = get_impl_info(struct_.trait_table[to_type]);
                auto temp = fn->new_value(trait_internal_struct);
                auto addr = fn->insn_address_of(temp);
                fn->insn_store_relative(addr, TRAIT_TYPE_FIELD_OFFSET, fn->new_constant(jit_nint((void*) impl)));
                fn->insn_store_relative(addr, TRAIT_DATA_FIELD_OFFSET, value);
                return temp;
            }
            break;
        }
        case TypeKind::String: {
            auto src_data = value;
            if (from_type->kind == TypeKind::String) {
                auto addr = fn->insn_address_of(value);
                src_data = fn->insn_load_relative(addr, STRING_DATA_FIELD_OFFSET, jit_type_nint);
            }
            return compile_string_alloc(fn, src_data);
        }
        case TypeKind::Any: {
            if (from_type->kind != TypeKind::Any) {
                auto temp = fn->new_value(any_internal_struct);
                auto addr = fn->insn_address_of(temp);
                auto typ = fn->new_constant((jit_nint) get_type_info(from_type));
                fn->insn_store_relative(addr, TRAIT_TYPE_FIELD_OFFSET, typ);
                auto val = value;
                if (get_resolver()->type_is_int(from_type)) {
                    val = fn->insn_convert(value, jit_type_nint);
                }
                fn->insn_store_relative(addr, TRAIT_DATA_FIELD_OFFSET, val);
                return temp;
            }
            return value;
        }
        case TypeKind::Optional: {
            auto val = value;
            jit_value flag;
            bool skip_check = false;
            auto elem_type = to_type->get_elem();
            auto opt = compile_wrapped_type(to_type);
            if (from_type == to_type) {
                auto addr = fn->insn_address_of(value);
                val = fn->insn_load_relative(addr, 0, opt.elem);
                flag = fn->insn_load_relative(addr, opt.get_opt_flag_offset(), jit_type_sbyte);
            } else {
                flag = fn->new_constant(jit_sbyte(1));
                skip_check = true;
            }
            auto temp = fn->new_value(opt.type);
            auto addr = fn->insn_address_of(temp);
            fn->insn_store_relative(addr, opt.get_opt_flag_offset(), flag);
            jit_label skip_data;
            if (!skip_check) {
                fn->insn_branch_if_not(flag, skip_data);
            }
            auto data = compile_value_copy(fn, val, elem_type);
            fn->insn_store_relative(addr, 0, data);
            fn->insn_label(skip_data);
            return temp;
        }
        case TypeKind::Box: {
            assert(from_type->kind == TypeKind::Box);
            auto box = compile_wrapped_type(to_type);
            auto elem_type = to_type->get_elem();
            auto addr = fn->insn_address_of(value);
            auto data = fn->insn_load_relative(addr, 0, jit_type_nint);
            auto val = fn->insn_load_relative(data, 0, box.elem);
            val = compile_value_copy(fn, val, elem_type);
            auto temp = fn->new_value(box.type);
            auto dest = fn->insn_address_of(temp);
            auto ptr = compile_mem_alloc(fn, fn->new_constant(box.elem_size));
            fn->insn_store_relative(dest, 0, ptr);
            fn->insn_store_relative(ptr, 0, val);
            return temp;
        }
        case TypeKind::Array: {
            assert(from_type->kind == TypeKind::Array);
            auto addr = fn->insn_address_of(value);
            return compile_array_loop(fn, addr, to_type, ArrayOp::Copy);
        }
        case TypeKind::Bool: {
            if (from_type->kind == TypeKind::Bool) {
                return value;
            } else if (from_type->kind == TypeKind::Optional) {
                auto addr = fn->insn_address_of(value);
                auto opt = compile_wrapped_type(from_type);
                return fn->insn_load_relative(addr, opt.get_opt_flag_offset(), jit_type_sbyte);
            }
            return fn->insn_to_bool(value);
        }
        default:
            break;
    }
    if (from_type->kind != to_type->kind || to_type->kind == TypeKind::Int) {
        return fn->insn_convert(value, compile_type(to_type), 0);
    }
    return value;
}

jit_value Compiler::compile_value_copy(Function* fn, const jit_value& value, ChiType* type) {
    return compile_conversion(fn, value, type, type);
}

ValueRef Compiler::compile_value_ref(Function* fn, ast::Node* expr) {
    switch (expr->type) {
        case ast::NodeType::DotExpr: {
            auto dot = compile_dot_expr(fn, expr);
            auto address = fn->insn_add_relative(dot.ctn_address, dot.field->offset);
            return {address, dot.field->type};
        }
        case ast::NodeType::IndexExpr: {
            auto& data = expr->data.index_expr;
            auto arr = compile_array_ref(fn, data.expr);
            auto index = compile_simple_value(fn, data.subscript);
            auto address = fn->insn_load_elem_address(arr.data, index, arr.elem);
            return {address, arr.elem};
        }
        case ast::NodeType::Identifier: {
            auto value = compile_simple_value(fn, expr);
            return {fn->insn_address_of(value), compile_type_of(expr)};
        }
        case ast::NodeType::ParenExpr: {
            return compile_value_ref(fn, expr->data.child_expr);
        }
        case ast::NodeType::UnaryOpExpr: {
            auto& data = expr->data.unary_op_expr;
            switch (data.op_type) {
                case TokenType::MUL:
                    return {compile_simple_value(fn, data.op1), compile_type_of(expr)};
                case TokenType::LNOT: {
                    auto ref = compile_value_ref(fn, data.op1);
                    auto elem_type = get_chitype(data.op1)->get_elem();
                    return {ref.address, compile_type(elem_type)};
                }
                default:
                    break;
            }
        }
        default:
            unreachable();
            return {};
    }
}

jit_value Compiler::create_string_constant(Function* fn, const string& str) {
    if (is_aot_enabled()) {
        m_ctx->string_literals.add(str.c_str());
    }
    return jit_value_create_nint_constant(fn->raw(), str_lit_type, (jit_nint) str.c_str());
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
            auto address = fn->insn_address_of(m_ctx->value_table[var]);
            compile_destruction(fn, address, var);
        }
    }
    fn->insn_label(ret_scope->back().label);
    ret_scope->pop_back();
    assert(ret_scope->empty());
    fn->pop_return_scope();
    fn->insn_branch_if(fn->is_returning, *fn->get_return_label());
}

void Compiler::compile_struct(ast::Node* node) {
    auto struct_type = get_resolver()->to_value_type(get_chitype(node));
    if (m_ctx->struct_table.get(struct_type)) {
        return;
    }
    if (struct_type->kind != TypeKind::Struct) {
        return;
    }
    if (!ChiTypeStruct::is_generic(struct_type)) {
        return _compile_struct(node, struct_type);
    }
    get_struct_data(struct_type);
    auto& struct_ = struct_type->data.struct_;
    for (auto subtype: struct_.subtypes) {
        if (subtype->is_placeholder) {
            continue;
        }
        _compile_struct(node, subtype);
    }
}

void Compiler::_compile_struct(ast::Node* node, ChiType* struct_type) {
    ChiTypeSubtype* subtype = nullptr;
    if (struct_type->kind == TypeKind::Subtype) {
        subtype = &struct_type->data.subtype;
        struct_type = subtype->resolved_struct;
    }
    auto& struct_ = struct_type->data.struct_;
    auto struct_jt = compile_type(struct_type);
    auto this_ = jit_type_create_pointer(struct_jt, 1);
    auto sdata = get_struct_data(struct_type);

    // default constructor
    auto cons_signature = jit_type_create_signature
            (jit_abi_cdecl, jit_type_void, &this_, 1, 1);
    auto constructor = add_fn(cons_signature, nullptr);
    init_method_fn(constructor, "_new", struct_type, subtype);
    sdata->constructor.reset(constructor);

    auto cons_this = constructor->get_param(0);
    for (auto& member: struct_.members) {
        auto node_type = member->node->type;
        if (node_type == ast::NodeType::FnDef) {
            auto method = compile_fn(member->node);
            init_method_fn(method, member->get_name(), struct_type, subtype);
        } else if (node_type == ast::NodeType::VarDecl) {
            auto& var = member->node->data.var_decl;
            auto var_type = member->resolved_type;
            // compile dependencies
            if (var_type->kind == TypeKind::Struct) {
                auto struct_decl = var_type->data.struct_.node;
                if (struct_decl && !m_ctx->struct_table.get(var_type)) {
                    compile_struct(struct_decl);
                }
            }
            // compile assignment
            if (var.expr) {
                auto field = var.resolved_field;
                auto offset = jit_type_get_offset(struct_jt, (uint32_t) field->field_index);
                auto value = compile_assignment_value(constructor, var.expr, member->node);
                constructor->insn_store_relative(cons_this, offset, value);
            }
        }
    }

    // default destructor
    auto des_signature = jit_type_create_signature
            (jit_abi_cdecl, jit_type_void, &this_, 1, 1);
    auto destructor = add_fn(des_signature, nullptr);
    init_method_fn(destructor, "_delete", struct_type, subtype);
    sdata->destructor.reset(destructor);

    auto des_this = destructor->get_param(0);
    for (long i = struct_.members.size - 1; i >= 0; i--) {
        auto& member = struct_.members[i];
        if (member->node->type == ast::NodeType::VarDecl) {
            if (should_destroy(member->node)) {
                auto offset = jit_type_get_offset(struct_jt, (uint32_t) member->field_index);
                auto address = destructor->insn_add_relative(des_this.raw(), offset);
                compile_destruction(destructor, address, member->node);
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
    auto member_type = get_chitype(expr);
    DotValue dot;
    dot.ctn_type = get_chitype(data.expr);
    if (dot.ctn_type->is_pointer()) {
        dot.ctn_address = compile_simple_value(fn, data.expr);
        dot.ctn_type = dot.ctn_type->get_elem();
    } else {
        dot.ctn_address = compile_value_ref(fn, data.expr).address;
    }
    dot.ctn_type = get_struct(dot.ctn_type).type;
    auto member = data.resolved_member;
    assert(member);
    if (member->is_field()) {
        jit_nuint offset;
        if (dot.ctn_type->data.struct_.kind == ContainerKind::Union) {
            offset = 0;
        } else {
            auto index = (uint32_t) member->field_index;
            offset = jit_type_get_offset(compile_type(dot.ctn_type), index);
        }
        dot.field.emplace();
        dot.field->offset = offset;
        dot.field->type = compile_type(member_type);
    } else {
        assert(member->node->type == ast::NodeType::FnDef);
        dot.method = member->node;
        if (ChiTypeStruct::is_trait(dot.ctn_type)) {
            auto trait_ref = dot.ctn_address;
            dot.ctn_address = fn->insn_load_relative(trait_ref, TRAIT_DATA_FIELD_OFFSET, jit_type_nint);
            auto impl_info = fn->insn_load_relative(trait_ref, TRAIT_TYPE_FIELD_OFFSET, jit_type_nint);
            auto method_index = fn->new_constant(jit_int(member->method_index));
            auto vtable = fn->insn_load_relative(impl_info, IMPL_VTABLE_FIELD_OFFSET, jit_type_nint);
            dot.vtable_fn = fn->insn_load_elem(vtable, method_index.raw(), jit_type_nint);
        }
    }
    return dot;
}

Function* Compiler::get_fn(ast::Node* node) {
    auto iter = m_ctx->function_table.get(node);
    return iter ? *iter : nullptr;
}

Function* Compiler::add_internal_method_fn(ChiType* type, ast::Node* method) {
    if (auto fn = get_fn(method)) {
        return fn;
    }
    auto fn = compile_fn(method);
    init_method_fn(fn, method->name, type, nullptr);
    return fn;
}

void Compiler::compile_construction(Function* fn, jit_value_t dest, ChiType* type, ast::Node* expr) {
    if (type->kind == TypeKind::Array) {
        return compile_array_construction(fn, dest);
    } else if (type->kind == TypeKind::String) {
        return compile_string_construction(fn, dest, {});
    } else if (type->kind == TypeKind::Optional) {
        auto opt = compile_wrapped_type(type);
        return fn->insn_store_relative(dest, opt.get_opt_flag_offset(), fn->new_constant(jit_sbyte(0)));
    } else if (type->kind == TypeKind::Box) {
        auto box = compile_wrapped_type(type);
        auto ptr = compile_mem_alloc(fn, fn->new_constant(box.elem_size));
        fn->insn_store_relative(dest, 0, ptr);
        return compile_construction(fn, ptr.raw(), type->get_elem(), expr);
    }
    auto struct_ = get_struct(type);
    assert(struct_.data->constructor.get());
    fn->insn_call(struct_.data->constructor.get(), &dest, 1);
    auto cons_member = struct_.spec->find_member("new");
    if (cons_member) {
        auto constructor = get_fn(cons_member->node);
        assert(constructor);
        array<jit_value_t> args;
        args.add(dest);
        if (expr) {
            assert(expr->type == ast::NodeType::ConstructExpr);
            auto& data = expr->data.construct_expr;
            for (auto arg: data.items) {
                auto value = compile_simple_value(fn, arg);
                args.add(value.raw());
            }
        }
        fn->insn_call(constructor, args.items, args.size);
    }
}

StructData* Compiler::get_struct_data(ChiType* struct_type) {
    if (auto data = m_ctx->struct_table.get(struct_type)) {
        return data->get();
    }
    m_ctx->struct_table.emplace(struct_type, new StructData());
    return m_ctx->struct_table[struct_type].get();
}

Struct Compiler::get_struct(cx::ChiType* struct_type) {
    struct_type = eval_type(struct_type);
    return {struct_type, &struct_type->data.struct_, get_struct_data(struct_type)};
}

void Compiler::compile_field_mem_free(Function* fn, jit_value& arr, jit_nuint offset) {
    auto data = fn->insn_load_relative(arr, offset, jit_type_nint);
    jit_value_t free_args[] = {data.raw()};
    fn->insn_call_native("_free", (void*) free, free_signature, free_args, 1);
}

void Compiler::compile_destruction_for_type(Function* fn, jit_value& address, ChiType* type) {
    switch (type->kind) {
        case TypeKind::String:
            compile_field_mem_free(fn, address, 0);
            break;

        case TypeKind::Array: {
            compile_array_loop(fn, address, type, ArrayOp::Destroy);
            compile_field_mem_free(fn, address, 0);
            break;
        }

        case TypeKind::Box: {
            auto data = fn->insn_load_relative(address, 0, jit_type_nint);
            compile_destruction_for_type(fn, data, type->get_elem());
            compile_field_mem_free(fn, address, 0);
            break;
        }

        case TypeKind::Optional:
            compile_destruction_for_type(fn, address, type->get_elem());
            break;

        case TypeKind::Struct: {
            auto struct_ = get_struct(type);
            jit_value_t args[] = {address.raw()};
            fn->insn_call(struct_.data->destructor.get(), args, 1);
            if (auto destructor = struct_.spec->find_member("delete")) {
                auto des_fn = get_fn(destructor->node);
                fn->insn_call(des_fn, args, 1);
            }
            break;
        }
        default:
            break;
    }
}

void Compiler::compile_destruction(Function* fn, jit_value& address, ast::Node* node) {
    return compile_destruction_for_type(fn, address, get_chitype(node));
}

Function* Compiler::new_fn(jit_type_t signature, ast::Node* node) {
    return new Function(signature, get_context(), node);
}

Function* Compiler::add_fn(jit_type_t signature, ast::Node* node) {
    auto fn = new_fn(signature, node);
    return m_ctx->add_fn(node, fn);
}

void Compiler::init_method_fn(Function* fn, const string& fn_name, ChiType* struct_type, ChiTypeSubtype* subtype) {
    auto ctn_name = get_resolver()->to_string(struct_type);
    fn->qualified_name = fmt::format("{}::{}", ctn_name, fn_name);
    if (is_aot_enabled()) {
        stringstream ss;
        for (auto c: ctn_name) {
            switch (c) {
                case '<':
                    ss << ".I.";
                    break;
                case '>':
                    ss << ".I";
                    break;
                case ',':
                    ss << ".V.";
                    break;
                default:
                    ss.put(c);
            }
        }
        fn->asm_name = fmt::format("_{}._{}", ss.str(), fn_name);
    }
    fn->container_subtype = subtype;
}

bool Compiler::should_destroy(ast::Node* node) {
    return should_destroy_for_type(get_chitype(node));
}

bool Compiler::should_destroy_for_type(ChiType* type) {
    switch (type->kind) {
        case TypeKind::Array:
        case TypeKind::String:
        case TypeKind::Box:
            return true;
        case TypeKind::Struct:
            return type->data.struct_.kind == ContainerKind::Struct;
        case TypeKind::Optional:
            return should_destroy_for_type(type->get_elem());
        default:
            return false;
    }
}

JumpTable* Compiler::add_jump_table(long* table_index) {
    *table_index = m_ctx->jump_tables.size;
    return m_ctx->jump_tables.emplace(new JumpTable())->get();
}

ImplInfo* Compiler::get_impl_info(TraitImpl* impl) {
    if (impl->itable_index < 0) {
        auto info = m_ctx->impls.emplace(new ImplInfo())->get();
        info->type = get_type_info(impl->impl_type);
        auto vtable = add_jump_table(&impl->itable_index);
        for (auto member: impl->impl_table) {
            auto fn = get_fn(member->node);
            auto fn_ptr = is_aot_enabled() ? (void*) fn : jit_function_to_vtable_pointer(fn->raw());
            vtable->add(fn_ptr);
        }
        info->vtable = vtable->items;
        info->vtable_size = vtable->size;
        return info;
    }
    return m_ctx->impls[impl->itable_index].get();
}

ChiType* Compiler::eval_type(ChiType* type) {
    if (type->is_placeholder && m_fn && m_fn->container_subtype) {
        type = get_resolver()->type_placeholders_sub(type, m_fn->container_subtype);
    }
    if (type->kind == TypeKind::Subtype) {
        return type->data.subtype.resolved_struct;
    }
    return type;
}

ChiType* Compiler::get_chitype(ast::Node* node) {
    return eval_type(node->resolved_type);
}

jit_value Compiler::compile_mem_alloc(Function* fn, const jit_value& size_value) {
    jit_value_t args[] = {size_value.raw()};
    return fn->insn_call_native("_malloc", (void*) sys_malloc, malloc_signature, args, 1);
}

void Compiler::compile_array_construction(Function* fn, const jit_value& dest) {
    static jit_type_t array_construct_params[] = {jit_type_nint};
    static auto array_construct_signature = jit_type_create_signature(jit_abi_cdecl, jit_type_void,
                                                                      array_construct_params, 1, 1);
    jit_value_t args[] = {dest.raw()};
    fn->insn_call_native("_cx_array_construct", (void*) cx_array_construct,
                         array_construct_signature, args, 1);
}

void Compiler::compile_array_reserve(Function* fn, const jit_value& dest, jit_uint elem_size, const jit_value& size) {
    static jit_type_t array_reserve_params[] = {jit_type_nint, jit_type_uint, jit_type_uint};
    static auto array_reserve_signature = jit_type_create_signature(jit_abi_cdecl, jit_type_void,
                                                                    array_reserve_params, 3, 1);
    jit_value_t args[] = {dest.raw(), fn->new_constant(elem_size).raw(), size.raw()};
    fn->insn_call_native("_cx_array_reserve", (void*) cx_array_reserve,
                         array_reserve_signature, args, 3);
}

Array Compiler::compile_array_ref(Function* fn, ast::Node* expr) {
    auto array_type = get_chitype(expr);
    auto array_ = compile_simple_value(fn, expr);
    auto this_ = fn->insn_address_of(array_);
    Array result;
    result.array_type = array_type;
    result.elem_type = array_type->get_elem();
    result.ptr = this_;
    result.data = fn->insn_load_relative(this_, ARRAY_DATA_FIELD_OFFSET, jit_type_nint);
    result.elem = compile_type(result.elem_type);
    result.elem_size = jit_type_get_size(result.elem);
    return result;
}

jit_value Compiler::compile_array_add(Function* fn, const jit_value& dest, jit_uint elem_size, const jit_value& value) {
    static jit_type_t array_add_params[] = {jit_type_nint, jit_type_uint};
    static auto array_add_signature = jit_type_create_signature(jit_abi_cdecl, jit_type_nint,
                                                                array_add_params, 2, 1);
    jit_value_t args[] = {dest.raw(), fn->new_constant(elem_size).raw()};
    auto address = fn->insn_call_native("_cx_array_add", (void*) cx_array_add,
                                        array_add_signature, args, 2);
    fn->insn_store_relative(address, 0, value);
    return address;
}

void Compiler::compile_string_construction(Function* fn, const jit_value& dest, optional<jit_value> data) {
    fn->insn_store_relative(dest, STRING_DATA_FIELD_OFFSET, fn->get_null_constant());
    fn->insn_store_relative(dest, STRING_SIZE_FIELD_OFFSET, fn->new_constant(jit_int(0)));
    if (data) {
        jit_value_t args[] = {dest.raw(), data->raw()};
        fn->insn_call_native("_cx_string_set_data", (void*) cx_string_set_data,
                             string_set_data_signature, args, 2);
    }
}

jit_value Compiler::compile_string_alloc(Function* fn, const jit_value& data) {
    auto temp = fn->new_value(string_internal_struct);
    auto addr = fn->insn_address_of(temp);
    compile_string_construction(fn, addr, data);
    return temp;
}

jit_value Compiler::compile_string_concat(Function* fn, const jit_value& s1, const jit_value& s2) {
    auto temp = fn->new_value(string_internal_struct);
    auto addr = fn->insn_address_of(temp);
    jit_value_t args[] = {addr.raw(), s1.raw(), s2.raw()};
    fn->insn_call_native("_cx_string_concat", (void*) cx_string_concat, string_concat_signature, args, 3);
    return temp;
}

jit_value Compiler::compile_arithmetic_op(Function* fn, ChiType* value_type,
                                          TokenType op_type, const jit_value& op1, const jit_value& op2) {
    switch (op_type) {
        case TokenType::ADD: {
            if (value_type->kind == TypeKind::String) {
                return compile_string_concat(fn, op1, op2);
            }
            return fn->insn_add(op1, op2);
        }
        case TokenType::MUL:
            return fn->insn_mul(op1, op2);
        case TokenType::SUB:
            return fn->insn_sub(op1, op2);
        case TokenType::DIV:
            return fn->insn_div(op1, op2);
        case TokenType::LSHIFT:
            return fn->insn_shl(op1, op2);
        case TokenType::RSHIFT:
            return fn->insn_shr(op1, op2);
        case TokenType::AND:
        case TokenType::LAND:
            return fn->insn_and(op1, op2);
        case TokenType::OR:
        case TokenType::LOR:
            return fn->insn_or(op1, op2);
        case TokenType::XOR:
            return fn->insn_xor(op1, op2);
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
        default:
            unreachable();
            return {};
    }
}

jit_value Compiler::compile_constant_value(Function* fn, const ConstantValue& value, ChiType* type) {
    auto t = compile_type(type);
    if (VARIANT_TRY(value, const_int_t, v)) {
        return fn->new_constant(jit_long(*v), t);
    } else if (VARIANT_TRY(value, const_float_t, v)) {
        return fn->new_constant(jit_float64(*v), t);
    } else if (VARIANT_TRY(value, string, v)) {
        return compile_string_alloc(fn, create_string_constant(fn, *v));
    }
    return {};
}

WrappedType Compiler::compile_wrapped_type(ChiType* type) {
    WrappedType opt;
    opt.type = compile_type(type);
    opt.elem = compile_type(type->get_elem());
    opt.elem_size = jit_type_get_size(opt.elem);
    return opt;
}

jit_value Compiler::compile_array_loop(Function* fn, jit_value& address, cx::ChiType* type, ArrayOp op) {
    jit_value dst, temp, arr;
    auto elem = compile_type(type->get_elem());
    auto index = fn->new_value(jit_type_uint);
    fn->store(index, fn->new_constant(0));
    auto src = fn->insn_load_relative(address, 0, jit_type_nint);
    auto size = fn->insn_load_relative(address, ARRAY_SIZE_FIELD_OFFSET, jit_type_uint);
    if (op == ArrayOp::Copy) {
        temp = fn->new_value(compile_type(type));
        arr = fn->insn_address_of(temp);
        compile_array_construction(fn, arr);
        compile_array_reserve(fn, arr, (jit_uint) jit_type_get_size(elem), size);
        dst = fn->insn_load_relative(arr, 0, jit_type_nint);
        fn->insn_store_relative(arr, ARRAY_SIZE_FIELD_OFFSET, size);
    }
    jit_label loop_start, loop_end;
    fn->insn_label(loop_start); // for loop
    fn->insn_branch_if(fn->insn_ge(index, size), loop_end); // if (index >= size) break;
    switch (op) {
        case ArrayOp::Destroy: { // delete src[i]
            auto item = fn->insn_load_elem_address(src, index, elem);
            compile_destruction_for_type(fn, item, type->get_elem());
            break;
        }
        case ArrayOp::Copy: { // dst[i] = src[i]
            auto val = fn->insn_load_elem(src, index, elem);
            auto elem_type = type->get_elem();
            fn->insn_store_elem(dst, index, compile_value_copy(fn, val, elem_type));
            break;
        }
    }
    fn->store(index, fn->insn_add(index, fn->new_constant(1))); // index++
    fn->insn_branch(loop_start);
    fn->insn_label(loop_end);
    return temp;
}

TypeInfo* Compiler::get_type_info(ChiType* type) {
    if (auto cached = m_ctx->info_table.get(type->id)) {
        return cached->get();
    }
    return m_ctx->info_table.emplace(type->id, new TypeInfo(type))->get();
}

void Compiler::compile_panic(Function* fn, const string& message) {
    static jit_type_t panic_params[] = {jit_type_nint};
    static auto panic_signature = jit_type_create_signature(jit_abi_cdecl,
                                                            jit_type_void, panic_params, 1, 1);
    jit_value_t args[] = {create_string_constant(fn, message).raw()};
    fn->insn_call_native("_cx_panic", (void*) cx_panic, panic_signature, args, 1);
}

void Compiler::init_compilation() {
    auto rctx = m_ctx->resolver.get_context();
    for (auto method: rctx->internal_methods) {
        add_internal_method_fn(method->resolved_type->data.fn.container, method);
    }
}
