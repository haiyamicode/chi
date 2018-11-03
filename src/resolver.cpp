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

Resolver::Resolver(ResolveContext* ctx) {
    m_ctx = ctx;
}

ast::Node* Resolver::add_primitive(const string& name, ChiType* type) {
    auto node = create_node(ast::NodeType::Primitive);
    node->token = nullptr;
    node->name = name;
    m_ctx->builtins.add(node);

    auto sym = create_type(TypeId::TypeSymbol);
    sym->data.type_symbol.name = &node->name;
    type->name = name;
    sym->data.type_symbol.giving_type = type;
    node->resolved_type = sym;
    return node;
}

void Resolver::create_primitives() {
    add_primitive("bool", create_type(TypeId::Bool));
    add_primitive("string", create_type(TypeId::String));
    add_primitive("void", create_type(TypeId::Void));
    add_primitive("array", create_type(TypeId::Array));
    add_primitive("int", create_int_type(32, false));
    add_primitive("int8", create_int_type(8, false));
    add_primitive("int16", create_int_type(16, false));
    add_primitive("int32", create_int_type(32, false));
    add_primitive("int64", create_int_type(64, false));
    add_primitive("uint", create_int_type(32, true));
    add_primitive("uint8", create_int_type(8, true));
    add_primitive("uint16", create_int_type(16, true));
    add_primitive("uint32", create_int_type(32, true));
    add_primitive("uint64", create_int_type(64, true));
    add_primitive("char", create_int_type(8, false));
    add_primitive("float", create_float_type(32));
    add_primitive("double", create_float_type(64));
}

void Resolver::add_builtin_fn(const std::string& name, ChiType* type, ast::BuiltinId builtin_id) {
    auto fn = create_node(ast::NodeType::FnDef);
    fn->name = name;
    fn->data.fn_def.builtin_id = builtin_id;
    m_ctx->builtins.add(fn);
    fn->resolved_type = type;
}

void Resolver::create_builtins() {
    // printf
    auto printf_type = create_type(TypeId::Fn);
    printf_type->data.fn.return_type = create_type(TypeId::Void);
    auto& params = printf_type->data.fn.params;
    params.add(create_type(TypeId::String));
    params.add(create_type(TypeId::Int));
    add_builtin_fn("printf", printf_type, ast::BuiltinId::Printf);
}

ChiType* Resolver::create_type(TypeId type_id) {
    return m_ctx->allocator->create_type(type_id);
}

ast::Node* Resolver::create_node(ast::NodeType type) {
    return m_ctx->allocator->create_node(type);
}

ast::Node* Resolver::get_builtin(const string& name) {
    for (auto& node: m_ctx->builtins) {
        if (node->name == name) {
            return node;
        }
    }
    return nullptr;
}

ChiType* Resolver::node_get_type(ast::Node* node) {
    return node->resolved_type;
}

void Resolver::resolve(ast::Package* package) {
    for (auto& module: package->modules) {
        resolve(&module);
    }
}

void Resolver::resolve(ast::Module* module) {
    m_module = module;
    ResolveScope scope;
    resolve(module->root, scope);
}

bool Resolver::can_assign(ChiType* from_type, ChiType* to_type) {
    static auto int_type = create_int_type(32, false);
    switch (to_type->id) {
        case TypeId::Void:
            return false;
        case TypeId::Pointer:
            return from_type->id == TypeId::Pointer || can_assign(from_type, int_type);
        case TypeId::Int:
            return type_is_int(from_type) ||
                   from_type->id == TypeId::Pointer || from_type->id == TypeId::Bool;
        case TypeId::Struct: {
            auto& dest = to_type->data.struct_;
            if (dest.kind == ContainerKind::Trait && from_type->id == TypeId::Pointer) {
                auto& ptr = from_type->data.pointer;
                if (ptr.elem->id != TypeId::Struct) {
                    return false;
                }
                auto& src = ptr.elem->data.struct_;
                if (src.kind == ContainerKind::Struct) {
                    for (auto& impl: src.traits) {
                        if (impl->trait_type == to_type) {
                            return true;
                        }
                    }
                    return false;
                }
            }
            return from_type == to_type;
        }
        case TypeId::Bool:
            return from_type->id == TypeId::Bool || type_is_int(from_type);
        default:
            break;
    }
    return from_type->id == to_type->id;
}

