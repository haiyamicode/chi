/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "resolver.h"
#include "errors.h"

using namespace cx;

using ast::NodeType;

Resolver::Resolver(ResolveContext *ctx) { m_ctx = ctx; }

ast::Node *Resolver::add_primitive(const string &name, ChiType *type) {
    auto node = create_node(ast::NodeType::Primitive);
    node->token = nullptr;
    node->name = name;
    type->name = name;
    m_ctx->builtins.add(node);
    auto sym = create_type_symbol(node->name, type);
    node->resolved_type = sym;
    return node;
}

void Resolver::context_init_primitives() {
    if (m_ctx->system_types.any) {
        panic("primitives already initialized");
        return;
    }
    auto &system_types = m_ctx->system_types;
    system_types.any = create_type(TypeKind::Any);
    system_types.char_ = create_int_type(8, false);
    system_types.uint8 = create_int_type(8, true);
    system_types.int_ = create_int_type(32, false);
    system_types.int32 = create_int_type(32, false);
    system_types.uint32 = create_int_type(32, true);
    system_types.int64 = create_int_type(64, false);
    system_types.float_ = create_float_type(32);
    system_types.double_ = create_float_type(64);
    system_types.void_ = create_type(TypeKind::Void);
    system_types.void_ptr = create_pointer_type(system_types.void_, TypeKind::Pointer);
    system_types.null_ptr = create_pointer_type(system_types.void_, TypeKind::Pointer);
    system_types.null_ptr->data.pointer.is_null = true;
    system_types.void_ref = create_pointer_type(system_types.void_, TypeKind::Reference);
    system_types.bool_ = create_type(TypeKind::Bool);
    system_types.string = create_type(TypeKind::String);
    system_types.str_lit = create_pointer_type(system_types.char_, TypeKind::Pointer);
    system_types.array = create_type(TypeKind::Array);
    system_types.optional = create_type(TypeKind::Optional);
    system_types.box = create_type(TypeKind::Box);
    system_types.result = create_type(TypeKind::Result);
    system_types.error = create_type(TypeKind::Error);
    system_types.promise = create_type(TypeKind::Promise);

    add_primitive("bool", system_types.bool_);
    add_primitive("string", system_types.string);
    add_primitive("any", system_types.any);
    add_primitive("void", system_types.void_);
    add_primitive("int", system_types.int_);
    add_primitive("int64", system_types.int64);
    add_primitive("char", system_types.char_);
    add_primitive("float", system_types.float_);
    add_primitive("double", system_types.double_);
    add_primitive("uint8", system_types.uint8);
    add_primitive("int8", create_int_type(8, false));
    add_primitive("int16", create_int_type(16, false));
    add_primitive("int32", create_int_type(32, false));
    add_primitive("uint", create_int_type(32, true));
    add_primitive("uint16", create_int_type(16, true));
    add_primitive("uint32", create_int_type(32, true));
    add_primitive("uint64", create_int_type(64, true));

    // non-primitive builtins
    add_primitive("Box", system_types.box);
    add_primitive("Result", system_types.result);
    add_primitive("Error", system_types.error);
    add_primitive("Promise", system_types.promise);

    // intrinsic symbols
    m_ctx->intrinsic_symbols["std.ops.Index"] = IntrinsicSymbol::OpIndex;
    m_ctx->intrinsic_symbols["std.iter.At"] = IntrinsicSymbol::IterAt;
    m_ctx->intrinsic_symbols["std.iter.Begin"] = IntrinsicSymbol::IterBegin;
    m_ctx->intrinsic_symbols["std.iter.Next"] = IntrinsicSymbol::IterNext;
    m_ctx->intrinsic_symbols["std.iter.End"] = IntrinsicSymbol::IterEnd;
}

ChiType *Resolver::create_type(TypeKind kind) { return m_ctx->allocator->create_type(kind); }

ChiType *Resolver::create_type_symbol(optional<string> name, ChiType *type) {
    auto tysym = create_type(TypeKind::TypeSymbol);
    tysym->name = name;
    tysym->data.type_symbol.giving_type = type;
    tysym->data.type_symbol.underlying_type = type;
    return tysym;
}

ast::Node *Resolver::create_node(ast::NodeType type) { return m_ctx->allocator->create_node(type); }

ast::Node *Resolver::get_builtin(const string &name) {
    for (auto &node : m_ctx->builtins) {
        if (node->name == name) {
            return node;
        }
    }
    return nullptr;
}

ChiType *Resolver::node_get_type(ast::Node *node) {
    if (!node->resolved_type)
        return nullptr;
    return node->resolved_type;
}

void Resolver::resolve(ast::Package *package) {
    for (auto &module : package->modules) {
        resolve(module.get());
    }
}

void Resolver::resolve(ast::Module *module) {
    ResolveScope scope;
    m_module = module;
    auto module_scope = scope.set_module(module);
    resolve(module->root, module_scope);
}

bool Resolver::can_assign(ChiType *from_type, ChiType *to_type) {
    from_type = from_type->eval();
    to_type = to_type->eval();

    if (is_same_type(from_type, to_type)) {
        return true;
    }
    switch (to_type->kind) {
    case TypeKind::Void:
        return from_type->kind == TypeKind::Void;
    case TypeKind::String:
        return from_type->kind == TypeKind::String || from_type == get_system_types()->str_lit;
    case TypeKind::Array:
        return from_type->kind == TypeKind::Array &&
               can_assign(from_type->get_elem(), to_type->get_elem());
    case TypeKind::Pointer:
    case TypeKind::Reference: {
        return from_type->kind == TypeKind::Pointer ||
               can_assign(from_type, get_system_types()->int_);
    }
    case TypeKind::Int:
        return type_is_int(from_type);
    case TypeKind::Any:
        return from_type->kind != TypeKind::Struct || ChiTypeStruct::is_interface(from_type);
    case TypeKind::Struct: {
        if (from_type == to_type) {
            return true;
        }
        if (ChiTypeStruct::is_interface(to_type) && ChiTypeStruct::is_pointer_type(from_type)) {
            auto ft = from_type->get_elem();
            if (ft->kind != TypeKind::Struct) {
                return false;
            }
            auto &ss = ft->data.struct_;
            if (ss.kind == ContainerKind::Struct) {
                for (auto &impl : ss.interfaces) {
                    if (is_same_type(impl->interface_type, to_type)) {
                        return true;
                    }
                }
                return false;
            }
        }
        return false;
    }
    case TypeKind::Bool:
        return type_is_int(from_type) || from_type->kind == TypeKind::Optional;
    case TypeKind::Optional: {
        if (from_type->kind == TypeKind::Pointer) {
            return from_type->data.pointer.is_null;
        }
        return from_type == to_type || to_type->get_elem() == from_type;
    }
    case TypeKind::Fn:
        return from_type->kind == TypeKind::Fn && to_string(from_type) == to_string(to_type);
    case TypeKind::FnLambda:
        if (from_type->kind == TypeKind::Fn) {
            return to_string(from_type) == to_string(to_type->data.fn_lambda.fn);
        }
        return from_type->kind == TypeKind::FnLambda && to_string(from_type) == to_string(to_type);
    default:
        break;
    }
    return false;
}

ChiType *Resolver::to_value_type(ChiType *type) {
    if (type->kind == TypeKind::TypeSymbol) {
        return type->data.type_symbol.giving_type;
    }
    return type;
}

ChiType *Resolver::resolve_value(ast::Node *node, ResolveScope &scope) {
    auto value_type = to_value_type(resolve(node, scope));
    if (ChiTypeStruct::is_generic(value_type)) {
        error(node, errors::MISSING_TYPE_ARGUMENTS, to_string(value_type));
    }
    return value_type;
}

