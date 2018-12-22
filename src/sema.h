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

    MAKE_ENUM(TypeKind, TypeSymbol, Fn, Void, Int, Float, Bool, String,
              Struct, Pointer, Array, Enum, Any, Subtype, Placeholder, Optional, Box)

    struct ChiTypeTypeSymbol {
        ChiType* giving_type;
        ChiType* underlying_type;
        bool is_placeholder; // is generic placeholder symbol
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
        bool is_variadic;
        ChiType* container = nullptr;

        ChiType* get_param_at(size_t index);
    };

    struct ChiStructMember {
        ast::Node* node;
        ChiType* orig_parent;
        ChiType* resolved_type;
        long field_index = -1;
        long method_index = -1;

        string get_name();

        bool is_field() { return field_index > -1; }
    };

    typedef array<ChiStructMember*> ImplTable;

    struct TraitImpl {
        ChiType* trait_type;
        ChiType* impl_type;
        ImplTable impl_table;
        long itable_index = -1;
    };

    MAKE_ENUM(ResolveStatus, None, MemberTypesKnown, EmbedsResolved, BodiesResolved, Done);

    MAKE_ENUM(ContainerKind, Struct, Enum, Union, Trait)

    typedef array<ChiType*> TypeList;

    struct ChiTypeStruct {
        ContainerKind kind;
        ast::Node* node;
        array<box<ChiStructMember>> members;
        array<ChiStructMember*> fields;
        array<ChiType*> type_params;
        array<ChiType*> subtypes;
        array<box<TraitImpl>> traits;
        map<string, ChiStructMember*> member_table;
        map<ChiType*, TraitImpl*> trait_table;
        ResolveStatus resolve_status;
        int vtable_size = 0;

        ChiStructMember* add_member(const string& name, ast::Node* node, ChiType* resolved_type);

        ChiStructMember* find_member(const string& name);

        TraitImpl* add_trait(ChiType* trait, ChiType* impl);

        static bool is_trait(ChiType* type);

        static bool is_generic(ChiType* type);
    };

    struct ChiTypePointer {
        ChiType* elem;
    };

    struct ChiTypeArray {
        ChiType* elem;
        ChiType* internal;
    };

    struct ChiTypeSubtype {
        ChiType* generic;
        array<ChiType*> args;
        ChiType* resolved_struct;
    };

    struct ChiTypePlaceholder {
        ChiType* trait;
        long index;
    };

    typedef uint32_t TypeId;

    struct ChiType {
        TypeKind kind;
        optional<string> name;
        bool is_placeholder = false;
        TypeId id;

        union Data {
            ChiTypeFn fn;
            ChiTypeTypeSymbol type_symbol;
            ChiTypeStruct struct_;
            ChiTypePointer pointer;
            ChiTypeArray array;
            ChiTypeInt int_;
            ChiTypeFloat float_;
            ChiTypeSubtype subtype;
            ChiTypePlaceholder placeholder;

            Data() {}

            ~Data() {}
        } data;

        ChiType(TypeKind kind, TypeId id) {
            this->kind = kind;
            this->id = id;
            if (kind == TypeKind::Struct) {
                new(&data.struct_) ChiTypeStruct();
            } else {
                memset(&data, 0, sizeof(data));
            }
        }

        ~ChiType() {
#define CHITYPE_CASE_DESTROY_FIELD(field, type, type_struct) case TypeKind::type: data.field.~type_struct(); break;
            switch (kind) {
                CHITYPE_CASE_DESTROY_FIELD(fn, Fn, ChiTypeFn)
                CHITYPE_CASE_DESTROY_FIELD(struct_, Struct, ChiTypeStruct)
                CHITYPE_CASE_DESTROY_FIELD(subtype, Subtype, ChiTypeSubtype)
                default:
                    break;
            }
        }

        ChiType* get_elem();

        bool is_raw_pointer() { return kind == TypeKind::Pointer; }

        bool is_pointer() { return is_raw_pointer() || kind == TypeKind::Box; }
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

    struct TypeInfo {
        const char* name;
        TypeKind kind;

        TypeInfo(ChiType* type) {
            kind = type->kind;
            name = type->name ? type->name->c_str() : nullptr;
        }
    };

    typedef int64_t const_int_t;
    typedef double const_float_t;
    typedef variant<const_int_t, const_float_t, string> ConstantValue;
}
