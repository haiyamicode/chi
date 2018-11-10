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

static jit_type_t malloc_params[] = {jit_type_nuint};
static auto malloc_signature = jit_type_create_signature(jit_abi_cdecl, jit_type_nint, malloc_params, 1, 1);

void* sys_malloc(size_t size) {
    return malloc(size);
}

static jit_type_t free_params[] = {jit_type_nint};
static auto free_signature = jit_type_create_signature(jit_abi_cdecl, jit_type_void, free_params, 1, 1);

static jit_type_t trait_fields[] = {jit_type_nint, jit_type_nint};
static auto trait_struct = jit_type_create_struct(trait_fields, 2, 1);

static auto ptr_size = jit_type_get_size(jit_type_nint);

void sys_printf(const char* format, int value) {
    print(format, value);
}

typedef array<void*> FunctionTable;
static array<box<FunctionTable>> jump_tables;

FunctionTable* add_jump_table(long* table_index) {
    *table_index = jump_tables.size;
    return jump_tables.emplace(new FunctionTable())->get();
}

void* get_vtable_fn(void* impl_ptr, int32_t fn_index) {
    auto impl = (TraitImpl*) impl_ptr;
    auto& table = jump_tables[impl->id];
    return table->at(size_t(fn_index));
}

static jit_type_t get_vtable_fn_params[] = {jit_type_nint, jit_type_int};
static auto get_vtable_fn_signature = jit_type_create_signature(jit_abi_cdecl, jit_type_nint, get_vtable_fn_params, 2,
                                                                1);

Function::Function(jit_type_t signature, CompileContext* _ctx, ast::Node* _node) : jit_function(
        _ctx->jit_ctx, signature), ctx(_ctx), node(_node) {
    set_recompilable();
    if (node) {
        qualified_name = node->name;
    }
    is_returning = this->new_value(jit_type_sys_bool);
}

void Function::build() {
    Compiler compiler(ctx, this);
    compiler.compile_fn_body(this);
}

jit_value Function::insn_call(Function* fn_ref, jit_value_t* args, long num_args) {
    return jit_function::insn_call(fn_ref->get_jit_name(), fn_ref->raw(), fn_ref->signature(), args,
                                   (uint32_t) num_args);
}

void Function::set_qualified_name(const std::string& container_name, const std::string& name) {
    qualified_name = container_name + "::" + name;
}