ChiType *Resolver::_resolve(ast::Node *node, ResolveScope &scope, uint32_t flags) {
    switch (node->type) {
    case NodeType::Root: {
        auto &data = node->data.root;
        // first pass: skip function and struct bodies
        scope.skip_fn_bodies = true;
        for (auto decl : data.top_level_decls) {
            resolve(decl, scope);
            if (decl->type == NodeType::FnDef && decl->name == "main") {
                node->module->package->entry_fn = decl;
                decl->data.fn_def.decl_spec->flags |= ast::DECL_IS_ENTRY;
            }
        }

        // second pass: resolve struct members
        for (auto decl : data.top_level_decls) {
            if (decl->type == NodeType::StructDecl) {
                _resolve(decl, scope);
            }
        }

        // third pass: resolve struct embeds
        for (auto decl : data.top_level_decls) {
            if (decl->type == NodeType::StructDecl) {
                _resolve(decl, scope);
            }
        }

        // fourth pass: resolve function and method bodies
        scope.skip_fn_bodies = false;
        for (auto decl : data.top_level_decls) {
            if (decl->type == NodeType::StructDecl || decl->type == NodeType::FnDef) {
                _resolve(decl, scope);
            }
        }

        // final pass: ensure subtypes are resolved
        for (auto decl : data.top_level_decls) {
            if (decl->type == NodeType::StructDecl && decl->data.struct_decl.type_params.size > 0) {
                auto &struct_ = decl->data.struct_decl;
                if (struct_.type_params.size > 0) {
                    auto struct_type = to_value_type(decl->resolved_type);
                    for (auto subtype : struct_type->data.struct_.subtypes) {
                        resolve_subtype(subtype);
                    }
                }
            }
        }
        return nullptr;
    }
    case NodeType::FnDef: {
        auto &data = node->data.fn_def;
        if (data.fn_kind == ast::FnKind::Lambda) {
            auto &data = node->data.fn_def;
            auto fn_scope = scope.set_parent_fn_node(node);
            auto proto = resolve(data.fn_proto, fn_scope);
            fn_scope = fn_scope.set_parent_fn(proto);
            resolve(data.body, fn_scope);

            // resolve captures
            for (auto decl : data.captures) {
                auto type = resolve(decl, scope);
                proto->data.fn_lambda.captures.add(type);
            }

            // create bound form of the lambda function if it has captures
            auto fn_type = proto->data.fn_lambda.fn;
            auto &fn_data = fn_type->data.fn;
            auto bound_type = create_type(TypeKind::Fn);
            proto->data.fn_lambda.bound_fn = bound_type;

            // binding struct
            auto bstruct = create_type(TypeKind::Struct);
            bstruct->display_name = fmt::format("__lambda_{}::Bind", proto->id);
            auto &bstruct_data = bstruct->data.struct_;
            bstruct_data.kind = ContainerKind::Struct;
            for (int i = 0; i < data.captures.size; i++) {
                auto capture = data.captures[i];
                auto name = fmt::format("capture_{}", i);
                bstruct_data.add_member(
                    capture->name, get_dummy_var(name),
                    get_pointer_type(capture->resolved_type, TypeKind::Reference));
            }

            // create signature with binding struct as first parameter
            proto->data.fn_lambda.bind_struct = bstruct;
            auto &bound_fn = bound_type->data.fn;
            bound_fn.params.add(get_pointer_type(bstruct, TypeKind::Reference));
            for (auto param : fn_data.params) {
                bound_fn.params.add(param);
            }
            bound_fn.return_type = fn_data.return_type;
            bound_fn.is_variadic = fn_data.is_variadic;
            bound_fn.container_ref = fn_data.container_ref;
            return proto;
        }

        auto fn_scope = scope.set_parent_fn_node(node);
        auto proto = resolve(data.fn_proto, fn_scope, flags | IS_FN_DECL_PROTO);
        if (data.body && should_resolve_fn_body(scope)) {
            fn_scope = fn_scope.set_parent_fn(proto);
            resolve(data.body, fn_scope);
        }
        return proto;
    }
    case NodeType::FnProto: {
        auto &data = node->data.fn_proto;
        auto is_fn_decl = flags & IS_FN_DECL_PROTO;
        auto return_type =
            data.return_type ? resolve_value(data.return_type, scope) : get_system_types()->void_;
        TypeList param_types;
        bool is_variadic = false;

        if (data.is_vararg) {
            is_variadic = true;
        }
        for (int i = 0; i < data.params.size; i++) {
            auto param = data.params[i];
            auto &pdata = param->data.param_decl;
            auto is_last = i == data.params.size - 1;
            if (pdata.is_variadic && !is_last) {
                error(param, errors::VARIADIC_NOT_FINAL, param->name);
                return create_type(TypeKind::Fn);
            }
            auto param_type = resolve_value(param, scope);
            if (pdata.is_variadic) {
                param_type = get_array_type(param_type);
                param->resolved_type = param_type;
                is_variadic = true;
            }
            param_types.add(param_type);
        }

        auto fn_type = get_fn_type(return_type, &param_types, is_variadic, scope.parent_struct);
        if (!is_fn_decl) {
            return get_lambda_for_fn(fn_type);
        }
        return fn_type;
    }
    case NodeType::Identifier: {
        auto &data = node->data.identifier;
        if (data.kind == ast::IdentifierKind::This) {
            auto type = create_type(TypeKind::This);
            type->data.pointer.elem = get_pointer_type(scope.parent_struct, TypeKind::Reference);
            return type;
        }
        if (data.kind == ast::IdentifierKind::Value && scope.block) {
            auto replacement = scope.block->scope->find_one(data.decl->name);
            if (replacement && replacement->type == NodeType::VarDecl &&
                replacement->data.var_decl.is_generated && replacement != data.decl) {
                data.decl = replacement;
            }
        }
        auto type = resolve(data.decl, scope);
        if (auto decl_fn = data.decl->parent_fn) {
            if (decl_fn != scope.parent_fn_node) {
                data.decl->escape.escaped = true;
                auto &fn_def = scope.parent_fn_node->data.fn_def;
                auto &captures = fn_def.captures;
                auto &capture_map = fn_def.capture_map;

                // deduplicate captures by the declaration
                auto existing = capture_map.get(data.decl);
                if (!existing) {
                    auto idx = captures.size;
                    data.decl->escape.local_index = idx;
                    captures.add(data.decl);
                    capture_map[data.decl] = idx;
                } else {
                    data.decl->escape.local_index = *existing;
                }
            }
        }
        if (data.decl->type == NodeType::VarDecl && !scope.is_lhs) {
            auto init_at = data.decl->data.var_decl.initialized_at;
            if (!init_at || init_at->token->pos.offset > node->token->pos.offset) {
                error(data.decl, errors::VARIABLE_USED_BEFORE_INITIALIZED, data.decl->name);
            }
        }
        return type;
    }
    case NodeType::TypeSigil: {
        auto &data = node->data.sigil_type;
        auto type = resolve_value(data.type, scope);
        return create_type_symbol({}, get_pointer_type(type, get_sigil_type_kind(data.sigil)));
    }
    case NodeType::ParamDecl: {
        auto &data = node->data.param_decl;
        auto result = resolve_value(data.type, scope);
        return result;
    }
    case NodeType::VarDecl: {
        auto &data = node->data.var_decl;
        ChiType *var_type = nullptr;
        if (data.type) {
            var_type = resolve_value(data.type, scope);
        }
        if (data.expr) {
            auto var_scope = var_type ? scope.set_value_type(var_type) : scope;
            var_scope = var_scope.set_move_outlet(node);
            auto expr_type = resolve(data.expr, var_scope);
            if (var_type) {
                if (data.expr->type != NodeType::ConstructExpr ||
                    data.expr->data.construct_expr.type) {
                    check_assignment(data.expr, expr_type, var_type);
                }
            } else {
                var_type = expr_type;
            }
        }
        if (data.is_const) {
            data.resolved_value = resolve_constant_value(data.expr);
            return var_type;
        }
        return var_type;
    }
    case NodeType::BinOpExpr: {
        auto &data = node->data.bin_op_expr;
        auto op1_scope = scope;
        if (data.op_type == TokenType::ASS) {
            op1_scope = scope.set_is_lhs(true);
        }
        auto t1 = resolve(data.op1, op1_scope);

        ChiType *t2;
        if (data.op_type == TokenType::ASS) {
            auto var = data.op1->get_decl();
            if (var && var->type == NodeType::VarDecl && !var->data.var_decl.initialized_at) {
                if (!var->data.var_decl.is_field || scope.parent_fn->name != "new") {
                    var->data.var_decl.initialized_at = node;
                }
            }
            auto var_scope = scope.set_value_type(t1).set_move_outlet(data.op1);
            t2 = resolve(data.op2, var_scope);
        } else {
            t2 = resolve(data.op2, scope);
        }
        check_assignment(data.op2, t2, t1);
        switch (data.op_type) {
        case TokenType::EQ:
        case TokenType::LT:
        case TokenType::LE:
        case TokenType::GT:
        case TokenType::GE:
            return get_system_types()->bool_;
        default:
            check_binary_op(node, data.op_type, t1);
            return t1;
        }
    }
    case NodeType::UnaryOpExpr: {
        auto &data = node->data.unary_op_expr;
        auto t = resolve(data.op1, scope);
        switch (auto tt = data.op_type) {
        case TokenType::SUB:
        case TokenType::ADD:
        case TokenType::INC:
        case TokenType::DEC:
            check_assignment(data.op1, t, get_system_types()->int_);
            return t->kind == TypeKind::Bool ? get_system_types()->int_ : t;
        case TokenType::MUL: {
            if (ChiTypeStruct::is_pointer_type(t) && t->get_elem()->kind != TypeKind::Void) {
                return t->get_elem();
            } else {
                goto invalid;
            }
            break;
        }
        case TokenType::LNOT: {
            if (data.is_suffix) {
                if (ChiTypeStruct::is_pointer_type(t) && t->get_elem()->kind != TypeKind::Void) {
                    return t->get_elem();
                } else if (t->kind == TypeKind::Optional) {
                    return t->get_elem();
                } else if (t->kind == TypeKind::Box) {
                    return t->get_elem();
                }
                goto invalid;
            } else {
                check_assignment(data.op1, t, get_system_types()->bool_);
                return get_system_types()->bool_;
            }
            break;
        }
        case TokenType::AND: {
            if (!is_addressable(data.op1)) {
                error(node, errors::CANNOT_GET_REFERENCE_UNADDRESSABLE);
            }
            if (scope.is_escaping) {
                auto decl = find_root_decl(data.op1);
                if (decl) {
                    decl->escape.escaped = decl->can_escape();
                } else {
                    decl->escape.escaped = false;
                }
            }
            return get_pointer_type(t, TypeKind::Reference);
        }
        default:
            unreachable();
        }
    invalid:
        error(data.op1, errors::INVALID_OPERATOR, get_token_symbol(data.op_type), to_string(t));
        return nullptr;
    }
    case NodeType::TryExpr: {
        auto &data = node->data.try_expr;
        auto expr_type = resolve(data.expr, scope);
        if (data.expr->type != NodeType::FnCallExpr) {
            error(data.expr, errors::TRY_NOT_CALL);
        }
        scope.parent_fn_def().has_try = true;
        return get_result_type(expr_type, get_system_types()->error);
    }
    case NodeType::CastExpr: {
        auto &data = node->data.cast_expr;
        auto dest_type = resolve_value(data.dest_type, scope);
        check_cast(node, resolve(data.expr, scope), dest_type);
        return dest_type;
    }
    case NodeType::LiteralExpr: {
        auto token = node->token;
        switch (token->type) {
        case TokenType::BOOL:
            return get_system_types()->bool_;
        case TokenType::NULLP:
            return get_system_types()->null_ptr;
        case TokenType::INT:
            return get_system_types()->int_;
        case TokenType::STRING:
            return get_system_types()->string;
        case TokenType::FLOAT:
            return get_system_types()->float_;
        default:
            unreachable();
        }
    }
    case NodeType::ReturnStmt: {
        auto &data = node->data.return_stmt;
        auto expr_scope =
            scope.set_is_escaping(true).set_value_type(scope.parent_fn->data.fn.return_type);
        auto expr_type = data.expr ? resolve(data.expr, expr_scope) : get_system_types()->void_;
        assert(scope.parent_fn);
        auto return_type = scope.parent_fn->data.fn.return_type;
        check_assignment(data.expr, expr_type, return_type);
        return return_type;
    }
    case NodeType::ParenExpr: {
        auto &child = node->data.child_expr;
        return resolve(child, scope);
    }
    case NodeType::DotExpr: {
        auto &data = node->data.dot_expr;
        auto field_name = data.field->str;
        auto expr_type = resolve(data.expr, scope, flags);
        if (expr_type->kind == TypeKind::Fn) {
            expr_type = get_lambda_for_fn(expr_type);
        } else if (expr_type->kind == TypeKind::Module) {
            auto symbol = expr_type->data.module.scope->find_export(field_name);
            if (!symbol) {
                error(node, errors::MEMBER_NOT_FOUND, field_name, to_string(expr_type));
                return nullptr;
            }
            data.resolved_decl = symbol;
            return symbol->resolved_type;
        }
        auto member = get_struct_member(expr_type, field_name);
        if (!member) {
            error(node, errors::MEMBER_NOT_FOUND, field_name, to_string(expr_type));
            return nullptr;
        }
        data.resolved_member = member;
        data.resolved_decl = member->node;
        if (resolve_struct_type(expr_type)->is_generic()) {
            data.resolve_variant = true;
        }
        return member->resolved_type;
    }
    case NodeType::ConstructExpr: {
        auto &data = node->data.construct_expr;
        if (scope.move_outlet && !data.is_new) {
            data.resolved_outlet = scope.move_outlet;
            node->escape.moved = true;
        }
        ChiType *value_type;
        ChiType *result_type;
        if (data.type) {
            value_type = resolve_value(data.type, scope);
            result_type =
                data.is_new ? get_pointer_type(value_type, TypeKind::Reference) : value_type;
        } else {
            if (!scope.value_type) {
                error(node, errors::CONSTRUCT_CANNOT_INFER_TYPE);
                return nullptr;
            }
            result_type = scope.value_type;
            if (data.is_new != result_type->is_raw_pointer()) {
                error(node, errors::CONSTRUCT_CANNOT_INFER_TYPE);
            }
            value_type = data.is_new ? result_type->get_elem() : result_type;
        }
        auto constructor = ChiTypeStruct::get_constructor(value_type);
        if (constructor) {
            auto &fn_type = constructor->resolved_type->data.fn;
            resolve_fn_call(node, scope, &fn_type, &data.items);
        } else {
            if (result_type->kind == TypeKind::Optional) {
                if (data.items.size != 1) {
                    error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, 1, data.items.size);
                    return nullptr;
                }
                auto item = data.items[0];
                auto item_type = resolve(item, scope, flags);
                check_assignment(item, item_type, result_type->get_elem());
                return result_type;
            } else {
                if (data.items.size != 0) {
                    error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, 0, data.items.size);
                    return nullptr;
                }
            }
        }
        return result_type;
    }
    case NodeType::FnCallExpr: {
        auto &data = node->data.fn_call_expr;
        auto fn_type = resolve(data.fn_ref_expr, scope);
        if (fn_type->kind != TypeKind::Fn && fn_type->kind != TypeKind::FnLambda) {
            error(data.fn_ref_expr, errors::CANNOT_CALL_NON_FUNCTION);
            return nullptr;
        }
        if (fn_type->kind == TypeKind::FnLambda) {
            auto &fn_lambda = fn_type->data.fn_lambda;
            fn_type = fn_lambda.fn;
        }
        auto &fn = fn_type->data.fn;
        resolve_fn_call(node, scope, &fn, &data.args);
        return fn.return_type;
    }
    case NodeType::IfStmt: {
        auto &data = node->data.if_stmt;
        auto cond_type = resolve(data.condition, scope);
        if (data.condition->type == NodeType::Identifier && cond_type->kind == TypeKind::Optional) {
            auto name = data.condition->token->get_name();
            auto expr = create_node(ast::NodeType::UnaryOpExpr);
            expr->data.unary_op_expr.is_suffix = true;
            expr->data.unary_op_expr.op_type = TokenType::LNOT;
            expr->data.unary_op_expr.op1 = data.condition;
            expr->resolved_type = cond_type->get_elem();
            auto var = get_dummy_var(name, expr);
            var->token = data.condition->token;
            var->parent_fn = scope.parent_fn_node;
            var->resolved_type = cond_type->get_elem();
            var->data.var_decl.initialized_at = node;
            auto &block_data = data.then_block->data.block;
            block_data.implicit_vars.add(var);
            block_data.scope->put(name, var);
        }
        check_assignment(data.condition, cond_type, get_system_types()->bool_);
        resolve(data.then_block, scope);
        if (data.else_node) {
            resolve(data.else_node, scope);
        }
        return nullptr;
    }
    case NodeType::Block: {
        auto &data = node->data.block;
        auto child_scope = scope.set_block(&data);
        for (auto stmt : data.statements) {
            resolve(stmt, child_scope);
        }
        return nullptr;
    }
    case NodeType::StructDecl: {
        auto &data = node->data.struct_decl;
        ChiType *type_sym;
        ChiType *struct_type;
        ChiTypeStruct *struct_;
        if (!node->resolved_type) {
            struct_type = create_type(TypeKind::Struct);
            struct_type->name = node->name;
            struct_ = &struct_type->data.struct_;
            struct_->node = node;
            struct_->kind = data.kind;

            // first pass, all members are skipped
            for (auto param : data.type_params) {
                struct_->type_params.add(resolve(param, scope));
            }
            struct_->resolve_status = ResolveStatus::None;
            type_sym = create_type_symbol(node->name, struct_type);
            if (data.kind == ContainerKind::Enum) {
                type_sym->data.type_symbol.giving_type = get_system_types()->int_;
            }
            return type_sym;
        }
        type_sym = node->resolved_type;
        struct_type = type_sym->data.type_symbol.underlying_type;
        struct_ = &struct_type->data.struct_;
        auto struct_scope = scope.set_parent_struct(struct_type);
        if (struct_->resolve_status == ResolveStatus::None) {
            // second pass
            scope.next_enum_value = 0;
            for (auto member : data.members) {
                resolve_struct_member(struct_type, member, struct_scope);
            }
            struct_->resolve_status = ResolveStatus::MemberTypesKnown;
        } else if (struct_->resolve_status == ResolveStatus::MemberTypesKnown) {
            // third pass
            for (auto member : data.members) {
                if (member->type == NodeType::VarDecl && member->data.var_decl.is_embed) {
                    resolve_struct_embed(struct_type, member, scope);
                }
            }
            for (auto implement : data.implements) {
                auto impl_trait = resolve_value(implement, scope);
                if (!ChiTypeStruct::is_interface(impl_trait)) {
                    error(implement, errors::NON_INTERFACE_IMPL_TYPE, to_string(impl_trait));
                }
                resolve_vtable(impl_trait, struct_type, implement);
            }
            struct_->resolve_status = ResolveStatus::EmbedsResolved;
        } else {
            // fourth pass
            for (auto member : data.members) {
                if (member->type == NodeType::FnDef) {
                    auto fn_type = node_get_type(member);
                    auto fn_scope = struct_scope.set_parent_fn(fn_type).set_parent_fn_node(member);
                    if (auto body = member->data.fn_def.body) {
                        resolve(body, fn_scope);
                    }
                }
            }

            for (auto member : data.members) {
                if (member->type == NodeType::VarDecl && !member->data.var_decl.initialized_at) {
                    auto not_needed = member->data.var_decl.is_embed &&
                                      ChiTypeStruct::get_constructor(struct_type);
                    if (!not_needed) {
                        error(member, errors::UNINITIALIZED_FIELD, member->name,
                              to_string(struct_type));
                    }
                }
            }

            struct_->resolve_status = ResolveStatus::Done;
        }
        return type_sym;
    }
    case NodeType::SubtypeExpr: {
        auto &data = node->data.subtype_expr;
        auto type = to_value_type(resolve(data.type, scope));
        if (type->kind == TypeKind::Array || type->kind == TypeKind::Optional ||
            type->kind == TypeKind::Box || type->kind == TypeKind::Promise) {
            if (data.args.size != 1) {
                error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS,
                      to_string(get_system_type(type->kind)), 1, data.args.size);
            }
            auto elem_type = to_value_type(resolve(data.args[0], scope));
            return get_wrapped_type(elem_type, type->kind);
        }
        auto &params = type->data.struct_.type_params;
        if (params.size != data.args.size) {
            error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS, to_string(type), params.size,
                  data.args.size);
            return nullptr;
        }
        array<ChiType *> args;
        for (auto arg : data.args) {
            args.add(resolve_value(arg, scope));
        }
        auto subtype = get_subtype(type, &args);
        return create_type_symbol({}, subtype);
    }
    case NodeType::IndexExpr: {
        auto &data = node->data.index_expr;
        auto expr_type = resolve(data.expr, scope);
        auto subscript_type = resolve(data.subscript, scope);

        switch (expr_type->kind) {
        case TypeKind::Pointer:
            check_assignment(data.subscript, subscript_type, get_system_types()->int_);
            break;
        case TypeKind::Struct:
        case TypeKind::Subtype: {
            auto method = ChiTypeStruct::get_symbol(expr_type, IntrinsicSymbol::OpIndex);
            if (!method) {
                error(node, errors::CANNOT_SUBSCRIPT, to_string(expr_type));
                return nullptr;
            }
            auto index_type = method->resolved_type->data.fn.get_param_at(0);
            check_assignment(data.subscript, subscript_type, index_type);
            data.resolved_method = method;
            return method->resolved_type->data.fn.return_type->get_elem();
        }
        default:
            error(node, errors::CANNOT_SUBSCRIPT, to_string(expr_type));
            return nullptr;
        }
        return expr_type->get_elem();
    }
    case NodeType::EnumMember: {
        auto &data = node->data.enum_member;
        if (data.value) {
            auto value_type = resolve(data.value, scope);
            check_assignment(data.value, value_type, get_system_types()->int_);
            data.resolved_value = get<int64_t>(resolve_constant_value(data.value));
            scope.next_enum_value = data.resolved_value + 1;
        } else {
            data.resolved_value = scope.next_enum_value++;
        }
        return get_system_types()->int_;
    }
    case NodeType::ForStmt: {
        auto &data = node->data.for_stmt;
        if (data.init) {
            resolve(data.init, scope);
        }
        if (data.condition) {
            auto cond_type = resolve(data.condition, scope);
            check_assignment(data.condition, cond_type, get_system_types()->bool_);
        }
        if (data.post) {
            resolve(data.post, scope);
        }
        if (data.expr) {
            auto expr_type = resolve(data.expr, scope);
            auto sty = resolve_struct_type(expr_type);
            if (!sty || !sty->intrinsics.get(IntrinsicSymbol::Iterable)) {
                error(node, errors::FOR_EXPR_NOT_ITERABLE, to_string(expr_type));
                return nullptr;
            }

            if (data.bind) {
                auto index_fn = sty->member_intrinsics.get(IntrinsicSymbol::OpIndex);
                if (!index_fn) {
                    error(node, errors::CANNOT_INDEX, to_string(expr_type));
                    return nullptr;
                }
                auto ref_type = (*index_fn)->resolved_type->data.fn.return_type;
                auto bind_scope = scope.set_value_type(ref_type);
                resolve(data.bind, bind_scope);
            }
        }
        auto loop_scope = scope.set_parent_loop(node);
        resolve(data.body, loop_scope);
        return nullptr;
    }
    case NodeType::BranchStmt: {
        if (!scope.parent_loop) {
            error(node, errors::STMT_NOT_WITHIN_LOOP, node->token->to_string());
        }
        return nullptr;
    }
    case NodeType::TypeParam: {
        auto &data = node->data.type_param;
        auto phty = create_type(TypeKind::Placeholder);
        phty->name = node->name;
        if (data.type) {
            phty->data.placeholder.trait = to_value_type(resolve(data.type, scope));
        }
        phty->data.placeholder.index = data.index;
        phty->is_placeholder = true;
        return create_type_symbol(node->name, phty);
    }
    case NodeType::PrefixExpr: {
        auto &data = node->data.prefix_expr;
        switch (data.prefix->type) {
        case TokenType::KW_DELETE: {
            auto expr_type = resolve(data.expr, scope);
            if (!expr_type->is_raw_pointer()) {
                error(node, errors::INVALID_OPERATOR, data.prefix->to_string(),
                      to_string(expr_type));
            }
            return get_system_types()->void_;
        }
        case TokenType::KW_SIZEOF: {
            auto type = resolve_value(data.expr, scope);
            data.expr->resolved_type = type;
            return get_system_types()->int_;
        }
        default:
            panic("unhandled prefix operator {}", data.prefix->to_string());
        }
        break;
    }
    case NodeType::ExternDecl: {
        auto &data = node->data.extern_decl;
        if (!node->resolved_type) {
            node->resolved_type = get_system_types()->void_;
            for (auto member : data.members) {
                resolve(member, scope);
            }
        }
        return node->resolved_type;
    }
    case NodeType::Error: {
        node->resolved_type = get_system_types()->void_;
        return node->resolved_type;
    }
    case NodeType::ImportDecl: {
        auto &data = node->data.import_decl;
        auto path = m_ctx->allocator->find_module_path(data.path->str, scope.module->path);
        if (path.empty()) {
            error(node, errors::MODULE_NOT_FOUND, data.path->str);
            return nullptr;
        }

        auto src = io::Buffer::from_file(path);
        auto module = m_ctx->allocator->process_source(scope.module->package, &src, path);
        Resolver resolver(m_ctx);
        resolver.resolve(module);

        data.resolved_module = module;
        auto type = create_type(TypeKind::Module);
        type->name = "Module:" + module->full_path();
        type->data.module.scope = module->scope;
        scope.module->imports.add(module);

        for (auto symbol : data.symbols) {
        }
        return type;
    }
    case NodeType::BindIdentifier: {
        return scope.value_type;
    }
    case NodeType::ImportSymbol: {
        auto &data = node->data.import_symbol;
        auto module = data.import->data.import_decl.resolved_module;
        auto decl = module->scope->find_one(data.name->get_name());
        if (!decl) {
            error(node, errors::SYMBOL_NOT_FOUND_MODULE, data.name->get_name(), module->path);
            return nullptr;
        }
        data.resolved_decl = decl;
        return decl->resolved_type;
    }
    default:
        print("\n");
        panic("unhandled node {}", PRINT_ENUM(node->type));
    }
    return nullptr;
}

