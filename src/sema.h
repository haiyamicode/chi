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
enum class NodeType;
} // namespace ast
struct ChiType;
struct Scope;
struct ChiTypeSubtype;
struct Context;

MAKE_ENUM(TypeKind, TypeSymbol, Fn, Void, Int, Float, Bool, String, Struct, Pointer, Reference,
          Array, Enum, Any, Subtype, Placeholder, Optional, Box, Result, Error, FnLambda, Promise,
          Infer, Module, This, Unknown)

MAKE_ENUM(Visibility, Public, Private)

MAKE_ENUM(IntrinsicSymbol, None, Index, IndexInterable, CopyFrom, Display)

struct ChiTypeTypeSymbol {
    ChiType *giving_type = nullptr;
    ChiType *underlying_type = nullptr;
    bool is_placeholder = false; // is generic placeholder symbol
};

typedef uint32_t TypeId;

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
    ChiType *container_ref = nullptr;
    bool is_extern = false;

    ChiType *get_param_at(size_t index);
    int get_va_start();
    bool should_use_sret();
};

struct ChiStructMember {
    ast::Node *node = nullptr;
    ChiType *orig_parent = nullptr;
    ChiType *resolved_type = nullptr;
    long field_index = -1;
    long method_index = -1;
    IntrinsicSymbol symbol = IntrinsicSymbol::None;
    ChiStructMember *root_variant = nullptr;
    map<TypeId, ChiStructMember *> variants = {};
    ChiStructMember *parent_member = nullptr;
    long vtable_index = -1;

    string get_name();

    bool is_field() { return field_index > -1; }
    bool is_method() { return method_index > -1; }
};

typedef array<ChiStructMember *> ImplMembers;

struct InterfaceImpl {
    ChiType *interface_type = nullptr;
    IntrinsicSymbol inteface_symbol = IntrinsicSymbol::None;
    ChiType *impl_type = nullptr;
    ImplMembers impl_members = {};
    long itable_index = -1;
};

MAKE_ENUM(ResolveStatus, None, MemberTypesKnown, EmbedsResolved, BodiesResolved, Done);

MAKE_ENUM(ContainerKind, Struct, Enum, Union, Interface)

typedef array<ChiType *> TypeList;

struct ChiTypeStruct {
    ContainerKind kind = ContainerKind::Struct;
    ast::Node *node = nullptr;
    array<ChiStructMember *> members = {};
    array<ChiStructMember *> fields = {};
    array<ChiType *> type_params = {};
    array<ChiType *> subtypes = {};
    array<InterfaceImpl *> interfaces = {};
    map<string, ChiStructMember *> member_table = {};
    map<ChiType *, InterfaceImpl *> interface_table = {};
    optional<string> display_name = std::nullopt;

    ResolveStatus resolve_status = ResolveStatus::None;
    int vtable_size = 0;
    map<IntrinsicSymbol, ChiStructMember *> member_intrinsics = {};

    ChiStructMember *add_member(Context *allocator, const string &name, ast::Node *node,
                                ChiType *resolved_type);

    ChiStructMember *find_member(const string &name);

    InterfaceImpl *add_interface(Context *allocator, ChiType *trait, ChiType *impl);

    bool is_generic() { return type_params.len > 0; }

    static bool is_interface(ChiType *type);
    static bool is_interface(ChiTypeStruct *type) { return type->kind == ContainerKind::Interface; }

    static bool is_pointer_type(ChiType *type);

    static bool is_generic(ChiType *type);

    static ChiStructMember *get_constructor(ChiType *type);
    static ChiStructMember *get_destructor(ChiType *type);
};

struct ChiTypePointer {
    ChiType *elem = nullptr;
    bool is_null = false;
};

struct ChiTypeArray {
    ChiType *elem = nullptr;
    ChiType *internal = nullptr;
};

struct ChiTypeResult {
    ChiType *value = nullptr;
    ChiType *error = nullptr;
    ChiType *internal = nullptr; // internal struct type
};

struct ChiTypeSubtype {
    ChiType *generic = nullptr;
    TypeList args = {};
    ChiType *resolved_struct = nullptr;
};

struct ChiTypePlaceholder {
    ChiType *trait = nullptr;
    long index = 0;
};

struct ChiTypeFnLambda {
    ChiType *fn = nullptr;
    ChiType *internal = nullptr;
    TypeList captures = {};

    // function type with captures
    ChiType *bound_fn = nullptr;
    // binding struct which stores captures
    ChiType *bind_struct = nullptr;
};

struct ChiTypePromise {
    ChiType *value = nullptr;
    ChiType *internal = nullptr;
};

struct ChiTypeModule {
    Scope *scope = nullptr;
};

struct ChiType {
    TypeKind kind = TypeKind::Void;
    optional<string> name = {};
    bool is_placeholder = false;
    TypeId id = 0;
    optional<string> display_name = {};
    string global_id = "";

    ChiType() = delete;
    ChiType(const ChiType &) = delete;
    ChiType &operator=(const ChiType &) = delete;

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
        ChiTypeResult result;
        ChiTypeFnLambda fn_lambda;
        ChiTypePromise promise;
        ChiTypeModule module;

