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
        ast::Node* owner = nullptr;

        Scope(Scope* parent) { this->parent = parent; }

        ast::Node* find_one(const string& symbol);

        NodeList* find_all(const string& symbol);

        void put(const string& name, ast::Node* node);

    private:
        map<string, NodeList> symbols;
    };
}