ChiType *Resolver::resolve(ast::Node *node, ResolveScope &scope, uint32_t flags) {
    auto cached = node_get_type(node);
    if (cached) {
        return cached;
    }
    auto result = _resolve(node, scope, flags);
    node->resolved_type = result;

    if (node->type == NodeType::VarDecl || node->type == NodeType::ParamDecl) {
        if (scope.parent_fn_node && should_destroy(node)) {
            scope.parent_fn_def().cleanup_vars.add(node);
        }
    }
    if (!result)
        return nullptr;
    return result;
}

string Resolver::to_string(ChiType *type) {
    assert(type);
    if (type->name) {
        return *type->name;
    }
    if (type->display_name) {
        return *type->display_name;
    }
    switch (type->kind) {
    case TypeKind::Subtype: {
        auto &data = type->data.subtype;
        std::stringstream ss;
        ss << to_string(data.generic) << "<";
        for (int i = 0; i < data.args.size; i++) {
            ss << to_string(data.args[i]);
            if (i < data.args.size - 1) {
                ss << ",";
            }
        }
        ss << ">";
        return ss.str();
    }
    case TypeKind::String:
        return "string";
    case TypeKind::Pointer:
        return "*" + to_string(type->get_elem());
    case TypeKind::Reference:
        return "&" + to_string(type->get_elem());
    case TypeKind::Optional:
        return "?" + to_string(type->get_elem());
    case TypeKind::Box:
        return "^" + to_string(type->get_elem());
    case TypeKind::Array:
        return fmt::format("Array<{}>", to_string(type->get_elem()));
    case TypeKind::Result:
        return fmt::format("Result<{},{}>", to_string(type->get_elem()),
                           to_string(type->data.result.error));
    default:
        break;
    }
    assert(type->kind < TypeKind::__COUNT);
    return to_string(type->kind, &type->data);
}

