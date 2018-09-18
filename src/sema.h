/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "ast.h"

namespace cx {
    MAKE_ENUM(TypeId, Int)

    struct ChiType {
        TypeId id;
        string name;
    };

    typedef array<ast::Node*> NodeList;

    struct Scope {
        Scope* parent;

        Scope(Scope* parent) { this->parent = parent; }

        ast::Node* find_one(string symbol);

        NodeList* find_all(string symbol);

        void put(string name, ast::Node* node) { symbols[name] = {node}; }

    private:
        map<string, NodeList> symbols;
    };
}