ChiType* Resolver::to_value_type(ChiType* type) {
    if (type->id == TypeId::TypeSymbol) {
        return type->data.type_symbol.giving_type;
    }
    return type;
}

ChiType* Resolver::_resolve(ast::Node* node, ResolveScope& scope) {
    static auto bool_type = create_type(TypeId::Bool);
    static auto int_type = create_int_type(32, false);
    static auto void_type = create_type(TypeId::Void);

    switch (node->type) {
        case NodeType::Root: {
            // first pass: skip function and struct bodies
            scope.skip_fn_bodies = true;
            for (auto decl: node->data.root.top_level_decls) {
                resolve(decl, scope);
                if (decl->type == NodeType::FnDef && decl->name == "main") {
                    node->module->package->entry_fn = decl;
                }
            }

            // second pass: resolve struct members
            for (auto decl: node->data.root.top_level_decls) {
                if (decl->type == NodeType::StructDecl) {
                    _resolve(decl, scope);
                }
            }

            // third pass: resolve struct embeds
            for (auto decl: node->data.root.top_level_decls) {
                if (decl->type == NodeType::StructDecl) {
                    _resolve(decl, scope);
                }
            }

            // fourth pass: resolve function and method bodies
            scope.skip_fn_bodies = false;
            for (auto decl: node->data.root.top_level_decls) {
                if (decl->type == NodeType::StructDecl || decl->type == NodeType::FnDef) {
                    _resolve(decl, scope);
                }
            }
            return nullptr;
        }
        case NodeType::FnDef: {
            auto& data = node->data.fn_def;
            auto proto = resolve(data.fn_proto, scope);
            node->resolved_type = proto;
            if (should_resolve_fn_body(scope)) {
                auto fn_scope = scope.set_parent_fn(proto);
                resolve(data.body, fn_scope);
            }
            return proto;
        }
        case NodeType::FnProto: {
            auto& data = node->data.fn_proto;
            auto type = create_type(TypeId::Fn);
            type->data.fn.return_type = to_value_type(resolve(data.return_type, scope));
            for (auto param: data.params) {
                type->data.fn.params.add(to_value_type(resolve(param, scope)));
            }
            return type;
        }
        case NodeType::Identifier: {
            auto& data = node->data.identifier;
            if (data.kind == ast::IdentifierKind::This) {
                return create_pointer_type(scope.parent_struct, true);
            }
            auto type = resolve(data.decl, scope);
            return type;
        }
        case NodeType::TypeSigil: {
            auto& data = node->data.type_sigil;
            auto type = to_value_type(resolve(data.type, scope));
            auto sym = create_type(TypeId::TypeSymbol);
            sym->data.type_symbol.name = nullptr;
            sym->data.type_symbol.giving_type = create_pointer_type(type, false);
            return sym;
        }
        case NodeType::ParamDecl: {
            auto& data = node->data.param_decl;
            return to_value_type(resolve(data.type, scope));
        }
        case NodeType::VarDecl: {
            auto& data = node->data.var_decl;
            auto var_type = to_value_type(resolve(data.type, scope));
            if (data.expr) {
                auto var_scope = scope.set_value_type(var_type);
                auto expr_type = resolve(data.expr, var_scope);
                check_assignment(node, expr_type, var_type);
            }
            return var_type;
        }
        case NodeType::BinOpExpr: {
            auto& data = node->data.bin_op_expr;
            auto t1 = resolve(data.op1, scope);
            auto t2 = resolve(data.op2, scope);
            check_assignment(node, t2, t1);
            switch (data.op_type) {
                case TokenType::EQ:
                case TokenType::LT:
                case TokenType::LE:
                case TokenType::GT:
                case TokenType::GE:
                    return bool_type;
                default:
                    return t1;
            }
        }
        case NodeType::UnaryOpExpr: {
            auto& data = node->data.unary_op_expr;
            auto t = resolve(data.op1, scope);
            switch (data.op_type) {
                case TokenType::SUB:
                case TokenType::ADD:
                case TokenType::INC:
                case TokenType::DEC:
                    check_assignment(node, t, int_type);
                    return t->id == TypeId::Bool ? int_type : t;
                case TokenType::LNOT:
                    check_assignment(node, t, bool_type);
                    return bool_type;
                case TokenType::MUL:
                    if (t->id != TypeId::Pointer || t->data.pointer.elem->id == TypeId::Void) {
                        error(node, errors::INVALID_OPERATOR, get_token_symbol(data.op_type), to_string(t));
                        return nullptr;
                    }
                    return t->data.pointer.elem;
                case TokenType::AND:
                    return create_pointer_type(t, false);
                default:
                    unreachable();
            }
        }
        case NodeType::CastExpr: {
            auto& data = node->data.cast_expr;
            auto dest_type = to_value_type(resolve(data.dest_type, scope));
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
                    return create_type(TypeId::String);
                default:
                    unreachable();
            }
        }
        case NodeType::ReturnStmt: {
            auto& data = node->data.return_stmt;
            auto type = data.expr ? resolve(data.expr, scope) : void_type;
            assert(scope.parent_fn);
            check_assignment(node, type, scope.parent_fn->data.fn.return_type);
            return nullptr;
        }
        case NodeType::ParenExpr: {
            auto& child = node->data.child_expr;
            return resolve(child, scope);
        }
        case NodeType::DotExpr: {
            auto& data = node->data.dot_expr;
            auto field_name = data.field->str;
            auto expr_type = resolve(data.expr, scope);
            auto sty = expr_type;
            if (sty->id == TypeId::Pointer) {
                sty = sty->data.pointer.elem;
            } else if (sty->id == TypeId::Array) {
                sty = sty->data.array.internal;
            } else if (sty->id == TypeId::TypeSymbol) {
                if (auto underlying_type = sty->data.type_symbol.underlying_type) {
                    sty = underlying_type;
                }
            }
            auto member = get_struct_member(sty, field_name);
            if (!member) {
                error(node, errors::MEMBER_NOT_FOUND, field_name, to_string(expr_type));
                return nullptr;
            }
            data.resolved_member = member;
            return member->field ? member->field->type : node_get_type(member->node);
        }
        case NodeType::ComplitExpr: {
            auto& data = node->data.complit_expr;
            if (!scope.value_type) {
                error(node, errors::COMPLIT_CANNOT_INFER_TYPE);
                return nullptr;
            }
            auto value_type = scope.value_type;
            auto constructor = get_struct_member(value_type, "new");
            if (constructor) {
                auto constructor_type = node_get_type(constructor->node);
                auto& fn_type = constructor_type->data.fn;
                resolve_fn_call(node, scope, &fn_type, &data.items);
                fn_type.container = value_type;
            } else {
                if (data.items.size != 0) {
                    error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, 0, data.items.size);
                    return nullptr;
                }
            }
            return value_type;
        }
        case NodeType::FnCallExpr: {
            auto& data = node->data.fn_call_expr;
            auto fn_type = resolve(data.fn_ref_expr, scope);
            if (fn_type->id != TypeId::Fn) {
                error(data.fn_ref_expr, errors::CANNOT_CALL_NON_FUNCTION);
                return nullptr;
            }
            auto& fn = fn_type->data.fn;
            resolve_fn_call(node, scope, &fn, &data.args);
            return fn.return_type;
        }
        case NodeType::IfStmt: {
            auto& data = node->data.if_stmt;
            auto cond_type = resolve(data.condition, scope);
            check_assignment(data.condition, cond_type, bool_type);
            resolve(data.then_block, scope);
            if (data.else_node) {
                resolve(data.else_node, scope);
            }
            return nullptr;
        }
        case NodeType::Block: {
            auto& data = node->data.block;
            for (auto stmt: data.statements) {
                resolve(stmt, scope);
            }
            return nullptr;
        }
        case NodeType::StructDecl: {
            auto& data = node->data.struct_decl;
            ChiType* sym;
            ChiType* struct_type;
            ChiTypeStruct* struct_;
            if (!node->resolved_type) {
                struct_type = create_type(TypeId::Struct);
                struct_type->name = node->name;
                struct_ = &struct_type->data.struct_;
                struct_->node = node;
                struct_->kind = data.kind;

                // first pass, all members are skipped
                struct_->resolve_status = ResolveStatus::None;
                sym = create_type(TypeId::TypeSymbol);
                sym->data.type_symbol.name = &node->name;
                if (data.kind == ContainerKind::Enum) {
                    sym->data.type_symbol.giving_type = int_type;
                    sym->data.type_symbol.underlying_type = struct_type;
                } else {
                    sym->data.type_symbol.giving_type = struct_type;
                    sym->data.type_symbol.underlying_type = struct_type;
                }
                return sym;
            }
            sym = node->resolved_type;
            struct_type = sym->data.type_symbol.underlying_type;
            struct_ = &struct_type->data.struct_;
            auto struct_scope = scope.set_parent_struct(struct_type);
            if (struct_->resolve_status == ResolveStatus::None) {
                // second pass
                scope.next_enum_value = 0;
                for (auto member: data.members) {
                    resolve_struct_member(struct_type, member, struct_scope);
                }
                struct_->resolve_status = ResolveStatus::MemberTypesKnown;
            } else if (struct_->resolve_status == ResolveStatus::MemberTypesKnown) {
                // third pass
                for (auto member: data.members) {
                    if (member->type == NodeType::VarDecl && member->data.var_decl.is_embed) {
                        resolve_struct_embed(struct_type, member, scope);
                    }
                }
                struct_->resolve_status = ResolveStatus::EmbedsResolved;
            } else {
                // fourth pass
                for (auto member: data.members) {
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
            return sym;
        }
        case NodeType::SubtypeExpr: {
            auto& data = node->data.subtype_expr;
            auto type = to_value_type(resolve(data.type, scope));
            if (type->id == TypeId::Array) {
                if (data.args.size != 1) {
                    error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS, "array", 1, data.args.size);
                }
                auto elem_type = to_value_type(resolve(data.args[0], scope));
                return create_array_type(elem_type);
            } else {
                panic("unhandled");
            }
            break;
        }
        case NodeType::IndexExpr: {
            auto& data = node->data.index_expr;
            auto expr_type = resolve(data.expr, scope);
            if (expr_type->id != TypeId::Array) {
                error(node, errors::CANNOT_SUBSCRIPT, to_string(expr_type));
                return nullptr;
            }
            auto subscript_type = resolve(data.subscript, scope);
            check_assignment(data.subscript, subscript_type, int_type);
            return expr_type->data.array.elem;
        }
        case NodeType::EnumMember: {
            auto& data = node->data.enum_member;
            if (data.value) {
                auto value_type = resolve(data.value, scope);
                check_assignment(data.value, value_type, int_type);
                data.resolved_value = resolve_constant_value(data.value);
                scope.next_enum_value = data.resolved_value + 1;
            } else {
                data.resolved_value = scope.next_enum_value++;
            }
            return int_type;
        }
        case NodeType::ForStmt: {
            auto& data = node->data.for_stmt;
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
        default:
            print("\n");
            panic("unhandled {}", PRINT_ENUM(node->type));
    }
    return nullptr;
}

