/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "lexer.h"

namespace cx {
    namespace ast {
        struct Node;
    }
    struct ChiType;

    MAKE_ENUM(TypeId, TypeSymbol, Fn, Void, Int, Float, Bool, String,
              Struct, Pointer, Array, Enum)

    struct ChiTypeTypeSymbol {
        ChiType* giving_type;
        ChiType* underlying_type;
        string* name;
    };

    struct ChiTypeInt {
        int bit_count;
        bool is_unsigned;
    };

    struct ChiTypeFloat {
        int bit_count;
    };

    struct ChiTypeFn {
        ChiType* return_type;
        array<ChiType*> params;
        ChiType* container = nullptr;
    };

    struct ChiStructField {
        ChiType* type;
        ChiType* struct_;
        long index;
    };

    struct ChiStructMember {
        ast::Node* node;
        ChiStructField* field;
        ChiType* orig;
        long vtable_index = -1;
    };

    typedef array<ChiStructMember*> ImplTable;

    struct TraitImpl {
        ChiType* trait_type;
        ChiType* impl_type;
        ImplTable impl_table;
        long id = -1;
    };

    MAKE_ENUM(ResolveStatus, None, MemberTypesKnown, EmbedsResolved, Done);

    MAKE_ENUM(ContainerKind, Struct, Enum, Union, Trait)

    struct ChiTypeStruct {
        ContainerKind kind;
        ast::Node* node;
        array<box<ChiStructField>> fields;
        array<box<ChiStructMember>> members;
        map<string, ChiStructMember*> members_table;
        array<box<TraitImpl>> traits;
        map<ChiType*, TraitImpl*> traits_table;
        ResolveStatus resolve_status;
        int vtable_size = 0;

        ChiStructField* add_field();

        ChiStructMember* add_member(const string& name, ast::Node* node, ChiStructField* field);

        ChiStructMember* find_member(const string& name);

        TraitImpl* add_trait(ChiType* trait, ChiType* impl);

        static bool is_trait(ChiType* type);
    };

    struct ChiTypePointer {
        ChiType* elem;
        bool is_ref;
    };

    struct ChiTypeArray {
        ChiType* elem;
        ChiType* internal;
    };

    struct ChiType {
        TypeId id;
        optional<string> name;

        union Data {
            ChiTypeFn fn;
            ChiTypeTypeSymbol type_symbol;
            ChiTypeStruct struct_;
            ChiTypePointer pointer;
            ChiTypeArray array;
            ChiTypeInt int_;
            ChiTypeFloat float_;

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

    struct Scope {
        typedef array<ast::Node*> NodeList;

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
