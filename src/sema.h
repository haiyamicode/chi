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
struct ChiTypeStruct;
struct ChiTypeEnum;
struct ChiLifetime;

MAKE_ENUM(TypeKind, TypeSymbol, Fn, Void, Int, Float, Bool, Char, Rune, String, Struct, Pointer,
          Reference, MutRef, MoveRef, Array, FixedArray, Enum, EnumValue, Any, Subtype, Placeholder, Optional,
          Result, FnLambda, Promise, Infer, Module, This, ThisType, Unknown, Bytes,
          Undefined, ZeroInit, Never)

MAKE_ENUM(Visibility, Public, Private, Protected)

MAKE_ENUM(IntrinsicSymbol, None, Index, IndexMut, IndexMutIterable, CopyFrom, Display, Add, Sub, Mul, Div, Rem, Neg, BitAnd, BitOr, BitXor, BitNot, Shl, Shr, Sized, AllowUnsized, Construct, Unwrap, UnwrapMut, MutIterator, MutIterable, Slice, Eq, Ord)

MAKE_ENUM(DotKind, Field, EnumVariant, MethodToLambda, TypeTrait);

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
    bool is_static = false; // static method variant — container_ref for ID only, no 'this' param
    array<ChiType *> type_params = {};
    array<ChiLifetime *> lifetime_params = {};

    ChiType *get_param_at(size_t index);
    int get_va_start();
    bool should_use_sret();
    bool is_generic() const { return type_params.len > 0; }
};

struct WhereBound {
    long param_index = -1;    // Index into parent struct's type_params
    ChiType *trait = nullptr; // Required interface type
};

struct WhereCondition {
    array<WhereBound> bounds = {};  // All bounds must be satisfied
};

struct ChiStructMember {
    ast::Node *node = nullptr;
    ChiType *orig_parent = nullptr;
    ChiType *resolved_type = nullptr;
    ChiTypeStruct *parent_struct = nullptr;
    long field_index = -1;
    long method_index = -1;
    IntrinsicSymbol symbol = IntrinsicSymbol::None;
    ChiStructMember *root_variant = nullptr;
    map<TypeId, ChiStructMember *> variants = {};
    ChiStructMember *parent_member = nullptr;
    long vtable_index = -1;
    WhereCondition *where_condition = nullptr;

    string get_name();
    Visibility get_visibility();
    bool check_access(bool is_internal, bool is_write);

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
    array<ChiStructMember *> static_members = {};
    array<ChiType *> type_params = {};
    array<ChiType *> subtypes = {};
    array<InterfaceImpl *> interfaces = {};
    array<ChiType *> embeds = {}; // For interface embeds
    map<string, ChiStructMember *> member_table = {};
    map<string, ChiStructMember *> static_member_table = {};
    map<ChiType *, InterfaceImpl *> interface_table = {};
    optional<string> display_name = std::nullopt;
    string global_id = "";
    ChiType *type = nullptr;

    ResolveStatus resolve_status = ResolveStatus::None;
    int vtable_size = 0;
    map<IntrinsicSymbol, ChiStructMember *> member_intrinsics = {};
    ChiLifetime *this_lifetime = nullptr;  // implicit 'this lifetime

    ChiStructMember *add_member(Context *allocator, const string &name, ast::Node *node,
                                ChiType *resolved_type);

    ChiStructMember *find_member(const string &name);

    ChiStructMember *find_static_member(const string &name);

    InterfaceImpl *add_interface(Context *allocator, ChiType *trait, ChiType *impl);

    bool is_generic() { return type_params.len > 0; }

    static bool is_interface(ChiType *type);
    static bool is_interface(ChiTypeStruct *type) { return type->kind == ContainerKind::Interface; }

    static bool is_pointer_type(ChiType *type);
    static bool is_mutable_pointer(ChiType *type);

    static bool is_generic(ChiType *type);

    ChiStructMember *get_constructor();
    static ChiStructMember *get_constructor(ChiType *type);
    static ChiStructMember *get_destructor(ChiType *type);
};

