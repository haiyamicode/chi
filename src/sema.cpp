/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "sema.h"

using namespace cx;

ChiStructMember* ChiTypeStruct::add_member(const string& name, ast::Node* node, ChiStructField* field) {
    auto& member = members_table[name];
    member = std::make_unique<ChiStructMember>();
    member->node = node;
    member->field = field;
    return member.get();
}

ChiStructMember* ChiTypeStruct::find_member(const string& name) {
    auto found = members_table.get(name);
    return found ? found->get() : nullptr;
}

ChiStructField* ChiTypeStruct::add_field() {
    auto field = fields.emplace(new ChiStructField())->get();
    field->index = fields.size - 1;
    return field;
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
