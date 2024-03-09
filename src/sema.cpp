/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "sema.h"
#include "ast.h"

using namespace cx;

ChiStructMember *ChiTypeStruct::add_member(const string &name, ast::Node *node,
                                           ChiType *resolved_type) {
    auto member = members.emplace(new ChiStructMember())->get();
    member->node = node;
    member->resolved_type = resolved_type;
    if (node->type == ast::NodeType::FnDef) {
        member->method_index = vtable_size++;
    } else {
        member->field_index = fields.size;
        fields.add(member);
    }
    member_table[name] = member;
    return member;
}

ChiStructMember *ChiTypeStruct::find_member(const string &name) {
    auto found = member_table.get(name);
    return found ? *found : nullptr;
}

InterfaceImpl *ChiTypeStruct::add_interface(ChiType *iface, ChiType *impl) {
    auto entry = interfaces.emplace(new InterfaceImpl())->get();
    entry->interface_type = iface;
    entry->impl_type = impl;
    interface_table[iface] = entry;
    return entry;
}

bool ChiTypeStruct::is_interface(ChiType *type) {
    return type->kind == TypeKind::Struct && type->data.struct_.kind == ContainerKind::Interface;
}

bool ChiTypeStruct::is_generic(ChiType *type) {
    return type->kind == TypeKind::Struct && type->data.struct_.type_params.size > 0;
}

bool ChiTypeStruct::is_pointer_type(ChiType *type) {
    return type->kind == TypeKind::Pointer || type->kind == TypeKind::Reference;
}

ChiStructMember *ChiTypeStruct::get_constructor(ChiType *type) {
    if (type->kind == TypeKind::Struct) {
        return type->data.struct_.find_member("new");
    }
    return nullptr;
}

ChiStructMember *ChiTypeStruct::get_destructor(ChiType *type) {
    if (type->kind == TypeKind::Struct) {
        return type->data.struct_.find_member("delete");
    }
    return nullptr;
}

ChiStructMember *ChiTypeStruct::get_symbol(ChiType *type, IntrinsicSymbol symbol) {
    if (type->kind == TypeKind::Subtype) {
        return get_symbol(type->data.subtype.resolved_struct, symbol);
    } else if (type->kind == TypeKind::Struct) {
        auto it = type->data.struct_.intrinsics.get(symbol);
        return it ? *it : nullptr;
    }
    return nullptr;
}

string ChiStructMember::get_name() { return node->name; }

ast::Node *Scope::find_one(const string &symbol) {
    if (auto val = symbols.get(symbol)) {
        return *val;
    }
    return nullptr;
}

ast::Node *Scope::find_export(const string &symbol) {
    auto node = find_one(symbol);
    if (!node || !node->get_declspec().is_exported()) {
        return nullptr;
    }
    return node;
}

array<ast::Node *> Scope::get_all() {
    array<ast::Node *> list = {};
    for (auto entry : symbols.data) {
        list.add(entry.second);
    }
    return list;
}

ast::Node *Scope::find_parent(ast::NodeType type) {
    for (auto scope = this; scope; scope = scope->parent) {
        if (!scope) {
            break;
        }
        if (scope->owner->type == type) {
            return scope->owner;
        }
    }
    return nullptr;
}

void Scope::put(const string &name, ast::Node *node) { symbols[name] = node; }

ChiType *ChiTypeFn::get_param_at(size_t index) {
    return index < get_va_start() ? params[index] : params.last()->get_elem();
}

int ChiTypeFn::get_va_start() { return params.size - (int)is_variadic; }

ChiType *ChiType::get_elem() {
    switch (kind) {
    case TypeKind::Pointer:
    case TypeKind::Optional:
    case TypeKind::Reference:
    case TypeKind::Box:
        return data.pointer.elem;
    case TypeKind::Array:
        return data.array.elem;
    case TypeKind::Result:
        return data.result.value;
    case TypeKind::Promise:
        return data.promise.value;
    default:
        unreachable();
        return nullptr;
    }
}
