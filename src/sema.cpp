/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "sema.h"
#include "ast.h"

using namespace cx;

ChiStructMember* ChiTypeStruct::add_member(const string& name, ast::Node* node, ChiType* resolved_type) {
    auto member = members.emplace(new ChiStructMember())->get();
    member->node = node;
    member->resolved_type = resolved_type;
    if (node->type == ast::NodeType::FnDef) {
        member->method_index = vtable_size++;
    } else {
        member->field_index = fields.size;
        fields.add(member);
    }
    members_table[name] = member;
    return member;
}

ChiStructMember* ChiTypeStruct::find_member(const string& name) {
    auto found = members_table.get(name);
    return found ? *found : nullptr;
}

TraitImpl* ChiTypeStruct::add_trait(ChiType* trait, ChiType* impl) {
    auto entry = traits.emplace(new TraitImpl())->get();
    entry->trait_type = trait;
    entry->impl_type = impl;
    traits_table[trait] = entry;
    return entry;
}

bool ChiTypeStruct::is_trait(ChiType* type) {
    return type->id == TypeId::Struct && type->data.struct_.kind == ContainerKind::Trait;
}

bool ChiTypeStruct::is_generic(ChiType* type) {
    return type->id == TypeId::Struct && type->data.struct_.type_params.size > 0;
}

string ChiStructMember::get_name() { return node->name; }

ast::Node* Scope::find_one(const string& symbol) {
    if (auto val = symbols.get(symbol)) {
        return val->at(0);
    }
    return nullptr;
}

Scope::NodeList* Scope::find_all(const string& symbol) {
    if (auto list = symbols.get(symbol)) {
        return list;
    }
    return nullptr;
}

void Scope::put(const string& name, ast::Node* node) {
    symbols[name];
    symbols[name].add(node);
}

ChiType* ChiTypeFn::get_param_at(size_t index) {
    return index < params.size ? params[index] : params.last();
}

ChiType* ChiType::get_elem() {
    switch (id) {
        case TypeId::Pointer:
        case TypeId::Optional:
            return data.pointer.elem;
        case TypeId::Array:
            return data.array.elem;
        default:
            unreachable();
            return nullptr;
    }
}
