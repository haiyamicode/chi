/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "resolver.h"
#include "error.h"

using namespace cx;

Resolver::Resolver() {
    init_primitives();
}

void Resolver::add_primitive(const string& name) {
    auto node = m_primitives.emplace(ast::NodeType::Identifier);
    node->token = nullptr;
    node->name = name;
    node->data.identifier.kind = ast::IdentifierKind::TypeName;
    node->data.identifier.is_builtin = true;
}

void Resolver::init_primitives() {
    add_primitive("int");
    add_primitive("void");
}

Scope* ModuleResolver::push_scope() {
    m_current_scope = m_scopes.emplace(m_current_scope);
    return m_current_scope;
}

void ModuleResolver::pop_scope() {
    m_current_scope = m_current_scope->parent;
}

bool ModuleResolver::declare_symbol(const string& name, ast::Node* node) {
    if (m_resolver->get_primitive(name) || m_current_scope->find_one(name)) {
        return false;
    }
    m_current_scope->put(name, node);
    return true;
}

ast::Node* ModuleResolver::find_symbol(const string& name) {
    if (auto prim = m_resolver->get_primitive(name)) {
        return prim;
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


ast::Node* Resolver::get_primitive(const string& name) {
    for (auto& node: m_primitives) {
        if (node.name == name) {
            return &node;
        }
    }
    return nullptr;
}