string Resolver::to_string(TypeKind kind, ChiType::Data *data) {
    switch (kind) {
    case TypeKind::Struct: {
        auto &struct_ = data->struct_;
        std::stringstream ss;
        ss << "struct ";
        if (struct_.type_params.size > 0) {
            ss << "<";
            for (int i = 0; i < struct_.type_params.size; i++) {
                ss << to_string(struct_.type_params[i]);
                if (i < struct_.type_params.size - 1) {
                    ss << ",";
                }
            }
            ss << ">";
        }
        ss << "{";
        for (int i = 0; i < struct_.members.size; i++) {
            auto &member = struct_.members[i];
            ss << to_string(member->resolved_type);
            if (i < struct_.members.size - 1) {
                ss << ",";
            }
        }
        ss << "}";
        return ss.str();
    }
    case TypeKind::Fn: {
        auto &fn = data->fn;
        std::stringstream ss;
        ss << "func(";
        if (fn.container_ref) {
            ss << to_string(fn.container_ref);
            if (fn.params.size > 0) {
                ss << ", ";
            }
        }
        for (int i = 0; i < fn.params.size; i++) {
            if (fn.is_variadic && i == fn.params.size - 1) {
                ss << "...";
            }
            ss << to_string(fn.params[i]);
            if (i < fn.params.size - 1) {
                ss << ",";
            }
        }
        ss << ")";
        if (fn.return_type) {
            ss << " " << to_string(fn.return_type);
        }
        return ss.str();
    }
    case TypeKind::FnLambda: {
        auto &fn_lambda = data->fn_lambda;
        return fmt::format("Lambda<{}>", to_string(fn_lambda.fn));
    }
    default:
        break;
    }
    return PRINT_ENUM(kind);
}