enum class LifetimeKind {
    Param,   // regular parameter — owner is ParamDecl node
    This,    // implicit 'this' — owner is null
    Return,  // function return — owner is null
};

struct ChiLifetime {
    string name;                  // "this", "x", "h", etc.
    LifetimeKind kind;
    ast::Node *owner = nullptr;   // ParamDecl node for Param kind, null for This/Return
    ChiType *origin = nullptr;    // containing function or struct type
    array<ChiLifetime *> outlives;  // 'a: 'b → a.outlives = {b}
};

struct ChiTypePointer {
    ChiType *elem = nullptr;
    bool is_null = false;
    array<ChiLifetime *> lifetimes;
};

struct ChiTypeArray {
    ChiType *elem = nullptr;
    ChiType *internal = nullptr;
};

struct ChiTypeFixedArray {
    ChiType *elem = nullptr;
    uint32_t size = 0;
};

struct ChiTypeResult {
    ChiType *value = nullptr;
    ChiType *error = nullptr;
    ChiType *internal = nullptr; // internal struct type
};

struct ChiTypeSubtype {
    ChiType *generic = nullptr;
    TypeList args = {};
    ChiType *final_type = nullptr;
    ast::Node *root_node = nullptr;
    ast::Node *generated_fn = nullptr;
};

struct ChiEnumVariant {
    int index = -1;
    ast::Node *node = nullptr;
    ChiTypeEnum *enum_ = nullptr;
    ChiType *resolved_type = nullptr;
    long value = -1;
    string name = "";
};

struct ChiTypeEnum {
    ast::Node *node = nullptr;
    ChiType *discriminator = nullptr;
    ChiType *base_struct = nullptr;
    ResolveStatus resolve_status = ResolveStatus::None;
    int compiled_data_size = -1;
    ChiType *enum_header_struct = nullptr;
    ChiType *base_value_type = nullptr;

    array<ChiType *> type_params = {};
    array<ChiType *> subtypes = {};
    array<ChiEnumVariant *> variants = {};
    map<string, ChiEnumVariant *> variant_table = {};
    ChiType *resolved_generic = nullptr; // non-null for concrete instantiations of generic enums

    ChiEnumVariant *add_variant(Context *allocator, const string &name, ast::Node *node,
                                ChiType *resolved_type);
    ChiEnumVariant *find_member(const string &name);
    bool is_generic() { return type_params.len > 0; }
};

struct ChiTypeEnumValue {
    ChiType *enum_type = nullptr;
    ChiType *variant_struct = nullptr;
    ChiEnumVariant *member = nullptr;
    string discriminator_field = "__value";
    ChiType *resolved_struct = nullptr;
    ChiType *discriminator_type = nullptr;

    ChiTypeEnum *parent_enum();
};

