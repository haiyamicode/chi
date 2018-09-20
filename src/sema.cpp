/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "sema.h"

using namespace cx;

ast::Node* Scope::find_one(const string& symbol) {
    if (auto val = symbols.get(symbol)) {
        auto list = val;
        return val->at(0);
    }
    return 0;
}

NodeList* Scope::find_all(const string& symbol) {
    if (auto list = symbols.get(symbol)) {
        return list;
    }
    return nullptr;
}

void Scope::put(const string& name, ast::Node* node) {
    symbols[name];
    symbols[name].add(node);
}
