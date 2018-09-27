/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "resolver.h"
#include "error.h"

using namespace cx;

using ast::NodeType;

Resolver::Resolver(ResolveContext* ctx) {
    m_ctx = ctx;
}

void Resolver::add_primitive(const string& name, ChiType* type) {
    auto node = create_node(ast::NodeType::Primitive);
    node->token = nullptr;
    node->name = name;
    m_ctx->builtins.add(node);

    auto type_name = create_type(TypeId::TypeName);
    type_name->data.type_name.name = &node->name;
    type_name->data.type_name.giving_type = type;
    m_ctx->types[node] = type_name;
}

void Resolver::create_primitives() {
    add_primitive("bool", create_type(TypeId::Bool));
    add_primitive("int", create_type(TypeId::Int));
    add_primitive("void", create_type(TypeId::Void));
}

void Resolver::add_builtin(const std::string& name, ChiType* type) {
    auto fn = create_node(ast::NodeType::FnDef);
    fn->name = name;
    fn->data.fn_def.is_builtin = true;
    m_ctx->builtins.add(fn);
    m_ctx->types[fn] = type;
}

void Resolver::create_builtins() {
    auto printf_type = create_type(TypeId::Fn);
    printf_type->data.fn.return_type = create_type(TypeId::Void);
    auto& params = printf_type->data.fn.params;
    params.add(create_type(TypeId::String));
    params.add(create_type(TypeId::Int));
    add_builtin("printf", printf_type);
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

ChiType* Resolver::get_node_type(ast::Node* node) {
    auto result = m_ctx->types.get(node);
    return result ? *result : nullptr;
}

void Resolver::resolve(ast::Package* package) {
    for (auto& module: package->modules) {
        resolve(&module);
    }
}

void Resolver::resolve(ast::Module* module) {
    m_module = module;
    resolve(module->root, ResolveScope());
}

bool Resolver::can_assign(ChiType* from_type, ChiType* to_type) {
    return from_type->id == to_type->id;
}

ChiType* Resolver::to_value_type(ChiType* type) {
    if (type->id == TypeId::TypeName) {
        return type->data.type_name.giving_type;
    }
    return type;
}

ChiType* Resolver::_resolve(ast::Node* node, const ResolveScope& scope) {
    static auto bool_type = create_type(TypeId::Bool);
    static auto void_type = create_type(TypeId::Void);

    switch (node->type) {
        case NodeType::Root:
            for (auto decl: node->data.root.top_level_decls) {
                resolve(decl, scope);
                if (decl->type == NodeType::FnDef && decl->name == "main") {
                    node->module->package->entry_fn = decl;
                }
            }
            return nullptr;
        case NodeType::FnDef: {
            auto& data = node->data.fn_def;
            auto proto = resolve(data.fn_proto, scope);
            if (should_resolve_fn_body(scope)) {
                resolve(data.body, scope.set_parent_fn(proto));
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
                return scope.parent_struct;
            }
            auto type = resolve(data.decl, scope);
            return type;
        }
        case NodeType::ParamDecl: {
            auto& data = node->data.param_decl;
            return to_value_type(resolve(data.type, scope));
        }
        case NodeType::VarDecl: {
            auto& data = node->data.var_decl;
            auto var_type = to_value_type(resolve(data.type, scope));
            if (data.expr) {
                auto expr_type = resolve(data.expr, scope.set_value_type(var_type));
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
        case NodeType::LiteralExpr: {
            auto token = node->token;
            switch (token->type) {
                case TokenType::INT:
                    return create_type(TypeId::Int);
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
            auto member = get_struct_member(expr_type, field_name);
            if (!member) {
                error(node, errors::FIELD_NOT_FOUND, field_name, to_string(expr_type));
                return nullptr;
            }
            return member->type;
        }
        case NodeType::ComplitExpr: {
            auto& data = node->data.complit_expr;
            if (!scope.value_type) {
                error(node, errors::COMPLIT_CANNOT_INFER_TYPE);
                return nullptr;
            }
            auto value_type = scope.value_type;
            auto constructor = get_struct_member(value_type, "new");
            if (!constructor) {
                error(node, errors::FIELD_NOT_FOUND, "new", to_string(value_type));
                return nullptr;
            }
            resolve_fn_call(node, scope, &constructor->type->data.fn, &data.items);
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
            auto struct_type = create_type(TypeId::Struct);
            auto struct_ = &struct_type->data.struct_;
            auto struct_scope = scope.set_parent_struct(struct_type);
            // first pass, method bodies are skipped
            struct_->resolve_status = ResolveStatus::None;
            for (auto member: data.members) {
                resolve_struct_member(struct_type, member, struct_scope);
            }
            struct_->resolve_status = ResolveStatus::MemberTypesKnown;
            // second pass, resolve method bodies
            for (auto member: data.members) {
                if (member->type == NodeType::FnDef) {
                    auto fn_type = get_node_type(member);
                    resolve(member->data.fn_def.body, struct_scope.set_parent_fn(fn_type));
                }
            }
            auto struct_typename = create_type(TypeId::TypeName);
            struct_typename->data.type_name.name = &node->name;
            struct_typename->data.type_name.giving_type = struct_type;
            return struct_typename;
        }
        default:
            print("\n");
            panic("unhandled {}", PRINT_ENUM(node->type));
    }
    return nullptr;
}

ChiType* Resolver::resolve(ast::Node* node, const ResolveScope& scope) {
    auto cached = get_node_type(node);
    if (cached) {
        return cached;
    }
    auto result = _resolve(node, scope);
    m_ctx->types[node] = result;
    return result;
}

string Resolver::to_string(ChiType* type) {
    return PRINT_ENUM(type->id);
}

void Resolver::check_assignment(ast::Node* node, ChiType* from_type, ChiType* to_type) {
    if (!can_assign(from_type, to_type)) {
        error(node, errors::CANNOT_CONVERT, to_string(from_type), to_string(to_type));
    }
}

void Resolver::context_init_builtins() {
    create_primitives();
    create_builtins();
}

ChiStructMember* Resolver::resolve_struct_member(ChiType* struct_type, ast::Node* node, const ResolveScope& scope) {
    auto& struct_ = struct_type->data.struct_;
    auto member = struct_.members.emplace();
    member->struct_ = struct_type;
    member->node = node;
    member->type = resolve(node, scope);
    struct_.members_table[node->name] = member;
    return member;
}

bool Resolver::should_resolve_fn_body(const ResolveScope& scope) {
    auto parent_struct = scope.parent_struct;
    return !parent_struct || parent_struct->data.struct_.resolve_status >= ResolveStatus::MemberTypesKnown;
}

ChiStructMember* Resolver::get_struct_member(ChiType* struct_type, const string& field_name) {
    if (struct_type->id != TypeId::Struct) {
        return nullptr;
    }
    auto& data = struct_type->data.struct_;
    auto member = data.members_table.get(field_name);
    return member ? *member : nullptr;
}

void Resolver::resolve_fn_call(ast::Node* node, const ResolveScope& scope, ChiTypeFn* fn, NodeList* args) {
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

Scope* ScopeResolver::push_scope(ast::Node* owner) {
    m_current_scope = m_scopes.emplace(m_current_scope);
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
