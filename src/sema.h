/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "ast.h"

namespace cx {
    struct ChiType;

    MAKE_ENUM(TypeId, TypeName, Fn, Void, Int, Bool, String)

    struct ChiTypeTypeName {
        ChiType* giving_type;
        string* name;
    };

    struct ChiTypeFn {
        ChiType* return_type;
        array<ChiType*> params;
    };

    struct ChiType {
        TypeId id;

        union Data {
            ChiTypeFn fn;
            ChiTypeTypeName type_name;

            Data() {}

            ~Data() {}
        } data;

        ChiType(TypeId id) {
            this->id = id;
            memset(&data, 0, sizeof(data));
        }

        ~ChiType() {
#define CHITYPE_CASE_DESTROY_FIELD(field, type, type_struct) case TypeId::type: data.field.~type_struct(); break;
            switch (id) {
                CHITYPE_CASE_DESTROY_FIELD(fn, Fn, ChiTypeFn)
                default:
                    break;
            }
        }
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