void Resolver::check_assignment(ast::Node *value, ChiType *from_type, ChiType *to_type) {
    if (!can_assign(from_type, to_type)) {
        error(value, errors::CANNOT_CONVERT, to_string(from_type), to_string(to_type));
    }
}

bool Resolver::is_addressable(ast::Node *node) {
    switch (node->type) {
    case NodeType::Identifier:
    case NodeType::DotExpr:
    case NodeType::IndexExpr:
        return true;

    case NodeType::ParenExpr:
        return is_addressable(node->data.child_expr);

    case NodeType::UnaryOpExpr: {
        auto &data = node->data.unary_op_expr;
        return data.is_suffix && data.op_type == TokenType::LNOT;
    }

    default:
        return false;
    }
}

void Resolver::check_cast(ast::Node *value, ChiType *from_type, ChiType *to_type) {
    check_assignment(value, from_type, to_type);
}

void Resolver::context_init_builtins(ast::Module *builtin_module) {
    for (auto &decl : builtin_module->exports) {
        m_ctx->builtins.add(decl);
    }
}

string Resolver::resolve_term_string(ast::Node *term) {
    switch (term->type) {
    case NodeType::Identifier:
        return term->name;
    case NodeType::DotExpr:
        return resolve_term_string(term->data.dot_expr.expr) + "." +
               term->data.dot_expr.field->get_name();
    default:
        panic("unhandled term node: {}", PRINT_ENUM(term->type));
    }
    return "";
}

