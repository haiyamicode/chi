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

void Resolver::create_primitives() {
    auto &system_types = m_ctx->system_types;
    system_types.any = create_type(TypeKind::Any);
    system_types.char_ = create_int_type(8, false);
    system_types.uint8 = create_int_type(8, true);
    system_types.int_ = create_int_type(32, false);
    system_types.int64 = create_int_type(64, false);
    system_types.float_ = create_float_type(32);
    system_types.double_ = create_float_type(64);
    system_types.void_ = create_type(TypeKind::Void);
    system_types.bool_ = create_type(TypeKind::Bool);
    system_types.string = create_type(TypeKind::String);
    system_types.str_lit = create_pointer_type(system_types.char_, TypeKind::Pointer);
    system_types.array = create_type(TypeKind::Array);
    system_types.optional = create_type(TypeKind::Optional);
    system_types.box = create_type(TypeKind::Box);
    system_types.array = create_type(TypeKind::Array);

    add_primitive("bool", system_types.bool_);
    add_primitive("string", system_types.string);
    add_primitive("any", system_types.any);
    add_primitive("void", system_types.void_);
    add_primitive("int", system_types.int_);
    add_primitive("int64", system_types.int64);
    add_primitive("char", system_types.char_);
    add_primitive("float", system_types.float_);
    add_primitive("double", system_types.double_);
    add_primitive("optional", system_types.optional);
    add_primitive("array", system_types.array);
    add_primitive("box", system_types.box);
    add_primitive("uint8", system_types.uint8);
    add_primitive("int8", create_int_type(8, false));
    add_primitive("int16", create_int_type(16, false));
    add_primitive("int32", create_int_type(32, false));
    add_primitive("uint", create_int_type(32, true));
    add_primitive("uint16", create_int_type(16, true));
    add_primitive("uint32", create_int_type(32, true));
    add_primitive("uint64", create_int_type(64, true));
}

void Resolver::add_builtin_fn(const std::string &name, ChiType *type, ast::BuiltinId builtin_id) {
    auto fn = create_node(ast::NodeType::FnDef);
    fn->name = name;
    fn->data.fn_def.builtin_id = builtin_id;
    m_ctx->builtins.add(fn);
    fn->resolved_type = type;
}

