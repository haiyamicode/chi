/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "sema.h"
#include "ast.h"

using namespace cx;

ChiStructMember* ChiTypeStruct::add_member(const string& name, ast::Node* node, ChiStructField* field) {
    auto member = members.emplace(new ChiStructMember())->get();
    member->node = node;
    member->field = field;
    if (node->type == ast::NodeType::FnDef) {
        member->vtable_index = vtable_size++;
    }
    members_table[name] = member;
    return member;
}

ChiStructMember* ChiTypeStruct::find_member(const string& name) {
    auto found = members_table.get(name);
    return found ? *found : nullptr;
}

ChiStructField* ChiTypeStruct::add_field() {
    auto field = fields.emplace(new ChiStructField())->get();
    field->index = fields.size - 1;
    return field;
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