struct ChiTypePlaceholder {
    array<ChiType *> traits = {};
    long index = 0;
    // Source information to disambiguate placeholders
    ast::Node *source_decl =
        nullptr; // The struct/function declaration that owns this type parameter
    string name; // The name of the type parameter (T, U, etc.)
    ChiLifetime *lifetime_bound = nullptr; // Resolved lifetime from T: 'a
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

// Infer type - marks a type position where the type should be inferred from usage.
// Used for lambda return type inference when the expected type contains a placeholder.
// After inference, inferred_type is set to the concrete type.
struct ChiTypeInfer {
    ChiType *placeholder = nullptr;    // The original placeholder this replaces (e.g., U)
    ChiType *inferred_type = nullptr;  // Set after body resolution to the inferred concrete type
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
        ChiTypeFixedArray fixed_array;
        ChiTypeInt int_;
        ChiTypeFloat float_;
        ChiTypeSubtype subtype;
        ChiTypePlaceholder placeholder;
        ChiTypeResult result;
        ChiTypeFnLambda fn_lambda;
        ChiTypePromise promise;
        ChiTypeModule module;
        ChiTypeEnum enum_;
        ChiTypeEnumValue enum_value;
        ChiTypeInfer infer;

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
            CHITYPE_CASE_INIT_FIELD(fixed_array, FixedArray, ChiTypeFixedArray)
            CHITYPE_CASE_INIT_FIELD(pointer, Pointer, ChiTypePointer)
        case TypeKind::Optional:
        case TypeKind::Reference:
        case TypeKind::MutRef:
        case TypeKind::MoveRef:
        case TypeKind::This:
            new (&data.pointer) ChiTypePointer();
            break;
            CHITYPE_CASE_INIT_FIELD(int_, Int, ChiTypeInt)
            CHITYPE_CASE_INIT_FIELD(float_, Float, ChiTypeFloat)
            CHITYPE_CASE_INIT_FIELD(placeholder, Placeholder, ChiTypePlaceholder)
            CHITYPE_CASE_INIT_FIELD(result, Result, ChiTypeResult)
            CHITYPE_CASE_INIT_FIELD(fn_lambda, FnLambda, ChiTypeFnLambda)
            CHITYPE_CASE_INIT_FIELD(promise, Promise, ChiTypePromise)
            CHITYPE_CASE_INIT_FIELD(module, Module, ChiTypeModule)
            CHITYPE_CASE_INIT_FIELD(enum_, Enum, ChiTypeEnum)
            CHITYPE_CASE_INIT_FIELD(enum_value, EnumValue, ChiTypeEnumValue)
            CHITYPE_CASE_INIT_FIELD(infer, Infer, ChiTypeInfer)
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
            CHITYPE_CASE_DESTROY_FIELD(fixed_array, FixedArray, ChiTypeFixedArray)
            CHITYPE_CASE_DESTROY_FIELD(pointer, Pointer, ChiTypePointer)
        case TypeKind::Optional:
        case TypeKind::Reference:
        case TypeKind::MutRef:
        case TypeKind::This:
            data.pointer.~ChiTypePointer();
            break;
            CHITYPE_CASE_DESTROY_FIELD(int_, Int, ChiTypeInt)
            CHITYPE_CASE_DESTROY_FIELD(float_, Float, ChiTypeFloat)
            CHITYPE_CASE_DESTROY_FIELD(placeholder, Placeholder, ChiTypePlaceholder)
            CHITYPE_CASE_DESTROY_FIELD(result, Result, ChiTypeResult)
            CHITYPE_CASE_DESTROY_FIELD(fn_lambda, FnLambda, ChiTypeFnLambda)
            CHITYPE_CASE_DESTROY_FIELD(promise, Promise, ChiTypePromise)
            CHITYPE_CASE_DESTROY_FIELD(module, Module, ChiTypeModule)
            CHITYPE_CASE_DESTROY_FIELD(enum_, Enum, ChiTypeEnum)
            CHITYPE_CASE_DESTROY_FIELD(enum_value, EnumValue, ChiTypeEnumValue)
            CHITYPE_CASE_DESTROY_FIELD(infer, Infer, ChiTypeInfer)
        default:
            break;
        }
    }

    void clone(ChiType *b) {
        b->kind = kind;
        b->name = name;
        b->is_placeholder = is_placeholder;
        b->id = id;
        b->display_name = display_name;
        b->global_id = global_id;

#define CHITYPE_CASE_CLONE_FIELD(field, type, type_struct)                                         \
    case TypeKind::type:                                                                           \
        b->data.field = data.field;                                                                \
        break;

        switch (kind) {
            CHITYPE_CASE_CLONE_FIELD(fn, Fn, ChiTypeFn)
            CHITYPE_CASE_CLONE_FIELD(struct_, Struct, ChiTypeStruct)
            CHITYPE_CASE_CLONE_FIELD(subtype, Subtype, ChiTypeSubtype)
            CHITYPE_CASE_CLONE_FIELD(array, Array, ChiTypeArray)
            CHITYPE_CASE_CLONE_FIELD(fixed_array, FixedArray, ChiTypeFixedArray)
            CHITYPE_CASE_CLONE_FIELD(pointer, Pointer, ChiTypePointer)
        case TypeKind::Optional:
        case TypeKind::Reference:
        case TypeKind::MutRef:
        case TypeKind::This:
            b->data.pointer = data.pointer;
            break;
            CHITYPE_CASE_CLONE_FIELD(int_, Int, ChiTypeInt)
            CHITYPE_CASE_CLONE_FIELD(float_, Float, ChiTypeFloat)
            CHITYPE_CASE_CLONE_FIELD(placeholder, Placeholder, ChiTypePlaceholder)
            CHITYPE_CASE_CLONE_FIELD(result, Result, ChiTypeResult)
            CHITYPE_CASE_CLONE_FIELD(fn_lambda, FnLambda, ChiTypeFnLambda)
            CHITYPE_CASE_CLONE_FIELD(promise, Promise, ChiTypePromise)
            CHITYPE_CASE_CLONE_FIELD(module, Module, ChiTypeModule)
            CHITYPE_CASE_CLONE_FIELD(enum_, Enum, ChiTypeEnum)
            CHITYPE_CASE_CLONE_FIELD(enum_value, EnumValue, ChiTypeEnumValue)
            CHITYPE_CASE_CLONE_FIELD(infer, Infer, ChiTypeInfer)
        default:
            break;
        }
    }

    ChiType *get_elem();

    bool is_raw_pointer() { return kind == TypeKind::Pointer; }

    bool is_pointer_like() {
        return kind == TypeKind::Reference || kind == TypeKind::Pointer || kind == TypeKind::MutRef ||
               kind == TypeKind::MoveRef;
    }

    bool is_reference() {
        return kind == TypeKind::Reference || kind == TypeKind::MutRef || kind == TypeKind::MoveRef;
    }

    bool is_int_like() {
        return kind == TypeKind::Int || kind == TypeKind::Bool || kind == TypeKind::Char || kind == TypeKind::Rune;
    }

    string get_display_name() {
        if (display_name) {
            return *display_name;
        }
        return name.value_or("");
    }

    ChiType *eval() {
        if (kind == TypeKind::This) {
            // In interfaces, This might not have elem set
            return data.pointer.elem ? get_elem() : this;
        }
        // Recursively evaluate pointer/reference types
        if (kind == TypeKind::Pointer || kind == TypeKind::Reference || kind == TypeKind::MutRef) {
            auto elem = get_elem();
            if (elem && elem->kind == TypeKind::This) {
                // Element needs evaluation - this pointer type wraps an unevaluated This
                return this;  // Return as-is, let the caller handle substitution
            }
        }
        return this;
    }

    bool is_primitive_abi_type() {
        switch (kind) {
        case TypeKind::Int:
        case TypeKind::Float:
        case TypeKind::Pointer:
        case TypeKind::Reference:
        case TypeKind::MutRef:
        case TypeKind::Bool:
        case TypeKind::Char:
        case TypeKind::Rune:
        case TypeKind::Void:
        case TypeKind::Never:
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

struct TypeInfo;

struct ChiTypePointerInfoData {
    TypeInfo *elem;
};

union TypeInfoData {
    ChiTypeInt int_;
    ChiTypeFloat float_;
    ChiTypePointerInfoData pointer;
};

#pragma pack(push, 1)
struct TypeInfo {
    int32_t kind = 0;
    int32_t size = 0;
    TypeInfoData data;
    void *destructor = nullptr; // void(*)(void*) — null if no destruction needed
    void *copier = nullptr;     // void(*)(void*, void*) — null = bitwise copy
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
    LANG_FLAG_SAFE = 1 << 1,
    LANG_FLAG_VERBOSE = 1 << 2,
};

inline bool has_lang_flag(uint32_t flags, LANG_FLAG flag) { return (flags & flag) != 0; }

typedef int64_t const_int_t;
typedef double const_float_t;
typedef variant<const_int_t, const_float_t, string> ConstantValue;

} // namespace cx