        Data() {}

        ~Data() {}
    } data;

    explicit ChiType(TypeKind kind, TypeId id) {
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
            CHITYPE_CASE_INIT_FIELD(result, Result, ChiTypeResult)
            CHITYPE_CASE_INIT_FIELD(fn_lambda, FnLambda, ChiTypeFnLambda)
            CHITYPE_CASE_INIT_FIELD(promise, Promise, ChiTypePromise)
            CHITYPE_CASE_INIT_FIELD(module, Module, ChiTypeModule)
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
            CHITYPE_CASE_DESTROY_FIELD(result, Result, ChiTypeResult)
            CHITYPE_CASE_DESTROY_FIELD(fn_lambda, FnLambda, ChiTypeFnLambda)
            CHITYPE_CASE_DESTROY_FIELD(promise, Promise, ChiTypePromise)
            CHITYPE_CASE_DESTROY_FIELD(module, Module, ChiTypeModule)
        default:
            break;
        }
    }

    void clone(ChiType *b) {
#define CHITYPE_CASE_CLONE_FIELD(field, type, type_struct)                                         \
    case TypeKind::type:                                                                           \
        b->data.field = data.field;                                                                \
        break;

        switch (kind) {
            CHITYPE_CASE_CLONE_FIELD(fn, Fn, ChiTypeFn)
            CHITYPE_CASE_CLONE_FIELD(struct_, Struct, ChiTypeStruct)
            CHITYPE_CASE_CLONE_FIELD(subtype, Subtype, ChiTypeSubtype)
            CHITYPE_CASE_CLONE_FIELD(array, Array, ChiTypeArray)
            CHITYPE_CASE_CLONE_FIELD(pointer, Pointer, ChiTypePointer)
            CHITYPE_CASE_CLONE_FIELD(int_, Int, ChiTypeInt)
            CHITYPE_CASE_CLONE_FIELD(float_, Float, ChiTypeFloat)
            CHITYPE_CASE_CLONE_FIELD(placeholder, Placeholder, ChiTypePlaceholder)
            CHITYPE_CASE_CLONE_FIELD(result, Result, ChiTypeResult)
            CHITYPE_CASE_CLONE_FIELD(fn_lambda, FnLambda, ChiTypeFnLambda)
            CHITYPE_CASE_CLONE_FIELD(promise, Promise, ChiTypePromise)
            CHITYPE_CASE_CLONE_FIELD(module, Module, ChiTypeModule)
        default:
            break;
        }
    }

    ChiType *get_elem();

    bool is_raw_pointer() { return kind == TypeKind::Pointer; }

    bool is_pointer() { return kind == TypeKind::Reference || kind == TypeKind::Pointer; }

    string get_display_name() {
        if (display_name) {
            return *display_name;
        }
        return name.value_or("");
    }

    ChiType *eval() {
        if (kind == TypeKind::This) {
            return get_elem();
        }
        return this;
    }

    bool is_primitive_abi_type() {
        switch (kind) {
        case TypeKind::Int:
        case TypeKind::Float:
        case TypeKind::This:
        case TypeKind::Pointer:
        case TypeKind::Reference:
        case TypeKind::Bool:
        case TypeKind::Void:
            return true;
        default:
            return false;
        }
    }
};

struct Scope {
    Scope *parent = nullptr;
    ast::Node *owner = nullptr;

    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

    explicit Scope(Scope *parent) { this->parent = parent; }

    ast::Node *find_one(const string &symbol, bool recursive = false);

    ast::Node *find_export(const string &symbol);

    array<ast::Node *> get_all();

    array<ast::Node *> get_all_recursive();

    void put(const string &name, ast::Node *node);
    void check_put(const string &name, ast::Node *node);

    ast::Node *find_parent(ast::NodeType type);

  private:
    map<string, ast::Node *> symbols = {};
};

union TypeInfoData {
    ChiTypeInt int_;
};

#pragma pack(push, 1)
struct TypeInfo {
    int32_t kind = 0;
    int32_t size = 0;
    TypeInfoData data;
    int32_t meta_table_len = 0;
    void *meta_table; // Dummy pointer, we store a variable amount of data for the
                      // meta table, starting at this field, each item is a TypeVtableEntry
};
#pragma pack(pop)

#pragma pack(push, 1)
struct TypeMetaEntry {
    int32_t vtable_index = -1;
    IntrinsicSymbol symbol = IntrinsicSymbol::None;
    uint32_t name_len = 0;
    char name[256];
};
#pragma pack(pop)

enum LANG_FLAG : uint32_t {
    LANG_FLAG_NONE = 0,
    LANG_FLAG_MANAGED = 1 << 0,
};

inline bool has_lang_flag(uint32_t flags, LANG_FLAG flag) { return (flags & flag) != 0; }

typedef int64_t const_int_t;
typedef double const_float_t;
typedef variant<const_int_t, const_float_t, string> ConstantValue;

} // namespace cx
