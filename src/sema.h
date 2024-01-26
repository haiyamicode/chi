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

MAKE_ENUM(TypeKind, TypeSymbol, Fn, Void, Int, Float, Bool, String, Struct, Pointer, Reference,
          Array, Enum, Any, Subtype, Placeholder, Optional, Box)

struct ChiTypeTypeSymbol {
    ChiType *giving_type = nullptr;
    ChiType *underlying_type = nullptr;
    bool is_placeholder = false; // is generic placeholder symbol
};

struct ChiTypeInt {
    int bit_count = 0;
    bool is_unsigned = false;
};

struct ChiTypeFloat {
    int bit_count = 0;
};

struct ChiTypeFn {
    ChiType *return_type = nullptr;
    array<ChiType *> params = {};
    bool is_variadic = false;
    ChiType *container = nullptr;

    ChiType *get_param_at(size_t index);
    int get_va_start();
};

struct ChiStructMember {
    ast::Node *node = nullptr;
    ChiType *orig_parent = nullptr;
    ChiType *resolved_type = nullptr;
    long field_index = -1;
    long method_index = -1;

    string get_name();

    bool is_field() { return field_index > -1; }
};

typedef array<ChiStructMember *> ImplTable;

struct TraitImpl {
    ChiType *trait_type = nullptr;
    ChiType *impl_type = nullptr;
    ImplTable impl_table = {};
    long itable_index = -1;
};

MAKE_ENUM(ResolveStatus, None, MemberTypesKnown, EmbedsResolved, BodiesResolved, Done);

MAKE_ENUM(ContainerKind, Struct, Enum, Union, Trait)

typedef array<ChiType *> TypeList;

struct ChiTypeStruct {
    ContainerKind kind = ContainerKind::Struct;
    ast::Node *node = nullptr;
    array<box<ChiStructMember>> members = {};
    array<ChiStructMember *> fields = {};
    array<ChiType *> type_params = {};
    array<ChiType *> subtypes = {};
    array<box<TraitImpl>> traits = {};
    map<string, ChiStructMember *> member_table = {};
    map<ChiType *, TraitImpl *> trait_table = {};
    ResolveStatus resolve_status = ResolveStatus::None;
    int vtable_size = 0;

    ChiStructMember *add_member(const string &name, ast::Node *node, ChiType *resolved_type);

    ChiStructMember *find_member(const string &name);

    TraitImpl *add_trait(ChiType *trait, ChiType *impl);

    static bool is_trait(ChiType *type);

    static bool is_pointer_type(ChiType *type);

    static bool is_generic(ChiType *type);
};

struct ChiTypePointer {
    ChiType *elem = nullptr;
};

struct ChiTypeArray {
    ChiType *elem = nullptr;
    ChiType *internal = nullptr;
};

struct ChiTypeSubtype {
    ChiType *generic = nullptr;
    array<ChiType *> args = {};
    ChiType *resolved_struct = nullptr;
};

struct ChiTypePlaceholder {
    ChiType *trait = nullptr;
    long index = 0;
};

typedef uint32_t TypeId;

struct ChiType {
    TypeKind kind = TypeKind::Void;
    optional<string> name = {};
    bool is_placeholder = false;
    TypeId id = 0;

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
        memset(&data, 0, sizeof(data));

#define CHITYPE_CASE_INIT_FIELD(field, type, type_struct)                                          \
    case TypeKind::type:                                                                           \
        new (&data.field) type_struct();                                                           \
        break;

        switch (kind) {
            CHITYPE_CASE_INIT_FIELD(fn, Fn, ChiTypeFn)
            CHITYPE_CASE_INIT_FIELD(type_symbol, TypeSymbol, ChiTypeTypeSymbol)
            CHITYPE_CASE_INIT_FIELD(struct_, Struct, ChiTypeStruct)
            CHITYPE_CASE_INIT_FIELD(subtype, Subtype, ChiTypeSubtype)
            CHITYPE_CASE_INIT_FIELD(array, Array, ChiTypeArray)
            CHITYPE_CASE_INIT_FIELD(pointer, Pointer, ChiTypePointer)
            CHITYPE_CASE_INIT_FIELD(int_, Int, ChiTypeInt)
            CHITYPE_CASE_INIT_FIELD(float_, Float, ChiTypeFloat)
            CHITYPE_CASE_INIT_FIELD(placeholder, Placeholder, ChiTypePlaceholder)
        default:
            break;
        }
    }

    ~ChiType() {
#define CHITYPE_CASE_DESTROY_FIELD(field, type, type_struct)                                       \
    case TypeKind::type:                                                                           \
        data.field.~type_struct();                                                                 \
        break;

        switch (kind) {
            CHITYPE_CASE_DESTROY_FIELD(fn, Fn, ChiTypeFn)
            CHITYPE_CASE_DESTROY_FIELD(struct_, Struct, ChiTypeStruct)
            CHITYPE_CASE_DESTROY_FIELD(subtype, Subtype, ChiTypeSubtype)
            CHITYPE_CASE_DESTROY_FIELD(array, Array, ChiTypeArray)
            CHITYPE_CASE_DESTROY_FIELD(pointer, Pointer, ChiTypePointer)
            CHITYPE_CASE_DESTROY_FIELD(int_, Int, ChiTypeInt)
            CHITYPE_CASE_DESTROY_FIELD(float_, Float, ChiTypeFloat)
            CHITYPE_CASE_DESTROY_FIELD(placeholder, Placeholder, ChiTypePlaceholder)
        default:
            break;
        }
    }

    ChiType *get_elem();

    bool is_raw_pointer() { return kind == TypeKind::Pointer; }

    bool is_pointer() {
        return kind == TypeKind::Reference || kind == TypeKind::Pointer || kind == TypeKind::Box;
    }
};

struct Scope {
    Scope *parent = nullptr;
    ast::Node *owner = nullptr;

    explicit Scope(Scope *parent) { this->parent = parent; }

    ast::Node *find_one(const string &symbol);

    array<ast::Node *> get_all();

    void put(const string &name, ast::Node *node);

  private:
    map<string, ast::Node *> symbols = {};
};

struct TypeInfo {
    int32_t kind = 0;
    int32_t size = 0;

    // store 32 bytes of data
    char data[32];
};

union TypeInfoData {
    ChiTypeInt int_;
};

enum LANG_FLAG : uint32_t {
    LANG_FLAG_NONE = 0,
    LANG_FLAG_MANAGED = 1 << 0,
};

inline bool has_lang_flag(uint32_t flags, LANG_FLAG flag) { return (flags & flag) != 0; }

typedef int64_t const_int_t;
typedef double const_float_t;
typedef variant<const_int_t, const_float_t, string> ConstantValue;
} // namespace cx