ChiType* Resolver::resolve(ast::Node* node, ResolveScope& scope) {
    auto cached = node_get_type(node);
    if (cached) {
        return cached;
    }
    auto result = _resolve(node, scope);
    node->resolved_type = result;
    return result;
}

string Resolver::to_string(ChiType* type) {
    if (type->name) {
        return *type->name;
    }
    switch (type->id) {
        case TypeId::TypeSymbol:
            return *type->data.type_symbol.name;
        case TypeId::String:
            return "string";
        case TypeId::Pointer:
            return to_string(type->data.pointer.elem) + "*";
        default:
            break;
    }
    return PRINT_ENUM(type->id);
}

void Resolver::check_assignment(ast::Node* node, ChiType* from_type, ChiType* to_type) {
    if (!can_assign(from_type, to_type)) {
        error(node, errors::CANNOT_CONVERT, to_string(from_type), to_string(to_type));
    }
}

void Resolver::check_cast(ast::Node* node, ChiType* from_type, ChiType* to_type) {
    check_assignment(node, from_type, to_type);
}

void Resolver::context_init_builtins() {
    create_primitives();
    create_builtins();
}

void Resolver::resolve_struct_member(ChiType* struct_type, ast::Node* node, ResolveScope& scope) {
    auto& struct_ = struct_type->data.struct_;
    ChiStructField* field = nullptr;
    if (node->type == NodeType::VarDecl) {
        field = struct_.add_field();
        field->struct_ = struct_type;
        field->type = resolve(node, scope);
        node->data.var_decl.resolved_field = field;
    } else if (node->type == NodeType::FnDef) {
        auto fn_type = resolve(node, scope);
        fn_type->data.fn.container = struct_type;
    } else if (node->type == NodeType::EnumMember) {
        resolve(node, scope);
    }
    struct_.add_member(node->name, node, field);
}