void Resolver::create_builtins() {
    // printf
    auto printf_type = create_type(TypeKind::Fn);
    printf_type->data.fn.return_type = create_type(TypeKind::Void);
    auto &printf_params = printf_type->data.fn.params;
    printf_params.add(get_system_types()->string);
    printf_params.add(get_array_type(get_system_types()->any));
    printf_type->data.fn.is_variadic = true;
    add_builtin_fn("printf", printf_type, ast::BuiltinId::Printf);

    // debug
    auto test_type = create_type(TypeKind::Fn);
    test_type->data.fn.return_type = create_type(TypeKind::Void);
    test_type->data.fn.params.add(get_system_types()->string);
    add_builtin_fn("debug", test_type, ast::BuiltinId::Debug);
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

ChiType *Resolver::node_get_type(ast::Node *node) { return node->resolved_type; }

void Resolver::resolve(ast::Package *package) {
    for (auto &module : package->modules) {
        resolve(&module);
    }
}

void Resolver::resolve(ast::Module *module) {
    m_module = module;
    ResolveScope scope;
    resolve(module->root, scope);
}

bool Resolver::can_assign(ChiType *from_type, ChiType *to_type) {
    static auto int_type = get_system_types()->int_;
    static auto str_lit_type = get_system_types()->str_lit;
    if (is_same_type(from_type, to_type)) {
        return true;
    }
    switch (to_type->kind) {
    case TypeKind::Void:
        return from_type->kind == TypeKind::Void;
    case TypeKind::String:
        return from_type->kind == TypeKind::String || from_type == str_lit_type;
    case TypeKind::Pointer:
    case TypeKind::Reference: {
        auto tt = to_type->get_elem();
        if (ChiTypeStruct::is_trait(tt) && ChiTypeStruct::is_pointer_type(from_type)) {
            auto ft = from_type->get_elem();
            if (ft->kind != TypeKind::Struct) {
                return false;
            }
            auto &ss = ft->data.struct_;
            if (ss.kind == ContainerKind::Struct) {
                for (auto &impl : ss.traits) {
                    if (is_same_type(impl->trait_type, tt)) {
                        return true;
                    }
                }
                return false;
            }
        }
        return from_type->kind == TypeKind::Pointer || can_assign(from_type, int_type);
    }
    case TypeKind::Int:
        return type_is_int(from_type);
    case TypeKind::Any:
        return from_type->kind != TypeKind::Struct || ChiTypeStruct::is_trait(from_type);
    case TypeKind::Struct:
        return from_type == to_type;
    case TypeKind::Bool:
        return type_is_int(from_type) || from_type->kind == TypeKind::Optional;
    case TypeKind::Optional:
        return from_type == to_type || to_type->get_elem() == from_type;
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

ChiType *Resolver::_resolve(ast::Node *node, ResolveScope &scope) {
    static auto bool_type = get_system_types()->bool_;
    static auto int_type = get_system_types()->int_;
    static auto void_type = get_system_types()->void_;
    static auto string_type = get_system_types()->string;

    switch (node->type) {
    case NodeType::Root: {
        auto &data = node->data.root;
        // first pass: skip function and struct bodies
        scope.skip_fn_bodies = true;
        for (auto decl : data.top_level_decls) {
            resolve(decl, scope);
            if (decl->type == NodeType::FnDef && decl->name == "main") {
                node->module->package->entry_fn = decl;
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
        auto proto = resolve(data.fn_proto, scope);
        node->resolved_type = proto;
        if (should_resolve_fn_body(scope)) {
            auto fn_scope = scope.set_parent_fn(proto);
            resolve(data.body, fn_scope);
        }
        return proto;
    }
    case NodeType::FnProto: {
        auto &data = node->data.fn_proto;
        auto type = create_type(TypeKind::Fn);
        auto return_type = data.return_type ? resolve_value(data.return_type, scope) : void_type;
        type->data.fn.return_type = return_type;
        type->is_placeholder = return_type->is_placeholder;
        for (int i = 0; i < data.params.size; i++) {
            auto param = data.params[i];
            auto &pdata = param->data.param_decl;
            auto is_last = i == data.params.size - 1;
            if (pdata.is_variadic && !is_last) {
                error(param, errors::VARIADIC_NOT_FINAL, param->name);
                return type;
            }
            auto param_type = resolve_value(param, scope);
            if (pdata.is_variadic) {
                param_type = get_array_type(param_type);
                param->resolved_type = param_type;
            }
            type->data.fn.params.add(param_type);
            if (param_type->is_placeholder) {
                type->is_placeholder = true;
            }
        }
        return type;
    }
    case NodeType::Identifier: {
        auto &data = node->data.identifier;
        if (data.kind == ast::IdentifierKind::This) {
            return get_pointer_type(scope.parent_struct, TypeKind::Reference);
        }
        if (data.kind == ast::IdentifierKind::Value && !m_tmods.is_empty()) {
            if (auto replacement = m_tmods.get(data.decl)) {
                node->orig_type = data.decl->resolved_type;
                return *replacement;
            }
        }
        auto type = resolve(data.decl, scope);
        return type;
    }
    case NodeType::TypeSigil: {
        auto &data = node->data.sigil_type;
        auto type = resolve_value(data.type, scope);
        return create_type_symbol({}, get_pointer_type(type, get_sigil_type_kind(data.sigil)));
    }
    case NodeType::ParamDecl: {
        auto &data = node->data.param_decl;
        return resolve_value(data.type, scope);
    }
    case NodeType::VarDecl: {
        auto &data = node->data.var_decl;
        ChiType *var_type = nullptr;
        if (data.type) {
            var_type = resolve_value(data.type, scope);
        }
        if (data.expr) {
            auto var_scope = var_type ? scope.set_value_type(var_type) : scope;
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
        }
        return var_type;
    }
    case NodeType::BinOpExpr: {
        auto &data = node->data.bin_op_expr;
        auto t1 = resolve(data.op1, scope);
        ChiType *t2;
        if (data.op_type == TokenType::ASS) {
            auto var_scope = scope.set_value_type(t1);
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
            return bool_type;
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
            check_assignment(data.op1, t, int_type);
            return t->kind == TypeKind::Bool ? int_type : t;
        case TokenType::LNOT: {
            if (data.is_suffix) {
                if (ChiTypeStruct::is_pointer_type(t) && t->get_elem()->kind != TypeKind::Void) {
                    return t->get_elem();
                } else if (t->kind == TypeKind::Optional) {
                    return t->get_elem();
                } else if (t->kind == TypeKind::Box) {
                    return get_pointer_type(t->get_elem());
                }
                goto invalid;
            } else {
                check_assignment(data.op1, t, bool_type);
                return bool_type;
            }
            break;
        }
        case TokenType::MUL: {
            if (!is_addressable(data.op1)) {
                error(node, errors::CANNOT_GET_POINTER_UNADDRESSABLE);
            }
            return get_pointer_type(t);
        }
        case TokenType::AND: {
            if (!is_addressable(data.op1)) {
                error(node, errors::CANNOT_GET_REFERENCE_UNADDRESSABLE);
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
            return bool_type;
        case TokenType::INT:
            return int_type;
        case TokenType::STRING:
            return string_type;
        default:
            unreachable();
        }
    }
    case NodeType::ReturnStmt: {
        auto &data = node->data.return_stmt;
        auto expr_type = data.expr ? resolve(data.expr, scope) : void_type;
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
        auto expr_type = resolve(data.expr, scope);
        auto member = get_struct_member(expr_type, field_name);
        if (!member) {
            error(node, errors::MEMBER_NOT_FOUND, field_name, to_string(expr_type));
            return nullptr;
        }
        data.resolved_member = member;
        return member->resolved_type;
    }
    case NodeType::ConstructExpr: {
        auto &data = node->data.construct_expr;
        ChiType *value_type;
        ChiType *result_type;
        if (data.type) {
            value_type = resolve_value(data.type, scope);
            result_type = data.is_new ? get_pointer_type(value_type) : value_type;
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
        auto constructor = get_struct_member(value_type, "new");
        if (constructor) {
            auto &fn_type = constructor->resolved_type->data.fn;
            resolve_fn_call(node, scope, &fn_type, &data.items);
        } else {
            if (data.items.size != 0) {
                error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, 0, data.items.size);
                return nullptr;
            }
        }
        return result_type;
    }
    case NodeType::FnCallExpr: {
        auto &data = node->data.fn_call_expr;
        auto fn_type = resolve(data.fn_ref_expr, scope);
        if (fn_type->kind != TypeKind::Fn) {
            error(data.fn_ref_expr, errors::CANNOT_CALL_NON_FUNCTION);
            return nullptr;
        }
        auto &fn = fn_type->data.fn;
        resolve_fn_call(node, scope, &fn, &data.args);
        return fn.return_type;
    }
    case NodeType::IfStmt: {
        auto &data = node->data.if_stmt;
        auto cond_type = resolve(data.condition, scope);
        ast::Node *tmod_iden = nullptr;
        if (data.condition->type == NodeType::Identifier && cond_type->kind == TypeKind::Optional) {
            tmod_iden = data.condition;
            set_tmod(tmod_iden, cond_type->get_elem());
        }
        check_assignment(data.condition, cond_type, bool_type);
        resolve(data.then_block, scope);
        if (tmod_iden) {
            unset_tmod(tmod_iden);
        }
        if (data.else_node) {
            resolve(data.else_node, scope);
        }
        return nullptr;
    }
    case NodeType::Block: {
        auto &data = node->data.block;
        for (auto stmt : data.statements) {
            resolve(stmt, scope);
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
                type_sym->data.type_symbol.giving_type = int_type;
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
                if (!ChiTypeStruct::is_trait(impl_trait)) {
                    error(implement, errors::NON_TRAIT_IMPL_TYPE, to_string(impl_trait));
                }
                resolve_vtable(impl_trait, struct_type, implement);
            }
            struct_->resolve_status = ResolveStatus::EmbedsResolved;
        } else {
            // fourth pass
            for (auto member : data.members) {
                if (member->type == NodeType::FnDef) {
                    auto fn_type = node_get_type(member);
                    auto fn_scope = struct_scope.set_parent_fn(fn_type);
                    if (auto body = member->data.fn_def.body) {
                        resolve(body, fn_scope);
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
            type->kind == TypeKind::Box) {
            if (data.args.size != 1) {
                error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS,
                      to_string(get_system_type(type->kind)), 1, data.args.size);
            }
            auto elem_type = to_value_type(resolve(data.args[0], scope));
            return get_wrapped_type(elem_type, type->kind);
        }
        auto &params = type->data.struct_.type_params;
        if (params.size != data.args.size) {
            error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS, params.size, data.args.size);
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
        if (expr_type->kind != TypeKind::Array) {
            error(node, errors::CANNOT_SUBSCRIPT, to_string(expr_type));
            return nullptr;
        }
        auto subscript_type = resolve(data.subscript, scope);
        check_assignment(data.subscript, subscript_type, int_type);
        return expr_type->get_elem();
    }
    case NodeType::EnumMember: {
        auto &data = node->data.enum_member;
        if (data.value) {
            auto value_type = resolve(data.value, scope);
            check_assignment(data.value, value_type, int_type);
            data.resolved_value = get<int64_t>(resolve_constant_value(data.value));
            scope.next_enum_value = data.resolved_value + 1;
        } else {
            data.resolved_value = scope.next_enum_value++;
        }
        return int_type;
    }
    case NodeType::ForStmt: {
        auto &data = node->data.for_stmt;
        if (data.init) {
            resolve(data.init, scope);
        }
        if (data.condition) {
            auto cond_type = resolve(data.condition, scope);
            check_assignment(data.condition, cond_type, bool_type);
        }
        if (data.post) {
            resolve(data.post, scope);
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
        auto expr_type = resolve(data.expr, scope);
        if (!expr_type->is_raw_pointer()) {
            error(node, errors::INVALID_OPERATOR, data.prefix->to_string(), to_string(expr_type));
        }
        return void_type;
    }
    default:
        print("\n");
        panic("unhandled {}", PRINT_ENUM(node->type));
    }

    return nullptr;
}

ChiType *Resolver::resolve(ast::Node *node, ResolveScope &scope) {
    auto cached = node_get_type(node);
    if (cached) {
        return cached;
    }
    auto result = _resolve(node, scope);
    node->resolved_type = result;
    return result;
}

string Resolver::to_string(ChiType *type) {
    if (type->name) {
        return *type->name;
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
        return "()" + to_string(type->get_elem());
    case TypeKind::Box:
        return "^" + to_string(type->get_elem());
    case TypeKind::Array:
        return fmt::format("{}<{}>", to_string(get_system_type(type->kind)),
                           to_string(type->get_elem()));
    default:
        break;
    }
    return PRINT_ENUM(type->kind);
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

void Resolver::context_init_builtins() {
    create_primitives();
    create_builtins();
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
        member->resolved_type->data.fn.container = struct_type;
    } else if (node->type == NodeType::EnumMember) {
        member->resolved_type = resolve(node, scope);
    }
    return member;
}

void Resolver::resolve_vtable(ChiType *base_type, ChiType *derived_type, ast::Node *base_node) {
    auto &base = base_type->data.struct_;
    auto &derived = derived_type->data.struct_;
    TraitImpl *trait_impl = nullptr;
    if (base.kind == ContainerKind::Trait) {
        trait_impl = derived.add_trait(base_type, derived_type);
    }
    for (auto &member : base.members) {
        auto node = member->node;
        if (member->node->type == NodeType::FnDef) {
            auto method = derived.find_member(node->name);
            if (node->data.fn_def.body) {
                if (!method) {
                    method = derived.add_member(node->name, node, member->resolved_type);
                    method->orig_parent = base_type;
                }
            } else if (!method) {
                error(base_node, errors::METHOD_NOT_IMPLEMENTED, node->name);
                break;
            }
            if (trait_impl) {
                assert(method);
                trait_impl->impl_table.add(method);
            }
        }
    }
    for (auto &impl : base.traits) {
        resolve_vtable(impl->trait_type, derived_type, base_node);
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
    if (base.kind != ContainerKind::Struct && base.kind != ContainerKind::Trait) {
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

bool Resolver::should_resolve_fn_body(ResolveScope &scope) {
    auto parent_struct = scope.parent_struct;
    if (!parent_struct) {
        return !scope.skip_fn_bodies;
    }
    auto &struct_ = parent_struct->data.struct_;
    return struct_.kind != ContainerKind::Trait &&
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
    case TypeKind::Array:
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
        auto fn = create_type(TypeKind::Fn);
        fn->data.fn.return_type = type_placeholders_sub(data.return_type, subs);
        fn->data.fn.container = data.container;
        type_placeholders_sub_each(&data.params, subs, &fn->data.fn.params);
        return fn;
    }
    default:
        return type;
    }
}

ChiStructMember *Resolver::get_struct_member(ChiType *struct_type, const string &field_name) {
    auto sty = struct_type;
    if (sty->is_pointer()) {
        sty = sty->get_elem();
    }
    if (sty->kind == TypeKind::Array) {
        sty = sty->data.array.internal;
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
    auto &data = sty->data.struct_;
    return data.find_member(field_name);
}

void Resolver::resolve_fn_call(ast::Node *node, ResolveScope &scope, ChiTypeFn *fn,
                               NodeList *args) {
    auto n_args = args->size;
    auto n_params = fn->params.size;
    bool ok = fn->is_variadic ? n_args >= n_params - 1 : n_args == n_params;
    if (!ok) {
        error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, n_params, n_args);
    }
    auto va_start = n_params - (int)fn->is_variadic;
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
    static ChiType *size_type = get_system_types()->int_;
    if (auto cached = m_ctx->array_of.get(elem)) {
        return *cached;
    }
    auto type = create_type(TypeKind::Array);
    type->data.array.elem = elem;
    auto internal_type = create_type(TypeKind::Struct);
    auto &internal_struct = internal_type->data.struct_;

    auto data_field_node = create_node(NodeType::VarDecl);
    auto data_field_type = get_pointer_type(elem);
    data_field_node->resolved_type = data_field_type;
    internal_struct.add_member("data", data_field_node, data_field_type);

    auto size_field_node = create_node(NodeType::VarDecl);
    size_field_node->resolved_type = size_type;
    internal_struct.add_member("size", size_field_node, size_type);

    auto cap_field_node = create_node(NodeType::VarDecl);
    cap_field_node->resolved_type = size_type;
    internal_struct.add_member("capacity", cap_field_node, size_type);

    auto flags_type = get_system_types()->uint8;
    auto flags_field_node = create_node(NodeType::VarDecl);
    flags_field_node->resolved_type = flags_type;
    internal_struct.add_member("flags", flags_field_node, flags_type);

    auto add_fn = create_node(NodeType::FnDef);
    add_fn->name = "add";
    add_fn->data.fn_def.builtin_id = ast::BuiltinId::ArrayAdd;
    add_fn->data.fn_def.fn_kind = ast::FnKind::InstanceMethod;
    auto add_fn_type = create_type(TypeKind::Fn);
    add_fn_type->data.fn.return_type = get_pointer_type(elem, TypeKind::Reference);
    add_fn_type->data.fn.params.add(elem);
    add_fn_type->data.fn.container = type;
    add_fn->resolved_type = add_fn_type;
    internal_struct.add_member(add_fn->name, add_fn, add_fn_type);
    m_ctx->internal_methods.add(add_fn);

    auto del_fn = create_node(NodeType::FnDef);
    del_fn->name = "delete";
    del_fn->data.fn_def.builtin_id = ast::BuiltinId::ArrayDelete;
    del_fn->data.fn_def.fn_kind = ast::FnKind::InstanceMethod;
    auto del_fn_type = create_type(TypeKind::Fn);
    del_fn_type->data.fn.return_type = get_system_types()->void_;
    del_fn_type->data.fn.container = type;
    del_fn->resolved_type = del_fn_type;
    internal_struct.add_member(del_fn->name, del_fn, del_fn_type);
    m_ctx->internal_methods.add(del_fn);

    auto copy_fn = create_node(NodeType::FnDef);
    copy_fn->name = "copy";
    copy_fn->data.fn_def.builtin_id = ast::BuiltinId::ArrayCopy;
    copy_fn->data.fn_def.fn_kind = ast::FnKind::InstanceMethod;
    auto copy_fn_type = create_type(TypeKind::Fn);
    copy_fn_type->data.fn.return_type = type;
    copy_fn_type->data.fn.container = type;
    copy_fn->resolved_type = copy_fn_type;
    internal_struct.add_member("copy", copy_fn, copy_fn_type);
    m_ctx->internal_methods.add(copy_fn);

    type->data.array.internal = internal_type;
    m_ctx->array_of[elem] = type;
    type->is_placeholder = elem->is_placeholder;
    return type;
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
    auto &cpy = sty->data.struct_;
    cpy.kind = base.kind;
    cpy.node = base.node;
    for (auto &member : base.members) {
        auto type = type_placeholders_sub(member->resolved_type, &data);
        if (member->node->type == NodeType::FnDef) {
            type->data.fn.container = subtype;
        }
        auto node = create_node(member->node->type);
        node->name = member->node->name;
        node->token = member->node->token;
        node->resolved_type = type;
        memcpy(&node->data, &member->node->data, sizeof(member->node->data));
        cpy.add_member(member->get_name(), node, type);
    }
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

Scope *ScopeResolver::push_scope(ast::Node *owner) {
    m_current_scope = m_scopes.emplace(std::make_unique<Scope>(m_current_scope))->get();
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