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

Resolver::Resolver(Allocator* allocator) {
    m_allocator = allocator;
    init_primitives();
    init_builtins();
}

void Resolver::add_primitive(const string& name, ChiType* type) {
    auto node = create_node(ast::NodeType::Primitive);
    node->token = nullptr;
    node->name = name;
    m_builtins.add(node);

    auto type_name = create_type(TypeId::TypeName);
    type_name->data.type_name.name = &node->name;
    type_name->data.type_name.giving_type = type;
    m_types[node] = type_name;
}

void Resolver::init_primitives() {
    add_primitive("bool", create_type(TypeId::Bool));
    add_primitive("int", create_type(TypeId::Int));
    add_primitive("void", create_type(TypeId::Void));
}

void Resolver::add_builtin(const std::string& name, ChiType* type) {
    auto fn = create_node(ast::NodeType::FnDef);
    fn->name = name;
    fn->data.fn_def.is_builtin = true;
    m_builtins.add(fn);
    m_types[fn] = type;
}

void Resolver::init_builtins() {
    auto printf_type = create_type(TypeId::Fn);
    printf_type->data.fn.return_type = create_type(TypeId::Void);
    auto& params = printf_type->data.fn.params;
    params.add(create_type(TypeId::String));
    params.add(create_type(TypeId::Int));
    add_builtin("printf", printf_type);
}

ChiType* Resolver::create_type(TypeId type_id) {
    return m_allocator->create_type(type_id);
}

ast::Node* Resolver::create_node(ast::NodeType type) {
    return m_allocator->create_node(type);
}

Scope* ModuleResolver::push_scope() {
    m_current_scope = m_scopes.emplace(m_current_scope);
    return m_current_scope;
}

void ModuleResolver::pop_scope() {
    m_current_scope = m_current_scope->parent;
}

bool ModuleResolver::declare_symbol(const string& name, ast::Node* node) {
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

ast::Node* ModuleResolver::find_symbol(const string& name) {
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

ModuleResolver::ModuleResolver(Resolver* resolver) {
    m_resolver = resolver;
    push_scope();
}


ast::Node* Resolver::get_builtin(const string& name) {
    for (auto& node: m_builtins) {
        if (node->name == name) {
            return node;
        }
    }
    return nullptr;
}

ChiType* Resolver::get_node_type(ast::Node* node) {
    auto result = m_types.get(node);
    return result ? *result : nullptr;
}

void Resolver::resolve(ast::Package* package) {
    for (auto& module: package->modules) {
        resolve(&module);
    }
}

void Resolver::resolve(ast::Module* module) {
    m_module = module;
    resolve(module->root);
}

bool Resolver::can_assign(ChiType* from_type, ChiType* to_type) {
    return from_type->id == to_type->id;
}

ChiType* Resolver::_resolve(ast::Node* node) {
    static auto bool_type = create_type(TypeId::Bool);
    static auto void_type = create_type(TypeId::Void);

    switch (node->type) {
        case NodeType::Root:
            for (auto decl: node->data.root.top_level_decls) {
                resolve(decl);
            }
            return nullptr;
        case NodeType::FnDef: {
            auto& data = node->data.fn_def;
            auto proto = resolve(data.fn_proto);
            m_types[node] = proto;
            m_current_fn = &proto->data.fn;
            resolve(data.body);
            return proto;
        }
        case NodeType::FnProto: {
            auto& data = node->data.fn_proto;
            auto type = create_type(TypeId::Fn);
            type->data.fn.return_type = resolve(data.return_type);
            for (auto param: data.params) {
                type->data.fn.params.add(resolve(param));
            }
            return type;
        }
        case NodeType::Identifier: {
            auto& data = node->data.identifier;
            auto type = resolve(data.decl);
            if (type->id == TypeId::TypeName) {
                return type->data.type_name.giving_type;
            }
            return type;
        }
        case NodeType::ParamDecl: {
            auto& data = node->data.param_decl;
            return resolve(data.type);
        }
        case NodeType::VarDecl: {
            auto& data = node->data.var_decl;
            auto var_type = resolve(data.type);
            if (data.expr) {
                auto expr_type = resolve(data.expr);
                check_assignment(node, expr_type, var_type);
            }
            return var_type;
        }
        case NodeType::BinOpExpr: {
            auto& data = node->data.bin_op_expr;
            auto t1 = resolve(data.op1);
            auto t2 = resolve(data.op2);
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
            auto type = data.expr ? resolve(data.expr) : void_type;
            check_assignment(data.expr, type, m_current_fn->return_type);
            return nullptr;
        }
        case NodeType::ParenExpr: {
            auto& child = node->data.child_expr;
            return resolve(child);
        }
        case NodeType::FnCallExpr: {
            auto& data = node->data.fn_call_expr;
            auto fn_type = resolve(data.fn_ref_expr);
            if (fn_type->id != TypeId::Fn) {
                error(data.fn_ref_expr, errors::CANNOT_CALL_NON_FUNCTION);
                return nullptr;
            }
            auto& fn = fn_type->data.fn;
            auto n_args = data.args.size;
            auto n_params = fn.params.size;
            if (n_args != n_params) {
                error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, n_params, n_args);
            }
            for (int i = 0; i < n_args; i++) {
                auto arg = data.args[i];
                auto arg_type = resolve(arg);
                check_assignment(arg, arg_type, fn.params[i]);
            }
            return fn.return_type;
        }
        case NodeType::IfStmt: {
            auto& data = node->data.if_stmt;
            auto cond_type = resolve(data.condition);
            check_assignment(data.condition, cond_type, bool_type);
            resolve(data.then_block);
            if (data.else_node) {
                resolve(data.else_node);
            }
            return nullptr;
        }
        case NodeType::Block: {
            auto& data = node->data.block;
            for (auto stmt: data.statements) {
                resolve(stmt);
            }
            return nullptr;
        }
        default:
            print("\n");
            panic("unhandled {}", PRINT_ENUM(node->type));
    }
    return nullptr;
}

ChiType* Resolver::resolve(ast::Node* node) {
    auto cached = get_node_type(node);
    if (cached) {
        return cached;
    }
    auto result = _resolve(node);
    m_types[node] = result;
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