void Resolver::resolve_vtable(ChiType* base_type, ChiType* derived_type, ast::Node* embed_node) {
    auto& base = base_type->data.struct_;
    auto& derived = derived_type->data.struct_;
    TraitImpl* trait_impl = nullptr;
    if (base.kind == ContainerKind::Trait) {
        trait_impl = derived.add_trait(base_type, derived_type);
    }
    for (auto& member: base.members) {
        auto node = member->node;
        if (node->type == NodeType::FnDef) {
            auto method = derived.find_member(node->name);
            if (node->data.fn_def.body) {
                if (!method) {
                    method = derived.add_member(node->name, member->node, member->field);
                    method->orig = base_type;
                }
            } else if (!method) {
                error(embed_node, errors::METHOD_NOT_IMPLEMENTED, node->name);
                break;
            }
            if (trait_impl) {
                assert(method);
                trait_impl->impl_table.add(method);
            }
        }
    }
    for (auto& impl: base.traits) {
        resolve_vtable(impl->trait_type, derived_type, embed_node);
    }
}

void Resolver::resolve_struct_embed(ChiType* struct_type, ast::Node* node, ResolveScope& parent_scope) {
    auto& container = struct_type->data.struct_;
    auto base_type = node_get_type(node);
    if (base_type->id != TypeId::Struct) {
        error(node, errors::INVALID_EMBED);
        return;
    }
    auto& base = base_type->data.struct_;
    if (base.kind != ContainerKind::Struct && base.kind != ContainerKind::Trait) {
        error(node, errors::INVALID_EMBED);
        return;
    }
    if (base.resolve_status < ResolveStatus::Done) {
        _resolve(base.node, parent_scope);
    }
    resolve_vtable(base_type, struct_type, node);
}