ChiStructMember *Resolver::resolve_struct_member(ChiType *struct_type, ast::Node *node,
                                                 ResolveScope &scope) {
    auto &struct_ = struct_type->data.struct_;
    auto member = struct_.add_member(node->name, node, nullptr);
    if (node->type == NodeType::VarDecl) {
        member->resolved_type = resolve(node, scope);
        node->data.var_decl.resolved_field = member;
    } else if (node->type == NodeType::FnDef) {
        member->resolved_type = resolve(node, scope);
        for (auto attr : node->get_declspec().attributes) {
            auto term_string = resolve_term_string(attr->data.decl_attribute.term);
            auto sym = m_ctx->intrinsic_symbols.get(term_string);
            if (sym) {
                member->symbol = *sym;
                struct_.member_intrinsics[member->symbol] = member;
                if (*sym == IntrinsicSymbol::IterBegin) {
                    struct_.intrinsics[IntrinsicSymbol::Iterable] = true;
                }
            } else {
                error(node, errors::INVALID_ATTRIBUTE_TERM, term_string);
                continue;
            }
        }
    } else if (node->type == NodeType::EnumMember) {
        member->resolved_type = resolve(node, scope);
    }
    return member;
}

void Resolver::resolve_vtable(ChiType *base_type, ChiType *derived_type, ast::Node *base_node) {
    auto &base = base_type->data.struct_;
    auto &derived = derived_type->data.struct_;
    InterfaceImpl *iface_impl = nullptr;
    if (base.kind == ContainerKind::Interface) {
        iface_impl = derived.add_interface(base_type, derived_type);
    }
    for (auto &base_member : base.members) {
        auto node = base_member->node;
        if (base_member->is_method()) {
            auto child_method = derived.find_member(node->name);
            if (node->data.fn_def.body) {
                if (!child_method) {
                    child_method = derived.add_member(node->name, node, base_member->resolved_type);
                    child_method->orig_parent = base_type;
                }
            } else if (!child_method) {
                error(base_node, errors::METHOD_NOT_IMPLEMENTED, node->name);
                break;
            }
            if (!compare_impl_type(base_member->resolved_type, child_method->resolved_type)) {
                error(base_node, errors::IMPLEMENT_NOT_MATCH, node->name, to_string(base_type));
                break;
            }
            if (iface_impl) {
                assert(child_method);
                iface_impl->impl_members.add(child_method);
            }
        }
        if (base_member->is_field()) {
            auto child_field = derived.find_member(node->name);
            if (!child_field) {
                child_field = derived.add_member(node->name, node, base_member->resolved_type);
                child_field->orig_parent = base_type;
                child_field->parent_member = base_node->data.var_decl.resolved_field;
                child_field->field_index = base_member->field_index;
            }
        }
    }
    for (auto &impl : base.interfaces) {
        resolve_vtable(impl->interface_type, derived_type, base_node);
    }
}

void Resolver::resolve_struct_embed(ChiType *struct_type, ast::Node *base_node,
                                    ResolveScope &parent_scope) {
    auto &current = struct_type->data.struct_;
    auto em_type = node_get_type(base_node);
    if (em_type->kind != TypeKind::Struct) {
        error(base_node, errors::INVALID_EMBED);
        return;
    }
    auto &base = em_type->data.struct_;
    if (base.kind != ContainerKind::Struct && base.kind != ContainerKind::Interface) {
        error(base_node, errors::INVALID_EMBED);
        return;
    }
    if (current.kind != base.kind) {
        error(base_node, errors::CANNOT_EMBED_INTO, to_string(em_type), to_string(struct_type));
    }
    if (base.resolve_status < ResolveStatus::Done) {
        _resolve(base.node, parent_scope);
    }
    resolve_vtable(em_type, struct_type, base_node);
}

bool Resolver::should_destroy(ast::Node *node) {
    auto is_managed = has_lang_flag(node->module->get_lang_flags(), LANG_FLAG_MANAGED);
    auto is_on_heap = is_managed && node->is_heap_allocated();
    return !is_on_heap && ChiTypeStruct::get_destructor(node_get_type(node));
}

bool Resolver::should_resolve_fn_body(ResolveScope &scope) {
    auto parent_struct = scope.parent_struct;
    if (!parent_struct) {
        return !scope.skip_fn_bodies;
    }
    auto &struct_ = parent_struct->data.struct_;
    return struct_.kind != ContainerKind::Interface &&
           struct_.resolve_status >= ResolveStatus::MemberTypesKnown;
}

void Resolver::type_placeholders_sub_each(TypeList *list, ChiTypeSubtype *subs, TypeList *output) {
    for (auto arg : *list) {
        output->add(type_placeholders_sub(arg, subs));
    }
}

ChiType *Resolver::type_placeholders_sub(ChiType *type, ChiTypeSubtype *subs) {
    switch (type->kind) {
    case TypeKind::Placeholder:
        return subs->args[type->data.placeholder.index];

    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::Optional:
    case TypeKind::Box:
        return get_wrapped_type(type_placeholders_sub(type->get_elem(), subs), type->kind);

    case TypeKind::Subtype: {
        auto &data = type->data.subtype;
        array<ChiType *> args;
        type_placeholders_sub_each(&data.args, subs, &args);
        return get_subtype(data.generic, &args);
    }
    case TypeKind::Fn: {
        auto &data = type->data.fn;
        auto return_type = type_placeholders_sub(data.return_type, subs);
        array<ChiType *> params = {};
        type_placeholders_sub_each(&data.params, subs, &params);
        return get_fn_type(return_type, &params, data.is_variadic, data.container_ref->get_elem());
    }
    default:
        return type;
    }
}

ChiTypeStruct *Resolver::resolve_struct_type(ChiType *type) {
    auto sty = type;
    if (sty->kind == TypeKind::This) {
        sty = sty->get_elem();
    }
    if (sty->is_pointer()) {
        sty = sty->get_elem();
    }
    if (sty->kind == TypeKind::Array) {
        sty = sty->data.array.internal;
    } else if (sty->kind == TypeKind::Result) {
        sty = sty->data.result.internal;
    } else if (sty->kind == TypeKind::FnLambda) {
        sty = sty->data.fn_lambda.internal;
    } else if (sty->kind == TypeKind::Promise) {
        sty = sty->data.promise.internal;
    } else if (sty->kind == TypeKind::TypeSymbol) {
        if (auto underlying_type = sty->data.type_symbol.underlying_type) {
            sty = underlying_type;
        }
    } else if (sty->kind == TypeKind::Subtype) {
        sty = resolve_subtype(sty);
    }
    if (sty->kind != TypeKind::Struct) {
        return nullptr;
    }
    return &sty->data.struct_;
}

ChiStructMember *Resolver::get_struct_member(ChiType *struct_type, const string &field_name) {
    auto sty = resolve_struct_type(struct_type);
    return sty->find_member(field_name);
}

void Resolver::resolve_fn_call(ast::Node *node, ResolveScope &scope, ChiTypeFn *fn,
                               NodeList *args) {
    auto n_args = args->size;
    auto n_params = fn->params.size;
    auto params_required = n_params - (fn->is_variadic ? 1 : 0);
    bool ok = fn->is_variadic ? n_args >= params_required : n_args == n_params;
    if (!ok) {
        error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, params_required, n_args);
        return;
    }
    for (size_t i = 0; i < n_args; i++) {
        auto param_type = fn->get_param_at(i);
        auto arg = args->at(i);
        scope.value_type = param_type;
        auto arg_type = resolve(arg, scope);
        check_assignment(arg, arg_type, param_type);
    }
    scope.value_type = nullptr;
}

ChiType *Resolver::create_pointer_type(ChiType *elem, TypeKind kind) {
    auto pt = create_type(kind);
    pt->data.pointer.elem = elem;
    pt->is_placeholder = elem->is_placeholder;
    return pt;
}

ChiType *Resolver::get_pointer_type(ChiType *elem, TypeKind kind) {
    auto &m = m_ctx->pointer_of[(int)kind];
    if (auto cached = m.get(elem)) {
        return *cached;
    }
    auto pt = create_pointer_type(elem, kind);
    m[elem] = pt;
    return pt;
}

