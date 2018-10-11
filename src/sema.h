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

    MAKE_ENUM(TypeId, TypeName, Fn, Void, Int, Bool, String, Struct, Pointer)

    struct ChiTypeTypeName {
        ChiType* giving_type;
        string* name;
    };

    struct ChiTypeFn {
        ChiType* return_type;
        array<ChiType*> params;
        ChiType* struct_ = nullptr;
    };

    struct ChiStructField {
        ChiType* type;
        ChiType* struct_;
        ast::Node* node;
        long index;
    };

    struct ChiStructMember {
        ast::Node* node;
        ChiStructField* field;
    };

    MAKE_ENUM(ResolveStatus, None, MemberTypesKnown);
    struct ChiTypeStruct {
        ast::Node* node;
        array<ChiStructField> fields;
        map<string, ChiStructMember> members_table;
        ResolveStatus resolve_status;
    };

    struct ChiTypePointer {
        ChiType* base;
        bool is_ref;
    };

    struct ChiType {
        TypeId id;

        union Data {
            ChiTypeFn fn;
            ChiTypeTypeName type_name;
            ChiTypeStruct struct_;
            ChiTypePointer pointer;

            Data() {}

            ~Data() {}
        } data;

        union Meta {
            ChiStructMember* struct_member;
        } meta;

        ChiType(TypeId id) {
            this->id = id;
            if (id == TypeId::Struct) {
                new(&data.struct_) ChiTypeStruct();
            } else {
                memset(&data, 0, sizeof(data));
            }
        }

        ~ChiType() {
#define CHITYPE_CASE_DESTROY_FIELD(field, type, type_struct) case TypeId::type: data.field.~type_struct(); break;
            switch (id) {
                CHITYPE_CASE_DESTROY_FIELD(fn, Fn, ChiTypeFn)
                CHITYPE_CASE_DESTROY_FIELD(struct_, Struct, ChiTypeStruct)
                default:
                    break;
            }
        }
    };

    typedef array<ast::Node*> NodeList;

    struct Scope {
        Scope* parent;
        ast::Node* owner = nullptr;

        explicit Scope(Scope* parent) { this->parent = parent; }

        ast::Node* find_one(const string& symbol);

        NodeList* find_all(const string& symbol);

        void put(const string& name, ast::Node* node);

    private:
        map<string, NodeList> symbols;
    };
}