bool Resolver::should_resolve_fn_body(ResolveScope& scope) {
    auto parent_struct = scope.parent_struct;
    if (!parent_struct) {
        return !scope.skip_fn_bodies;
    }
    auto& struct_ = parent_struct->data.struct_;
    return struct_.kind != ContainerKind::Trait && struct_.resolve_status >= ResolveStatus::MemberTypesKnown;
}

ChiStructMember* Resolver::get_struct_member(ChiType* struct_type, const string& field_name) {
    if (struct_type->id != TypeId::Struct) {
        return nullptr;
    }
    auto& data = struct_type->data.struct_;
    return data.find_member(field_name);
}

void Resolver::resolve_fn_call(ast::Node* node, ResolveScope& scope, ChiTypeFn* fn, NodeList* args) {
    auto n_args = args->size;
    auto n_params = fn->params.size;
    if (n_args != n_params) {
        error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, n_params, n_args);
    }
    for (int i = 0; i < n_args; i++) {
        auto arg = args->at(i);
        auto arg_type = resolve(arg, scope);
        check_assignment(arg, arg_type, fn->params[i]);
    }
}

ChiType* Resolver::create_pointer_type(ChiType* elem, bool is_ref) {
    auto type = create_type(TypeId::Pointer);
    type->data.pointer.elem = elem;
    type->data.pointer.is_ref = is_ref;
    return type;
}