ChiType *Resolver::get_array_type(ChiType *elem) {
    if (auto cached = m_ctx->array_of.get(elem)) {
        return *cached;
    }
    auto type = create_type(TypeKind::Array);
    type->data.array.elem = elem;
    m_ctx->array_of[elem] = type;

    auto stype = create_type(TypeKind::Struct);
    auto &struct_ = stype->data.struct_;
    struct_.add_member("data", get_dummy_var("data"), get_pointer_type(elem));
    struct_.add_member("size", get_dummy_var("size"), get_system_types()->uint32);
    struct_.add_member("capacity", get_dummy_var("capacity"), get_system_types()->uint32);
    struct_.add_member("flags", get_dummy_var("flags"), get_system_types()->uint8);

    struct_.kind = ContainerKind::Struct;
    type->data.promise.internal = stype;

    return type;
}

ChiType *Resolver::get_promise_type(ChiType *value) {
    if (auto cached = m_ctx->promise_of.get(value)) {
        return *cached;
    }
    auto type = create_type(TypeKind::Promise);
    type->data.promise.value = value;
    m_ctx->promise_of[value] = type;

    auto stype = create_type(TypeKind::Struct);
    auto &struct_ = stype->data.struct_;
    type->data.promise.internal = stype;
    TypeList cb_params = {};
    struct_.add_member(
        "callback", get_dummy_var("callback"),
        get_lambda_for_fn(get_fn_type(get_system_types()->void_, &cb_params, false)));
    struct_.kind = ContainerKind::Struct;
    return type;
}

ast::Node *Resolver::get_dummy_var(const string &name, ast::Node *expr) {
    auto node = create_node(NodeType::VarDecl);
    node->name = name;
    node->data.var_decl.is_generated = true;
    node->data.var_decl.expr = expr;
    return node;
}

ChiType *Resolver::get_fn_type(ChiType *ret, TypeList *params, bool is_variadic,
                               ChiType *container) {
    ChiTypeFn fn;
    fn.return_type = ret;
    fn.is_variadic = is_variadic;
    fn.params = *params;
    if (container) {
        fn.container_ref = get_pointer_type(container, TypeKind::Reference);
    }

    auto key = to_string(TypeKind::Fn, (ChiType::Data *)&fn);
    if (auto cached = m_ctx->composite_types.get(key)) {
        return *cached;
    }
    auto type = create_type(TypeKind::Fn);
    type->data.fn = fn;
    m_ctx->composite_types[key] = type;
    for (auto param : fn.params) {
        if (param->is_placeholder) {
            type->is_placeholder = true;
            break;
        }
    }
    type->is_placeholder = type->is_placeholder || ret->is_placeholder;
    return type;
}

ChiType *Resolver::get_lambda_for_fn(ChiType *fn_type) {
    auto lambda = create_type(TypeKind::FnLambda);
    lambda->data.fn_lambda.fn = fn_type;

    auto struct_type = create_type(TypeKind::Struct);
    auto &struct_ = struct_type->data.struct_;
    struct_type->display_name = "Lambda<" + to_string(fn_type) + ">" + "::Internal";

    struct_.kind = ContainerKind::Struct;
    struct_.add_member("ptr", get_dummy_var("ptr"), get_pointer_type(get_system_types()->void_));
    struct_.add_member("size", get_dummy_var("size"), get_system_types()->uint32);
    struct_.add_member("data", get_dummy_var("data"), get_pointer_type(get_system_types()->void_));

    lambda->data.fn_lambda.internal = struct_type;

    auto bound_type = create_type(TypeKind::Fn);
    lambda->data.fn_lambda.bound_fn = bound_type;
    auto bstruct = create_type(TypeKind::Struct);
    lambda->data.fn_lambda.bind_struct = bstruct;
    auto &bound_fn = bound_type->data.fn;

    auto &fn_data = fn_type->data.fn;
    bound_fn.params.add(get_system_types()->void_ref);
    for (auto param : fn_data.params) {
        bound_fn.params.add(param);
    }
    bound_fn.return_type = fn_data.return_type;
    bound_fn.is_variadic = fn_data.is_variadic;
    bound_fn.container_ref = fn_data.container_ref;
    return lambda;
}

ChiType *Resolver::get_result_type(ChiType *value, ChiType *err) {
    if (value->kind == TypeKind::Void) {
        value = get_system_types()->bool_;
    }
    auto key = fmt::format("Result<{},{}>", to_string(value), to_string(err));
    if (auto cached = m_ctx->composite_types.get(key)) {
        return *cached;
    }
    auto result_type = create_type(TypeKind::Result);
    auto &data = result_type->data.result;
    data.value = value;
    data.error = err;
    assert(to_string(result_type) == key);
    m_ctx->composite_types[key] = result_type;

    // create internal struct for accessing the fields
    data.internal = create_type(TypeKind::Struct);
    auto &struct_ = data.internal->data.struct_;
    auto dummy_node = create_node(NodeType::StructDecl);
    dummy_node->name = key;
    dummy_node->resolved_type = result_type;
    struct_.node = dummy_node;
    struct_.kind = ContainerKind::Struct;
    struct_.node = nullptr;

    auto err_optional = get_pointer_type(err, TypeKind::Optional);
    struct_.add_member("err", dummy_node, err_optional);
    struct_.add_member("value", dummy_node, value);
    return result_type;
}

ChiType *Resolver::create_int_type(int bit_count, bool is_unsigned) {
    auto type = create_type(TypeKind::Int);
    type->data.int_.bit_count = bit_count;
    type->data.int_.is_unsigned = is_unsigned;
    return type;
}

ChiType *Resolver::create_float_type(int bit_count) {
    auto type = create_type(TypeKind::Float);
    type->data.float_.bit_count = bit_count;
    return type;
}

ConstantValue Resolver::resolve_constant_value(ast::Node *node) {
    switch (node->type) {
    case NodeType::LiteralExpr: {
        auto token = node->token;
        switch (token->type) {
        case TokenType::INT: {
            return token->val.i;
        }
        case TokenType::FLOAT: {
            return token->val.d;
        }
        case TokenType::STRING: {
            return token->str;
        }
        case TokenType::BOOL: {
            return (int64_t)token->val.b;
        }
        case TokenType::NULLP: {
            return (int64_t)0;
        }
        default:
            break;
        }
        break;
    }

#define _BIN_OP_CAST(op1, op, op2, type) get<type>(op1) op get<type>(op2)
#define _BIN_OP_INT(op1, op, op2) _BIN_OP_CAST(op1, op, op2, const_int_t)

    case NodeType::BinOpExpr: {
        auto &data = node->data.bin_op_expr;
        auto a = resolve_constant_value(data.op1);
        auto b = resolve_constant_value(data.op2);
        switch (data.op_type) {
        case TokenType::ADD:
            return _BIN_OP_INT(a, +, b);
        case TokenType::SUB:
            return _BIN_OP_INT(a, -, b);
        case TokenType::LSHIFT:
            return _BIN_OP_INT(a, <<, b);
        default:
            break;
        }
    }

    case NodeType::ParenExpr: {
        auto child = node->data.child_expr;
        return resolve_constant_value(child);
    }

    case NodeType::UnaryOpExpr: {
        auto &data = node->data.unary_op_expr;
        switch (data.op_type) {
        case TokenType::SUB: {
            auto v = resolve_constant_value(data.op1);
            return -get<const_int_t>(v);
        }
        default:
            break;
        }
    }

    case NodeType::Identifier: {
        auto &data = node->data.identifier;
        if (data.decl) {
            return resolve_constant_value(data.decl);
        }
    }
    case NodeType::VarDecl: {
        auto &data = node->data.var_decl;
        if (data.is_const) {
            return data.resolved_value;
        }
    }
    default:
        break;
    }
    error(node, errors::VALUE_NOT_CONSTANT);
    return {};
}

bool Resolver::is_same_type(ChiType *a, ChiType *b) { return to_value_type(a) == to_value_type(b); }