Compiler::Compiler(CompileContext* ctx, Function* fn) {
    m_ctx = ctx;
    m_fn = fn;
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

Function* Compiler::compile_fn(ast::Node* node) {
    if (auto fn = m_ctx->functions.get(node)) {
        return fn->get();
    }
    auto fn = new_fn(compile_type_of(node), node);
    m_ctx->functions.emplace(node, fn);
    return fn;
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
    auto cached = m_ctx->types.get(type);
    if (cached) {
        return *cached;
    }
    auto result = _compile_type(type);
    m_ctx->types[type] = result;
    return result;
}

jit_type_t Compiler::_compile_type(ChiType* type) {
    switch (type->id) {
        case TypeId::Bool:
            return jit_type_sbyte;
        case TypeId::Int: {
            return to_jit_int_type(type);
        }
        case TypeId::Void:
            return jit_type_void;
        case TypeId::Fn: {
            array<jit_type_t> params;
            auto& fn = type->data.fn;
            if (fn.container) {
                params.add(jit_type_create_pointer(compile_type(fn.container), 1));
            }
            for (auto param: fn.params) {
                params.add(compile_type(param));
            }
            auto return_type = compile_type(fn.return_type);
            return jit_type_create_signature
                    (jit_abi_cdecl, return_type, params.items, uint32_t(params.size), 1);
        }
        case TypeId::Struct: {
            auto& struct_ = type->data.struct_;
            if (struct_.kind == ContainerKind::Trait) {
                return trait_struct;
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
        case TypeId::Subtype: {
            return compile_type(eval_type(type));
        }
        case TypeId::String: {
            return string_type;
        }
        case TypeId::Pointer: {
            return jit_type_nint;
        }
        case TypeId::Array: {
            return compile_type(type->data.array.internal);
        }
        default:
            panic("unhandled {}", PRINT_ENUM(type->id));
            return {};
    }
}

inline jit_type_t Compiler::compile_type_of(cx::ast::Node* node) {
    return compile_type(get_type_of(node));
}

void Compiler::compile_fn_body(Function* fn) {
    static auto printf_signature = compile_type_of(m_ctx->resolver.get_builtin("printf"));
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
        auto fn_type = get_type_of(fn->node);
        auto& fn_end = fn->push_return_scope()->emplace_back();
        fn->return_value = fn->new_value(compile_type(fn_type->data.fn.return_type));
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
            auto var = fn->new_value(compile_type_of(stmt));
            add_value(stmt, var);
            if (data.expr) {
                fn->store(var, compile_assignment_value(fn, data.expr, stmt));
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
            if (fn_expr->type == ast::NodeType::Identifier) {
                auto& iden = fn_expr->data.identifier;
                fn_ref = iden.decl;
            } else if (fn_expr->type == ast::NodeType::DotExpr) {
                auto& dot_expr = fn_expr->data.dot_expr;
                auto dot = compile_dot_expr(fn, fn_expr);
                assert(dot.method);
                fn_ref = *dot.method;
                auto builtin_id = fn_ref->data.fn_def.builtin_id;
                if (builtin_id == ast::BuiltinId::ArrayAdd) {
                    return compile_array_add(fn, dot_expr.expr, data.args[0]);
                }
                args.add(dot.ctn_value.raw());
                if (dot.vtable_fn) {
                    fn_ref_vtable = *dot.vtable_fn;
                }
            } else {
                panic("unhandled");
            }
            assert(fn_ref);
            for (auto& arg: data.args) {
                auto value = compile_simple_value(fn, arg);
                args.add(value.raw());
            }
            if (fn_ref_vtable) {
                return fn->insn_call_indirect_vtable(*fn_ref_vtable, compile_type_of(fn_ref),
                                                     args.items, uint32_t(args.size));
            }
            auto fn_instance = get_fn(fn_ref);
            assert(fn_instance);
            return fn->insn_call(fn_instance, args.items, args.size);
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
                case TokenType::BOOL:
                    return fn->new_constant(jit_sbyte(token->val.b));
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
            auto op2 = compile_assignment_value(fn, data.op2, data.op1);
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
                default:
                    break;
            }
            auto value = compile_simple_value(fn, data.op1);
            switch (data.op_type) {
                case TokenType::SUB:
                    return fn->insn_neg(value);
                case TokenType::ADD:
                    return value;
                case TokenType::LNOT:
                    return fn->insn_to_not_bool(value);
                case TokenType::MUL:
                    return fn->insn_load_relative(value, 0, compile_type_of(expr));
                default:
                    unreachable();
            }
            break;
        }
        case ast::NodeType::ConstructExpr: {
            auto& data = expr->data.construct_expr;
            auto ctn_type = get_type_of(expr);
            jit_value this_;
            jit_value value;
            if (data.type && ctn_type->id == TypeId::Pointer) {
                ctn_type = ctn_type->data.pointer.elem;
                jit_nuint size_value = jit_type_get_size(compile_type(ctn_type));
                jit_value_t args[] = {fn->new_constant(size_value).raw()};
                this_ = fn->insn_call_native("malloc", (void*) sys_malloc, malloc_signature, args, 1);
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
            auto ctn_type = get_type_of(data.expr);
            if (ctn_type->id == TypeId::TypeSymbol) {
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
            auto from_type = get_type_of(data.expr);
            auto to_type = get_type_of(expr);
            return compile_type_conversion(fn, value, from_type, to_type);
        }
        default:
            panic("unhandled {}", PRINT_ENUM(expr->type));
            break;
    }
    return fn->new_constant(jit_int(0));
}

jit_value Compiler::compile_assignment_value(Function* fn, ast::Node* value, ast::Node* dest) {
    auto value_ = compile_simple_value(fn, value);
    auto src_type = get_type_of(value);
    auto dest_type = get_type_of(dest);
    return compile_type_conversion(fn, value_, src_type, dest_type);
}

jit_value Compiler::compile_type_conversion(Function* fn, const jit_value& value,
                                            ChiType* from_type, ChiType* to_type) {
    if (from_type == to_type) {
        return value;
    }
    if (to_type->id == TypeId::Struct && from_type->id == TypeId::Pointer) {
        assert(to_type->data.struct_.kind == ContainerKind::Trait);
        auto& impl_type = from_type->data.pointer.elem->data.struct_;
        auto impl = impl_type.traits_table[to_type];
        build_jump_table(impl);
        auto temp = fn->new_value(trait_struct);
        auto addr = fn->insn_address_of(temp);
        auto impl_ptr = jit_nint((void*) impl);
        fn->insn_store_relative(addr, 0, fn->new_constant(impl_ptr));
        fn->insn_store_relative(addr, ptr_size, value);
        return temp;
    } else {
        if (from_type->id != to_type->id || to_type->id == TypeId::Int) {
            return fn->insn_convert(value, compile_type(to_type), 0);
        }
    }
    return value;
}

ValueRef Compiler::compile_value_ref(Function* fn, ast::Node* expr) {
    switch (expr->type) {
        case ast::NodeType::DotExpr: {
            auto dot = compile_dot_expr(fn, expr);
            auto address = fn->insn_add_relative(dot.ctn_value, dot.field->offset);
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
            return {fn->insn_address_of(value), compile_type_of(expr)};
        }
        case ast::NodeType::ParenExpr: {
            return compile_value_ref(fn, expr->data.child_expr);
        }
        case ast::NodeType::UnaryOpExpr: {
            auto& data = expr->data.unary_op_expr;
            assert(data.op_type == TokenType::MUL);
            return {compile_simple_value(fn, data.op1), compile_type_of(expr)};
        }
        default:
            unreachable();
            return {};
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
    auto struct_type = get_resolver()->to_value_type(get_type_of(node));
    if (m_ctx->structs.get(struct_type)) {
        return;
    }
    if (struct_type->id != TypeId::Struct) {
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
    if (struct_type->id == TypeId::Subtype) {
        subtype = &struct_type->data.subtype;
        struct_type = subtype->resolved_struct;
    }
    auto& struct_ = struct_type->data.struct_;
    auto struct_jit = compile_type(struct_type);
    auto this_ = jit_type_create_pointer(struct_jit, 1);
    auto sdata = get_struct_data(struct_type);

    // default constructor
    auto cons_signature = jit_type_create_signature
            (jit_abi_cdecl, jit_type_void, &this_, uint32_t(1), 1);
    auto constructor = new_fn(cons_signature, nullptr);
    fn_method(constructor, "_new", struct_type, subtype);
    sdata->constructor.reset(constructor);

    auto cons_this = constructor->get_param(0);
    for (auto& member: struct_.members) {
        auto node_type = member->node->type;
        if (node_type == ast::NodeType::FnDef) {
            auto method = compile_fn(member->node);
            fn_method(method, member->get_name(), struct_type, subtype);
        } else if (node_type == ast::NodeType::VarDecl) {
            auto& var = member->node->data.var_decl;
            auto var_type = member->resolved_type;
            // compile dependencies
            if (var_type->id == TypeId::Struct) {
                auto struct_decl = var_type->data.struct_.node;
                if (struct_decl && !m_ctx->structs.get(var_type)) {
                    compile_struct(struct_decl);
                }
            }
            // compile assignment
            if (var.expr) {
                auto field = var.resolved_field;
                auto offset = jit_type_get_offset(struct_jit, (uint32_t) field->field_index);
                auto value = compile_assignment_value(constructor, var.expr, member->node);
                constructor->insn_store_relative(cons_this, offset, value);
            }
        }
    }

    // default destructor
    auto des_signature = jit_type_create_signature
            (jit_abi_cdecl, jit_type_void, &this_, uint32_t(1), 1);
    auto destructor = new_fn(des_signature, nullptr);
    fn_method(destructor, "_delete", struct_type, subtype);
    sdata->destructor.reset(destructor);

    auto des_this = destructor->get_param(0);
    for (long i = struct_.members.size - 1; i >= 0; i--) {
        auto& member = struct_.members[i];
        if (member->node->type == ast::NodeType::VarDecl) {
            if (should_destroy(member->node)) {
                auto offset = jit_type_get_offset(struct_jit, (uint32_t) member->field_index);
                auto address = destructor->insn_add_relative(des_this.raw(), offset);
                compile_var_destroy(destructor, member->node, address);
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
    auto member_type = get_type_of(expr);
    DotValue dot;
    dot.ctn_type = get_type_of(data.expr);
    if (dot.ctn_type->id == TypeId::Pointer) {
        dot.ctn_value = compile_simple_value(fn, data.expr);
        dot.ctn_type = dot.ctn_type->data.pointer.elem;
    } else {
        dot.ctn_value = compile_value_ref(fn, data.expr).address;
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
            auto trait_ref = dot.ctn_value;
            dot.ctn_value = fn->insn_load_relative(trait_ref, ptr_size, jit_type_nint);
            auto impl_ref = fn->insn_load_relative(trait_ref, 0, jit_type_nint);
            auto method_index = fn->new_constant(jit_int(member->method_index));
            jit_value_t args[] = {impl_ref.raw(), method_index.raw()};
            dot.vtable_fn = fn->insn_call_native("_get_vtable_fn", (void*) get_vtable_fn,
                                                 get_vtable_fn_signature, args, 2);
        }
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
    auto struct_ = get_struct(struct_type);
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
    if (auto data = m_ctx->structs.get(struct_type)) {
        return data->get();
    }
    m_ctx->structs.emplace(struct_type, new StructData());
    return m_ctx->structs[struct_type].get();
}

Struct Compiler::get_struct(cx::ChiType* struct_type) {
    struct_type = eval_type(struct_type);
    return {struct_type, &struct_type->data.struct_, get_struct_data(struct_type)};
}

Array Compiler::compile_array_ref(Function* fn, ast::Node* expr) {
    auto array_type = get_type_of(expr);
    auto array_ = compile_simple_value(fn, expr);
    auto this_ = fn->insn_address_of(array_);
    Array result;
    result.ptr = this_;
    result.data = fn->insn_load_relative(this_, ARRAY_DATA_FIELD_OFFSET, jit_type_nint);
    result.size = fn->insn_load_relative(this_, ARRAY_SIZE_FIELD_OFFSET, jit_type_nint);
    result.elem_type = compile_type(array_type->data.array.elem);
    result.elem_size = jit_type_get_size(result.elem_type);
    return result;
}

jit_value Compiler::compile_array_add(Function* fn, ast::Node* expr, ast::Node* value_arg) {
    auto arr = compile_array_ref(fn, expr);
    auto elem_size = fn->new_constant(arr.elem_size);
    auto inc = fn->new_constant(jit_int(1));
    auto old_size = arr.size;
    auto this_ = arr.ptr;
    auto new_size = fn->insn_add(old_size, inc);
    fn->insn_store_relative(this_, ARRAY_SIZE_FIELD_OFFSET, new_size);
    auto mem_size = fn->insn_mul(new_size, elem_size);
    jit_value_t ra_args[] = {arr.data.raw(), mem_size.raw()};
    auto new_data = fn->insn_call_native("realloc", (void*) sys_realloc, realloc_signature, ra_args, 2);
    fn->insn_store_relative(this_, ARRAY_DATA_FIELD_OFFSET, new_data);
    auto address = fn->insn_load_elem_address(new_data, old_size, arr.elem_type);
    auto value = compile_simple_value(fn, value_arg);
    fn->insn_store_relative(address, 0, value);
    return address;
}

void Compiler::compile_array_destroy(Function* fn, jit_value& arr) {
    auto data = fn->insn_load_relative(arr, ARRAY_DATA_FIELD_OFFSET, jit_type_nint);
    jit_value_t free_args[] = {data.raw()};
    fn->insn_call_native("free", (void*) free, free_signature, free_args, 1);
}

void Compiler::compile_var_destroy(Function* fn, ast::Node* var, jit_value& address) {
    auto type = get_type_of(var);
    switch (type->id) {
        case TypeId::Array:
            compile_array_destroy(fn, address);
            break;
        case TypeId::Struct: {
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

Function* Compiler::new_fn(jit_type_t signature, ast::Node* node) {
    return new Function(signature, get_context(), node);
}

void Compiler::fn_method(Function* fn, const string& name, ChiType* struct_type, ChiTypeSubtype* subtype) {
    fn->set_qualified_name(get_resolver()->to_string(struct_type), name);
    fn->container_subtype = subtype;
}

bool Compiler::should_destroy(ast::Node* node) {
    auto type = get_type_of(node);
    switch (type->id) {
        case TypeId::Array:
        case TypeId::Struct:
            return type->data.struct_.kind == ContainerKind::Struct;
        default:
            return false;
    }
}

void Compiler::build_jump_table(TraitImpl* impl) {
    if (impl->id < 0) {
        auto vtable = add_jump_table(&impl->id);
        for (auto member: impl->impl_table) {
            auto fn = compile_fn(member->node);
            auto fn_ptr = jit_function_to_vtable_pointer(fn->raw());
            vtable->add(fn_ptr);
        }
    }
}

ChiType* Compiler::eval_type(ChiType* type) {
    if (type->is_placeholder && m_fn && m_fn->container_subtype) {
        type = get_resolver()->type_placeholders_sub(type, m_fn->container_subtype);
    }
    if (type->id == TypeId::Subtype) {
        return type->data.subtype.resolved_struct;
    }
    return type;
}

ChiType* Compiler::get_type_of(ast::Node* node) {
    return eval_type(node->resolved_type);
}