ChiType* Resolver::create_array_type(ChiType* elem) {
    static ChiType* size_type = create_type(TypeId::Int);
    if (auto cached = m_ctx->array_types.get(elem)) {
        return *cached;
    }
    auto type = create_type(TypeId::Array);
    type->data.array.elem = elem;
    auto internal_type = create_type(TypeId::Struct);
    auto& internal_data = internal_type->data.struct_;

    auto data_field = internal_data.add_field();
    data_field->struct_ = internal_type;
    data_field->type = create_pointer_type(elem, false);
    auto data_field_node = create_node(NodeType::VarDecl);
    data_field_node->resolved_type = data_field->type;
    internal_data.add_member("data", data_field_node, data_field);

    auto size_field = internal_data.add_field();
    size_field->struct_ = internal_type;
    size_field->type = size_type;
    auto size_field_node = create_node(NodeType::VarDecl);
    size_field_node->resolved_type = size_field->type;
    internal_data.add_member("size", size_field_node, size_field);

    auto add_fn = create_node(NodeType::FnDef);
    add_fn->data.fn_def.builtin_id = ast::BuiltinId::ArrayAdd;
    add_fn->data.fn_def.fn_kind = ast::FnKind::InstanceMethod;
    auto add_fn_type = create_type(TypeId::Fn);
    add_fn_type->data.fn.return_type = create_pointer_type(elem, false);
    add_fn_type->data.fn.params.add(elem);
    add_fn->resolved_type = add_fn_type;
    internal_data.add_member("add", add_fn, nullptr);

    type->data.array.internal = internal_type;
    m_ctx->array_types[elem] = type;
    return type;
}

ChiType* Resolver::create_int_type(int bit_count, bool is_unsigned) {
    auto type = create_type(TypeId::Int);
    type->data.int_.bit_count = bit_count;
    type->data.int_.is_unsigned = is_unsigned;
    return type;
}

ChiType* Resolver::create_float_type(int bit_count) {
    auto type = create_type(TypeId::Float);
    type->data.float_.bit_count = bit_count;
    return type;
}

int64_t Resolver::resolve_constant_value(ast::Node* node) {
    switch (node->type) {
        case NodeType::LiteralExpr:
            return node->token->val.i;
        default:
            panic("unhandled {}", PRINT_ENUM(node->type));
    }
    return 0;
}

Scope* ScopeResolver::push_scope(ast::Node* owner) {
    m_current_scope = m_scopes.emplace(std::make_unique<Scope>(m_current_scope))->get();
    m_current_scope->owner = owner;
    return m_current_scope;
}

void ScopeResolver::pop_scope() {
    m_current_scope = m_current_scope->parent;
}

bool ScopeResolver::declare_symbol(const string& name, ast::Node* node) {
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

ast::Node* ScopeResolver::find_symbol(const string& name) {
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

ScopeResolver::ScopeResolver(cx::Resolver* resolver) {
    m_resolver = resolver;
    push_scope(nullptr);
}

#define RS_SET_PROP_COPY(prop, value) auto cpy = *this; cpy.prop = value; return cpy;

ResolveScope ResolveScope::set_parent_fn(ChiType* fn) const {
    RS_SET_PROP_COPY(parent_fn, fn);
}

ResolveScope ResolveScope::set_parent_struct(ChiType* struct_) const {
    RS_SET_PROP_COPY(parent_struct, struct_);
}

ResolveScope ResolveScope::set_value_type(ChiType* value_type) const {
    RS_SET_PROP_COPY(value_type, value_type);
}

ResolveScope ResolveScope::set_parent_loop(ast::Node* loop) const {
    RS_SET_PROP_COPY(parent_loop, loop);
}