ChiType *Resolver::get_subtype(ChiType *generic, TypeList *type_args) {
    assert(generic->kind == TypeKind::Struct);
    auto &gen = generic->data.struct_;
    for (auto subtype : gen.subtypes) {
        assert(subtype->kind == TypeKind::Subtype);
        auto &subtype_data = subtype->data.subtype;
        if (subtype_data.args.size != type_args->size) {
            continue;
        }
        bool matches = true;
        for (size_t i = 0; i < type_args->size; i++) {
            auto a = type_args->at(i);
            auto b = subtype_data.args[i];
            if (!is_same_type(a, b)) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return subtype;
        }
    }
    auto sub = create_type(TypeKind::Subtype);
    sub->data.subtype.generic = generic;
    for (auto arg : *type_args) {
        sub->data.subtype.args.add(arg);
        if (arg->is_placeholder) {
            sub->is_placeholder = true;
        }
    }
    gen.subtypes.add(sub);
    if (gen.resolve_status >= ResolveStatus::MemberTypesKnown) {
        resolve_subtype(sub);
    }
    return sub;
}

ChiType *Resolver::resolve_subtype(ChiType *subtype) {
    auto &data = subtype->data.subtype;
    if (data.resolved_struct) {
        return data.resolved_struct;
    }
    auto &base = data.generic->data.struct_;
    auto sty = create_type(TypeKind::Struct);
    sty->name = to_string(subtype);

    auto &scpy = sty->data.struct_;
    scpy.kind = base.kind;
    scpy.node = base.node;
    for (auto &member : base.members) {
        auto type = m_ctx->allocator->create_type(member->resolved_type->kind);
        member->resolved_type->clone(type);
        if (member->is_method()) {
            type->data.fn.container_ref = get_pointer_type(subtype, TypeKind::Reference);
        }

        type = type_placeholders_sub(type, &data);
        auto node = create_node(member->node->type);
        node->name = member->node->name;
        node->token = member->node->token;
        node->resolved_type = type;
        memcpy(&node->data, &member->node->data, sizeof(member->node->data));

        if (member->node->type == NodeType::FnDef) {
            node->data.fn_def.is_generated = true;
        }

        auto new_member = scpy.add_member(member->get_name(), node, type);
        if (member->symbol != IntrinsicSymbol::None) {
            scpy.member_intrinsics[member->symbol] = new_member;
        }
        member->variants[&data] = new_member;
    }
    scpy.intrinsics = base.intrinsics;
    data.resolved_struct = sty;
    return sty;
}

void Resolver::check_binary_op(ast::Node *node, TokenType op_type, ChiType *type) {
    if (is_assignment_op(op_type)) {
        return;
    }
    bool ok;
    switch (op_type) {
    case TokenType::ADD:
        ok = type_is_int(type) || type->kind == TypeKind::String;
        break;
    default:
        ok = type_is_int(type);
        break;
    }
    if (!ok) {
        error(node, errors::INVALID_OPERATOR, get_token_symbol(op_type), to_string(type));
    }
}

ChiType *Resolver::get_system_type(TypeKind kind) {
    auto types = get_system_types();
    switch (kind) {
    case TypeKind::Int:
        return types->int64;
    case TypeKind::Float:
        return types->double_;
    case TypeKind::String:
        return types->string;
    case TypeKind::Array:
        return types->array;
    case TypeKind::Optional:
        return types->optional;
    case TypeKind::Box:
        return types->box;
    case TypeKind::Bool:
        return types->bool_;
    case TypeKind::Void:
        return types->void_;
    default:
        panic("unhandled");
        return nullptr;
    }
}

ChiType *Resolver::get_wrapped_type(ChiType *elem, TypeKind kind) {
    switch (kind) {
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::Optional:
    case TypeKind::Box:
        return get_pointer_type(elem, kind);
    case TypeKind::Array:
        return get_array_type(elem);
    case TypeKind::Promise:
        return get_promise_type(elem);
    default:
        unreachable();
        return {};
    }
}

TypeKind Resolver::get_sigil_type_kind(cx::ast::SigilKind sigil) {
    switch (sigil) {
    case ast::SigilKind::Pointer:
        return TypeKind::Pointer;
    case ast::SigilKind::Reference:
        return TypeKind::Reference;
    case ast::SigilKind::Optional:
        return TypeKind::Optional;
    case ast::SigilKind::Box:
        return TypeKind::Box;
    default:
        unreachable();
        return {};
    }
}

ast::Node *Resolver::find_root_decl(ast::Node *node) {
    switch (node->type) {
    case NodeType::Identifier: {
        if (node->data.identifier.kind != ast::IdentifierKind::Value) {
            return node;
        }
        return node->data.identifier.decl;
    }
    case NodeType::DotExpr:
        return find_root_decl(node->data.dot_expr.expr);
    case NodeType::ParenExpr:
        return find_root_decl(node->data.child_expr);
    case NodeType::IndexExpr:
        return find_root_decl(node->data.index_expr.expr);
    default:
        panic("unhandled find_root_decl {}", PRINT_ENUM(node->type));
    }
    return nullptr;
}

bool Resolver::compare_impl_type(ChiType *base, ChiType *impl) {
    if (base == impl) {
        return true;
    }
    if (base->kind == TypeKind::Fn) {
        if (base->data.fn.params.size != impl->data.fn.params.size) {
            return false;
        }
        for (int i = 0; i < base->data.fn.params.size; ++i) {
            if (!compare_impl_type(base->data.fn.params[i], impl->data.fn.params[i])) {
                return false;
            }
        }
        if (!compare_impl_type(base->data.fn.return_type, impl->data.fn.return_type)) {
            return false;
        }
        return true;
    }
    return can_assign(base, impl);
}

Scope *ScopeResolver::push_scope(ast::Node *owner) {
    auto new_scope = m_resolver->get_context()->allocator->create_scope(m_current_scope);
    m_current_scope = new_scope;
    m_current_scope->owner = owner;
    return m_current_scope;
}

void ScopeResolver::pop_scope() { m_current_scope = m_current_scope->parent; }

bool ScopeResolver::declare_symbol(const string &name, ast::Node *node) {
    if (m_current_scope->find_one(name)) {
        return false;
    }
    if (auto builtin = m_resolver->get_builtin(name)) {
        if (builtin->type == NodeType::Primitive) {
            return false;
        }
    }
    m_current_scope->put(name, node);
    return true;
}

ast::Node *ScopeResolver::find_symbol(const string &name) {
    if (auto builtin = m_resolver->get_builtin(name)) {
        return builtin;
    }
    auto scope = m_current_scope;
    while (scope) {
        if (auto node = scope->find_one(name)) {
            return node;
        }
        scope = scope->parent;
    }
    return nullptr;
}

ScopeResolver::ScopeResolver(cx::Resolver *resolver) {
    m_resolver = resolver;
    push_scope(nullptr);
}

#define RS_SET_PROP_COPY(prop, value)                                                              \
    auto cpy = *this;                                                                              \
    cpy.prop = value;                                                                              \
    return cpy;

ResolveScope ResolveScope::set_parent_fn(ChiType *fn) const { RS_SET_PROP_COPY(parent_fn, fn); }

ResolveScope ResolveScope::set_parent_struct(ChiType *struct_) const {
    RS_SET_PROP_COPY(parent_struct, struct_);
}

ResolveScope ResolveScope::set_value_type(ChiType *value_type) const {
    RS_SET_PROP_COPY(value_type, value_type);
}

ResolveScope ResolveScope::set_parent_loop(ast::Node *loop) const {
    RS_SET_PROP_COPY(parent_loop, loop);
}

ResolveScope ResolveScope::set_is_escaping(bool is_escaping) const {
    RS_SET_PROP_COPY(is_escaping, is_escaping);
}

ResolveScope ResolveScope::set_parent_fn_node(ast::Node *fn) const {
    RS_SET_PROP_COPY(parent_fn_node, fn);
}

ResolveScope ResolveScope::set_module(ast::Module *module) const {
    RS_SET_PROP_COPY(module, module);
}

ResolveScope ResolveScope::set_move_outlet(ast::Node *outlet) const {
    RS_SET_PROP_COPY(move_outlet, outlet);
}

ResolveScope ResolveScope::set_block(ast::Block *block) const { RS_SET_PROP_COPY(block, block); }

ResolveScope ResolveScope::set_is_lhs(bool is_lhs) const { RS_SET_PROP_COPY(is_lhs, is_lhs); }
