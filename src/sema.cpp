/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "sema.h"

using namespace cx;

ast::Node* Scope::find_one(string symbol) {
    if (auto val = symbols.get(symbol)) {
        return val->at(0);
    }
    return {};
}

NodeList* Scope::find_all(string symbol) {
    if (auto list = symbols.get(symbol)) {
        return &list.value();
    }
    return {};
}
