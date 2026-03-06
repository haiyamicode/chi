/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "resolver.h"
#include "ast.h"
#include "c_importer.h"
#include "context.h"
#include "enum.h"
#include "errors.h"
#include "fmt/core.h"
#include "lexer.h"
#include "package_config.h"
#include "sema.h"
#include "util.h"

using namespace cx;

using ast::NodeType;

// Check if a name matches a pattern (supports * wildcard)
static bool matches_pattern(const std::string &name, const std::string &pattern) {
    size_t name_idx = 0;
    size_t pattern_idx = 0;
    size_t star_idx = std::string::npos;
    size_t match_idx = 0;

    while (name_idx < name.length()) {
        if (pattern_idx < pattern.length() && pattern[pattern_idx] == '*') {
            // Remember the position of * and the current match position
            star_idx = pattern_idx;
            match_idx = name_idx;
            pattern_idx++;
        } else if (pattern_idx < pattern.length() &&
                   (pattern[pattern_idx] == name[name_idx] || pattern[pattern_idx] == '?')) {
            // Characters match or pattern has ?
            name_idx++;
            pattern_idx++;
        } else if (star_idx != std::string::npos) {
            // No match, but we have a * - backtrack
            pattern_idx = star_idx + 1;
            match_idx++;
            name_idx = match_idx;
        } else {
            // No match and no * to fall back on
            return false;
        }
    }

    // Skip any trailing * in pattern
    while (pattern_idx < pattern.length() && pattern[pattern_idx] == '*') {
        pattern_idx++;
    }

    return pattern_idx == pattern.length();
}

Resolver::Resolver(ResolveContext *ctx) { m_ctx = ctx; }

ast::Node *Resolver::add_primitive(const string &name, ChiType *type) {
    auto node = create_node(ast::NodeType::Primitive);
    node->token = nullptr;
    node->name = name;
    type->name = name;
    m_ctx->builtins.add(node);
    type->global_id = name;
    auto sym = create_type_symbol(node->name, type);
    node->resolved_type = sym;
    return node;
}

void Resolver::context_init_primitives() {
    if (m_ctx->system_types.any) {
        // Already initialized, skip
        return;
    }
    auto &system_types = m_ctx->system_types;
    system_types.any = create_type(TypeKind::Any);
    system_types.byte_ = create_type(TypeKind::Byte);
    system_types.rune_ = create_type(TypeKind::Rune);
    system_types.uint8 = create_int_type(8, true);
    system_types.int_ = create_int_type(32, false);
    system_types.int32 = create_int_type(32, false);
    system_types.uint32 = create_int_type(32, true);
    system_types.int64 = create_int_type(64, false);
    system_types.uint64 = create_int_type(64, true);
    system_types.float_ = create_float_type(32);
    system_types.float64 = create_float_type(64);
    system_types.void_ = create_type(TypeKind::Void);
    system_types.void_ptr = create_pointer_type(system_types.void_, TypeKind::Pointer);
    system_types.null_ptr = create_pointer_type(system_types.void_, TypeKind::Pointer);
    system_types.null_ptr->data.pointer.is_null = true;
    system_types.void_ref = create_pointer_type(system_types.void_, TypeKind::Reference);
    system_types.bool_ = create_type(TypeKind::Bool);
    system_types.string = create_type(TypeKind::String);
    system_types.str_lit = create_pointer_type(system_types.byte_, TypeKind::Pointer);
    system_types.array = create_type(TypeKind::Array);
    system_types.span = create_type(TypeKind::Span);
    system_types.optional = create_type(TypeKind::Optional);
    system_types.result = create_type(TypeKind::Result);
    // Promise is now defined as a Chi-native struct in runtime.xs
    // system_types.promise = create_type(TypeKind::Promise);
    system_types.undefined = create_type(TypeKind::Undefined);
    system_types.zeroinit = create_type(TypeKind::ZeroInit);
    system_types.never_ = create_type(TypeKind::Never);

    // Create a system lambda type for LLVM compatibility
    // All lambdas use the same underlying structure: {ptr, size, data, flags}
    system_types.lambda = create_type(TypeKind::Struct);
    auto &lambda_struct = system_types.lambda->data.struct_;
    system_types.lambda->display_name = "Lambda";
    system_types.lambda->name = "Lambda";
    lambda_struct.kind = ContainerKind::Struct;
    lambda_struct.add_member(get_allocator(), "ptr", get_dummy_var("ptr"),
                             get_pointer_type(system_types.void_));
    lambda_struct.add_member(get_allocator(), "size", get_dummy_var("size"), system_types.uint32);
    lambda_struct.add_member(get_allocator(), "data", get_dummy_var("data"),
                             get_pointer_type(system_types.void_));

    add_primitive("bool", system_types.bool_);
    add_primitive("string", system_types.string);
    add_primitive("any", system_types.any);
    add_primitive("void", system_types.void_);
    add_primitive("int", system_types.int_);
    add_primitive("int64", system_types.int64);
    add_primitive("byte", system_types.byte_);
    add_primitive("rune", system_types.rune_);
    add_primitive("float", system_types.float_);
    add_primitive("float64", system_types.float64);
    add_primitive("uint8", system_types.uint8);
    add_primitive("int8", create_int_type(8, false));
    add_primitive("int16", create_int_type(16, false));
    add_primitive("int32", create_int_type(32, false));
    add_primitive("uint", create_int_type(32, true));
    add_primitive("uint16", create_int_type(16, true));
    add_primitive("uint32", create_int_type(32, true));
    add_primitive("uint64", create_int_type(64, true));
    add_primitive("never", system_types.never_);

    // non-primitive builtins
    add_primitive("Result", system_types.result);
    // Promise is now defined as a Chi-native struct in runtime.xs
    // add_primitive("Promise", system_types.promise);

    // intrinsic symbols
    m_ctx->intrinsic_symbols["std.ops.Index"] = IntrinsicSymbol::Index;
    m_ctx->intrinsic_symbols["std.ops.IndexMut"] = IntrinsicSymbol::IndexMut;
    m_ctx->intrinsic_symbols["std.ops.IndexMutIterable"] = IntrinsicSymbol::IndexMutIterable;
    m_ctx->intrinsic_symbols["std.ops.CopyFrom"] = IntrinsicSymbol::CopyFrom;
    m_ctx->intrinsic_symbols["std.ops.DisallowCopy"] = IntrinsicSymbol::DisallowCopy;
    m_ctx->intrinsic_symbols["std.ops.Display"] = IntrinsicSymbol::Display;
    m_ctx->intrinsic_symbols["std.ops.Add"] = IntrinsicSymbol::Add;
    m_ctx->intrinsic_symbols["std.ops.Sub"] = IntrinsicSymbol::Sub;
    m_ctx->intrinsic_symbols["std.ops.Mul"] = IntrinsicSymbol::Mul;
    m_ctx->intrinsic_symbols["std.ops.Div"] = IntrinsicSymbol::Div;
    m_ctx->intrinsic_symbols["std.ops.Rem"] = IntrinsicSymbol::Rem;
    m_ctx->intrinsic_symbols["std.ops.Neg"] = IntrinsicSymbol::Neg;
    m_ctx->intrinsic_symbols["std.ops.BitAnd"] = IntrinsicSymbol::BitAnd;
    m_ctx->intrinsic_symbols["std.ops.BitOr"] = IntrinsicSymbol::BitOr;
    m_ctx->intrinsic_symbols["std.ops.BitXor"] = IntrinsicSymbol::BitXor;
    m_ctx->intrinsic_symbols["std.ops.Not"] = IntrinsicSymbol::BitNot;
    m_ctx->intrinsic_symbols["std.ops.Shl"] = IntrinsicSymbol::Shl;
    m_ctx->intrinsic_symbols["std.ops.Shr"] = IntrinsicSymbol::Shr;
    m_ctx->intrinsic_symbols["std.ops.Sized"] = IntrinsicSymbol::Sized;
    m_ctx->intrinsic_symbols["std.ops.AllowUnsized"] = IntrinsicSymbol::AllowUnsized;
    m_ctx->intrinsic_symbols["std.ops.Construct"] = IntrinsicSymbol::Construct;
    m_ctx->intrinsic_symbols["std.ops.Unwrap"] = IntrinsicSymbol::Unwrap;
    m_ctx->intrinsic_symbols["std.ops.UnwrapMut"] = IntrinsicSymbol::UnwrapMut;
    m_ctx->intrinsic_symbols["std.ops.Deref"] = IntrinsicSymbol::Deref;
    m_ctx->intrinsic_symbols["std.ops.DerefMut"] = IntrinsicSymbol::DerefMut;
    m_ctx->intrinsic_symbols["std.ops.MutIterator"] = IntrinsicSymbol::MutIterator;
    m_ctx->intrinsic_symbols["std.ops.MutIterable"] = IntrinsicSymbol::MutIterable;
    m_ctx->intrinsic_symbols["std.ops.Slice"] = IntrinsicSymbol::Slice;
    m_ctx->intrinsic_symbols["std.ops.Eq"] = IntrinsicSymbol::Eq;
    m_ctx->intrinsic_symbols["std.ops.Ord"] = IntrinsicSymbol::Ord;
    m_ctx->intrinsic_symbols["std.ops.Hash"] = IntrinsicSymbol::Hash;
}

ChiType *Resolver::create_type(TypeKind kind) { return m_ctx->allocator->create_type(kind); }

ChiType *Resolver::create_type_symbol(optional<string> name, ChiType *type) {
    auto tysym = create_type(TypeKind::TypeSymbol);
    tysym->name = name;
    tysym->data.type_symbol.giving_type = type;
    tysym->data.type_symbol.underlying_type = type;
    tysym->is_placeholder = type->is_placeholder;
    return tysym;
}

ast::Node *Resolver::create_node(ast::NodeType type) { return m_ctx->allocator->create_node(type); }

ast::Node *Resolver::get_builtin(const string &name) {
    for (auto &node : m_ctx->builtins) {
        if (node->name == name) {
            return node;
        }
    }
    return nullptr;
}

ChiType *Resolver::node_get_type(ast::Node *node) {
    if (!node->resolved_type)
        return nullptr;
    return node->resolved_type;
}

void Resolver::resolve(ast::Package *package) {
    for (auto &module : package->modules) {
        resolve(module.get());
    }
}

void Resolver::resolve(ast::Module *module) {
    ResolveScope scope;
    m_module = module;
    auto module_scope = scope.set_module(module);
    resolve(module->root, module_scope);

    for (const auto &pair : m_ctx->array_of.get()) {
        resolve_struct_type(pair.second);
    }
    for (const auto &pair : m_ctx->span_of.get()) {
        resolve_struct_type(pair.second);
    }
    for (const auto &pair : m_ctx->mut_span_of.get()) {
        resolve_struct_type(pair.second);
    }

    // Dump generic instantiations if requested (for debugging)
    if (getenv("DUMP_GENERICS")) {
        m_ctx->generics.dump(this);
    }
}

bool Resolver::can_assign_fn(ChiType *from_fn, ChiType *to_fn, bool is_explicit) {
    if (!from_fn || !to_fn) {
        return false;
    }

    // Both types must be function types
    if (from_fn->kind != TypeKind::Fn || to_fn->kind != TypeKind::Fn) {
        return false;
    }

    auto &from_data = from_fn->data.fn;
    auto &to_data = to_fn->data.fn;

    // Check parameter count
    if (from_data.params.len != to_data.params.len) {
        return false;
    }

    // Check variadic flag
    if (from_data.is_variadic != to_data.is_variadic) {
        return false;
    }

    // Check return type (covariant)
    if (!can_assign(from_data.return_type, to_data.return_type, is_explicit)) {
        return false;
    }

    // Check parameter types (contravariant)
    for (size_t i = 0; i < from_data.params.len; i++) {
        if (!can_assign(to_data.params[i], from_data.params[i], is_explicit)) {
            return false;
        }
    }

    return true;
}

// Helper function to get integer type size in bits
static int get_int_type_size(ChiType *type) {
    switch (type->kind) {
    case TypeKind::Bool:
        return 1;
    case TypeKind::Byte:
        return 8;
    case TypeKind::Rune:
        return 32;
    case TypeKind::Int:
        return type->data.int_.bit_count;
    default:
        return 0;
    }
}

// Helper function to check if integer conversion is safe (no data loss)
static bool is_safe_int_conversion(ChiType *from, ChiType *to) {
    if (to->kind != TypeKind::Int) {
        return false;
    }

    int from_size = get_int_type_size(from);
    int to_size = get_int_type_size(to);

    if (from_size == 0 || to_size == 0) {
        return false;
    }

    // If target is larger or equal size, it's safe
    if (to_size >= from_size) {
        return true;
    }

    return false;
}

bool Resolver::can_assign(ChiType *from_type, ChiType *to_type, bool is_explicit) {
    if (!from_type || !to_type) {
        return false;
    }

    from_type = from_type->eval();
    to_type = to_type->eval();

    // If either type is Infer with inferred_type, use the inferred type
    if (from_type->kind == TypeKind::Infer && from_type->data.infer.inferred_type) {
        from_type = from_type->data.infer.inferred_type;
    }
    if (to_type->kind == TypeKind::Infer && to_type->data.infer.inferred_type) {
        to_type = to_type->data.infer.inferred_type;
    }

    if (is_same_type(from_type, to_type)) {
        return true;
    }

    // never is the bottom type — assignable to any type
    if (from_type->kind == TypeKind::Never) {
        return true;
    }

    switch (to_type->kind) {
    case TypeKind::Void:
        return from_type->kind == TypeKind::Void;
    case TypeKind::String:
        return from_type->kind == TypeKind::String || from_type == get_system_types()->str_lit;
    case TypeKind::Array:
        return from_type->kind == TypeKind::Array &&
               can_assign(from_type->get_elem(), to_type->get_elem(), is_explicit);
    case TypeKind::Span:
        if (from_type->kind != TypeKind::Span) return false;
        if (!can_assign(from_type->get_elem(), to_type->get_elem(), is_explicit)) return false;
        // []mut T -> []T is allowed (downgrade), []T -> []mut T is not
        if (to_type->data.span.is_mut && !from_type->data.span.is_mut) return false;
        return true;
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef: {
        // Allow null pointer to any pointer type - check this FIRST
        if (from_type->kind == TypeKind::Pointer && from_type->data.pointer.is_null) {
            return true;
        }

        // Allow null_ptr system type (special *void) to any pointer
        if (from_type == get_system_types()->null_ptr) {
            return true;
        }

        // Handle pointer/reference conversions
        if (from_type->kind == TypeKind::Pointer || from_type->kind == TypeKind::Reference ||
            from_type->kind == TypeKind::MutRef || from_type->kind == TypeKind::MoveRef) {

            // Explicit cast: allow any reference/pointer → pointer conversion regardless of element types
            if (is_explicit && to_type->kind == TypeKind::Pointer) {
                return true;
            }

            auto from_elem = from_type->get_elem();
            auto to_elem = to_type->get_elem();

            if (from_elem && to_elem) {
                // Check if the element types are the same
                bool elem_same = is_same_type(from_elem, to_elem);

                if (elem_same) {
                    // Pointer <-> Reference/MutRef conversions are allowed
                    if (from_type->kind == TypeKind::Pointer ||
                        to_type->kind == TypeKind::Pointer) {
                        return true;
                    }
                    // MoveRef → Ref, MutRef, MoveRef are all allowed (ownership subsumes borrow)
                    if (from_type->kind == TypeKind::MoveRef) {
                        return true;
                    }
                    // MutRef -> Ref is allowed
                    if (from_type->kind == TypeKind::MutRef &&
                        to_type->kind == TypeKind::Reference) {
                        return true;
                    }
                    // Ref -> MutRef is NOT allowed
                    if (from_type->kind == TypeKind::Reference &&
                        to_type->kind == TypeKind::MutRef) {
                        return false;
                    }
                    // Ref/MutRef -> MoveRef is NOT allowed (can't create ownership from borrow)
                    if (to_type->kind == TypeKind::MoveRef &&
                        (from_type->kind == TypeKind::Reference ||
                         from_type->kind == TypeKind::MutRef)) {
                        return false;
                    }
                    // Same kind is allowed
                    if (from_type->kind == to_type->kind) {
                        return true;
                    }
                }
                // Allow void* conversions
                else if (to_type->kind == TypeKind::Pointer && to_elem->kind == TypeKind::Void) {
                    // Any pointer/reference can convert to void*
                    return true;
                } else if (from_type->kind == TypeKind::Pointer &&
                           to_type->kind == TypeKind::Pointer) {
                    // Allow void* to any pointer
                    return from_elem->kind == TypeKind::Void;
                }
                // Allow __CxString pointer/reference to convert to *string
                else if (to_type->kind == TypeKind::Pointer && to_elem->kind == TypeKind::String) {
                    if (from_elem->kind == TypeKind::Struct && m_ctx->rt_string_type &&
                        is_same_type(from_elem, m_ctx->rt_string_type)) {
                        return true;
                    }
                }
            }
        }

        // &Concrete → &Interface conversion
        auto to_elem = to_type->get_elem();
        if (to_elem && ChiTypeStruct::is_interface(to_elem)) {
            if (from_type->kind == TypeKind::Pointer || from_type->kind == TypeKind::Reference ||
                from_type->kind == TypeKind::MutRef || from_type->kind == TypeKind::MoveRef) {
                auto from_elem = from_type->get_elem();
                if (from_elem && from_elem->kind == TypeKind::Subtype)
                    from_elem = resolve_subtype(from_elem);
                if (from_elem && from_elem->kind == TypeKind::Struct &&
                    from_elem->data.struct_.kind == ContainerKind::Struct) {
                    if (struct_satisfies_interface(from_elem, to_elem)) return true;
                }
            }
        }

        // Int to pointer conversion must be explicit (after pointer/ref checks)
        if (from_type->is_int_like()) {
            return is_explicit;
        }


        return false;
    }
    case TypeKind::Int: {
        // Allow implicit conversion from bool, char, rune, and smaller int types
        if (from_type->kind == TypeKind::Bool || from_type->kind == TypeKind::Byte ||
            from_type->kind == TypeKind::Rune) {
            return is_safe_int_conversion(from_type, to_type);
        }
        if (from_type->kind == TypeKind::Int) {
            // Implicit: only widening. Explicit: narrowing allowed too.
            return is_safe_int_conversion(from_type, to_type) || is_explicit;
        }
        // Float to int requires explicit conversion
        if (from_type->kind == TypeKind::Float) {
            return is_explicit;
        }
        // Pointer to int requires explicit conversion
        if (from_type->kind == TypeKind::Pointer) {
            return is_explicit;
        }
        // Plain enum to int requires explicit cast
        if (is_explicit && from_type->kind == TypeKind::EnumValue) {
            auto enum_type = from_type->data.enum_value.enum_type;
            return enum_type && enum_type->data.enum_.is_plain;
        }
        return false;
    }
    case TypeKind::Float: {
        // Allow implicit conversion from any int type
        if (from_type->is_int_like()) {
            return true;
        }
        // Implicit: only widening. Explicit: narrowing allowed too.
        if (from_type->kind == TypeKind::Float) {
            return to_type->data.float_.bit_count >= from_type->data.float_.bit_count ||
                   is_explicit;
        }
        return false;
    }
    case TypeKind::Byte:
        if (from_type->kind == TypeKind::Rune) {
            return is_explicit;
        }
        return from_type->kind == TypeKind::Byte || (is_explicit && from_type->is_int_like());
    case TypeKind::Rune:
        if (from_type->kind == TypeKind::Byte) {
            return true; // char -> rune: implicit (safe widening)
        }
        if (from_type->kind == TypeKind::Rune) {
            return true;
        }
        // int -> rune: implicit if safe (same or smaller size), explicit otherwise
        if (from_type->is_int_like()) {
            return is_safe_int_conversion(from_type, get_system_types()->uint32) || is_explicit;
        }
        return false;
    case TypeKind::Any:
        return true;
    case TypeKind::Struct: {
        if (from_type == to_type) {
            return true;
        }
        // Allow primitive string to convert to __CxString
        if (m_ctx->rt_string_type && is_same_type(to_type, m_ctx->rt_string_type)) {
            if (from_type->kind == TypeKind::String) {
                return true;
            }
        }
        return false;
    }
    case TypeKind::Bool:
        // Allow implicit conversion from any int type to bool
        return from_type->is_int_like() || from_type->kind == TypeKind::Optional ||
               from_type->kind == TypeKind::Result || from_type->is_pointer_like() ||
               (is_explicit && from_type->kind == TypeKind::Byte);
    case TypeKind::Optional: {
        // Allow null pointer, same optional type, or implicit T -> ?T wrap
        if (from_type->kind == TypeKind::Pointer) {
            return from_type->data.pointer.is_null;
        }
        return from_type == to_type || can_assign(from_type, to_type->get_elem(), is_explicit);
    }
    case TypeKind::Fn: {
        if (from_type->kind == TypeKind::FnLambda) {
            return can_assign_fn(from_type->data.fn_lambda.fn, to_type, is_explicit);
        }
        return from_type->kind == TypeKind::Fn && can_assign_fn(from_type, to_type, is_explicit);
    }
    case TypeKind::FnLambda:
        if (from_type->kind == TypeKind::Fn) {
            return can_assign_fn(from_type, to_type->data.fn_lambda.fn, is_explicit);
        }
        if (from_type->kind == TypeKind::FnLambda) {
            return can_assign_fn(from_type->data.fn_lambda.fn, to_type->data.fn_lambda.fn,
                                 is_explicit);
        }
        return false;
    case TypeKind::EnumValue:
        if (from_type->kind == TypeKind::EnumValue) {
            return is_same_type(from_type->data.enum_value.enum_type,
                                to_type->data.enum_value.enum_type);
        }
        // int -> plain enum with explicit cast
        if (is_explicit && from_type->kind == TypeKind::Int) {
            auto enum_type = to_type->data.enum_value.enum_type;
            return enum_type && enum_type->data.enum_.is_plain;
        }
        return false;
    case TypeKind::ThisType:
        // ThisType should have been substituted by now in generic contexts
        // If we reach here, it means the substitution didn't happen properly
        return false;
    default:
        break;
    }
    return false;
}

ChiType *Resolver::to_value_type(ChiType *type) {
    if (!type)
        return nullptr;
    if (type->kind == TypeKind::TypeSymbol) {
        return type->data.type_symbol.giving_type;
    }
    return type;
}

static string build_narrowing_path(ast::Node *expr);

// Does lifetime 'a outlive 'b? True if a == b or 'a: 'b was declared (transitively).
static bool lifetime_outlives(ChiLifetime *a, ChiLifetime *b) {
    if (a == b)
        return true;
    for (size_t i = 0; i < a->outlives.len; i++) {
        if (lifetime_outlives(a->outlives[i], b))
            return true;
    }
    return false;
}

ChiType *Resolver::resolve_value(ast::Node *node, ResolveScope &scope) {
    auto value_type = to_value_type(resolve(node, scope));
    if (!value_type)
        return nullptr;
    if (ChiTypeStruct::is_generic(value_type)) {
        // Check if all type params have defaults — if so, instantiate with defaults
        auto &struct_ = value_type->data.struct_;
        auto &decl_params = struct_.node->data.struct_decl.type_params;
        bool all_have_defaults = true;
        for (auto param : decl_params) {
            if (!param->data.type_param.default_type) {
                all_have_defaults = false;
                break;
            }
        }
        if (all_have_defaults) {
            array<ChiType *> args;
            for (auto param : decl_params) {
                args.add(resolve_value(param->data.type_param.default_type, scope));
            }
            return to_value_type(get_subtype(value_type, &args));
        }
        error(node, errors::MISSING_TYPE_ARGUMENTS, format_type_display(value_type));
    }
    return value_type;
}

ChiType *Resolver::_resolve(ast::Node *node, ResolveScope &scope, uint32_t flags) {
    switch (node->type) {
    case NodeType::Root: {
        auto &data = node->data.root;
        // first pass: skip function and struct bodies
        scope.skip_fn_bodies = true;
        for (auto decl : data.top_level_decls) {
            resolve(decl, scope);

            if (decl->type == NodeType::FnDef && decl->name == "main") {
                node->module->package->entry_fn = decl;
                decl->data.fn_def.decl_spec->flags |= ast::DECL_IS_ENTRY;
            }
        }

        // second pass: resolve struct members
        for (auto decl : data.top_level_decls) {
            if (decl->type == NodeType::StructDecl || decl->type == NodeType::EnumDecl) {
                _resolve(decl, scope);
            }
        }

        // third pass: resolve struct embeds
        for (auto decl : data.top_level_decls) {
            if (decl->type == NodeType::StructDecl || decl->type == NodeType::EnumDecl) {
                _resolve(decl, scope);
            }
        }

        // lifetime resolution pass: populate resolved lifetimes on all functions
        // (must run after struct members are resolved so is_borrowing_type works)
        for (auto decl : data.top_level_decls) {
            if (decl->type == NodeType::FnDef) {
                resolve_fn_lifetimes(decl);
            } else if (decl->type == NodeType::StructDecl) {
                for (auto member : decl->data.struct_decl.members) {
                    if (member->type == NodeType::FnDef) {
                        resolve_fn_lifetimes(member);
                    } else if (member->type == NodeType::ImplementBlock) {
                        for (auto impl_member : member->data.implement_block.members) {
                            if (impl_member->type == NodeType::FnDef) {
                                resolve_fn_lifetimes(impl_member);
                            }
                        }
                    }
                }
            }
        }

        // fourth pass: resolve function and method bodies
        scope.skip_fn_bodies = false;
        for (auto decl : data.top_level_decls) {
            if (decl->type == NodeType::StructDecl || decl->type == NodeType::EnumDecl ||
                decl->type == NodeType::FnDef) {
                _resolve(decl, scope);
            }
        }

        // final pass: ensure subtypes are resolved
        for (auto decl : data.top_level_decls) {
            if (decl->type == NodeType::StructDecl && decl->data.struct_decl.type_params.len > 0) {
                auto &struct_ = decl->data.struct_decl;
                if (struct_.type_params.len > 0) {
                    auto struct_type = to_value_type(decl->resolved_type);
                    for (auto subtype : struct_type->data.struct_.subtypes) {
                        resolve_subtype(subtype);
                    }
                }
            }
            // Finalize placeholder lambdas after __CxLambda is resolved
            if (decl->type == NodeType::FnDef) {
                auto fn_type = to_value_type(decl->resolved_type);
                finalize_placeholder_lambda_params(fn_type);
            }
            // if (decl->type == NodeType::FnDef &&
            //     decl->data.fn_def.fn_proto->data.fn_proto.type_params.len > 0) {
            //     auto fn_type = to_value_type(decl->resolved_type);
            //     if (fn_type && fn_type->kind == TypeKind::Fn) {
            //         for (auto variant : fn_type->data.fn.variants) {
            //             resolve_fn_subtype(variant->data.generated_fn.subtype);
            //         }
            //     }
            // }
        }
        return nullptr;
    }
    case NodeType::FnDef: {
        auto &data = node->data.fn_def;
        if (data.fn_kind == ast::FnKind::Lambda) {
            auto &data = node->data.fn_def;
            auto fn_scope = scope.set_parent_fn_node(node);
            auto proto = resolve(data.fn_proto, fn_scope, flags | IS_FN_DECL_PROTO | IS_FN_LAMBDA);
            // For lambda functions, we need to extract the underlying function type
            // from the FnLambda type for proper return type checking
            auto lambda_fn_type =
                proto->kind == TypeKind::FnLambda ? proto->data.fn_lambda.fn : proto;
            auto local_fn_scope = fn_scope.set_parent_fn(lambda_fn_type);
            if (data.body) {
                resolve(data.body, local_fn_scope);
                check_lifetime_constraints(&data);
            }

            // After body resolution, sync the FnLambda's is_placeholder with the inner Fn type
            // This is needed when return type was inferred from the body (placeholder -> concrete)
            if (proto->kind == TypeKind::FnLambda && !lambda_fn_type->is_placeholder) {
                proto->is_placeholder = false;
            }

            // resolve captures
            for (auto &cap : data.captures) {
                auto type = resolve(cap.decl, scope);
                proto->data.fn_lambda.captures.add(type);
            }

            // Borrow tracking: by-ref captures create edges in the function that
            // owns the captured variable, so the lambda is treated as borrowing
            // from that local. Only add edges for variables owned by the immediate
            // enclosing function — deeper captures propagate through the chain.
            if (scope.parent_fn_node) {
                for (auto &cap : data.captures) {
                    if (cap.mode == ast::CaptureMode::ByRef &&
                        cap.decl->parent_fn == scope.parent_fn_node) {
                        scope.parent_fn_node->data.fn_def.add_ref_edge(node, cap.decl);
                    }
                }
            }

            // create bound form of the lambda function - always create for consistent calling
            // convention
            auto fn_type = proto->data.fn_lambda.fn;
            auto &fn_data = fn_type->data.fn;

            // Always create bound function and bind struct for consistent lambda calling convention
            auto bound_type = create_type(TypeKind::Fn);
            proto->data.fn_lambda.bound_fn = bound_type;

            // binding struct
            auto bstruct = create_type(TypeKind::Struct);
            bstruct->display_name = fmt::format("__lambda_{}::Bind", proto->id);
            auto &bstruct_data = bstruct->data.struct_;
            bstruct_data.kind = ContainerKind::Struct;

            if (data.captures.len > 0) {
                // Add capture fields to binding struct
                // By-ref captures: pointer (Reference) to original variable
                // By-value captures: value type directly in struct
                for (int i = 0; i < data.captures.len; i++) {
                    auto &cap = data.captures[i];
                    auto name = fmt::format("capture_{}", i);
                    auto field_type =
                        (cap.mode == ast::CaptureMode::ByValue)
                            ? cap.decl->resolved_type
                            : get_pointer_type(cap.decl->resolved_type, TypeKind::Reference);
                    bstruct_data.add_member(get_allocator(), cap.decl->name, get_dummy_var(name),
                                            field_type);
                }
                // Mark bind struct as placeholder if any field has placeholder types
                for (auto field : bstruct_data.fields) {
                    if (field->resolved_type->is_placeholder) {
                        bstruct->is_placeholder = true;
                        break;
                    }
                }
            }

            // create signature with binding struct as first parameter
            proto->data.fn_lambda.bind_struct = bstruct;
            auto &bound_fn = bound_type->data.fn;

            // Always add binding struct as first parameter (even if empty for lambdas without
            // captures)
            bound_fn.params.add(get_pointer_type(bstruct, TypeKind::Reference));
            for (auto param : fn_data.params) {
                bound_fn.params.add(param);
            }
            bound_fn.return_type = fn_data.return_type;
            bound_fn.is_variadic = fn_data.is_variadic;
            bound_fn.container_ref = fn_data.container_ref;

            // Use __CxLambda directly (no longer generic)
            auto rt_lambda = m_ctx->rt_lambda_type;
            if (rt_lambda &&
                rt_lambda->data.struct_.resolve_status >= ResolveStatus::MemberTypesKnown) {
                proto->data.fn_lambda.internal = to_value_type(rt_lambda);
            } else {
                // Defer if __CxLambda not resolved yet
                proto->data.fn_lambda.internal = nullptr;
            }

            return proto;
        }

        auto fn_scope = scope.set_parent_fn_node(node);
        auto proto = resolve(data.fn_proto, fn_scope, flags | IS_FN_DECL_PROTO);
        if (data.body && should_resolve_fn_body(scope)) {
            fn_scope = fn_scope.set_parent_fn(proto);
            if (data.decl_spec && data.decl_spec->is_unsafe()) {
                fn_scope = fn_scope.set_is_unsafe_block(true);
            }
            resolve(data.body, fn_scope);


            check_lifetime_constraints(&data);

            // Add params to cleanup_vars on the body block
            auto &proto_data = data.fn_proto->data.fn_proto;
            for (auto param : proto_data.params) {
                if (should_destroy(param) && !param->escape.is_capture()) {
                    data.body->data.block.cleanup_vars.add(param);
                    data.has_cleanup = true;
                }
            }
        }
        return proto;
    }
    case NodeType::FnProto: {
        auto &data = node->data.fn_proto;
        auto is_fn_decl = flags & IS_FN_DECL_PROTO;
        auto is_lambda = flags & IS_FN_LAMBDA;

        // Create a new scope for type parameters
        auto fn_scope = scope;
        TypeList type_param_types;

        // Process explicit lifetime parameters: create shared ChiLifetime objects
        map<string, ChiLifetime *> lifetime_map;
        array<ChiLifetime *> fn_lifetime_list;
        for (auto lt_node : data.lifetime_params) {
            auto *lt = new ChiLifetime{lt_node->name, LifetimeKind::Param, nullptr, nullptr};
            lifetime_map[lt_node->name] = lt;
            fn_lifetime_list.add(lt);
        }
        // Second pass: wire up outlives bounds ('a: 'b)
        for (auto lt_node : data.lifetime_params) {
            auto &bound = lt_node->data.lifetime_param.bound;
            if (!bound.empty()) {
                auto *target = lifetime_map.get(bound);
                if (target) {
                    lifetime_map[lt_node->name]->outlives.add(*target);
                } else {
                    error(lt_node, "unknown lifetime '{}'", bound);
                }
            }
        }
        if (lifetime_map.size() > 0) {
            fn_scope.fn_lifetime_params = &lifetime_map;
        }

        // Process type parameters first
        for (auto param : data.type_params) {
            auto type_param = resolve(param, fn_scope);
            type_param_types.add(type_param);
        }

        // Get expected function type from context (for lambda type inference)
        ChiTypeFn *expected_fn = nullptr;
        if (is_lambda && scope.value_type) {
            if (scope.value_type->kind == TypeKind::Fn) {
                expected_fn = &scope.value_type->data.fn;
            } else if (scope.value_type->kind == TypeKind::FnLambda) {
                expected_fn = &scope.value_type->data.fn_lambda.fn->data.fn;
            }
        }

        // Infer return type from expected function type if not provided
        ChiType *return_type = nullptr;
        if (data.return_type) {
            return_type = resolve_value(data.return_type, fn_scope);
        } else if (expected_fn) {
            auto expected_return = expected_fn->return_type;
            // If expected return type is a placeholder, create an Infer type instead.
            // This allows us to infer the actual type from the lambda body rather than
            // copying the placeholder directly.
            if (expected_return && expected_return->kind == TypeKind::Placeholder) {
                auto infer_type = create_type(TypeKind::Infer);
                infer_type->data.infer.placeholder = expected_return;
                infer_type->is_placeholder = true; // Mark as placeholder until inferred
                return_type = infer_type;
            } else {
                return_type = expected_return;
            }
        } else {
            return_type = get_system_types()->void_;
        }

        // Async functions must explicitly return Promise<T>
        bool is_async = node->declspec().is_async();
        if (is_async && !is_promise_type(return_type)) {
            error(node, errors::ASYNC_MUST_RETURN_PROMISE);
        }

        TypeList param_types;
        bool is_variadic = false;
        bool is_static = node->declspec().is_static();
        auto container = is_static ? nullptr : scope.parent_struct;

        if (data.is_vararg) {
            is_variadic = true;
        }
        array<ChiLifetime *> all_ref_lifetimes; // all ref param lifetimes (for return elision)
        for (int i = 0; i < data.params.len; i++) {
            auto param = data.params[i];
            auto &pdata = param->data.param_decl;
            auto is_last = i == data.params.len - 1;
            if (pdata.is_variadic && !is_last) {
                error(param, errors::VARIADIC_NOT_FINAL, param->name);
                return create_type(TypeKind::Fn);
            }
            // Pass expected parameter type for inference if available
            ChiType *expected_param_type = nullptr;
            if (expected_fn && (size_t)i < expected_fn->params.len) {
                expected_param_type = expected_fn->params[i];
            }
            auto param_scope =
                expected_param_type ? fn_scope.set_value_type(expected_param_type) : fn_scope;
            auto param_type = resolve_value(param, param_scope);
            if (pdata.is_variadic) {
                param_type = get_array_type(param_type);
                param->resolved_type = param_type;
                is_variadic = true;
            }
            // Each ref param gets its own distinct lifetime.
            if (param_type && param_type->is_reference() &&
                param_type->data.pointer.lifetimes.len == 0) {
                auto *lt =
                    new ChiLifetime{string(param->name), LifetimeKind::Param, param, nullptr};
                all_ref_lifetimes.add(lt);
                auto *fresh = create_pointer_type(param_type->data.pointer.elem, param_type->kind);
                fresh->data.pointer.lifetimes.add(lt);
                param_type = fresh;
                param->resolved_type = param_type;
            } else if (param_type && !param_type->is_reference() && is_borrowing_type(param_type)) {
                // Borrowing value params get lifetimes so borrows flow through calls.
                // For lifetime-bounded placeholders (T: 'a), use the declared lifetime.
                ChiLifetime *lt;
                if (param_type->kind == TypeKind::Placeholder &&
                    param_type->data.placeholder.lifetime_bound) {
                    lt = param_type->data.placeholder.lifetime_bound;
                } else {
                    lt = new ChiLifetime{string(param->name), LifetimeKind::Param, param, nullptr};
                }
                pdata.borrow_lifetime = lt;
                all_ref_lifetimes.add(lt);
            }
            param_types.add(param_type);
        }

        // Lambda arity flexibility: if the lambda declares fewer params than the
        // expected function type, pad with the remaining expected param types.
        // This allows e.g. a.map(v => v * 2) when map expects func(T, uint32) U.
        if (is_lambda && expected_fn && param_types.len < expected_fn->params.len) {
            for (size_t i = param_types.len; i < expected_fn->params.len; i++) {
                param_types.add(expected_fn->params[i]);
            }
        }

        // Auto-default trailing ?T params to null (walk backwards, stop at first non-optional)
        for (int i = data.params.len - 1; i >= 0; i--) {
            auto &pdata = data.params[i]->data.param_decl;
            if (pdata.default_value || pdata.is_variadic)
                continue;
            auto ptype = data.params[i]->resolved_type;
            if (!ptype || ptype->kind != TypeKind::Optional)
                break;
            auto null_node = create_node(NodeType::LiteralExpr);
            auto null_token = get_allocator()->create_token();
            null_token->type = TokenType::NULLP;
            null_node->token = null_token;
            null_node->resolved_type = get_system_types()->null_ptr;
            pdata.default_value = null_node;
        }

        // Return type lifetime elision: return borrows from min(all ref params)
        if (return_type && return_type->is_reference() &&
            return_type->data.pointer.lifetimes.len == 0) {
            // Include 'this for methods
            if (container) {
                auto &st = container->data.struct_;
                if (!st.this_lifetime) {
                    st.this_lifetime =
                        new ChiLifetime{"this", LifetimeKind::This, nullptr, container};
                }
                all_ref_lifetimes.add(st.this_lifetime);
            }
            if (all_ref_lifetimes.len > 0) {
                // If only one ref source, use it directly; otherwise create a Return
                // lifetime that all ref lifetimes outlive
                ChiLifetime *elided_lt = nullptr;
                if (all_ref_lifetimes.len == 1) {
                    elided_lt = all_ref_lifetimes[0];
                } else {
                    elided_lt = new ChiLifetime{"fn", LifetimeKind::Return, nullptr, nullptr};
                    for (size_t i = 0; i < all_ref_lifetimes.len; i++) {
                        all_ref_lifetimes[i]->outlives.add(elided_lt);
                    }
                }
                auto *fresh =
                    create_pointer_type(return_type->data.pointer.elem, return_type->kind);
                fresh->data.pointer.lifetimes.add(elided_lt);
                return_type = fresh;
            }
        }

        auto is_extern = node->get_declspec() ? node->get_declspec()->is_extern() : false;
        // Only pass container for method declarations, not for function parameter types
        auto method_container = is_fn_decl ? container : nullptr;

        auto fn_type = get_fn_type(return_type, &param_types, is_variadic, method_container,
                                   is_extern, &type_param_types);

        if (fn_lifetime_list.len > 0) {
            fn_type->data.fn.lifetime_params = fn_lifetime_list;
        }

        if (is_lambda || !is_fn_decl) {
            return get_lambda_for_fn(fn_type);
        }
        return fn_type;
    }
    case NodeType::Identifier: {
        auto &data = node->data.identifier;
        if (data.kind == ast::IdentifierKind::This) {
            // Check for narrowed 'this' (e.g., enum variant narrowing in switch)
            if (scope.block && scope.block->scope) {
                auto narrowed = scope.block->scope->find_one("this");
                if (narrowed && narrowed->type == NodeType::VarDecl &&
                    narrowed->data.var_decl.is_generated && narrowed->data.var_decl.narrowed_from) {
                    data.decl = narrowed;
                    return narrowed->resolved_type;
                }
            }
            auto declspec =
                scope.parent_fn_node ? scope.parent_fn_node->declspec() : ast::DeclSpec();
            auto is_static = declspec.is_static();

            if (!scope.parent_struct) {
                error(node, errors::INVALID_THIS);
                return create_type(TypeKind::Unknown);
            }

            if (is_static) {
                error(node, errors::INVALID_THIS_IN_STATIC);
                return create_type(TypeKind::Unknown);
            }

            // Check if we're in an interface context
            bool is_interface = ChiTypeStruct::is_interface(scope.parent_struct);

            auto type = create_type(TypeKind::This);
            auto is_mut = declspec.is_mutable();
            auto this_type = scope.parent_struct;
            if (this_type->data.struct_.is_generic()) {
                auto &tparams = this_type->data.struct_.type_params;
                TypeList placeholders;
                for (auto tp : tparams) {
                    placeholders.add(to_value_type(tp));
                }
                this_type = get_subtype(this_type, &placeholders);
            }
            type->data.pointer.elem = this_type;
            // Attach 'this lifetime so satisfies_lifetime_constraint can check it
            auto &st = scope.parent_struct->data.struct_;
            if (!st.this_lifetime) {
                st.this_lifetime =
                    new ChiLifetime{"this", LifetimeKind::This, nullptr, scope.parent_struct};
            }
            type->data.pointer.lifetimes.add(st.this_lifetime);
            return type;
        }
        if (data.kind == ast::IdentifierKind::ThisType) {
            if (!scope.parent_struct) {
                error(node, errors::INVALID_THIS);
                return create_type(TypeKind::Unknown);
            }

            // Check if we're in an interface context
            bool is_interface = ChiTypeStruct::is_interface(scope.parent_struct);

            if (is_interface) {
                // In interfaces, use ThisType as a placeholder for later substitution
                return create_type(TypeKind::ThisType);
            } else {
                // In struct context, directly resolve to the struct type
                assert(scope.parent_type_symbol);
                if (scope.parent_struct->data.struct_.is_generic()) {
                    // For generic structs, This resolves to e.g. Wrapper<T> (a Subtype with
                    // placeholder args) so that placeholder substitution produces the correct
                    // concrete type. type_params contains TypeSymbols — unwrap to get the
                    // underlying Placeholders.
                    auto &tparams = scope.parent_struct->data.struct_.type_params;
                    TypeList placeholders;
                    for (auto tp : tparams) {
                        placeholders.add(to_value_type(tp));
                    }
                    return get_subtype(scope.parent_struct, &placeholders);
                }
                return scope.parent_type_symbol;
            }
        }
        if (!data.decl) {
            return create_type(TypeKind::Unknown);
        }
        if (data.kind == ast::IdentifierKind::Value && scope.block) {
            auto replacement = scope.block->scope->find_one(data.decl->name);
            if (replacement && replacement->type == NodeType::VarDecl &&
                replacement->data.var_decl.is_generated && replacement != data.decl) {
                data.decl = replacement;
            }
        }
        auto type = resolve(data.decl, scope);
        if (auto decl_fn = data.decl->parent_fn) {
            if (decl_fn != scope.parent_fn_node) {
                data.decl->escape.escaped = true;

                // Build capture path: each function that captures this variable
                // The path represents the chain from innermost to outermost capturing function

                // First, collect all functions in the chain from current to declaration
                array<ast::Node *> function_chain = {};
                auto current_fn = scope.parent_fn_node;
                while (current_fn && current_fn != decl_fn) {
                    function_chain.add(current_fn);
                    current_fn = current_fn->parent_fn;
                }

                // Now propagate captures and build path from innermost to outermost
                for (int i = 0; i < function_chain.len; i++) {
                    auto fn = function_chain[i];
                    auto &fn_def = fn->data.fn_def;
                    auto &captures = fn_def.captures;
                    auto &capture_map = fn_def.capture_map;

                    // Add capture to current function if not already present
                    auto existing = capture_map.get(data.decl);
                    int32_t capture_idx;
                    if (!existing) {
                        capture_idx = captures.len;
                        // Check if this capture is explicitly by-value
                        auto mode = ast::CaptureMode::ByRef;
                        for (auto &vc : fn_def.value_captures) {
                            if (vc == data.decl->name) {
                                mode = ast::CaptureMode::ByValue;
                                break;
                            }
                        }
                        captures.add({data.decl, mode});
                        capture_map[data.decl] = capture_idx;
                    } else {
                        capture_idx = *existing;
                    }

                    // Add this function to the capture path
                    ast::CapturePath path_entry;
                    path_entry.function = fn;
                    path_entry.capture_index = capture_idx;
                    node->escape.capture_path.add(path_entry);
                }
            }
        }

        if (data.decl->type == NodeType::VarDecl && !scope.is_lhs) {
            auto init_at = data.decl->data.var_decl.initialized_at;
            if (!init_at || init_at->token->pos.offset > node->token->pos.offset) {
                error(data.decl, errors::VARIABLE_USED_BEFORE_INITIALIZED, data.decl->name);
            }
        }

        // Check use-after-move/delete via sink_edges (safe mode only)
        if (scope.parent_fn_node && !scope.is_lhs &&
            has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_SAFE)) {
            auto &fn_def = scope.parent_fn_node->data.fn_def;
            if (fn_def.is_sunk(data.decl)) {
                auto *target = fn_def.sink_edges[data.decl];
                bool is_delete = target && target->type == NodeType::PrefixExpr &&
                                 target->data.prefix_expr.prefix->type == TokenType::KW_DELETE;
                array<Note> notes;
                if (target && target->token) {
                    notes.add({is_delete ? "deleted here" : "moved here", target->token->pos});
                }
                error_with_notes(node, std::move(notes), "'{}' used after {}", data.decl->name,
                                 is_delete ? "delete" : "move");
            }
        }

        // Track last-use position for sink check (NLL-like)
        if (scope.parent_fn_node && node->token) {
            auto &fn_def = scope.parent_fn_node->data.fn_def;
            fn_def.terminal_last_use[data.decl] = node->token->pos.offset;
        }

        // Convert function to lambda when used as value (not in call context)
        if (data.decl->type == NodeType::FnDef && !scope.is_fn_call && type->kind == TypeKind::Fn) {
            type = get_lambda_for_fn(type);
        }

        return type;
    }
    case NodeType::TypeSigil: {
        auto &data = node->data.sigil_type;
        auto type = resolve_value(data.type, scope);
        if (data.sigil == ast::SigilKind::FixedArray) {
            return get_fixed_array_type(type, data.fixed_size);
        }
        if (data.sigil == ast::SigilKind::Span) {
            return get_span_type(type, data.is_mut);
        }
        auto kind = get_sigil_type_kind(data.sigil);
        ChiType *final_type;
        if (!data.lifetime.empty()) {
            // Lifetime-annotated ref: create fresh type (not cached) with resolved lifetime
            final_type = create_pointer_type(type, kind);
            if (data.lifetime == "this" && scope.parent_struct) {
                auto *struct_type = scope.parent_struct;
                auto &st = struct_type->data.struct_;
                if (!st.this_lifetime) {
                    st.this_lifetime =
                        new ChiLifetime{"this", LifetimeKind::This, nullptr, struct_type};
                }
                final_type->data.pointer.lifetimes.add(st.this_lifetime);
            } else if (scope.fn_lifetime_params) {
                auto *lt = scope.fn_lifetime_params->get(data.lifetime);
                if (lt) {
                    final_type->data.pointer.lifetimes.add(*lt);
                } else {
                    error(node, "unknown lifetime '{}'", data.lifetime);
                    return create_type(TypeKind::Unknown);
                }
            } else {
                error(node, "unknown lifetime '{}'", data.lifetime);
                return create_type(TypeKind::Unknown);
            }
        } else {
            final_type = get_pointer_type(type, kind);
        }
        return create_type_symbol({}, final_type);
    }
    case NodeType::ParamDecl: {
        auto &data = node->data.param_decl;
        ChiType *result = nullptr;
        if (data.type) {
            result = resolve_value(data.type, scope);
        } else if (scope.value_type) {
            // Infer type from context (lambda type inference)
            result = scope.value_type;
        } else {
            error(node, "missing type annotation for parameter '{}'", node->name);
            return create_type(TypeKind::Void);
        }
        // Resolve default value if present
        if (data.default_value) {
            auto default_scope = scope.set_value_type(result);
            auto default_type = resolve(data.default_value, default_scope);
            check_assignment(data.default_value, default_type, result);
        }
        return result;
    }
    case NodeType::DestructureDecl: {
        auto &data = node->data.destructure_decl;

        // Create temp VarDecl BEFORE resolving — exactly like VarDecl sets move_outlet
        // so ConstructExpr can RVO directly into the temp
        auto temp_var = get_dummy_var("__destructure_tmp", data.expr);
        temp_var->module = node->module;
        temp_var->parent_fn = scope.parent_fn_node;
        temp_var->data.var_decl.kind = ast::VarKind::Immutable;
        temp_var->data.var_decl.initialized_at = node;
        data.temp_var = temp_var;

        // Resolve RHS with move_outlet set to temp — same as VarDecl
        auto expr_scope = scope.set_move_outlet(temp_var);
        auto expr_type = resolve(data.expr, expr_scope);
        if (!expr_type)
            return nullptr;

        temp_var->resolved_type = expr_type;

        if (scope.parent_fn_node) {
            temp_var->decl_order = scope.parent_fn_def()->next_decl_order++;
        }

        // Resolve each field pattern
        if (data.is_array) {
            resolve_array_destructure(node, data.fields, expr_type, scope, data.generated_vars);
        } else {
            resolve_destructure_fields(node, data.fields, expr_type, scope, data.generated_vars);
        }

        // Add temp to cleanup if needed
        if (scope.parent_fn_node && scope.block && should_destroy(temp_var, expr_type) &&
            !temp_var->escape.is_capture()) {
            scope.block->cleanup_vars.add(temp_var);
            scope.parent_fn_def()->has_cleanup = true;
        }

        return expr_type;
    }
    case NodeType::VarDecl: {
        auto &data = node->data.var_decl;
        ChiType *var_type = nullptr;
        if (data.type) {
            var_type = resolve_value(data.type, scope);
            // Interface embed nodes (...InterfaceName inside an interface) are allowed
            // to reference bare interface types. Struct embeds have is_field=true.
            bool is_interface_embed = data.is_embed && !data.is_field;
            if (var_type && ChiTypeStruct::is_interface(var_type) && !is_interface_embed) {
                error(node, errors::BARE_INTERFACE_TYPE, format_type_display(var_type),
                      format_type_display(var_type));
            }
        }
        // Struct fields must have explicit types
        if (data.is_field && !data.type) {
            error(node, "struct field '{}' must have an explicit type", node->name);
            return get_system_types()->void_;
        }
        // Defer field default expression resolution to the body pass,
        // so that other structs' interfaces are fully resolved first
        if (data.is_field && data.expr && var_type) {
            return var_type;
        }
        if (data.expr) {
            // Use explicit type, or scope.value_type as hint for type inference
            auto type_hint = var_type ? var_type : scope.value_type;
            auto var_scope = type_hint ? scope.set_value_type(type_hint) : scope;
            var_scope = var_scope.set_move_outlet(node);
            auto expr_type = resolve(data.expr, var_scope);
            // Escape analysis: track borrow sources for borrowing types
            if (scope.parent_fn_node && !scope.is_unsafe_block && expr_type &&
                is_borrowing_type(expr_type)) {
                add_borrow_source_edges(scope.parent_fn_node->data.fn_def, data.expr, node);
            }
            // Move tracking: &move x sinks source into this variable
            track_move_sink(scope.parent_fn_node, data.expr, expr_type, node,
                            var_type ? var_type : expr_type);
            if (var_type) {
                if (expr_type->kind == TypeKind::Undefined ||
                    expr_type->kind == TypeKind::ZeroInit) {
                    return var_type;
                }
                if (data.expr->type != NodeType::ConstructExpr ||
                    data.expr->data.construct_expr.type) {
                    check_assignment(data.expr, expr_type, var_type);
                }
            } else {
                var_type = expr_type;
            }
            // DisallowCopy: error if initializing from a named value
            if (var_type && is_non_copyable(var_type) && is_addressable(data.expr) &&
                !data.expr->escape.moved) {
                error(data.expr, errors::TYPE_NOT_COPYABLE,
                      format_type_display(var_type));
            }
        }
        if (!var_type) {
            // Failed to resolve variable type due to malformed expression
            error(node, "failed to resolve variable type");
            return get_system_types()->void_;
        }
        if (var_type->kind == TypeKind::Void) {
            error(node, errors::INVALID_VARIABLE_TYPE, format_type_display(var_type));
        }
        // Compile-time constants must be evaluable at compile time
        if (data.kind == ast::VarKind::Constant) {
            data.resolved_value = resolve_constant_value(data.expr);
            if (!data.resolved_value.has_value() && !scope.parent_fn_node) {
                error(node,
                      "const '{}' cannot be evaluated at compile time; use 'let' for "
                      "runtime-initialized values",
                      node->name);
            }
            return var_type;
        }
        // Global mutable variables are not supported (struct fields and let bindings are fine)
        if (!scope.parent_fn_node && !data.is_field && !data.is_embed &&
            data.kind == ast::VarKind::Mutable) {
            error(node, "global variables are not supported; use 'let' or 'const' for module-level "
                        "declarations");
            return var_type;
        }
        // Assign declaration order for local variables (used for intra-function lifetime ordering)
        if (scope.parent_fn_node && !data.is_field) {
            node->decl_order = scope.parent_fn_def()->next_decl_order++;
        }
        // Add to cleanup_vars on the current block
        if (scope.parent_fn_node && scope.block && should_destroy(node, var_type) &&
            !node->escape.is_capture()) {
            scope.block->cleanup_vars.add(node);
            scope.parent_fn_def()->has_cleanup = true;
        }
        return var_type;
    }
    case NodeType::BinOpExpr: {
        auto &data = node->data.bin_op_expr;
        auto op1_scope = scope;
        if (is_assignment_op(data.op_type)) {
            op1_scope = scope.set_is_lhs(true);
        }
        auto t1 = resolve(data.op1, op1_scope);
        if (!t1) {
            return nullptr;
        }

        ChiType *t2;
        if (is_assignment_op(data.op_type)) {
            auto var = data.op1->get_decl();
            if (var && var->type == NodeType::VarDecl) {
                // Cannot assign to immutable or constant variables
                if (var->data.var_decl.kind != ast::VarKind::Mutable) {
                    error(node, errors::ASSIGNMENT_TO_CONST, var->name);
                }
            }
            if (var && var->type == NodeType::VarDecl) {
                auto &vd = var->data.var_decl;
                // First assignment: no previous initialization at all
                bool is_first = !vd.initialized_at;
                // Field with only a default value (parser sets initialized_at = var itself)
                // being assigned for the first time in a constructor
                if (!is_first && vd.is_field && vd.initialized_at == var && scope.parent_fn &&
                    scope.parent_fn->name == "new") {
                    is_first = true;
                }
                if (is_first) {
                    data.is_initializing = true;
                    vd.initialized_at = node;
                    if (var->root_node) {
                        var->root_node->data.var_decl.initialized_at = node;
                    }
                }
            }
            auto var_scope = scope.set_value_type(t1);
            // For plain assignment, allow construct-into-target optimization.
            // For compound assignments (+=, -=, etc.), don't set move_outlet because
            // the old LHS value must be read by the operator method before being overwritten.
            if (data.op_type == TokenType::ASS) {
                var_scope = var_scope.set_move_outlet(data.op1);
            }
            t2 = resolve(data.op2, var_scope);
            if (scope.parent_fn_node) {
                auto lhs_decl = find_root_decl(data.op1);
                if (lhs_decl && !scope.is_unsafe_block && t2 && is_borrowing_type(t2)) {
                    auto &fn_def = scope.parent_fn_node->data.fn_def;
                    fn_def.bump_edge_offset(lhs_decl);
                    add_borrow_source_edges(fn_def, data.op2, lhs_decl);
                    fn_def.add_terminal(lhs_decl);
                }
                // Move tracking for assignments
                if (lhs_decl) {
                    track_move_sink(scope.parent_fn_node, data.op2, t2, lhs_decl, t1);
                }
            }
        } else {
            // Propagate type context for literal inference (e.g., `x == 0` where x is uint32)
            auto ctx_type = t1;
            // For ??, propagate the unwrapped type so RHS gets T context, not ?T
            if (data.op_type == TokenType::QUES_QUES && t1->kind == TypeKind::Optional) {
                ctx_type = t1->get_elem();
            }
            auto op2_scope = scope.set_value_type(ctx_type);
            t2 = resolve(data.op2, op2_scope);
        }

        if (!t2) {
            return nullptr;
        }

        // For plain assignment
        if (data.op_type == TokenType::ASS) {
            check_assignment(data.op2, t2, t1);
            // DisallowCopy: error if assigning from a named value
            if (is_non_copyable(t1) && is_addressable(data.op2) &&
                !data.op2->escape.moved) {
                error(data.op2, errors::TYPE_NOT_COPYABLE, format_type_display(t1));
            }
            return t1;
        }
        // For compound assignment operators, validate the base op
        if (is_assignment_op(data.op_type)) {
            // Check if an operator method handles this (e.g. Add for +=)
            auto base_op = get_assignment_op(data.op_type);
            auto intrinsic = get_operator_intrinsic_symbol(base_op);
            if (intrinsic != IntrinsicSymbol::None) {
                auto method_call = try_resolve_operator_method(
                    intrinsic, t1, t2, data.op1, data.op2, node, scope);
                if (method_call.has_value()) {
                    data.resolved_call = method_call->call_node;
                    return t1;
                }
            }
            check_binary_op(node, data.op_type, t1);
            check_assignment(data.op2, t2, t1);
            return t1;
        }

        // Non-assignment: operands are consumed by the operator, ensure any temps are owned
        ensure_temp_owner(data.op1, t1, scope);
        ensure_temp_owner(data.op2, t2, scope);

        switch (data.op_type) {
        case TokenType::EQ:
        case TokenType::NE:
        case TokenType::LT:
        case TokenType::LE:
        case TokenType::GT:
        case TokenType::GE: {
            // Pointer ordering comparisons require unsafe
            if ((t1->kind == TypeKind::Pointer || t2->kind == TypeKind::Pointer) &&
                data.op_type != TokenType::EQ && data.op_type != TokenType::NE) {
                if (!scope.is_unsafe_block) {
                    error(node, "pointer comparison requires unsafe block");
                    return nullptr;
                }
            }
            // Try operator method for struct types (e.g. Eq::eq for strings)
            IntrinsicSymbol cmp_sym = get_operator_intrinsic_symbol(data.op_type);
            if (cmp_sym != IntrinsicSymbol::None) {
                auto method_call = try_resolve_operator_method(cmp_sym, t1, t2, data.op1,
                                                               data.op2, node, scope);
                if (method_call.has_value()) {
                    data.resolved_call = method_call->call_node;
                }
            }
            return get_system_types()->bool_;
        }
        case TokenType::LAND:
        case TokenType::LOR:
            check_assignment(data.op1, t1, get_system_types()->bool_);
            check_assignment(data.op2, t2, get_system_types()->bool_);
            return get_system_types()->bool_;
        case TokenType::QUES_QUES: {
            if (t1->kind != TypeKind::Optional) {
                error(node, "left operand of ?? must be optional, got {}", format_type_display(t1));
                return nullptr;
            }
            auto elem_type = t1->get_elem();
            check_assignment(data.op2, t2, elem_type);
            return elem_type;
        }
        default: {
            // Pointer arithmetic (requires unsafe)
            if (data.op_type == TokenType::ADD || data.op_type == TokenType::SUB) {
                bool lhs_ptr = t1->kind == TypeKind::Pointer;
                bool rhs_ptr = t2->kind == TypeKind::Pointer;
                bool lhs_int = t1->is_int_like();
                bool rhs_int = t2->is_int_like();

                // ptr + int, ptr - int
                if (lhs_ptr && rhs_int) {
                    if (!scope.is_unsafe_block) {
                        error(node, "pointer arithmetic requires unsafe block");
                        return nullptr;
                    }
                    return t1;
                }
                // int + ptr
                if (lhs_int && rhs_ptr && data.op_type == TokenType::ADD) {
                    if (!scope.is_unsafe_block) {
                        error(node, "pointer arithmetic requires unsafe block");
                        return nullptr;
                    }
                    return t2;
                }
                // ptr - ptr (same element type)
                if (lhs_ptr && rhs_ptr && data.op_type == TokenType::SUB) {
                    if (!scope.is_unsafe_block) {
                        error(node, "pointer arithmetic requires unsafe block");
                        return nullptr;
                    }
                    if (!is_same_type(t1->get_elem(), t2->get_elem())) {
                        error(node, "pointer subtraction requires same element type, got {} and {}",
                              format_type_display(t1), format_type_display(t2));
                        return nullptr;
                    }
                    return get_system_types()->int64;
                }
            }

            // First, check if left operand has interface method for this operator
            IntrinsicSymbol intrinsic_symbol = get_operator_intrinsic_symbol(data.op_type);

            ChiType *result_type = t1;

            // If either operand is float, result is float
            if (t1->kind == TypeKind::Float || t2->kind == TypeKind::Float) {
                // If both are float, use the larger type
                if (t1->kind == TypeKind::Float && t2->kind == TypeKind::Float) {
                    result_type = t1->data.float_.bit_count >= t2->data.float_.bit_count ? t1 : t2;
                } else {
                    result_type = t1->kind == TypeKind::Float ? t1 : t2;
                }
            }
            // For integer operations, use the larger type
            else if (t1->is_int_like() && t2->is_int_like()) {
                // If either operand is rune, result is rune
                if (t1->kind == TypeKind::Rune || t2->kind == TypeKind::Rune) {
                    result_type = get_system_types()->rune_;
                } else {
                    int t1_size = get_int_type_size(t1);
                    int t2_size = get_int_type_size(t2);

                    if (t2_size > t1_size) {
                        result_type = t2;
                    } else if (t1_size == t2_size && t1->kind == TypeKind::Int &&
                               t2->kind == TypeKind::Int) {
                        // If same size, prefer unsigned if either is unsigned
                        if (t2->data.int_.is_unsigned && !t1->data.int_.is_unsigned) {
                            result_type = t2;
                        }
                    }
                }
            }

            // Check for intrinsic operator method (concrete types or trait bounds)
            if (intrinsic_symbol != IntrinsicSymbol::None) {
                auto method_call = try_resolve_operator_method(intrinsic_symbol, t1, t2, data.op1,
                                                               data.op2, node, scope);
                if (method_call.has_value()) {
                    data.resolved_call = method_call->call_node;
                    return method_call->return_type;
                }
            }

            // Check that both operands can be converted to the result type
            check_assignment(data.op1, t1, result_type);
            check_assignment(data.op2, t2, result_type);

            check_binary_op(node, data.op_type, result_type);
            return result_type;
        }
        }
    }
    case NodeType::UnaryOpExpr: {
        auto &data = node->data.unary_op_expr;
        if (data.op_type == TokenType::MUTREF) {
            scope = scope.set_is_lhs(true);
        }
        // For unary arithmetic operators, don't propagate value_type context to operand
        // because the operator determines its own result type (e.g., -1 should be int, not uint64)
        auto operand_scope = scope;
        if (data.op_type == TokenType::SUB || data.op_type == TokenType::ADD ||
            data.op_type == TokenType::NOT) {
            operand_scope = scope.set_value_type(nullptr);
        }
        auto t = resolve(data.op1, operand_scope);
        if (!t) {
            return nullptr;
        }
        ensure_temp_owner(data.op1, t, scope);
        switch (auto tt = data.op_type) {
        case TokenType::SUB:
            // For struct types, try Neg interface
            if (t->kind == TypeKind::Struct) {
                auto method_call = try_resolve_operator_method(
                    IntrinsicSymbol::Neg, t, nullptr, data.op1, nullptr, node, scope);
                if (method_call) {
                    data.resolved_call = method_call->call_node;
                    return method_call->return_type;
                }
            }
            [[fallthrough]];
        case TokenType::ADD:
        case TokenType::INC:
        case TokenType::DEC:
            if (t->kind == TypeKind::Pointer && (tt == TokenType::INC || tt == TokenType::DEC)) {
                if (!scope.is_unsafe_block) {
                    error(node, "pointer arithmetic requires unsafe block");
                    return nullptr;
                }
                return t;
            }
            check_assignment(data.op1, t,
                             t->kind == TypeKind::Float ? t : get_system_types()->int_);
            return t->kind == TypeKind::Bool ? get_system_types()->int_ : t;
        case TokenType::NOT: {
            // For struct types, try Not interface (bitwise not ~x)
            if (t->kind == TypeKind::Struct) {
                auto method_call = try_resolve_operator_method(
                    IntrinsicSymbol::BitNot, t, nullptr, data.op1, nullptr, node, scope);
                if (method_call) {
                    data.resolved_call = method_call->call_node;
                    return method_call->return_type;
                }
            }
            // Primitive bitwise NOT
            check_assignment(data.op1, t, get_system_types()->int_);
            return t->kind == TypeKind::Bool ? get_system_types()->int_ : t;
        }
        case TokenType::MUL: {
            if (ChiTypeStruct::is_pointer_type(t) && t->get_elem()->kind != TypeKind::Void) {
                if (t->is_raw_pointer() && !scope.is_unsafe_block &&
                    !has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_MANAGED)) {
                    error(node, "raw pointer dereference requires unsafe block");
                    return nullptr;
                }
                if (scope.is_lhs && !ChiTypeStruct::is_mutable_pointer(t)) {
                    error(data.op1, errors::CANNOT_MODIFY_IMMUTABLE_REFERENCE,
                          format_type_display(t), format_type_display(t));
                    return nullptr;
                }
                return t->get_elem();
            }
            // Check for ops.Deref / ops.DerefMut
            {
                auto symbol =
                    scope.is_lhs ? IntrinsicSymbol::DerefMut : IntrinsicSymbol::Deref;
                auto method_call = try_resolve_operator_method(symbol, t, nullptr, data.op1,
                                                               nullptr, node, scope);
                // Fallback: if only DerefMut is provided, use it for reads too
                if (!method_call && !scope.is_lhs) {
                    method_call = try_resolve_operator_method(
                        IntrinsicSymbol::DerefMut, t, nullptr, data.op1, nullptr, node, scope);
                }
                if (method_call) {
                    data.resolved_call = method_call->call_node;
                    auto ret = method_call->return_type;
                    if (ret && ret->is_reference())
                        return ret->get_elem();
                    return ret;
                }
            }
            goto invalid;
        }
        case TokenType::LNOT: {
            if (data.is_suffix) {
                if (t->kind == TypeKind::Optional) {
                    return t->get_elem();
                } else if (t->kind == TypeKind::Result) {
                    return t->data.result.value;
                }
                // Check for ops.Unwrap / ops.UnwrapMut
                {
                    auto symbol =
                        scope.is_lhs ? IntrinsicSymbol::UnwrapMut : IntrinsicSymbol::Unwrap;
                    auto method_call = try_resolve_operator_method(symbol, t, nullptr, data.op1,
                                                                   nullptr, node, scope);
                    // Fallback: if only UnwrapMut is provided, use it for reads too
                    if (!method_call && !scope.is_lhs) {
                        method_call = try_resolve_operator_method(
                            IntrinsicSymbol::UnwrapMut, t, nullptr, data.op1, nullptr, node, scope);
                    }
                    if (method_call) {
                        data.resolved_call = method_call->call_node;
                        auto ret = method_call->return_type;
                        if (ret && ret->is_reference())
                            return ret->get_elem();
                        return ret;
                    }
                }
                goto invalid;
            } else {
                check_assignment(data.op1, t, get_system_types()->bool_);
                return get_system_types()->bool_;
            }
            break;
        }
        case TokenType::AND:
        case TokenType::MUTREF: {
            if (!is_addressable(data.op1)) {
                error(node, errors::CANNOT_GET_REFERENCE_UNADDRESSABLE);
            }
            if (scope.move_outlet && scope.parent_fn_node) {
                auto ref_target = find_root_decl(data.op1);
                if (ref_target && !scope.is_unsafe_block) {
                    auto outlet = scope.move_outlet;
                    if (outlet->type != NodeType::VarDecl) {
                        auto resolved = find_root_decl(outlet);
                        if (resolved)
                            outlet = resolved;
                    }
                    scope.parent_fn_node->data.fn_def.add_ref_edge(outlet, ref_target);
                }
            }
            if (scope.is_escaping) {
                auto decl = find_root_decl(data.op1);
                if (decl) {
                    decl->escape.escaped = decl->can_escape();
                }
            }
            return get_pointer_type(t->eval(), data.op_type == TokenType::MUTREF
                                                   ? TypeKind::MutRef
                                                   : TypeKind::Reference);
        }
        case TokenType::MOVEREF: {
            if (!is_addressable(data.op1)) {
                error(node, errors::CANNOT_GET_REFERENCE_UNADDRESSABLE);
            }
            if (scope.parent_fn_node && has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_SAFE)) {
                auto *src_decl = find_root_decl(data.op1);
                if (src_decl) {
                    auto &fn_def = scope.parent_fn_node->data.fn_def;
                    if (fn_def.is_sunk(src_decl)) {
                        error(node, "'{}' used after move", src_decl->name);
                    }
                }
            }
            return get_pointer_type(t, TypeKind::MoveRef);
        }
        case TokenType::KW_MOVE: {
            // move x — value move optimization: produces T, sinks source
            if (!is_addressable(data.op1)) {
                error(node, errors::CANNOT_GET_REFERENCE_UNADDRESSABLE);
            }
            if (scope.parent_fn_node) {
                auto *src_decl = find_root_decl(data.op1);
                if (src_decl) {
                    auto &fn_def = scope.parent_fn_node->data.fn_def;
                    if (has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_SAFE) &&
                        fn_def.is_sunk(src_decl)) {
                        error(node, "'{}' used after move", src_decl->name);
                    }
                    // Sink the source at the move site — works for all contexts
                    // (return, assignment, function arg, etc.)
                    fn_def.add_sink_edge(src_decl, node);
                }
            }
            node->escape.moved = true;
            return t; // move produces the value type, not a reference
        }
        default:
            unreachable();
        }
    invalid:
        error(data.op1, errors::INVALID_OPERATOR, get_token_symbol(data.op_type),
              format_type_display(t));
        return nullptr;
    }
    case NodeType::TryExpr: {
        auto &data = node->data.try_expr;
        auto expr_type = resolve(data.expr, scope);
        if (data.expr->type != NodeType::FnCallExpr) {
            error(data.expr, errors::TRY_NOT_CALL);
        }
        scope.parent_fn_def()->has_try = true;

        if (data.catch_block) {
            // Catch block mode: try f() catch (...) { block } → yields T
            ChiType *err_var_type = nullptr;
            if (data.catch_expr) {
                auto catch_type = to_value_type(resolve(data.catch_expr, scope));
                if (catch_type && catch_type->kind == TypeKind::Struct) {
                    auto rt_error = m_ctx->rt_error_type;
                    if (rt_error && !catch_type->data.struct_.interface_table.get(rt_error)) {
                        error(data.catch_expr, errors::CATCH_NOT_ERROR,
                              format_type_display(catch_type));
                    }
                    err_var_type = get_pointer_type(catch_type, TypeKind::Reference);
                } else {
                    error(data.catch_expr, errors::CATCH_NOT_ERROR,
                          format_type_display(catch_type));
                }
            } else {
                // catch-all: error binding is &Error (interface reference)
                err_var_type = get_pointer_type(m_ctx->rt_error_type, TypeKind::Reference);
            }

            // Set up error binding var in catch block scope
            if (data.catch_err_var && err_var_type) {
                data.catch_err_var->resolved_type = err_var_type;
                data.catch_err_var->parent_fn = scope.parent_fn_node;
                data.catch_err_var->data.var_decl.is_generated = true;
                data.catch_err_var->data.var_decl.initialized_at = node;
                auto &block_data = data.catch_block->data.block;
                block_data.implicit_vars.add(data.catch_err_var);
                block_data.scope->put(data.catch_err_var->name, data.catch_err_var);
            }

            resolve(data.catch_block, scope);
            return expr_type;
        }

        if (data.catch_expr) {
            // No block, Result mode: try f() catch FileError → Result<T, &FileError>
            auto catch_type = to_value_type(resolve(data.catch_expr, scope));
            if (catch_type && catch_type->kind == TypeKind::Struct) {
                auto rt_error = m_ctx->rt_error_type;
                if (rt_error && !catch_type->data.struct_.interface_table.get(rt_error)) {
                    error(data.catch_expr, errors::CATCH_NOT_ERROR,
                          format_type_display(catch_type));
                }
                auto ref_type = get_pointer_type(catch_type, TypeKind::Reference);
                return get_result_type(expr_type, ref_type);
            }
            error(data.catch_expr, errors::CATCH_NOT_ERROR, format_type_display(catch_type));
        }
        // try f() → Result<T, &Error>
        return get_result_type(expr_type,
                               get_pointer_type(m_ctx->rt_error_type, TypeKind::Reference));
    }
    case NodeType::AwaitExpr: {
        auto &data = node->data.await_expr;
        auto expr_type = resolve(data.expr, scope);
        // Check that we're inside an async function
        auto parent_fn = scope.parent_fn_def();
        if (!parent_fn->is_async()) {
            error(node, "await can only be used inside async functions");
        }
        // Check that the expression returns a Promise<T>
        if (!is_promise_type(expr_type)) {
            error(node, "await requires a Promise type, got {}", format_type_display(expr_type));
            return get_system_types()->void_;
        }
        // Return the unwrapped value type
        return get_promise_value_type(expr_type);
    }
    case NodeType::CastExpr: {
        auto &data = node->data.cast_expr;
        auto dest_type = resolve_value(data.dest_type, scope);
        auto from_type = resolve(data.expr, scope);
        ensure_temp_owner(data.expr, from_type, scope);
        if (!scope.is_unsafe_block &&
            (from_type->is_raw_pointer() || dest_type->is_raw_pointer())) {
            error(node, "pointer cast requires unsafe block");
            return nullptr;
        }
        check_cast(node, from_type, dest_type);
        return dest_type;
    }
    case NodeType::LiteralExpr: {
        auto token = node->token;
        switch (token->type) {
        case TokenType::BOOL:
            return get_system_types()->bool_;
        case TokenType::NULLP:
            return get_system_types()->null_ptr;
        case TokenType::INT:
            if (scope.value_type && (scope.value_type->kind == TypeKind::Int ||
                                     scope.value_type->kind == TypeKind::Byte ||
                                     scope.value_type->kind == TypeKind::Rune)) {
                return scope.value_type;
            }
            return get_system_types()->int_;
        case TokenType::STRING:
            return get_system_types()->string;
        case TokenType::C_STRING:
            return create_pointer_type(get_system_types()->byte_, TypeKind::Pointer);
        case TokenType::FLOAT:
            if (scope.value_type && scope.value_type->kind == TypeKind::Float &&
                scope.value_type->data.float_.bit_count == 64) {
                return scope.value_type;
            }
            return get_system_types()->float_;
        case TokenType::KW_UNDEFINED:
            return get_system_types()->undefined;
        case TokenType::KW_ZEROINIT:
            return get_system_types()->zeroinit;
        case TokenType::CHAR: {
            auto codepoint = token->val.i;
            if (codepoint > 127) {
                return get_system_types()->rune_;
            }
            if (scope.value_type && scope.value_type->kind == TypeKind::Rune) {
                return get_system_types()->rune_;
            }
            return get_system_types()->byte_;
        }
        default:
            unreachable();
        }
    }
    case NodeType::ReturnStmt: {
        auto &data = node->data.return_stmt;
        assert(scope.parent_fn);
        auto return_type = scope.parent_fn->data.fn.return_type;

        if (return_type && return_type->kind == TypeKind::Never) {
            error(node, "cannot return from a function with return type 'never'");
            return nullptr;
        }

        // For async functions, the value type for the expression is the inner Promise value type
        ChiType *value_type_hint = return_type;
        if (scope.parent_fn_def()->is_async() && is_promise_type(return_type)) {
            value_type_hint = get_promise_value_type(return_type);
        }

        // Don't use Infer as value type hint - let expression infer its type.
        // Placeholders are kept: construct expressions need them to find the constructor
        // from trait bounds (e.g., return {v} where return type is T: IntConstruct).
        if (value_type_hint && value_type_hint->kind == TypeKind::Infer) {
            value_type_hint = nullptr;
        }

        auto expr_scope = scope.set_is_escaping(true).set_value_type(value_type_hint);
        auto expr_type = data.expr ? resolve(data.expr, expr_scope) : get_system_types()->void_;

        // For async functions returning Promise<T>, allow returning T directly
        ChiType *expected_type = return_type;
        if (scope.parent_fn_def()->is_async() && is_promise_type(return_type)) {
            expected_type = get_promise_value_type(return_type);
        }

        // If return type is an Infer type, set the inferred_type to the expression type.
        // This enables proper type inference for generic type parameters (e.g., map<U>).
        if (return_type && return_type->kind == TypeKind::Infer && expr_type &&
            expr_type->kind != TypeKind::Placeholder && expr_type->kind != TypeKind::Infer) {
            return_type->data.infer.inferred_type = expr_type;
            // Clear is_placeholder if the inferred type is concrete - this allows
            // codegen to compile the type correctly
            if (!expr_type->is_placeholder) {
                return_type->is_placeholder = false;
            }
        }

        // For Infer types, check assignment against the inferred type if available
        if (expected_type && expected_type->kind == TypeKind::Infer) {
            auto inferred = expected_type->data.infer.inferred_type;
            if (inferred) {
                expected_type = inferred;
            } else {
                // First return in this lambda - no check needed, we're setting the type
                expected_type = nullptr;
            }
        }

        if (expected_type) {
            // Bare `return;` in async func returning Promise<Unit> is valid
            bool is_void_unit_return = !data.expr && expected_type == m_ctx->rt_unit_type;
            if (!is_void_unit_return) {
                check_assignment(data.expr, expr_type, expected_type);
            }
        }

        // Track move in return expression (e.g. return move b)
        if (data.expr && scope.parent_fn_node) {
            track_move_sink(scope.parent_fn_node, data.expr, expr_type, node, return_type);
        }

        // Return-move optimization: when returning a simple local/param variable,
        // mark the expression as moved (bitwise copy instead of deep copy).
        // The codegen will skip destruction of this variable at the return site.
        if (data.expr && !data.expr->escape.moved && data.expr->type == NodeType::Identifier &&
            data.expr->data.identifier.kind == ast::IdentifierKind::Value) {
            auto *decl = data.expr->data.identifier.decl;
            if (decl && (decl->type == NodeType::VarDecl || decl->type == NodeType::ParamDecl)) {
                bool is_field = decl->type == NodeType::VarDecl && decl->data.var_decl.is_field;
                if (!is_field) {
                    auto *var_type = node_get_type(decl);
                    if (var_type && type_needs_destruction(var_type)) {
                        data.expr->escape.moved = true;
                    }
                }
            }
        }

        if (data.expr && scope.parent_fn_node && expr_type && is_borrowing_type(expr_type) &&
            !scope.is_unsafe_block) {
            auto &fn_def = scope.parent_fn_node->data.fn_def;
            bool is_ref = expr_type->is_reference();
            add_borrow_source_edges(fn_def, data.expr, node, is_ref);
            fn_def.add_terminal(node);
        }

        return return_type;
    }
    case NodeType::ThrowStmt: {
        auto &data = node->data.throw_stmt;
        auto expr_type = resolve(data.expr, scope);
        if (expr_type) {
            // The thrown value must be a reference to a struct implementing Error
            if (expr_type->kind != TypeKind::Reference && expr_type->kind != TypeKind::MutRef &&
                expr_type->kind != TypeKind::MoveRef) {
                error(data.expr, errors::THROW_NOT_REFERENCE);
            } else {
                auto elem = expr_type->get_elem();
                if (elem && elem->kind == TypeKind::Struct) {
                    auto rt_error = m_ctx->rt_error_type;
                    if (rt_error && !elem->data.struct_.interface_table.get(rt_error)) {
                        error(data.expr, errors::THROW_NOT_ERROR, format_type_display(elem));
                    }
                } else {
                    error(data.expr, errors::THROW_NOT_ERROR, format_type_display(expr_type));
                }
            }
        }
        // throw needs the personality function for unwinding
        scope.parent_fn_def()->has_try = true;
        return nullptr;
    }
    case NodeType::ParenExpr: {
        auto &child = node->data.child_expr;
        return resolve(child, scope);
    }
    case NodeType::DotExpr: {
        auto &data = node->data.dot_expr;
        auto field_name = data.field->str;
        auto expr_type = resolve(data.expr, scope, flags);
        if (!expr_type) {
            return nullptr;
        }
        ensure_temp_owner(data.expr, expr_type, scope);

        // Optional chaining: unwrap ?T to T, resolve member, wrap result in ?
        if (data.is_optional_chain) {
            if (expr_type->kind != TypeKind::Optional) {
                error(node, "optional chaining (?.) requires optional type, got {}",
                      format_type_display(expr_type));
                return nullptr;
            }
            expr_type = expr_type->get_elem();
        }

        if (field_name.empty()) {
            return create_type(TypeKind::Unknown);
        }

        if (expr_type->kind == TypeKind::Fn) {
            expr_type = get_lambda_for_fn(expr_type);
        } else if (expr_type->kind == TypeKind::Module) {
            auto symbol = expr_type->data.module.scope->find_export(field_name);
            if (!symbol) {
                error(node, errors::MEMBER_NOT_FOUND, field_name, format_type_display(expr_type));
                return nullptr;
            }
            data.resolved_decl = symbol;
            return symbol->resolved_type;
        }

        if (expr_type->kind == TypeKind::TypeSymbol) {
            auto underlying_type = expr_type->data.type_symbol.underlying_type;
            switch (underlying_type->kind) {
            case TypeKind::Enum: {
                auto member = underlying_type->data.enum_.find_member(field_name);
                if (!member) {
                    error(node, errors::MEMBER_NOT_FOUND, field_name,
                          format_type_display(underlying_type));
                    return nullptr;
                }
                data.resolved_decl = member->node;
                data.field->node = member->node;
                data.resolved_dot_kind = DotKind::EnumVariant;
                return member->resolved_type;
            }
            case TypeKind::Subtype: {
                auto &subtype_data = underlying_type->data.subtype;
                if (subtype_data.generic && subtype_data.generic->kind == TypeKind::Enum) {
                    // Generic enum subtype: find variant on the concrete enum
                    auto concrete_enum = subtype_data.final_type;
                    assert(concrete_enum && concrete_enum->kind == TypeKind::Enum);
                    auto member = concrete_enum->data.enum_.find_member(field_name);
                    if (!member) {
                        error(node, errors::MEMBER_NOT_FOUND, field_name,
                              format_type_display(underlying_type));
                        return nullptr;
                    }
                    data.resolved_decl = member->node;
                    data.field->node = member->node;
                    data.resolved_dot_kind = DotKind::EnumVariant;
                    return member->resolved_type;
                }
                auto resolved = resolve_subtype(underlying_type);
                if (!resolved) {
                    error(node, errors::MEMBER_NOT_FOUND, field_name,
                          format_type_display(underlying_type));
                    return nullptr;
                }
                auto member = resolved->data.struct_.find_static_member(field_name);
                if (!member) {
                    error(node, errors::MEMBER_NOT_FOUND, field_name,
                          format_type_display(underlying_type));
                    return nullptr;
                }
                data.resolved_decl = member->node;
                data.field->node = member->node;
                return member->resolved_type;
            }
            case TypeKind::Struct: {
                auto member = underlying_type->data.struct_.find_static_member(field_name);
                if (!member) {
                    error(node, errors::MEMBER_NOT_FOUND, field_name,
                          format_type_display(underlying_type));
                    return nullptr;
                }
                data.resolved_decl = member->node;
                data.field->node = member->node;
                auto result_type = member->resolved_type;

                // For static methods on generic structs, promote struct type params
                // to function type params so argument-based inference works
                // (e.g., Promise.exec(...) instead of requiring Promise<int>.exec(...))
                if (underlying_type->data.struct_.is_generic() &&
                    result_type->kind == TypeKind::Fn &&
                    result_type->data.fn.type_params.len == 0) {
                    auto &struct_tparams = underlying_type->data.struct_.type_params;
                    auto promoted = create_type(TypeKind::Fn);
                    result_type->clone(promoted);
                    for (auto tp : struct_tparams) {
                        promoted->data.fn.type_params.add(tp);
                    }
                    result_type = promoted;
                }

                return result_type;
            }
            case TypeKind::String: {
                // Handle builtin string type's static members via __CxString
                if (m_ctx->rt_string_type) {
                    auto member =
                        m_ctx->rt_string_type->data.struct_.find_static_member(field_name);
                    if (!member) {
                        error(node, errors::MEMBER_NOT_FOUND, field_name, "string");
                        return nullptr;
                    }
                    data.resolved_decl = member->node;
                    data.field->node = member->node;
                    return member->resolved_type;
                }
                error(node, errors::MEMBER_NOT_FOUND, field_name, "string");
                return nullptr;
            }
            default:
                error(node, errors::MEMBER_NOT_FOUND, field_name,
                      format_type_display(underlying_type));
                return nullptr;
            }
        }

        // Check if this is a placeholder type with trait bounds (or reference to one)
        auto check_type = expr_type;
        if (expr_type->is_pointer_like()) {
            check_type = expr_type->get_elem();
        }
        if (check_type->kind == TypeKind::Placeholder &&
            check_type->data.placeholder.traits.len > 0) {
            for (auto trait_type : check_type->data.placeholder.traits) {
                if (trait_type->kind == TypeKind::Struct &&
                    ChiTypeStruct::is_interface(trait_type)) {
                    auto trait_struct = &trait_type->data.struct_;
                    auto member = trait_struct->find_member(field_name);
                    if (member && member->is_method()) {
                        if (!scope.is_fn_call) {
                            error(node, errors::TRAIT_METHOD_NOT_CALLABLE,
                                  field_name, format_type_display(expr_type));
                            return nullptr;
                        }
                        data.resolved_struct_member = member;
                        data.resolved_decl = member->node;
                        data.field->node = member->node;
                        data.resolved_dot_kind = DotKind::TypeTrait;
                        // Substitute ThisType with the concrete placeholder type
                        return substitute_this_type(member->resolved_type, expr_type);
                    }
                }
            }
            error(node, errors::MEMBER_NOT_FOUND, field_name, format_type_display(expr_type));
            return nullptr;
        }

        // FixedArray: only .length is supported
        if (expr_type->kind == TypeKind::FixedArray) {
            if (field_name == "length") {
                data.resolved_value = (int64_t)expr_type->data.fixed_array.size;
                return get_system_types()->uint32;
            }
            error(node, errors::MEMBER_NOT_FOUND, field_name, format_type_display(expr_type));
            return nullptr;
        }

        auto stype = eval_struct_type(expr_type);
        if (!stype) {
            error(node, errors::MEMBER_NOT_FOUND, field_name, format_type_display(expr_type));
            return nullptr;
        }
        if (field_name == "new" || field_name == "delete") {
            if (!scope.parent_struct) {
                error(node, "'{}' cannot be called via dot syntax", field_name);
                return nullptr;
            }
        }
        // Auto-deref: if member not found, try Deref/DerefMut before reporting error
        if (!get_struct_member(stype, field_name)) {
            auto deref_ref_type = try_auto_deref(node, stype, field_name, scope);
            if (deref_ref_type) {
                // data.expr was replaced with deref call; redo lookup on deref'd type
                expr_type = deref_ref_type;
                stype = eval_struct_type(expr_type);
            }
        }
        auto is_internal = scope.parent_struct && is_friend_struct(scope.parent_struct, stype);
        auto member =
            get_struct_member_access(node, stype, field_name, is_internal, scope.is_lhs, &scope,
                                     expr_type);
        if (!member) {
            return nullptr;
        }
        data.resolved_struct_member = member;
        data.resolved_decl = member->node;
        data.field->node = member->node;
        auto expr_struct = resolve_struct_type(stype);
        if (expr_struct->is_generic()) {
            data.should_resolve_variant = true;
        }

        // For Array types, ensure we use the substituted member type
        if (expr_type->kind == TypeKind::Array && stype->kind == TypeKind::Struct) {
            // Check if the resolved subtype has a different member with substituted type
            auto resolved_member = stype->data.struct_.find_member(field_name);
            if (resolved_member && resolved_member->resolved_type != member->resolved_type) {
                auto rt = resolved_member->resolved_type;
                if (data.is_optional_chain && !resolved_member->is_method()) {
                    return get_wrapped_type(rt, TypeKind::Optional);
                }
                return rt;
            }
        }

        // Check if this is a generic method that needs special handling
        if (member->is_method() && member->resolved_type &&
            member->resolved_type->kind == TypeKind::Fn) {
            auto fn_type = member->resolved_type;
            if (fn_type->data.fn.is_generic()) {
                // For generic methods, we need to preserve the generic type for later inference
                // Instead of returning the resolved type, return the original generic type
                // This allows type inference to work properly during function calls
                return fn_type;
            }
        }

        // Check if this is a method being accessed as a value (not a function call)
        // Only convert to lambda if we're NOT in a function reference context
        if (member->is_method() && member->resolved_type &&
            member->resolved_type->kind == TypeKind::Fn && !scope.is_fn_call) {
            // Mark this DotExpr as needing method-to-lambda conversion
            data.resolved_dot_kind = DotKind::MethodToLambda;

            // Create a method lambda type - we'll generate a proxy function
            auto lambda_type = create_type(TypeKind::FnLambda);
            lambda_type->data.fn_lambda.fn = member->resolved_type;
            lambda_type->is_placeholder = member->resolved_type->is_placeholder;

            // Create a bound function type for the proxy (with binding struct as first param)
            auto bound_fn = create_type(TypeKind::Fn);
            auto &bound_fn_data = bound_fn->data.fn;
            auto &method_fn_data = member->resolved_type->data.fn;

            // Add binding struct as first parameter
            bound_fn_data.params.add(get_system_types()->void_ref);

            // Add all original method parameters (except 'this' which is handled via binding)
            for (auto param : method_fn_data.params) {
                bound_fn_data.params.add(param);
            }

            bound_fn_data.return_type = method_fn_data.return_type;
            bound_fn_data.is_variadic = method_fn_data.is_variadic;

            // Create a binding struct to hold the instance pointer
            auto bind_struct = create_type(TypeKind::Struct);
            bind_struct->data.struct_.kind = ContainerKind::Struct;
            bind_struct->name = "MethodLambdaBind";

            // Add the instance pointer as the only field
            auto instance_type = data.expr->resolved_type;
            if (!instance_type->is_pointer_like()) {
                instance_type = get_pointer_type(instance_type);
            }
            auto instance_member = bind_struct->data.struct_.add_member(
                get_allocator(), "instance", get_dummy_var("instance"), instance_type);

            // Set up the lambda type
            lambda_type->data.fn_lambda.bind_struct = bind_struct;
            lambda_type->data.fn_lambda.bound_fn = bound_fn;

            // Use __CxLambda directly (no longer generic)
            auto rt_lambda = m_ctx->rt_lambda_type;
            assert(rt_lambda && "__CxLambda type not found in runtime");

            // Check if __CxLambda is fully resolved
            if (rt_lambda->data.struct_.resolve_status < ResolveStatus::MemberTypesKnown) {
                // Not resolved yet - defer
                lambda_type->data.fn_lambda.internal = nullptr;
                lambda_type->is_placeholder = true;
            } else {
                // Use __CxLambda directly
                lambda_type->data.fn_lambda.internal = to_value_type(rt_lambda);
            }
            lambda_type->data.fn_lambda.captures.add(instance_type);

            return lambda_type;
        }

        auto result_type = member->resolved_type;

        // Check for narrowing redirect (path-based lookup)
        // Skip LHS of assignments — writes should go to the original optional field
        if (scope.block && !member->is_method() && !scope.is_lhs) {
            auto path = build_narrowing_path(node);
            if (!path.empty()) {
                auto narrowed = scope.block->scope->find_one(path);
                if (narrowed && narrowed->type == NodeType::VarDecl &&
                    narrowed->data.var_decl.is_generated && narrowed->data.var_decl.narrowed_from) {
                    data.narrowed_var = narrowed;
                    return narrowed->resolved_type;
                }
            }
        }

        if (data.is_optional_chain && !member->is_method()) {
            return get_wrapped_type(result_type, TypeKind::Optional);
        }
        return result_type;
    }
    case NodeType::ConstructExpr: {
        auto &data = node->data.construct_expr;
        auto dest_type = scope.value_type; // Save before resolve calls modify scope
        if (scope.move_outlet && !data.is_new) {
            node->resolved_outlet = scope.move_outlet;
            node->escape.moved = true;
        }
        ChiType *value_type;
        ChiType *result_type;
        if (data.type) {
            value_type = resolve_value(data.type, scope);
            if (data.items.len == 0 && value_type->kind == TypeKind::Placeholder) {
                bool has_construct_bound = false;
                for (auto t : value_type->data.placeholder.traits) {
                    if (!t || t->kind != TypeKind::Struct || !ChiTypeStruct::is_interface(t))
                        continue;
                    auto *new_member = t->data.struct_.find_member("new");
                    if (new_member && new_member->node &&
                        new_member->node->data.fn_def.fn_kind == ast::FnKind::Constructor &&
                        new_member->node->data.fn_def.fn_proto->data.fn_proto.params.len == 0) {
                        has_construct_bound = true;
                        break;
                    }
                }
                if (!has_construct_bound) {
                    error(node,
                          "cannot default-construct '{}': type parameter requires "
                          "'Construct' bound",
                          format_type_display(value_type));
                    return nullptr;
                }
            }
            result_type =
                data.is_new ? get_pointer_type(value_type, TypeKind::MoveRef) : value_type;
        } else {
            if (!scope.value_type ||
                (scope.value_type->kind == TypeKind::FixedArray && data.is_array_literal)) {
                // Array literals: infer Array<T> from first element type
                if (data.is_array_literal && scope.value_type &&
                    scope.value_type->kind == TypeKind::FixedArray) {
                    // [1, 2, 3] assigned to [N]T — treat as fixed array init
                    result_type = scope.value_type;
                    value_type = result_type;
                } else if (data.is_array_literal && data.items.len > 0) {
                    auto elem_type = resolve(data.items[0], scope);
                    if (!elem_type)
                        return nullptr;
                    array<ChiType *> args;
                    args.add(elem_type);
                    result_type = get_subtype(m_ctx->rt_array_type, &args);
                    value_type = result_type;
                } else {
                    error(node, errors::CONSTRUCT_CANNOT_INFER_TYPE);
                    return nullptr;
                }
            } else {
                result_type = scope.value_type;
                {
                    // Empty construct on placeholder (= {}) requires a constructor
                    // interface bound whose new() has zero params
                    if (data.items.len == 0 && result_type->kind == TypeKind::Placeholder) {
                        bool has_construct_bound = false;
                        for (auto t : result_type->data.placeholder.traits) {
                            if (!t || t->kind != TypeKind::Struct ||
                                !ChiTypeStruct::is_interface(t))
                                continue;
                            auto *new_member = t->data.struct_.find_member("new");
                            if (new_member && new_member->node &&
                                new_member->node->data.fn_def.fn_kind == ast::FnKind::Constructor &&
                                new_member->node->data.fn_def.fn_proto->data.fn_proto.params.len ==
                                    0) {
                                has_construct_bound = true;
                                break;
                            }
                        }
                        if (!has_construct_bound) {
                            error(node,
                                  "cannot default-construct '{}': type parameter requires "
                                  "'Construct' bound",
                                  format_type_display(result_type));
                            return nullptr;
                        }
                    }
                }
                bool is_heap_type = result_type->is_raw_pointer() ||
                                    result_type->kind == TypeKind::MutRef ||
                                    result_type->kind == TypeKind::MoveRef;
                if (data.is_new != is_heap_type) {
                    error(node, errors::CONSTRUCT_CANNOT_INFER_TYPE);
                }
                value_type = data.is_new ? result_type->get_elem() : result_type;
            }
        }
        // FixedArray construct: [N]T{items...} or array literal assigned to [N]T
        if (value_type->kind == TypeKind::FixedArray) {
            auto elem_type = value_type->data.fixed_array.elem;
            auto fa_size = value_type->data.fixed_array.size;
            if ((uint32_t)data.items.len > fa_size) {
                error(node, "too many items for [{}]{}: got {}, max {}", fa_size,
                      format_type_display(elem_type), data.items.len, fa_size);
            }
            auto item_scope = scope.set_value_type(elem_type);
            item_scope.move_outlet = nullptr; // Items need separate temporaries
            for (auto item : data.items) {
                auto item_type = resolve(item, item_scope);
                if (item_type) {
                    check_assignment(item, item_type, elem_type);
                }
            }
            return result_type;
        }

        auto struct_type = resolve_struct_type(value_type);
        auto constructor = struct_type ? struct_type->get_constructor() : nullptr;
        if (constructor) {
            // Check visibility of the constructor
            auto is_internal =
                scope.parent_struct && is_friend_struct(scope.parent_struct, value_type);
            if (!constructor->check_access(is_internal, false)) {
                error(node, errors::PRIVATE_MEMBER_NOT_ACCESSIBLE, "new",
                      format_type_display(value_type));
                return nullptr;
            }
            auto &fn_type = constructor->resolved_type->data.fn;
            resolve_fn_call(node, scope, &fn_type, &data.items, constructor->node);

            // Track borrow edges from constructor arguments to the constructed struct.
            // Constructor params with a borrow_lifetime (e.g., &'this int) mean the
            // argument is stored in the struct — the struct borrows from it.
            if (scope.parent_fn_node && !scope.is_unsafe_block &&
                constructor->node->type == NodeType::FnDef) {
                auto &fn_def = scope.parent_fn_node->data.fn_def;
                auto *ctor_proto = &constructor->node->data.fn_def.fn_proto->data.fn_proto;
                for (size_t i = 0;
                     i < ctor_proto->resolved_param_lifetimes.len && i < data.items.len; i++) {
                    if (ctor_proto->resolved_param_lifetimes[i]) {
                        add_borrow_source_edges(fn_def, data.items[i], node, true);
                    }
                }
            }
        } else if (value_type->kind == TypeKind::Placeholder && data.items.len > 0) {
            // Placeholder type with positional args: resolve args against
            // a matching constructor interface bound's new() signature
            ChiStructMember *iface_new = nullptr;
            for (auto t : value_type->data.placeholder.traits) {
                if (!t || t->kind != TypeKind::Struct || !ChiTypeStruct::is_interface(t))
                    continue;
                auto *nm = t->data.struct_.find_member("new");
                if (nm && nm->node && nm->node->data.fn_def.fn_kind == ast::FnKind::Constructor) {
                    iface_new = nm;
                    break;
                }
            }
            if (iface_new && iface_new->resolved_type) {
                auto &fn_type = iface_new->resolved_type->data.fn;
                resolve_fn_call(node, scope, &fn_type, &data.items, iface_new->node);
            } else {
                error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, 0, data.items.len);
                return nullptr;
            }
        } else {
            if (result_type->kind == TypeKind::Optional) {
                if (data.items.len != 1) {
                    error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, 1, data.items.len);
                    return nullptr;
                }
                auto item = data.items[0];
                auto item_type = resolve(item, scope, flags);
                check_assignment(item, item_type, result_type->get_elem());
                return result_type;
            } else {
                if (data.items.len != 0) {
                    error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, 0, data.items.len);
                    return nullptr;
                }
            }
        }

        // Can't construct in-place if dest type differs from construct type
        if (node->resolved_outlet && dest_type && dest_type != result_type) {
            node->resolved_outlet = nullptr;
        }

        if (data.spread_expr) {
            auto spread_type = resolve(data.spread_expr, scope);
            if (spread_type) {
                // Same type — always OK
                if (to_value_type(spread_type) != to_value_type(value_type)) {
                    // Cross-type spread: shared fields must have matching types;
                    // source-only fields are silently discarded, target-only fields keep defaults.
                    auto spread_struct = resolve_struct_type(spread_type);
                    if (!spread_struct) {
                        error(data.spread_expr, "cannot spread non-struct type '{}'",
                              format_type_display(spread_type));
                    } else {
                        for (auto src_field : spread_type->data.struct_.fields) {
                            auto field_name = src_field->get_name();
                            auto tgt_field = get_struct_member(value_type, field_name);
                            if (!tgt_field)
                                continue; // source-only field — discard
                            if (to_value_type(src_field->resolved_type) !=
                                to_value_type(tgt_field->resolved_type)) {
                                error(data.spread_expr, "field '{}' has type {} in {} but {} in {}",
                                      field_name, format_type_display(src_field->resolved_type),
                                      format_type_display(spread_type),
                                      format_type_display(tgt_field->resolved_type),
                                      format_type_display(value_type));
                            }
                        }
                    }
                }
            }
        }

        for (auto field_init : data.field_inits) {
            auto &data = field_init->data.field_init_expr;
            auto field_member =
                get_struct_member_access(field_init, value_type, data.field->str, false, true);
            if (!field_member) {
                return nullptr;
            }

            auto inner_scope =
                scope.set_value_type(field_member->resolved_type).set_move_outlet(field_init);
            auto init_value_type = resolve(data.value, inner_scope);
            data.resolved_field = field_member;
            check_assignment(data.value, init_value_type, field_member->resolved_type);

            if (scope.parent_fn_node && !scope.is_unsafe_block && init_value_type &&
                is_borrowing_type(init_value_type)) {
                auto outlet = scope.move_outlet ? scope.move_outlet : node;
                auto &fn_def = scope.parent_fn_node->data.fn_def;
                fn_def.add_ref_edge(outlet, field_init);
                add_borrow_source_edges(fn_def, data.value, field_init);
            }
        }

        // For structs without a constructor, check that all fields without defaults
        // are provided via field initializers or spread at the construction site
        if (struct_type && value_type->kind == TypeKind::Struct && !constructor) {
            std::set<string> provided;
            for (auto fi : data.field_inits) {
                provided.insert(fi->data.field_init_expr.field->str);
            }
            // Spread provides fields that exist in the source type
            if (data.spread_expr) {
                auto spread_type = to_value_type(node_get_type(data.spread_expr));
                if (spread_type && spread_type->kind == TypeKind::Struct) {
                    for (auto src_field : spread_type->data.struct_.own_fields()) {
                        provided.insert(src_field->get_name());
                    }
                }
            }
            for (auto field : value_type->data.struct_.own_fields()) {
                auto &vd = field->node->data.var_decl;
                if (!vd.initialized_at && !provided.count(field->get_name())) {
                    error(node, "missing field '{}' in construction of '{}'",
                          field->get_name(), format_type_display(value_type));
                }
            }
        }

        return result_type;
    }
    case NodeType::PackExpansion: {
        // Pack expansion is only valid in function call arguments
        // Resolve the inner expression - it should be a homogeneous variadic (Array<any>)
        auto &data = node->data.pack_expansion;
        auto expr_type = resolve(data.expr, scope);
        if (!expr_type) {
            return nullptr;
        }

        // For now, just validate it's an Array type (homogeneous variadics are Array<T>)
        auto val_type = to_value_type(expr_type);
        if (val_type->kind != TypeKind::Array) {
            error(node, "pack expansion can only be used on variadic parameters (Array type)");
            return nullptr;
        }

        // The type of the pack expansion is the array type
        return expr_type;
    }
    case NodeType::FnCallExpr: {
        auto &data = node->data.fn_call_expr;
        auto fn_ref_scope = scope.set_is_lhs(false).set_is_fn_call(true);
        auto fn_type = resolve(data.fn_ref_expr, fn_ref_scope);
        if (!fn_type) {
            return nullptr;
        }
        if (fn_type->kind != TypeKind::Fn && fn_type->kind != TypeKind::FnLambda) {
            error(data.fn_ref_expr, errors::CANNOT_CALL_NON_FUNCTION);
            return nullptr;
        }
        if (fn_type->kind == TypeKind::FnLambda) {
            auto &fn_lambda = fn_type->data.fn_lambda;
            fn_type = fn_lambda.fn;
        }
        if (data.fn_ref_expr->type == NodeType::Identifier &&
            data.fn_ref_expr->data.identifier.decl->name == "transform") {
        }

        auto &fn = fn_type->data.fn;
        auto fn_decl = data.fn_ref_expr->get_decl();

        // Unsafe functions cannot be called in safe mode (unless inside an unsafe block)
        if (fn_decl && fn_decl->type == NodeType::FnDef &&
            fn_decl->data.fn_def.decl_spec->is_unsafe() &&
            has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_SAFE) && !scope.is_unsafe_block) {
            error(node, errors::UNSAFE_CALL_IN_SAFE_MODE, fn_decl->name);
        }

        auto result = resolve_fn_call(node, scope, &fn, &data.args, fn_decl);

        // Annotation-driven edge creation: for method calls where a param has
        // 'this lifetime, create edge from receiver to that arg in the caller's graph.
        if (scope.parent_fn_node && data.fn_ref_expr->type == NodeType::DotExpr && fn_decl &&
            fn_decl->type == NodeType::FnDef) {
            auto *callee_proto = fn_decl->data.fn_def.fn_proto;
            if (callee_proto) {
                auto &callee_params = callee_proto->data.fn_proto.params;
                auto *receiver_expr = data.fn_ref_expr->data.dot_expr.expr;
                auto *receiver_decl = find_root_decl(receiver_expr);
                if (receiver_decl && !scope.is_unsafe_block) {
                    auto &fn_def = scope.parent_fn_node->data.fn_def;
                    for (size_t i = 0; i < callee_params.len && i < data.args.len; i++) {
                        auto *pt = callee_params[i]->resolved_type;
                        if (pt && pt->is_reference() && pt->data.pointer.lifetimes.len > 0) {
                            auto *arg_decl = find_root_decl(data.args[i]);
                            if (arg_decl) {
                                fn_def.add_ref_edge(receiver_decl, arg_decl);
                            }
                        }
                    }
                }
            }
        }

        // Assert narrowing: assert(expr) → narrow all optional/result vars in expr
        if (fn_decl && fn_decl == get_builtin("assert") && data.args.len >= 1) {
            array<ast::Node *> narrowables;
            collect_narrowables(data.args[0], true, narrowables);
            for (auto ident : narrowables) {
                auto var = create_narrowed_var(ident, node, scope);
                scope.block->scope->put(var->name, var);
                data.post_narrow_vars.add(var);
            }
        }

        // Optional chaining: a?.method() wraps return type in Optional
        if (result && data.fn_ref_expr->type == NodeType::DotExpr &&
            data.fn_ref_expr->data.dot_expr.is_optional_chain) {
            result = get_wrapped_type(result, TypeKind::Optional);
        }
        return result;
    }
    case NodeType::IfExpr: {
        auto &data = node->data.if_expr;

        if (scope.move_outlet) {
            node->resolved_outlet = scope.move_outlet;
            node->escape.moved = true;
        }

        auto cond_type = resolve(data.condition, scope);
        ensure_temp_owner(data.condition, cond_type, scope);

        // Positive narrowing: identifiers narrowed when condition is truthy
        array<ast::Node *> then_narrowables;
        collect_narrowables(data.condition, true, then_narrowables);
        for (auto ident : then_narrowables) {
            auto var = create_narrowed_var(ident, node, scope);
            auto &block_data = data.then_block->data.block;
            block_data.implicit_vars.add(var);
            block_data.scope->put(var->name, var);
        }

        check_assignment(data.condition, cond_type, get_system_types()->bool_);
        auto then_type = resolve(data.then_block, scope);
        ChiType *else_type = nullptr;
        if (data.else_node) {
            else_type = resolve(data.else_node, scope);
        }

        // Guard clause narrowing: identifiers narrowed when condition is falsy
        if (!data.else_node && always_terminates(data.then_block)) {
            array<ast::Node *> guard_narrowables;
            collect_narrowables(data.condition, false, guard_narrowables);
            for (auto ident : guard_narrowables) {
                auto var = create_narrowed_var(ident, node, scope);
                scope.block->scope->put(var->name, var);
                data.post_narrow_vars.add(var);
            }
        }

        // If-expression: both branches return non-void types
        if (data.else_node && then_type && else_type && then_type->kind != TypeKind::Void &&
            else_type->kind != TypeKind::Void) {
            check_assignment(data.else_node, else_type, then_type);
            return then_type;
        }

        return nullptr;
    }
    case NodeType::WhileStmt: {
        auto &data = node->data.while_stmt;
        if (data.condition) {
            auto cond_type = resolve(data.condition, scope);
            ensure_temp_owner(data.condition, cond_type, scope);
            check_assignment(data.condition, cond_type, get_system_types()->bool_);
        }
        auto loop_scope = scope.set_parent_loop(node);
        resolve(data.body, loop_scope);
        return nullptr;
    }
    case NodeType::Block: {
        auto &data = node->data.block;
        auto child_scope = scope.set_block(&data);
        if (data.is_unsafe) {
            if (has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_MANAGED)) {
                error(node, "'unsafe' blocks are not allowed in managed mode");
                return get_system_types()->void_;
            }
            child_scope = child_scope.set_is_unsafe_block(true);
        }
        for (auto stmt : data.statements) {
            auto stmt_type = resolve(stmt, child_scope);
            ensure_temp_owner(stmt, stmt_type, child_scope);
        }
        if (data.return_expr) {
            auto type = resolve(data.return_expr, child_scope);
            if (!type || type->kind == TypeKind::Void) {
                // Not value-producing (e.g. void if/switch) — reclassify as statement
                ensure_temp_owner(data.return_expr, type, child_scope);
                data.return_expr->index = data.statements.len;
                data.statements.add(data.return_expr);
                data.return_expr = nullptr;
                return get_system_types()->void_;
            }
            return type;
        }
        return get_system_types()->void_;
    }
    case NodeType::EnumDecl: {
        auto &data = node->data.enum_decl;
        ChiType *type_sym;
        ChiType *enum_type;
        if (!node->resolved_type) {
            enum_type = create_type(TypeKind::Enum);
            enum_type->name = node->name;
            enum_type->display_name = node->name;
            enum_type->global_id = resolve_global_id(node);
            enum_type->data.enum_.node = node;
            type_sym = create_type_symbol(node->name, enum_type);
            auto value_type = create_type(TypeKind::EnumValue);
            type_sym->data.type_symbol.giving_type = value_type;
            type_sym->data.type_symbol.underlying_type = enum_type;
            value_type->data.enum_value.enum_type = enum_type;
            value_type->name = node->name;
            value_type->display_name = node->name;
            string discriminator_field = data.get_discriminator_field();
            value_type->data.enum_value.discriminator_field = discriminator_field;
            auto discriminator_type = data.discriminator_type
                                          ? to_value_type(resolve(data.discriminator_type, scope))
                                          : m_ctx->system_types.uint32;

            // Validate discriminator type - only int is supported for now
            if (discriminator_type->kind != TypeKind::Int) {
                error(node, "enum discriminator must be integer type");
                return nullptr;
            }
            enum_type->data.enum_.discriminator = discriminator_type;
            value_type->data.enum_value.discriminator_type = discriminator_type;
            enum_type->data.enum_.base_value_type = value_type;

            // Create internal resolved struct for enum value
            auto vstruct = create_type(TypeKind::Struct);
            vstruct->name = node->name;
            vstruct->display_name = fmt::format("{}.InternalEnumHeader", node->name);
            auto &vstruct_data = vstruct->data.struct_;
            vstruct_data.display_name = vstruct->display_name;
            vstruct_data.node = node;
            auto allocator = get_allocator();
            vstruct_data.add_member(allocator, discriminator_field,
                                    get_dummy_var(discriminator_field), discriminator_type);
            enum_type->data.enum_.enum_header_struct = vstruct;

            // clone the enum header to the value struct
            auto vstruct_clone = create_type(TypeKind::Struct);
            vstruct->clone(vstruct_clone);
            vstruct_clone->name = node->name;
            vstruct_clone->display_name = fmt::format("{}.BaseEnumValue", node->name);
            value_type->data.enum_value.resolved_struct = vstruct_clone;

            for (auto param : data.type_params) {
                enum_type->data.enum_.type_params.add(resolve(param, scope));
            }

            if (data.base_struct) {
                auto base_struct_type = resolve(data.base_struct, scope);
                auto base_struct = eval_struct_type(base_struct_type);
                enum_type->data.enum_.base_struct = base_struct;
            }
            return type_sym;
        }

        type_sym = node->resolved_type;
        enum_type = type_sym->data.type_symbol.underlying_type;
        assert(enum_type->kind == TypeKind::Enum);
        auto &enum_data = enum_type->data.enum_;

        if (enum_data.resolve_status < ResolveStatus::Done) {
            if (enum_data.resolve_status == ResolveStatus::None) {
                scope.next_enum_value = 0;
                // resolve and add enum variants
                for (auto variant : data.variants) {
                    auto &variant_data = variant->data.enum_variant;
                    auto member_type = resolve(variant, scope);
                    auto new_member =
                        enum_data.add_variant(get_allocator(), variant->name, variant, member_type);

                    new_member->value = variant_data.resolved_value;
                    variant_data.resolve_status = ResolveStatus::MemberTypesKnown;
                }
            } else {
                // re-resolve enum variants on new pass
                for (auto member : data.variants) {
                    _resolve(member, scope);
                }
            }

            // we can only be sure what members are available after this step
            if (enum_data.resolve_status == ResolveStatus::MemberTypesKnown) {
                auto value_struct = eval_struct_type(enum_data.base_value_type);

                // add system members from runtime
                auto cx_enum_base = m_ctx->rt_enum_base;
                if (!cx_enum_base) {
                    panic("__CxEnumBase not found in runtime");
                }

                TypeList args = {enum_data.discriminator};
                auto subtype = get_subtype(to_value_type(cx_enum_base->resolved_type), &args);
                auto enum_base_struct = eval_struct_type(resolve_subtype(subtype));

                auto base_value_struct = resolve_struct_type(enum_data.base_value_type);
                for (auto base_member : enum_base_struct->data.struct_.members) {
                    if (base_member->is_method()) {
                        auto copy_node = create_node(NodeType::FnDef);
                        base_member->node->clone(copy_node);
                        auto new_member = base_value_struct->add_member(
                            get_allocator(), base_member->get_name(), copy_node,
                            base_member->resolved_type);
                        if (base_member->symbol != IntrinsicSymbol::None) {
                            new_member->symbol = base_member->symbol;
                            base_value_struct->member_intrinsics[base_member->symbol] = new_member;
                        }
                        // Tag enum name intrinsic methods
                        auto name = base_member->get_name();
                        if (name == "enum_name") {
                            new_member->symbol = IntrinsicSymbol::EnumName;
                            base_value_struct->member_intrinsics[IntrinsicSymbol::EnumName] = new_member;
                        } else if (name == "discriminator_name") {
                            new_member->symbol = IntrinsicSymbol::DiscriminatorName;
                            base_value_struct->member_intrinsics[IntrinsicSymbol::DiscriminatorName] = new_member;
                        }
                    }
                }

                // Propagate interfaces from __CxEnumBase to enum value struct
                for (auto iface : enum_base_struct->data.struct_.interfaces) {
                    resolve_vtable(iface->interface_type, enum_data.base_value_type, node);
                }

                if (data.base_struct) {
                    auto inner_scope = scope.set_parent_struct(enum_data.base_value_type);
                    _resolve(data.base_struct, inner_scope);

                    // copy members from custom base struct to enum value struct
                    auto base_struct = eval_struct_type(data.base_struct->resolved_type);
                    copy_struct_members(base_struct, value_struct);

                    // Propagate interfaces from custom struct (overrides __CxEnumBase's)
                    for (auto iface : base_struct->data.struct_.interfaces) {
                        resolve_vtable(iface->interface_type, enum_data.base_value_type, node);
                    }
                }

                // Determine if this is a plain enum (no extra data beyond discriminator)
                enum_data.is_plain = !data.base_struct;
                if (enum_data.is_plain) {
                    for (auto variant : enum_data.variants) {
                        if (variant->resolved_type->data.enum_value.variant_struct) {
                            enum_data.is_plain = false;
                            break;
                        }
                    }
                }
            } else if (enum_data.resolve_status == ResolveStatus::EmbedsResolved) {
                if (data.base_struct) {
                    auto inner_scope = scope.set_parent_struct(enum_data.base_value_type);
                    auto struct_ = resolve_struct_type(data.base_struct->resolved_type);
                    while (struct_->resolve_status < ResolveStatus::Done) {
                        _resolve(data.base_struct, inner_scope);
                    }
                }
            }
            enum_data.resolve_status = (ResolveStatus)((int)(enum_data.resolve_status) + 1);
        }
        return type_sym;
    }
    case NodeType::StructDecl: {
        auto &data = node->data.struct_decl;

        if (data.kind == ContainerKind::Union &&
            (has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_SAFE) ||
             has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_MANAGED))) {
            error(node, "'union' types are not allowed in safe mode");
            return create_type(TypeKind::Unknown);
        }

        ChiType *type_sym;
        ChiType *struct_type;
        ChiTypeStruct *struct_;
        if (!node->resolved_type) {
            struct_type = create_type(TypeKind::Struct);
            struct_type->name = node->name;
            struct_type->display_name = node->name;
            struct_type->global_id = resolve_global_id(node);
            struct_ = &struct_type->data.struct_;
            struct_->type = struct_type;
            struct_->node = node;
            struct_->kind = data.kind;
            struct_->display_name = struct_type->name;
            struct_->global_id = struct_type->global_id;

            // first pass, all members are skipped
            for (auto param : data.type_params) {
                struct_->type_params.add(resolve(param, scope));
            }
            struct_->resolve_status = ResolveStatus::None;
            type_sym = create_type_symbol(node->name, struct_type);
            if (scope.module->package->kind == ast::PackageKind::BUILTIN) {
                if (node->name == "Array") {
                    m_ctx->rt_array_type = struct_type;
                } else if (node->name == "Promise") {
                    m_ctx->rt_promise_type = struct_type;
                } else if (node->name == "__CxLambda") {
                    m_ctx->rt_lambda_type = struct_type;
                } else if (node->name == "__CxString") {
                    m_ctx->rt_string_type = struct_type;
                } else if (node->name == "__CxSpan") {
                    m_ctx->rt_span_type = struct_type;
                } else if (node->name == "__CxEnumBase") {
                    m_ctx->rt_enum_base = node;
                } else if (node->name == "Error") {
                    m_ctx->rt_error_type = struct_type;
                } else if (node->name == "Unit") {
                    m_ctx->rt_unit_type = struct_type;
                } else if (node->name == "Sized") {
                    m_ctx->rt_sized_interface = struct_type;
                } else if (node->name == "AllowUnsized") {
                    m_ctx->rt_allow_unsized_interface = struct_type;
                }
            }
            node->resolved_type = type_sym;
            return type_sym;
        }
        type_sym = node->resolved_type;
        struct_type = type_sym->data.type_symbol.underlying_type;
        struct_ = &struct_type->data.struct_;

        auto struct_scope =
            scope.parent_struct
                ? scope
                : scope.set_parent_struct(struct_type).set_parent_type_symbol(type_sym);

        if (struct_->resolve_status == ResolveStatus::None) {
            // second pass - resolve member types
            for (auto member : data.members) {
                if (member->type == NodeType::ImplementBlock) {
                    for (auto impl_member : member->data.implement_block.members) {
                        resolve_struct_member(struct_type, impl_member, struct_scope);
                    }
                } else if (member->type == NodeType::VarDecl &&
                           member->data.var_decl.is_embed && !member->data.var_decl.is_field) {
                    // Interface embed (...InterfaceName): resolve type only, don't add as
                    // struct member. Actual method copying is done in resolve_struct_embed.
                    // Struct embeds (...base: Type) have is_field=true and go through
                    // resolve_struct_member normally.
                    resolve(member, struct_scope);
                } else {
                    resolve_struct_member(struct_type, member, struct_scope);
                }
            }
            struct_->resolve_status = ResolveStatus::MemberTypesKnown;
        } else if (struct_->resolve_status == ResolveStatus::MemberTypesKnown) {
            // third pass - resolve embeds and impl blocks
            for (auto member : data.members) {
                if (member->type == NodeType::VarDecl && member->data.var_decl.is_embed) {
                    resolve_struct_embed(struct_type, member, scope);
                }
            }

            for (auto member : data.members) {
                if (member->type != NodeType::ImplementBlock)
                    continue;
                auto &impl_data = member->data.implement_block;

                // Skip where-blocks (handled below)
                if (impl_data.interface_types.len == 0)
                    continue;

                // Build where condition for `impl Interface where T: Bound` blocks
                auto *impl_where_cond = build_where_condition(impl_data, struct_, scope);

                for (auto iface_node : impl_data.interface_types) {
                    auto impl_trait = resolve_value(iface_node, scope);
                    if (!impl_trait)
                        continue;
                    auto trait_struct = resolve_struct_type(impl_trait);
                    if (!trait_struct || !ChiTypeStruct::is_interface(trait_struct)) {
                        error(iface_node, errors::NON_INTERFACE_IMPL_TYPE,
                              format_type_display(impl_trait));
                        if (!trait_struct)
                            continue;
                    }

                    resolve_vtable(impl_trait, struct_type, iface_node);
                    // Tag the InterfaceImpl with the where condition so resolve_subtype
                    // can filter it for concrete specializations
                    if (impl_where_cond) {
                        auto entry = struct_->interface_table.get(impl_trait);
                        if (entry)
                            (*entry)->where_condition = impl_where_cond;
                    }
                    if (struct_->is_generic()) {
                        for (auto subtype : struct_->subtypes) {
                            if (subtype->is_placeholder)
                                continue;
                            // Skip concrete subtypes that don't satisfy the where condition
                            if (impl_where_cond &&
                                !check_where_condition(impl_where_cond,
                                                       &subtype->data.subtype))
                                continue;
                            ChiType *subtype_impl_trait = impl_trait;
                            if (impl_trait->kind == TypeKind::Subtype) {
                                auto &subtype_data = subtype->data.subtype;
                                subtype_impl_trait =
                                    type_placeholders_sub(impl_trait, &subtype_data);
                            }
                            resolve_vtable(subtype_impl_trait, subtype, iface_node);
                        }
                    }
                }
            }

            // Resolve where-blocks: tag members with where conditions
            for (auto member : data.members) {
                if (member->type != NodeType::ImplementBlock)
                    continue;
                auto &impl_data = member->data.implement_block;
                if (impl_data.interface_types.len > 0 || impl_data.where_clauses.len == 0)
                    continue;

                auto *cond = build_where_condition(impl_data, struct_, scope);

                for (auto impl_member : impl_data.members) {
                    auto *sm = struct_->find_member(impl_member->name);
                    if (sm)
                        sm->where_condition = cond;
                    auto *ssm = struct_->find_static_member(impl_member->name);
                    if (ssm)
                        ssm->where_condition = cond;
                }
            }

            struct_->resolve_status = ResolveStatus::EmbedsResolved;
        } else {
            // fourth pass - resolve field defaults and method bodies
            for (auto member : data.members) {
                if (member->type == NodeType::VarDecl && member->data.var_decl.is_field &&
                    member->data.var_decl.expr && member->data.var_decl.type) {
                    auto &vd = member->data.var_decl;
                    auto field_type = member->resolved_type;
                    auto field_scope = struct_scope.set_value_type(field_type);
                    auto expr_type = resolve(vd.expr, field_scope);
                    if (expr_type && expr_type->kind != TypeKind::Undefined &&
                        expr_type->kind != TypeKind::ZeroInit) {
                        if (vd.expr->type != NodeType::ConstructExpr ||
                            vd.expr->data.construct_expr.type) {
                            check_assignment(vd.expr, expr_type, field_type);
                        }
                    }
                }
            }
            auto resolve_fn_body = [&](ast::Node *fn_member) {
                if (fn_member->type != NodeType::FnDef)
                    return;
                auto fn_type = node_get_type(fn_member);
                auto fn_scope = struct_scope.set_parent_fn(fn_type).set_parent_fn_node(fn_member);
                if (fn_member->data.fn_def.decl_spec &&
                    fn_member->data.fn_def.decl_spec->is_unsafe()) {
                    fn_scope = fn_scope.set_is_unsafe_block(true);
                }
                if (auto body = fn_member->data.fn_def.body) {
                    resolve(body, fn_scope);
                    check_lifetime_constraints(&fn_member->data.fn_def);

                    // Add params to cleanup_vars on the body block
                    auto &proto_data = fn_member->data.fn_def.fn_proto->data.fn_proto;
                    for (auto param : proto_data.params) {
                        if (should_destroy(param) && !param->escape.is_capture()) {
                            body->data.block.cleanup_vars.add(param);
                            fn_member->data.fn_def.has_cleanup = true;
                        }
                    }
                }
            };
            for (auto member : data.members) {
                if (member->type == NodeType::ImplementBlock) {
                    auto &impl_data = member->data.implement_block;

                    // For where-blocks, temporarily set placeholder traits so method
                    // bodies can access trait methods on the constrained type params
                    array<std::pair<ChiType *, array<ChiType *>>> saved_traits;
                    if (impl_data.interface_types.len == 0 && impl_data.where_clauses.len > 0) {
                        for (auto &clause : impl_data.where_clauses) {
                            auto param_name = clause.param_name->str;
                            for (auto tp : struct_->type_params) {
                                auto ph = to_value_type(tp);
                                if (ph->kind == TypeKind::Placeholder && ph->name == param_name) {
                                    saved_traits.add({ph, ph->data.placeholder.traits});
                                    ph->data.placeholder.traits.add(
                                        resolve_value(clause.bound_type, scope));
                                    break;
                                }
                            }
                        }
                    }

                    for (auto impl_member : impl_data.members) {
                        resolve_fn_body(impl_member);
                    }

                    // Restore original placeholder traits
                    for (auto &[ph, old_traits] : saved_traits) {
                        ph->data.placeholder.traits = old_traits;
                    }
                } else {
                    resolve_fn_body(member);
                }
            }

            for (auto member : data.members) {
                if (member->type == NodeType::VarDecl && !member->data.var_decl.initialized_at) {
                    auto not_needed = member->data.var_decl.is_embed &&
                                      (get_struct_member(struct_type, "new") ||
                                       struct_->kind == ContainerKind::Interface);
                    // Skip initialization check for extern C structs
                    auto is_extern = data.decl_spec && data.decl_spec->is_extern();
                    // Skip if the struct has no constructor — fields can be provided
                    // via field initializers at the construction site (checked per-site)
                    auto has_ctor = get_struct_member(struct_type, "new") != nullptr;
                    auto can_field_init = struct_->kind == ContainerKind::Struct &&
                                          !has_ctor;
                    if (!not_needed && !is_extern && !can_field_init) {
                        error(member, errors::UNINITIALIZED_FIELD, member->name,
                              format_type_display(struct_type));
                    }
                }
            }

            // Rule of Three: struct with func delete() must implement CopyFrom
            // or DisallowCopy (not required in managed mode — GC handles lifecycle)
            if (struct_->kind == ContainerKind::Struct &&
                !has_lang_flag(node->module->get_lang_flags(), LANG_FLAG_MANAGED) &&
                struct_->find_member("delete") &&
                !struct_->member_intrinsics.has_key(IntrinsicSymbol::CopyFrom) &&
                !is_non_copyable(struct_type)) {
                auto name = format_type_display(struct_type);
                error(node, errors::DESTRUCTOR_WITHOUT_COPY_FROM, name, name);
            }

            struct_->resolve_status = ResolveStatus::Done;
        }
        return type_sym;
    }
    case NodeType::SubtypeExpr: {
        auto &data = node->data.subtype_expr;
        auto resolved_sym = resolve(data.type, scope);
        auto type = to_value_type(resolved_sym);

        // Handle invalid/unresolved types gracefully
        if (!type) {
            return nullptr;
        }

        // For enum TypeSymbols, to_value_type returns the giving_type (EnumValue),
        // but we need the underlying_type (Enum) for generic instantiation
        if (type->kind == TypeKind::EnumValue && resolved_sym &&
            resolved_sym->kind == TypeKind::TypeSymbol) {
            type = resolved_sym->data.type_symbol.underlying_type;
        }

        // Generic typedef expansion: typedef StringMap<V> = Map<string, V>
        // When used as StringMap<int>, expand to Map<string, int>
        // Also follow ImportSymbol -> resolved_decl for cross-module typedefs
        auto decl_for_typedef = data.type->type == NodeType::Identifier ? data.type->data.identifier.decl : nullptr;
        if (decl_for_typedef && decl_for_typedef->type == NodeType::ImportSymbol &&
            decl_for_typedef->data.import_symbol.resolved_decl) {
            decl_for_typedef = decl_for_typedef->data.import_symbol.resolved_decl;
        }
        if (decl_for_typedef && decl_for_typedef->type == NodeType::TypedefDecl) {
            auto td_node = decl_for_typedef;
            auto &td = td_node->data.typedef_decl;
            if (td.type_params.len > 0) {
                // Resolve provided type args
                if (data.args.len > td.type_params.len) {
                    error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS,
                          td_node->name, td.type_params.len, data.args.len);
                    return nullptr;
                }
                // Check missing args have defaults
                for (auto i = data.args.len; i < td.type_params.len; i++) {
                    if (!td.type_params[i]->data.type_param.default_type) {
                        error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS,
                              td_node->name, td.type_params.len, data.args.len);
                        return nullptr;
                    }
                }
                // Build substitution args matching the typedef's type param indices
                ChiTypeSubtype subs;
                for (size_t i = 0; i < td.type_params.len; i++) {
                    ChiType *arg;
                    if (i < data.args.len) {
                        arg = resolve_value(data.args[i], scope);
                    } else {
                        arg = resolve_value(td.type_params[i]->data.type_param.default_type, scope);
                    }
                    if (!arg) return nullptr;
                    subs.args.add(arg);
                }
                // Substitute placeholders in the typedef's resolved RHS by index
                auto rhs_type = to_value_type(td_node->resolved_type);
                auto expanded = type_placeholders_sub(rhs_type, &subs);
                return create_type_symbol({}, expanded);
            }
        }

        if (type->kind == TypeKind::Array || type->kind == TypeKind::Optional) {
            if (data.args.len != 1) {
                error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS,
                      format_type_display(get_system_type(type->kind)), 1, data.args.len);
            }
            auto elem_type = to_value_type(resolve(data.args[0], scope));
            return get_wrapped_type(elem_type, type->kind);
        }
        array<ChiType *> *params_ptr = nullptr;
        array<ast::Node *> *decl_params_ptr = nullptr;
        if (type->kind == TypeKind::Struct) {
            params_ptr = &type->data.struct_.type_params;
            decl_params_ptr = &type->data.struct_.node->data.struct_decl.type_params;
        } else if (type->kind == TypeKind::Enum) {
            params_ptr = &type->data.enum_.type_params;
            decl_params_ptr = &type->data.enum_.node->data.enum_decl.type_params;
        } else {
            error(node, "cannot instantiate non-generic type '{}' with type arguments",
                  format_type_display(type));
            return nullptr;
        }
        auto &params = *params_ptr;
        auto &decl_params = *decl_params_ptr;
        if (data.args.len > params.len) {
            error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS, format_type_display(type), params.len,
                  data.args.len);
            return nullptr;
        }
        // Check that missing args all have defaults
        for (auto i = data.args.len; i < params.len; i++) {
            if (!decl_params[i]->data.type_param.default_type) {
                error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS, format_type_display(type),
                      params.len, data.args.len);
                return nullptr;
            }
        }
        array<ChiType *> args;
        for (auto arg : data.args) {
            auto resolved = resolve_value(arg, scope);
            if (resolved && resolved->kind == TypeKind::Void) {
                error(arg, "'void' cannot be used as a type parameter");
                return nullptr;
            }
            args.add(resolved);
        }
        // Fill in defaults for missing type args
        for (auto i = data.args.len; i < params.len; i++) {
            auto resolved = resolve_value(decl_params[i]->data.type_param.default_type, scope);
            if (resolved && resolved->kind == TypeKind::Void) {
                error(decl_params[i]->data.type_param.default_type,
                      "'void' cannot be used as a type parameter");
                return nullptr;
            }
            args.add(resolved);
        }

        // Validate trait bounds on type arguments
        for (size_t i = 0; i < args.len && i < params.len; i++) {
            auto param_type = to_value_type(params[i]);
            if (!param_type || param_type->data.placeholder.traits.len == 0)
                continue;
            auto type_arg = args[i];
            if (!type_arg || type_arg->is_placeholder)
                continue;
            for (auto trait : param_type->data.placeholder.traits) {
                if (!check_trait_bound(type_arg, trait)) {
                    error(node, "Type '{}' does not satisfy trait bound '{}'",
                          format_type_display(type_arg), format_type_display(trait));
                    return nullptr;
                }
            }
        }

        auto subtype =
            type->kind == TypeKind::Enum ? get_enum_subtype(type, &args) : get_subtype(type, &args);
        return create_type_symbol({}, subtype);
    }
    case NodeType::IndexExpr: {
        auto &data = node->data.index_expr;
        auto expr_type = resolve(data.expr, scope);
        if (!expr_type) {
            return nullptr;
        }
        ensure_temp_owner(data.expr, expr_type, scope);

        // Determine the expected index type based on expr_type
        ChiType *expected_index_type = nullptr;
        switch (expr_type->kind) {
        case TypeKind::Pointer:
        case TypeKind::FixedArray:
            expected_index_type = get_system_types()->uint32;
            break;
        case TypeKind::This:
        case TypeKind::Struct:
        case TypeKind::Subtype:
        case TypeKind::Array:
        case TypeKind::Span: {
            // Block writes on immutable spans
            if (scope.is_lhs && expr_type->kind == TypeKind::Span && !expr_type->data.span.is_mut) {
                error(node, errors::CANNOT_WRITE_IMMUTABLE_SPAN,
                      format_type_display(expr_type),
                      format_type_display(expr_type->data.span.elem));
                return nullptr;
            }
            auto struct_ = resolve_struct_type(expr_type);
            // LHS (write): require IndexMut
            // RHS (read): prefer Index, fall back to IndexMut
            const char *method_name = nullptr;
            if (scope.is_lhs) {
                if (!has_interface_impl(struct_, "std.ops.IndexMut")) {
                    error(node, errors::CANNOT_SUBSCRIPT, format_type_display(expr_type));
                    return nullptr;
                }
                method_name = "index_mut";
            } else {
                if (has_interface_impl(struct_, "std.ops.Index")) {
                    method_name = "index";
                } else if (has_interface_impl(struct_, "std.ops.IndexMut")) {
                    method_name = "index_mut";
                } else {
                    error(node, errors::CANNOT_SUBSCRIPT, format_type_display(expr_type));
                    return nullptr;
                }
            }
            auto method_p = struct_->member_table.get(method_name);
            assert(method_p);
            auto method = *method_p;
            expected_index_type = method->resolved_type->data.fn.get_param_at(0);
            data.resolved_method = method;
            break;
        }
        default:
            error(node, errors::CANNOT_SUBSCRIPT, format_type_display(expr_type));
            return nullptr;
        }

        // Resolve subscript with expected type context for literal inference
        auto subscript_scope = scope.set_value_type(expected_index_type);
        auto subscript_type = resolve(data.subscript, subscript_scope);
        if (!subscript_type) {
            return nullptr;
        }

        check_assignment(data.subscript, subscript_type, expected_index_type);

        if (expr_type->kind == TypeKind::Pointer || expr_type->kind == TypeKind::FixedArray) {
            return expr_type->get_elem();
        } else {
            return data.resolved_method->resolved_type->data.fn.return_type->get_elem();
        }
    }
    case NodeType::SliceExpr: {
        auto &data = node->data.slice_expr;
        auto expr_type = resolve(data.expr, scope);
        if (!expr_type) {
            return nullptr;
        }

        if (expr_type->kind != TypeKind::Struct && expr_type->kind != TypeKind::Subtype &&
            expr_type->kind != TypeKind::Array && expr_type->kind != TypeKind::Span &&
            expr_type->kind != TypeKind::String) {
            error(node, "cannot slice type {}", format_type_display(expr_type));
            return nullptr;
        }

        auto struct_ = resolve_struct_type(expr_type);
        if (!struct_) {
            error(node, "cannot slice type {}", format_type_display(expr_type));
            return nullptr;
        }

        bool has_slice = false;
        for (auto &i : struct_->interfaces) {
            if (i->inteface_symbol == IntrinsicSymbol::Slice) {
                has_slice = true;
                break;
            }
        }
        if (!has_slice) {
            error(node, "type {} does not implement ops.Slice", format_type_display(expr_type));
            return nullptr;
        }

        auto method_p = struct_->member_table.get("slice");
        assert(method_p);
        auto method = *method_p;
        data.resolved_method = method;

        auto uint32_type = get_system_types()->uint32;
        if (data.start) {
            auto start_scope = scope.set_value_type(uint32_type);
            auto start_type = resolve(data.start, start_scope);
            if (!start_type)
                return nullptr;
            check_assignment(data.start, start_type, uint32_type);
        }
        if (data.end) {
            auto end_scope = scope.set_value_type(uint32_type);
            auto end_type = resolve(data.end, end_scope);
            if (!end_type)
                return nullptr;
            check_assignment(data.end, end_type, uint32_type);
        }

        return method->resolved_type->data.fn.return_type;
    }
    case NodeType::EnumVariant: {
        auto &data = node->data.enum_variant;
        if (!data.resolved_type) {
            auto enum_value = create_type(TypeKind::EnumValue);
            auto variant_name = data.name->to_string();
            enum_value->name = variant_name;
            enum_value->display_name = fmt::format("{}.{}", data.parent->name, variant_name);
            auto enum_symbol = resolve(data.parent, scope);
            assert(enum_symbol->kind == TypeKind::TypeSymbol);
            assert(enum_symbol->data.type_symbol.giving_type->kind == TypeKind::EnumValue);
            auto enum_type = enum_symbol->data.type_symbol.underlying_type;

            // copy the prototype from parent
            enum_value->data.enum_value =
                enum_symbol->data.type_symbol.giving_type->data.enum_value;
            ChiType *inner_struct = nullptr;
            if (data.struct_body) {
                auto inner_struct_type = resolve(data.struct_body, scope);
                for (int i = 0; i < (int)ResolveStatus::MemberTypesKnown; i++) {
                    _resolve(data.struct_body, scope);
                }
                inner_struct = eval_struct_type(to_value_type(inner_struct_type));
                enum_value->data.enum_value.variant_struct = inner_struct;
            }
            data.resolved_type = enum_value;
            return enum_value;
        }

        if (data.resolve_status >= ResolveStatus::Done) {
            return data.resolved_type;
        }

        auto enum_value = data.resolved_type;
        if (data.resolve_status == ResolveStatus::EmbedsResolved) {
            // create final resolved struct value by deriving from parent and inner body
            auto base_value_type = enum_value->data.enum_value.parent_enum()->base_value_type;
            auto base_value_struct = base_value_type->data.enum_value.resolved_struct;
            auto variant_struct = enum_value->data.enum_value.variant_struct;
            auto vstruct = create_type(TypeKind::Struct);
            vstruct->display_name = enum_value->display_name;
            vstruct->name = enum_value->name;
            copy_struct_members(base_value_struct, vstruct);
            if (variant_struct) {
                auto data_member = vstruct->data.struct_.add_member(
                    get_allocator(), "__data", get_dummy_var("__data"),
                    variant_struct ? variant_struct : get_system_types()->bool_);
                copy_struct_members(variant_struct, vstruct, data_member);
            }
            enum_value->data.enum_value.resolved_struct = vstruct;

            auto &type_data = enum_value->data.enum_value;
            if (data.value) {
                auto value_type = resolve(data.value, scope);
                check_assignment(data.value, value_type, get_system_types()->int_);
                auto value = resolve_constant_value(data.value);
                assert(value);
                data.resolved_value = get<int64_t>(*value);
                scope.next_enum_value = data.resolved_value + 1;
            } else {
                data.resolved_value = scope.next_enum_value++;
            }
            data.resolve_status = ResolveStatus::Done;
            return enum_value;
        } else {
            data.resolve_status = (ResolveStatus)((int)data.resolve_status + 1);
            return enum_value;
        }
    }
    case NodeType::ForStmt: {
        auto &data = node->data.for_stmt;

        // Try to infer loop variable type from condition (e.g., `for var i=0; i<len; i++`)
        ChiType *loop_var_hint = nullptr;
        if (data.init && data.condition && data.init->type == NodeType::VarDecl) {
            auto &init_data = data.init->data.var_decl;
            // Only if var has no explicit type and has an initializer
            if (!init_data.type && init_data.expr) {
                // Check if condition is a comparison with the loop var
                if (data.condition->type == NodeType::BinOpExpr) {
                    auto &cond_data = data.condition->data.bin_op_expr;
                    auto op = cond_data.op_type;
                    if (op == TokenType::LT || op == TokenType::LE || op == TokenType::GT ||
                        op == TokenType::GE) {
                        // Check if op1 is the loop variable
                        if (cond_data.op1->type == NodeType::Identifier &&
                            cond_data.op1->name == data.init->name) {
                            // Resolve op2 to get the type hint
                            loop_var_hint = resolve(cond_data.op2, scope);
                        }
                    }
                }
            }
        }

        if (data.init) {
            auto init_scope = loop_var_hint ? scope.set_value_type(loop_var_hint) : scope;
            auto init_type = resolve(data.init, init_scope);
            ensure_temp_owner(data.init, init_type, scope);
        }
        if (data.condition) {
            auto cond_type = resolve(data.condition, scope);
            ensure_temp_owner(data.condition, cond_type, scope);
            check_assignment(data.condition, cond_type, get_system_types()->bool_);
        }
        if (data.post) {
            auto post_type = resolve(data.post, scope);
            ensure_temp_owner(data.post, post_type, scope);
        }
        if (data.expr) {
            if (data.expr->type == NodeType::RangeExpr) {
                auto &range = data.expr->data.range_expr;
                auto start_type = resolve(range.start, scope);
                auto end_type = resolve(range.end, scope);
                check_assignment(range.end, end_type, start_type);
                data.kind = ast::ForLoopKind::IntRange;
                if (data.bind) {
                    auto bind_scope = scope.set_value_type(start_type);
                    resolve(data.bind, bind_scope);
                }
            } else {

                auto expr_type = resolve(data.expr, scope);
                ensure_temp_owner(data.expr, expr_type, scope);

                // FixedArray iteration (also handles &[N]T and &mut [N]T)
                auto fa_type = expr_type;
                if (fa_type && fa_type->is_reference()) {
                    fa_type = fa_type->get_elem();
                }
                if (fa_type && fa_type->kind == TypeKind::FixedArray) {
                    if (data.bind) {
                        auto elem_type = fa_type->data.fixed_array.elem;
                        ChiType *value_type = elem_type;
                        switch (data.bind_sigil) {
                        case ast::SigilKind::Reference:
                            value_type = get_pointer_type(value_type, TypeKind::Reference);
                            break;
                        case ast::SigilKind::MutRef:
                            value_type = get_pointer_type(value_type, TypeKind::MutRef);
                            break;
                        default:
                            break;
                        }
                        auto bind_scope = scope.set_value_type(value_type);
                        resolve(data.bind, bind_scope);
                    }
                    if (data.index_bind) {
                        auto idx_scope = scope.set_value_type(get_system_types()->uint32);
                        resolve(data.index_bind, idx_scope);
                    }
                    resolve(data.body, scope);
                    return nullptr;
                }

                auto sty = resolve_struct_type(expr_type);
                if (!sty) {
                    error(node, errors::FOR_EXPR_NOT_ITERABLE, format_type_display(expr_type));
                    return nullptr;
                }

                if (sty->member_intrinsics.get(IntrinsicSymbol::IndexMutIterable)) {
                    // Index-based iteration (Array, etc.)
                    if (data.bind) {
                        auto index_fn = sty->member_table.get("index_mut");
                        if (!index_fn) {
                            error(node, errors::CANNOT_INDEX, format_type_display(expr_type));
                            return nullptr;
                        }
                        auto ref_type = (*index_fn)->resolved_type->data.fn.return_type;
                        auto value_type = ref_type->get_elem();
                        switch (data.bind_sigil) {
                        case ast::SigilKind::Reference:
                            value_type = get_pointer_type(value_type, TypeKind::Reference);
                            break;
                        case ast::SigilKind::MutRef:
                            value_type = get_pointer_type(value_type, TypeKind::MutRef);
                            break;
                        default:
                            break;
                        }
                        auto bind_scope = scope.set_value_type(value_type);
                        resolve(data.bind, bind_scope);
                    }
                    if (data.index_bind) {
                        auto idx_scope = scope.set_value_type(get_system_types()->uint32);
                        resolve(data.index_bind, idx_scope);
                    }
                } else if (sty->member_intrinsics.get(IntrinsicSymbol::MutIterable)) {
                    // Iterator-based iteration (MutIterable)
                    data.kind = ast::ForLoopKind::Iter;
                    auto iter_fn = sty->member_table.get("to_iter_mut");
                    if (!iter_fn) {
                        error(node, errors::FOR_EXPR_NOT_ITERABLE, format_type_display(expr_type));
                        return nullptr;
                    }
                    auto iter_type = (*iter_fn)->resolved_type->data.fn.return_type;
                    auto iter_sty = resolve_struct_type(iter_type);
                    if (!iter_sty ||
                        !iter_sty->member_intrinsics.get(IntrinsicSymbol::MutIterator)) {
                        error(node, errors::FOR_EXPR_NOT_ITERABLE, format_type_display(expr_type));
                        return nullptr;
                    }
                    if (data.bind) {
                        auto next_fn = iter_sty->member_table.get("next");
                        if (!next_fn) {
                            error(node, errors::FOR_EXPR_NOT_ITERABLE,
                                  format_type_display(expr_type));
                            return nullptr;
                        }
                        // next() returns ?&mut T — unwrap optional to get &mut T
                        auto opt_type = (*next_fn)->resolved_type->data.fn.return_type;
                        auto value_type = opt_type->get_elem(); // ?&mut T → &mut T
                        auto bind_scope = scope.set_value_type(value_type);
                        resolve(data.bind, bind_scope);
                    }
                    if (data.index_bind) {
                        auto idx_scope = scope.set_value_type(get_system_types()->uint32);
                        resolve(data.index_bind, idx_scope);
                    }
                } else {
                    error(node, errors::FOR_EXPR_NOT_ITERABLE, format_type_display(expr_type));
                    return nullptr;
                }

            } // else (non-RangeExpr)
        }
        auto loop_scope = scope.set_parent_loop(node);
        resolve(data.body, loop_scope);
        return nullptr;
    }
    case NodeType::BranchStmt: {
        if (!scope.parent_loop) {
            error(node, errors::STMT_NOT_WITHIN_LOOP, node->token->to_string());
        }
        return nullptr;
    }
    case NodeType::TypeParam: {
        auto &data = node->data.type_param;

        auto phty = create_type(TypeKind::Placeholder);
        phty->name = node->name;

        for (auto bound : data.type_bounds) {
            phty->data.placeholder.traits.add(to_value_type(resolve(bound, scope)));
        }

        // Implicit Sized bound: all type params get Sized unless AllowUnsized
        if (m_ctx->rt_sized_interface) {
            bool has_allow_unsized = false;
            bool has_sized = false;
            for (auto t : phty->data.placeholder.traits) {
                if (t && t->kind == TypeKind::Struct && t->data.struct_.node) {
                    auto sym = resolve_intrinsic_symbol(t->data.struct_.node);
                    if (sym == IntrinsicSymbol::AllowUnsized)
                        has_allow_unsized = true;
                    if (sym == IntrinsicSymbol::Sized)
                        has_sized = true;
                }
            }
            if (has_allow_unsized) {
                // Remove AllowUnsized from the list — it's an opt-out, not a real bound
                array<ChiType *> filtered;
                for (auto t : phty->data.placeholder.traits) {
                    if (t && t->kind == TypeKind::Struct && t->data.struct_.node &&
                        resolve_intrinsic_symbol(t->data.struct_.node) ==
                            IntrinsicSymbol::AllowUnsized)
                        continue;
                    filtered.add(t);
                }
                phty->data.placeholder.traits = filtered;
            } else if (!has_sized) {
                // No AllowUnsized and no explicit Sized: add implicit Sized
                phty->data.placeholder.traits.add(m_ctx->rt_sized_interface);
            }
        }

        // Resolve lifetime bound: T: 'a
        if (!data.lifetime_bound.empty() && scope.fn_lifetime_params) {
            auto *lt = scope.fn_lifetime_params->get(data.lifetime_bound);
            if (lt) {
                phty->data.placeholder.lifetime_bound = *lt;
            } else {
                error(node, "unknown lifetime '{}'", data.lifetime_bound);
            }
        }

        phty->data.placeholder.index = data.index;
        phty->data.placeholder.name = node->name;

        assert(data.source_decl && "Type parameter without source declaration");
        phty->data.placeholder.source_decl = data.source_decl;
        phty->global_id = fmt::format("{}.{}", resolve_global_id(data.source_decl), node->name);
        phty->is_placeholder = true;
        phty->display_name = node->name;
        auto type_symbol = create_type_symbol(node->name, phty);
        return type_symbol;
    }
    case NodeType::PrefixExpr: {
        auto &data = node->data.prefix_expr;
        switch (data.prefix->type) {
        case TokenType::KW_DELETE: {
            if (has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_MANAGED)) {
                error(node, "'delete' is not allowed in managed mode");
                return nullptr;
            }
            auto expr_type = resolve(data.expr, scope);
            if (!expr_type->is_raw_pointer() && expr_type->kind != TypeKind::MoveRef) {
                error(node, errors::INVALID_OPERATOR, data.prefix->to_string(),
                      format_type_display(expr_type));
            }
            if (expr_type->is_raw_pointer() && !scope.is_unsafe_block) {
                error(node, "'delete' on raw pointer requires unsafe block");
                return nullptr;
            }
            // delete sinks the variable and its current borrow leaves
            if (scope.parent_fn_node) {
                auto *deleted_decl = find_root_decl(data.expr);
                if (deleted_decl) {
                    auto &fn_def = scope.parent_fn_node->data.fn_def;
                    fn_def.add_sink_edge(deleted_decl, node);
                    // Propagate sink to the data this variable currently points to
                    auto *edges = fn_def.ref_edges.get(deleted_decl);
                    if (edges) {
                        size_t offset = fn_def.current_edge_offset(deleted_decl);
                        for (size_t i = offset; i < edges->len; i++) {
                            fn_def.add_sink_edge(edges->items[i], node);
                        }
                    }
                }
            }
            return get_system_types()->void_;
        }
        case TokenType::KW_SIZEOF: {
            auto type = resolve_value(data.expr, scope);
            data.expr->resolved_type = type;
            return get_system_types()->uint32;
        }
        default:
            panic("unhandled prefix operator {}", data.prefix->to_string());
        }
        break;
    }
    case NodeType::ExternDecl: {
        auto &data = node->data.extern_decl;
        if (!node->resolved_type) {
            node->resolved_type = get_system_types()->void_;
            auto *ctx = static_cast<CompilationContext *>(m_ctx->allocator);

            // Get include directories from package config
            std::vector<std::string> include_dirs;
            if (scope.module->package->config &&
                scope.module->package->config->c_interop.has_value()) {
                include_dirs = scope.module->package->config->c_interop->include_directories;
            }

            // Helper to process C header imports/exports - extracts symbols and resolves header
            // module
            auto process_c_header_symbols = [&](std::string header_path,
                                                array<ast::Node *> symbols) {
                // Check if this is a C header (ends with .h)
                if (header_path.size() > 2 && header_path.substr(header_path.size() - 2) == ".h") {
                    // Extract symbol patterns
                    std::vector<std::string> symbol_patterns;
                    for (auto symbol_node : symbols) {
                        symbol_patterns.push_back(symbol_node->data.import_symbol.name->str);
                    }

                    bool newly_created = false;
                    auto *header_module = import_c_header_as_module(
                        ctx, header_path, symbol_patterns, include_dirs, &newly_created);

                    // Always resolve - newly added symbols need their global_id set
                    // (already-resolved nodes will return early from resolver)
                    if (header_module) {
                        Resolver resolver(m_ctx);
                        resolver.resolve(header_module);
                    }
                }
            };

            // Process header imports: extern "C" { import {strlen, str*} from "header.h"; }
            for (auto import_node : data.imports) {
                auto &import_data = import_node->data.import_decl;
                process_c_header_symbols(import_data.path->str, import_data.symbols);
                resolve(import_node, scope);
            }

            // Process header exports: extern "C" { export {strlen, str*} from "string.h"; }
            for (auto export_node : data.exports) {
                auto &export_data = export_node->data.export_decl;
                process_c_header_symbols(export_data.path->str, export_data.symbols);
                resolve(export_node, scope);
            }

            // Process inline function declarations - add directly to current scope
            for (auto member : data.members) {
                resolve(member, scope);
            }
        }
        return node->resolved_type;
    }
    case NodeType::Error: {
        node->resolved_type = get_system_types()->void_;
        return node->resolved_type;
    }
    case NodeType::ImportDecl:
    case NodeType::ExportDecl: {
        auto is_export = node->type == NodeType::ExportDecl;
        auto &data = is_export ? node->data.export_decl : node->data.import_decl;

        // Check if this is a virtual module (e.g., from C interop)
        ast::Module *module = nullptr;
        auto *comp_ctx = static_cast<CompilationContext *>(m_ctx->allocator);

        // For "C" module, first try module-scoped lookup (e.g., "main.C")
        // This allows each module to have its own virtual "C" module
        std::string module_scoped_key = scope.module->id_path + "." + data.path->str;
        auto virtual_mod = comp_ctx->module_map.get(module_scoped_key);
        if (!virtual_mod) {
            // Fall back to global lookup for backwards compatibility
            virtual_mod = comp_ctx->module_map.get(data.path->str);
        }

        if (virtual_mod) {
            // Use the virtual module directly (already resolved)
            module = *virtual_mod;
            // Note: Virtual modules are pre-resolved, no need to resolve again
        } else {
            // Regular file-based module lookup
            auto path_info = m_ctx->allocator->find_module_path(data.path->str, scope.module->path);
            if (!path_info) {
                error(node, errors::MODULE_NOT_FOUND, data.path->str);
                return nullptr;
            }

            if (path_info->is_directory && !path_info->entry_path.size()) {
                error(node, errors::MODULE_INDEX_NOT_FOUND, data.path->str);
                return nullptr;
            }

            auto path = path_info->entry_path;
            auto abs_path = fs::absolute(path).string();
            auto cached_mod = comp_ctx->source_modules.get(abs_path);
            if (cached_mod) {
                module = *cached_mod;
            } else {
                // When the BUILTIN (runtime) module imports/exports a sub-module,
                // make all currently-resolved runtime exports available as builtins
                // so the sub-module can reference types like Array, panic, etc.
                if (scope.module->package->kind == ast::PackageKind::BUILTIN) {
                    context_init_builtins(scope.module);
                }
                auto target_package =
                    m_ctx->allocator->get_or_create_package(path_info->package_id_path);
                auto src = io::Buffer::from_file(path);
                module = m_ctx->allocator->process_source(target_package, &src, path);
                Resolver resolver(m_ctx);
                resolver.resolve(module);
                comp_ctx->source_modules[abs_path] = module;
            }
        }

        data.resolved_module = module;
        auto type = create_type(TypeKind::Module);
        type->name = "Module:" + module->full_path();
        type->data.module.scope = m_ctx->allocator->create_scope(nullptr);

        if (is_export) {
            if (data.match_all) {
                for (auto item : module->exports) {
                    scope.module->exports.add(item);
                    scope.module->import_scope->put(item->name, item);
                }
            } else {
                if (data.alias) {
                    for (auto item : module->exports) {
                        type->data.module.scope->put(item->name, item);
                    }
                    scope.module->exports.add(node);
                } else {
                    for (auto symbol : data.symbols) {
                        auto item_type = resolve(symbol, scope);
                        auto &item_data = symbol->data.import_symbol;
                        if (!item_data.resolved_decl) {
                            continue;
                        }

                        std::string symbol_name = item_data.name->get_name();

                        // Check if this is a wildcard pattern
                        if (symbol_name.find('*') != std::string::npos) {
                            // Export all matching symbols
                            for (auto export_item : module->exports) {
                                if (matches_pattern(export_item->name, symbol_name)) {
                                    scope.module->exports.add(export_item);
                                    scope.module->import_scope->put(export_item->name, export_item);
                                }
                            }
                        } else {
                            // Regular single symbol export
                            auto name = item_data.output_name();
                            scope.module->exports.add(item_data.resolved_decl);
                            scope.module->import_scope->put(name, item_data.resolved_decl);
                        }
                    }
                }
            }
        } else {
            scope.module->imports.add(module);
            if (data.match_all && !data.alias) {
                // import * from "module" - import all exports directly into scope
                for (auto item : module->exports) {
                    scope.module->import_scope->put(item->name, item);
                }
            } else if (data.alias) {
                // import * as alias from "module" OR import alias from "module"
                for (auto item : module->exports) {
                    type->data.module.scope->put(item->name, item);
                }
            } else {
                // import {X, Y} from "module" - import specific symbols
                for (auto symbol : data.symbols) {
                    auto item_type = resolve(symbol, scope);
                    auto &item_data = symbol->data.import_symbol;
                    // Skip if symbol wasn't resolved (error already reported)
                    if (!item_data.resolved_decl) {
                        continue;
                    }

                    std::string symbol_name = item_data.name->get_name();

                    // Check if this is a wildcard pattern
                    if (symbol_name.find('*') != std::string::npos) {
                        // Import all matching symbols (wildcards don't support aliases)
                        for (auto export_item : module->exports) {
                            if (matches_pattern(export_item->name, symbol_name)) {
                                type->data.module.scope->put(export_item->name, export_item);
                            }
                        }
                    } else {
                        // Regular single symbol import
                        auto name = item_data.output_name();
                        type->data.module.scope->put(name, item_data.resolved_decl);
                    }
                }
            }
        }
        return type;
    }
    case NodeType::BindIdentifier: {
        // Add to cleanup_vars if this bind variable needs destruction (for-range by value)
        auto bind_type = scope.value_type;
        if (scope.parent_fn_node && scope.block && should_destroy(node, bind_type) &&
            !node->escape.is_capture()) {
            scope.block->cleanup_vars.add(node);
            scope.parent_fn_def()->has_cleanup = true;
        }
        return bind_type;
    }
    case NodeType::ImportSymbol: {
        auto &data = node->data.import_symbol;
        auto module = data.import->data.import_decl.resolved_module;
        std::string symbol_name = data.name->get_name();

        // Check if this is a wildcard pattern (contains *)
        if (symbol_name.find('*') != std::string::npos) {
            // Wildcard import: match pattern against all module exports
            bool found_any = false;
            ast::Node *first_match = nullptr;

            for (auto export_item : module->exports) {
                if (matches_pattern(export_item->name, symbol_name)) {
                    if (!first_match) {
                        first_match = export_item;
                    }
                    found_any = true;
                }
            }

            if (!found_any) {
                error(node, errors::SYMBOL_NOT_FOUND_MODULE, symbol_name, module->path);
                return nullptr;
            }

            // Set resolved_decl to first match (for consistency)
            data.resolved_decl = first_match;
            return first_match ? first_match->resolved_type : nullptr;
        } else {
            // Regular single symbol import - search exports
            ast::Node *decl = nullptr;
            for (auto export_item : module->exports) {
                if (export_item->name == symbol_name) {
                    decl = export_item;
                    break;
                }
            }
            if (!decl) {
                // Fallback: also check import_scope for re-exported symbols
                decl = module->import_scope->find_one(symbol_name);
            }
            if (!decl) {
                error(node, errors::SYMBOL_NOT_FOUND_MODULE, symbol_name, module->path);
                return nullptr;
            }
            data.resolved_decl = decl;
            return decl->resolved_type;
        }
    }
    case NodeType::SwitchExpr: {
        auto &data = node->data.switch_expr;
        if (scope.move_outlet) {
            node->resolved_outlet = scope.move_outlet;
            node->escape.moved = true;
        }
        auto expr_type = resolve(data.expr, scope);
        ensure_temp_owner(data.expr, expr_type, scope);

        // Handle invalid switch expressions gracefully
        if (!expr_type) {
            return nullptr;
        }

        // Type switch on interface references
        if (data.is_type_switch) {
            auto iface_elem = expr_type->is_pointer_like() ? expr_type->get_elem() : nullptr;
            if (!iface_elem || !ChiTypeStruct::is_interface(iface_elem)) {
                error(data.expr, "type switch requires an interface reference");
                return nullptr;
            }

            ChiType *ret_type = scope.value_type;
            for (auto scase : data.cases) {
                if (!scase)
                    continue;

                if (!scase->data.case_expr.is_else) {
                    for (auto clause : scase->data.case_expr.clauses) {
                        auto clause_type = resolve(clause, scope);
                        // Unwrap TypeSymbol from type expression resolution
                        if (clause_type && clause_type->kind == TypeKind::TypeSymbol)
                            clause_type = clause_type->data.type_symbol.underlying_type;
                        // Verify the concrete type implements the interface
                        auto clause_elem =
                            clause_type ? (clause_type->is_pointer_like() ? clause_type->get_elem()
                                                                          : clause_type)
                                        : nullptr;
                        if (clause_elem && clause_elem->kind == TypeKind::Struct &&
                            !ChiTypeStruct::is_interface(clause_elem)) {
                            if (!clause_elem->data.struct_.interface_table.get(iface_elem)) {
                                error(clause, "type '{}' does not implement interface '{}'",
                                      format_type_display(clause_type),
                                      format_type_display(iface_elem));
                            }
                        }
                        clause->resolved_type = clause_type;
                    }

                    // Type narrowing: single-clause case
                    if (scase->data.case_expr.clauses.len == 1) {
                        auto clause_type = node_get_type(scase->data.case_expr.clauses[0]);
                        if (clause_type) {
                            auto switch_expr = data.expr;
                            if (switch_expr->type == NodeType::Identifier ||
                                switch_expr->type == NodeType::DotExpr) {
                                auto var =
                                    create_narrowed_var(switch_expr, node, scope, clause_type);
                                auto &block_data = scase->data.case_expr.body->data.block;
                                block_data.implicit_vars.add(var);
                                block_data.scope->put(var->name, var);
                            }
                        }
                    }
                }

                auto case_type = resolve(scase, scope);
                if (ret_type) {
                    check_assignment(scase, case_type, ret_type);
                    scase->resolved_type = ret_type;
                } else {
                    ret_type = case_type;
                }
            }
            return ret_type ? ret_type : get_system_types()->void_;
        }

        auto expr_comparator = resolve_comparator(expr_type, scope);

        ChiType *ret_type = scope.value_type;
        for (auto scase : data.cases) {
            // Skip null case expressions
            if (!scase)
                continue;

            // Resolve clauses BEFORE body so we can inject narrowed vars
            if (!scase->data.case_expr.is_else) {
                for (auto clause : scase->data.case_expr.clauses) {
                    auto clause_type = resolve(clause, scope);
                    resolve_constant_value(clause);
                    auto clause_comparator = resolve_comparator(clause_type, scope);

                    // Only check assignment if both comparators are valid
                    if (clause_comparator && expr_comparator) {
                        check_assignment(clause, clause_comparator, expr_comparator);

                        if (!clause_comparator->is_int_like()) {
                            error(clause, errors::INVALID_SWITCH_TYPE,
                                  format_type_display(clause_type));
                        }
                    }
                }

                // Enum variant narrowing: single-clause case matching a specific variant
                if (scase->data.case_expr.clauses.len == 1) {
                    auto underlying = expr_type->eval();
                    if (underlying && underlying->kind == TypeKind::EnumValue) {
                        auto clause = scase->data.case_expr.clauses[0];
                        auto clause_type = node_get_type(clause);
                        if (clause_type && clause_type->kind == TypeKind::EnumValue &&
                            clause_type->data.enum_value.member) {
                            auto switch_expr = data.expr;
                            if (switch_expr->type == NodeType::Identifier ||
                                switch_expr->type == NodeType::DotExpr) {
                                auto var =
                                    create_narrowed_var(switch_expr, node, scope, clause_type);
                                auto &block_data = scase->data.case_expr.body->data.block;
                                block_data.implicit_vars.add(var);
                                block_data.scope->put(var->name, var);
                            }
                        }
                    }
                }
            }

            auto case_type = resolve(scase, scope);

            if (ret_type) {
                check_assignment(scase, case_type, ret_type);
                scase->resolved_type = ret_type;
            } else {
                ret_type = case_type;
            }
        }
        return ret_type ? ret_type : get_system_types()->void_;
    }
    case NodeType::CaseExpr: {
        auto &data = node->data.case_expr;
        return resolve(data.body, scope);
    }
    case NodeType::EmptyStmt: {
        // Empty statements do nothing and have void type
        node->resolved_type = get_system_types()->void_;
        return node->resolved_type;
    }
    case NodeType::TypedefDecl: {
        // C interop typedefs are pre-resolved
        if (node->resolved_type) {
            return node->resolved_type;
        }
        auto &data = node->data.typedef_decl;
        // Resolve type params as placeholders
        for (auto param : data.type_params) {
            resolve(param, scope);
        }
        // Resolve the RHS type expression
        auto rhs_type = resolve_value(data.type, scope);
        if (!rhs_type) {
            return nullptr;
        }
        node->resolved_type = create_type_symbol(node->name, rhs_type);
        return node->resolved_type;
    }
    default:
        print("\n");
        panic("unhandled node {}", PRINT_ENUM(node->type));
    }
    return nullptr;
}

ChiType *Resolver::resolve_comparator(ChiType *type, ResolveScope &scope) {
    if (!type) {
        return nullptr;
    }
    switch (type->kind) {
    case TypeKind::This:
        return resolve_comparator(type->eval(), scope);
    case TypeKind::EnumValue:
        return type->data.enum_value.discriminator_type;
    case TypeKind::Reference:
    case TypeKind::Pointer:
    case TypeKind::MutRef:
    case TypeKind::MoveRef:
        return resolve_comparator(type->get_elem(), scope);
    default:
        return type;
    }
}

ChiType *Resolver::resolve(ast::Node *node, ResolveScope &scope, uint32_t flags) {
    if (!node)
        return nullptr;
    auto cached = node_get_type(node);
    if (cached) {
        return cached;
    }

    auto result = _resolve(node, scope, flags);
    node->resolved_type = result;
    if (!node->name.empty()) {
        node->global_id = resolve_global_id(node);
    }

    if (!result)
        return nullptr;
    return result;
}

string Resolver::resolve_global_id(ast::Node *node) {
    // For extern "C" functions, use C linkage (no module prefix)
    if (node->type == ast::NodeType::FnDef && node->data.fn_def.decl_spec &&
        node->data.fn_def.decl_spec->is_extern()) {
        return node->name;
    }
    return fmt::format("{}.{}", node->module->global_id(), resolve_qualified_name(node));
}

bool Resolver::has_interface_impl(ChiTypeStruct *struct_type, string interface_id) {
    auto sym_p = m_ctx->intrinsic_symbols.get(interface_id);
    if (!sym_p)
        return false;
    auto sym = *sym_p;
    // Sized is structural: everything is Sized except interfaces
    if (sym == IntrinsicSymbol::Sized) {
        return struct_type->kind != ContainerKind::Interface;
    }
    for (auto &i : struct_type->interfaces) {
        if (i->inteface_symbol == sym) {
            return true;
        }
    }
    return false;
}

string Resolver::resolve_qualified_name(ast::Node *node) {
    switch (node->type) {
    case NodeType::FnDef:
        if (node->resolved_type && node->resolved_type->kind == TypeKind::Fn) {
            auto container_ref = node->resolved_type->data.fn.container_ref;
            if (container_ref) {
                auto container = container_ref->get_elem();
                // Use for_display=true to get just the type name, not the full global_id
                return fmt::format("{}.{}", format_type_display(container), node->name);
            }
            return node->name;
        }
        if (node->parent) {
            return fmt::format("{}.{}", resolve_qualified_name(node->parent), node->name);
        }
        return node->name;
    case NodeType::GeneratedFn: {
        auto &data = node->data.generated_fn;
        auto fn_name = resolve_qualified_name(data.original_fn);
        return fmt::format("{}.{}", fn_name, format_type_id(data.fn_subtype));
    }
    default:
        break;
    }
    return node->name;
}

string Resolver::format_type(ChiType *type, bool for_display) {
    if (!type) {
        return "<invalid-type>";
    }
    if (for_display) {
        if (type->display_name) {
            return *type->display_name;
        }
        if (type->name) {
            return *type->name;
        }
    } else {
        if (!type->global_id.empty()) {
            return type->global_id;
        }
        auto name = format_type_display(type);
        return fmt::format("Type:{}:{}", type->id, name);
    }

    switch (type->kind) {
    case TypeKind::This:
        return format_type(type->eval(), for_display);
    case TypeKind::Subtype: {
        auto &data = type->data.subtype;
        switch (data.generic->kind) {
        case cx::TypeKind::Fn:
        case cx::TypeKind::FnLambda:
            return format_type(data.final_type, for_display);
        default: {
            std::stringstream ss;
            ss << format_type(data.generic, for_display) << "<";
            for (int i = 0; i < data.args.len; i++) {
                ss << format_type(data.args[i], for_display);
                if (i < data.args.len - 1) {
                    ss << ",";
                }
            }
            ss << ">";
            return ss.str();
        }
        }
    }
    case TypeKind::String:
        return "string";
    case TypeKind::Pointer:
        return "*" + format_type(type->get_elem(), for_display);
    case TypeKind::Reference:
        return "&" + format_type(type->get_elem(), for_display);
    case TypeKind::MutRef:
        return "&mut " + format_type(type->get_elem(), for_display);
    case TypeKind::MoveRef:
        return "&move " + format_type(type->get_elem(), for_display);
    case TypeKind::Optional:
        return "?" + format_type(type->get_elem(), for_display);
    case TypeKind::Array:
        return fmt::format("Array<{}>", format_type(type->get_elem(), for_display));
    case TypeKind::Span:
        return fmt::format("{}{}",
            type->data.span.is_mut ? "[]mut " : "[]",
            format_type(type->data.span.elem, for_display));
    case TypeKind::FixedArray:
        return fmt::format("[{}]{}", type->data.fixed_array.size,
                           format_type(type->data.fixed_array.elem, for_display));
    case TypeKind::Result:
        return fmt::format("Result<{},{}>", format_type(type->get_elem(), for_display),
                           format_type(type->data.result.error, for_display));
    case TypeKind::Unknown:
        return "unknown";
    case TypeKind::Undefined:
        return "undefined";
    case TypeKind::ZeroInit:
        return "zeroinit";
    case TypeKind::Never:
        return "never";
    default:
        break;
    }
    assert(type->kind < TypeKind::__COUNT);
    return format_type_data(type->kind, &type->data, for_display);
}

string Resolver::format_type_list(TypeList *type_list, bool for_display) {
    std::stringstream ss;
    for (int i = 0; i < type_list->len; i++) {
        ss << format_type(type_list->at(i), for_display);
        if (i < type_list->len - 1) {
            ss << ",";
        }
    }
    return ss.str();
}

string Resolver::format_type_data(TypeKind kind, ChiType::Data *data, bool for_display) {
    switch (kind) {
    case TypeKind::Struct: {
        auto &struct_ = data->struct_;
        std::stringstream ss;
        ss << "struct ";
        if (struct_.type_params.len > 0) {
            ss << "<";
            for (int i = 0; i < struct_.type_params.len; i++) {
                ss << format_type(struct_.type_params[i], for_display);
                if (i < struct_.type_params.len - 1) {
                    ss << ",";
                }
            }
            ss << ">";
        }
        ss << "{";
        for (int i = 0; i < struct_.members.len; i++) {
            auto &member = struct_.members[i];
            ss << format_type(member->resolved_type, for_display);
            if (i < struct_.members.len - 1) {
                ss << ",";
            }
        }
        ss << "}";
        return ss.str();
    }
    case TypeKind::Fn: {
        auto &fn = data->fn;
        std::stringstream ss;
        if (fn.is_extern) {
            ss << "extern ";
        }
        if (fn.container_ref && !for_display) {
            ss << "(";
            ss << format_type(fn.container_ref, for_display);
            ss << ") ";
        }
        ss << "func(";
        for (int i = 0; i < fn.params.len; i++) {
            if (fn.is_variadic && i == fn.params.len - 1) {
                ss << "...";
            }
            ss << format_type(fn.params[i], for_display);
            if (i < fn.params.len - 1) {
                ss << ",";
            }
        }
        ss << ")";
        if (fn.return_type) {
            ss << " " << format_type(fn.return_type, for_display);
        }
        return ss.str();
    }
    case TypeKind::FnLambda: {
        auto &fn_lambda = data->fn_lambda;
        return fmt::format("Lambda<{}>", format_type(fn_lambda.fn, for_display));
    }
    case TypeKind::Enum: {
        auto &enum_ = data->enum_;
        std::stringstream ss;
        ss << "enum ";
        for (int i = 0; i < enum_.variants.len; i++) {
            auto &member = enum_.variants[i];
            ss << member->name;
            auto &enum_value = member->resolved_type->data.enum_value;
            if (member->resolved_type->data.enum_value.variant_struct) {
                ss << "{" << format_type(enum_value.variant_struct, for_display) << "}";
            }
            if (i < enum_.variants.len - 1) {
                ss << ",";
            }
        }
        return ss.str();
    }
    default:
        break;
    }
    return PRINT_ENUM(kind);
}

void Resolver::check_assignment(ast::Node *value, ChiType *from_type, ChiType *to_type,
                                bool is_explicit) {
    // If from_type is null (failed to resolve), skip assignment check
    if (!from_type || !to_type) {
        return;
    }

    // Check for negative literal being assigned to unsigned type
    if (!is_explicit && to_type->kind == TypeKind::Int && to_type->data.int_.is_unsigned) {
        if (value->type == ast::NodeType::LiteralExpr && value->token->type == TokenType::INT) {
            // Check if the literal is negative (preceded by unary minus)
            // Note: This is handled by UnaryOpExpr, not here
        } else if (value->type == ast::NodeType::UnaryOpExpr) {
            auto &unary = value->data.unary_op_expr;
            if (unary.op_type == TokenType::SUB && unary.op1->type == ast::NodeType::LiteralExpr &&
                unary.op1->token->type == TokenType::INT) {
                error(value, "cannot convert negative literal to unsigned type {}",
                      format_type_display(to_type));
                return;
            }
        }
    }

    if (!can_assign(from_type, to_type, is_explicit)) {
        if (!is_explicit) {
            auto can_convert_explitcitly = can_assign(from_type, to_type, true);
            if (can_convert_explitcitly) {
                error(value, errors::CANNOT_CONVERT_IMPLICIT, format_type_display(from_type),
                      format_type_display(to_type));
                return;
            }
        }
        error(value, errors::CANNOT_CONVERT, format_type_display(from_type),
              format_type_display(to_type));
    }
}

bool Resolver::is_addressable(ast::Node *node) {
    switch (node->type) {
    case NodeType::Identifier:
    case NodeType::DotExpr:
    case NodeType::IndexExpr:
    case NodeType::LiteralExpr: // static lifetime, no temp needed
        return true;

    case NodeType::ParenExpr:
        return is_addressable(node->data.child_expr);

    case NodeType::UnaryOpExpr: {
        auto &data = node->data.unary_op_expr;
        return data.op_type == TokenType::MUL ||
               data.op_type == TokenType::KW_MOVE ||
               (data.op_type == TokenType::LNOT && data.is_suffix && data.resolved_call);
    }

    default:
        return false;
    }
}

bool Resolver::is_ref_mutable(ast::Node *node, ResolveScope &scope) {
    switch (node->type) {
    case NodeType::Identifier: {
        auto decl = node->get_decl();
        if (!decl) {
            return false;
        }
        return decl->is_mutable();
    }
    default:
        return true;
    }
}

void Resolver::check_cast(ast::Node *value, ChiType *from_type, ChiType *to_type) {
    check_assignment(value, from_type, to_type, true);
}

void Resolver::context_init_builtins(ast::Module *builtin_module) {
    for (auto &decl : builtin_module->exports) {
        m_ctx->builtins.add(decl);
    }
}

string Resolver::resolve_term_string(ast::Node *term) {
    switch (term->type) {
    case NodeType::Identifier:
        return term->name;
    case NodeType::DotExpr:
        return resolve_term_string(term->data.dot_expr.expr) + "." +
               term->data.dot_expr.field->get_name();
    default:
        panic("unhandled term node: {}", PRINT_ENUM(term->type));
    }
    return "";
}

IntrinsicSymbol Resolver::resolve_intrinsic_symbol(ast::Node *node) {
    if (node->symbol != IntrinsicSymbol::None) {
        return node->symbol;
    }

    auto sym_p = m_ctx->intrinsic_symbols.get(resolve_global_id(node));
    if (sym_p) {
        return *sym_p;
    }

    return IntrinsicSymbol::None;
}

IntrinsicSymbol Resolver::get_operator_intrinsic_symbol(TokenType op_type) {
    switch (op_type) {
    case TokenType::ADD:
        return IntrinsicSymbol::Add;
    case TokenType::SUB:
        return IntrinsicSymbol::Sub;
    case TokenType::MUL:
        return IntrinsicSymbol::Mul;
    case TokenType::DIV:
        return IntrinsicSymbol::Div;
    case TokenType::MOD:
        return IntrinsicSymbol::Rem;
    case TokenType::AND:
        return IntrinsicSymbol::BitAnd;
    case TokenType::OR:
        return IntrinsicSymbol::BitOr;
    case TokenType::XOR:
        return IntrinsicSymbol::BitXor;
    case TokenType::LSHIFT:
        return IntrinsicSymbol::Shl;
    case TokenType::RSHIFT:
        return IntrinsicSymbol::Shr;
    case TokenType::EQ:
    case TokenType::NE:
        return IntrinsicSymbol::Eq;
    case TokenType::LT:
    case TokenType::LE:
    case TokenType::GT:
    case TokenType::GE:
        return IntrinsicSymbol::Ord;
    default:
        return IntrinsicSymbol::None;
    }
}


ChiStructMember *Resolver::resolve_struct_member(ChiType *struct_type, ast::Node *node,
                                                 ResolveScope &scope) {
    auto &struct_ = struct_type->data.struct_;
    auto member = struct_.add_member(get_allocator(), node->name, node, nullptr);
    member->symbol = resolve_intrinsic_symbol(struct_.node);

    if (node->type == NodeType::VarDecl) {
        member->resolved_type = resolve(node, scope);
        node->data.var_decl.resolved_field = member;

        // &move references cannot be struct fields
        if (member->resolved_type && member->resolved_type->kind == TypeKind::MoveRef) {
            error(node, errors::MOVE_REF_IN_STRUCT_FIELD);
        }

        // Reference fields implicitly have 'this lifetime
        if (member->resolved_type && member->resolved_type->is_pointer_like()) {
            if (!struct_.this_lifetime) {
                struct_.this_lifetime =
                    new ChiLifetime{"this", LifetimeKind::This, nullptr, struct_type};
            }
        }
    } else if (node->type == NodeType::FnDef) {
        member->resolved_type = resolve(node, scope);
        for (auto attr : node->declspec_ref().attributes) {
            auto term_string = resolve_term_string(attr->data.decl_attribute.term);
            auto sym = m_ctx->intrinsic_symbols.get(term_string);
            if (sym) {
                member->symbol = *sym;
                struct_.member_intrinsics[member->symbol] = member;
            } else {
                error(node, errors::INVALID_ATTRIBUTE_TERM, term_string);
                continue;
            }
        }
    }
    return member;
}

void Resolver::resolve_vtable(ChiType *base_type, ChiType *derived_type, ast::Node *base_node,
                              bool from_embedding) {
    auto &base = *resolve_struct_type(base_type);
    auto &derived = *resolve_struct_type(derived_type);
    InterfaceImpl *iface_impl = nullptr;
    auto trait_symbol = resolve_intrinsic_symbol(base.node);
    if (base.kind == ContainerKind::Interface) {
        iface_impl = derived.add_interface(get_allocator(), base_type, derived_type);
        iface_impl->inteface_symbol = trait_symbol;
    }

    for (auto &base_member : base.members) {
        auto node = base_member->node;
        if (base_member->is_method()) {
            auto child_method = derived.find_member(node->name);

            if (node->data.fn_def.body) {
                if (!child_method) {
                    // Don't promote constructors or destructors through embedding
                    if (node->name == "new" || node->name == "delete")
                        continue;
                    // Create a new fn type with container_ref pointing to the
                    // implementing struct so codegen compiles it with the correct
                    // 'this' type (thin pointer to concrete struct, not fat
                    // interface pointer).
                    auto &base_fn = base_member->resolved_type->data.fn;
                    auto concrete_fn_type =
                        get_fn_type(base_fn.return_type, &base_fn.params, base_fn.is_variadic,
                                    derived_type, base_fn.is_extern, nullptr);
                    child_method =
                        derived.add_member(get_allocator(), node->name, node, concrete_fn_type);
                    child_method->orig_parent = base_type;
                }
            } else if (!child_method) {
                error(base_node, errors::METHOD_NOT_IMPLEMENTED, node->name);
                break;
            }

            // If base_type is a subtype (like Index<uint32, T>), we need to substitute
            // the placeholders in the base member type
            ChiType *base_member_type = base_member->resolved_type;
            if (base_type->kind == TypeKind::Subtype) {
                auto &subtype_data = base_type->data.subtype;
                // Use selective substitution to only substitute placeholders from the interface
                base_member_type = type_placeholders_sub_selective(base_member_type, &subtype_data,
                                                                   subtype_data.root_node);
            }

            // Substitute This types in interface methods with the implementing type
            base_member_type = substitute_this_type(base_member_type, derived_type);

            // Signature check only applies to interface implementation, not struct embedding.
            // For promoted methods (from struct embedding), also skip — the embedded struct
            // already verified interface compliance.
            bool is_promoted = child_method->orig_parent != nullptr;
            if (iface_impl && !is_promoted &&
                !compare_impl_type(base_member_type, child_method->resolved_type)) {
                if (from_embedding) {
                    // The derived struct has explicitly overridden this method with a different
                    // signature (e.g. for a different generic specialisation). The embedded
                    // type's interface no longer applies to the derived type — invalidate it.
                    derived.interface_table.unset(base_type);
                    derived.interfaces.pop();
                    return;
                }
                error(base_node, errors::IMPLEMENT_NOT_MATCH, node->name,
                      format_type_display(base_type));
                break;
            }
            if (iface_impl) {
                assert(child_method);
                child_method->is_impl_method = true;
                iface_impl->impl_members.add(child_method);

                if (trait_symbol != IntrinsicSymbol::None) {
                    child_method->symbol = trait_symbol;
                } else if (base_member->symbol != IntrinsicSymbol::None) {
                    child_method->symbol = base_member->symbol;
                } else if (base.kind == ContainerKind::Interface) {
                    // Check embedded interfaces for intrinsic symbols
                    for (auto embed_type : base.embeds) {
                        if (embed_type && embed_type->kind == TypeKind::Struct &&
                            ChiTypeStruct::is_interface(embed_type)) {
                            auto &embed_data = embed_type->data.struct_;

                            // Find matching method in embedded interface
                            for (auto embed_member : embed_data.members) {
                                if (embed_member->is_method() &&
                                    embed_member->get_name() == base_member->get_name()) {
                                    // Check if embedded interface has intrinsic symbol
                                    auto embed_intrinsic =
                                        resolve_intrinsic_symbol(embed_data.node);
                                    if (embed_intrinsic != IntrinsicSymbol::None) {
                                        child_method->symbol = embed_intrinsic;
                                        break;
                                    }
                                    // Also check if the specific member has intrinsic
                                    if (embed_member->symbol != IntrinsicSymbol::None) {
                                        child_method->symbol = embed_member->symbol;
                                        break;
                                    }
                                }
                            }
                            if (child_method->symbol != IntrinsicSymbol::None) {
                                break;
                            }
                        }
                    }
                }
                if (child_method->symbol != IntrinsicSymbol::None) {
                    derived.member_intrinsics[child_method->symbol] = child_method;
                }
                iface_impl->inteface_symbol = base_member->symbol;
            }
        }
        if (base_member->is_field()) {
            auto child_field = derived.find_member(node->name);
            if (!child_field) {
                child_field = derived.add_member(get_allocator(), node->name, node,
                                                 base_member->resolved_type, false);
                child_field->orig_parent = base_type;
                // If base_member is itself promoted (multi-level embed), chain through
                // the already-promoted intermediary in derived rather than jumping
                // directly to the embed field (which would produce an invalid GEP index).
                if (base_member->parent_member) {
                    auto intermediate = derived.find_member(base_member->parent_member->get_name());
                    child_field->parent_member = intermediate ? intermediate
                                                              : base_node->data.var_decl.resolved_field;
                } else {
                    child_field->parent_member = base_node->data.var_decl.resolved_field;
                }
                child_field->field_index = base_member->field_index;
            }
        }
    }
    for (auto &impl : base.interfaces) {
        resolve_vtable(impl->interface_type, derived_type, base_node, from_embedding);
    }
}

void Resolver::resolve_struct_embed(ChiType *struct_type, ast::Node *base_node,
                                    ResolveScope &parent_scope) {
    auto &current = struct_type->data.struct_;
    auto em_type = node_get_type(base_node);
    auto em_struct = resolve_struct_type(em_type);
    if (!em_struct) {
        error(base_node, errors::INVALID_EMBED);
        return;
    }
    auto &base = *em_struct;
    if (base.kind != ContainerKind::Struct && base.kind != ContainerKind::Interface) {
        error(base_node, errors::INVALID_EMBED);
        return;
    }
    if (current.kind != base.kind) {
        error(base_node, errors::CANNOT_EMBED_INTO, format_type_display(em_type),
              format_type_display(struct_type));
    }
    if (base.resolve_status < ResolveStatus::Done) {
        _resolve(base.node, parent_scope);
    }

    if (current.kind == ContainerKind::Interface) {
        current.embeds.add(em_type);
        for (auto embed_member : base.members) {
            if (!embed_member->is_method())
                continue;
            auto existing_member = current.find_member(embed_member->get_name());
            if (existing_member)
                continue;
            auto copied_member =
                current.add_member(get_allocator(), embed_member->get_name(),
                                   embed_member->node, embed_member->resolved_type);
            copied_member->orig_parent = em_type;
            copied_member->symbol = embed_member->symbol;
            if (copied_member->symbol != IntrinsicSymbol::None) {
                current.member_intrinsics[copied_member->symbol] = copied_member;
            }
        }
        return;
    }

    resolve_vtable(em_type, struct_type, base_node, /*from_embedding=*/true);

    // Set parent_member on promoted methods so codegen generates forwarding proxies
    auto embed_field = base_node->data.var_decl.resolved_field;
    for (auto &member : current.members) {
        if (member->is_method() && member->orig_parent == em_type && !member->parent_member) {
            member->parent_member = embed_field;
        }
    }
}

bool Resolver::is_borrowing_type(ChiType *type) {
    if (!type)
        return false;
    switch (type->kind) {
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef:
        return true;
    case TypeKind::Optional:
    case TypeKind::Array:
    case TypeKind::Span:
        return is_borrowing_type(type->get_elem());
    case TypeKind::Result:
        return is_borrowing_type(type->get_elem()) || is_borrowing_type(type->data.result.error);
    case TypeKind::Subtype: {
        auto final_type = type->data.subtype.final_type;
        return final_type ? is_borrowing_type(final_type) : false;
    }
    case TypeKind::Fn:
        // Function types are always potentially borrowing because a func() value
        // can hold a lambda with by-ref captures (type erasure hides the borrows)
        return true;
    case TypeKind::FnLambda: {
        if (is_borrowing_type(type->data.fn_lambda.fn))
            return true;
        // By-ref captures are stored as reference fields in the bind struct —
        // the lambda borrows from the captured variables' lifetimes
        auto *bs = type->data.fn_lambda.bind_struct;
        if (bs && bs->kind == TypeKind::Struct) {
            for (auto field : bs->data.struct_.fields) {
                if (is_borrowing_type(field->resolved_type))
                    return true;
            }
        }
        return false;
    }
    case TypeKind::Struct: {
        auto &st = type->data.struct_;
        if (st.member_intrinsics.has_key(IntrinsicSymbol::CopyFrom)) {
            // CopyFrom handles the container's own data, but if any type parameter
            // is borrowing, the borrow propagates through copied elements
            for (auto tp : st.type_params) {
                if (is_borrowing_type(tp))
                    return true;
            }
            return false;
        }
        for (auto field : st.fields) {
            if (is_borrowing_type(field->resolved_type))
                return true;
        }
        return false;
    }
    case TypeKind::Placeholder:
        return type->data.placeholder.lifetime_bound != nullptr;
    default:
        return false;
    }
}

// Check if a type needs destruction (has destructor or has fields that need destruction)
bool Resolver::type_needs_destruction(ChiType *type) {
    if (!type)
        return false;

    // Any may contain a destructible value
    if (type->kind == TypeKind::Any)
        return true;

    // Strings need destruction
    if (type->kind == TypeKind::String)
        return true;

    // Interface types need vtable-based destruction
    if (type->kind == TypeKind::Struct && ChiTypeStruct::is_interface(type))
        return true;

    // Lambdas may own type-erased captures that must be released.
    if (type->kind == TypeKind::FnLambda) {
        auto internal = type->data.fn_lambda.internal;
        return internal ? type_needs_destruction(internal) : false;
    }

    // Optional needs destruction if its element type needs destruction
    if (type->kind == TypeKind::Optional) {
        auto elem_type = type->get_elem();
        return elem_type && type_needs_destruction(elem_type);
    }

    // FixedArray needs destruction if its element type needs destruction
    if (type->kind == TypeKind::FixedArray) {
        return type_needs_destruction(type->data.fixed_array.elem);
    }

    // Result always needs destruction — it owns the error object's heap allocation
    if (type->kind == TypeKind::Result) {
        return true;
    }

    // &move T owns the pointee — RAII auto-destroy + free at scope exit
    if (type->kind == TypeKind::MoveRef) {
        return true;
    }

    // For Subtype (generic instantiation), check the final resolved type
    if (type->kind == TypeKind::Subtype) {
        auto final_type = type->data.subtype.final_type;
        if (final_type) {
            return type_needs_destruction(final_type);
        }
        // If final_type is not resolved yet, check the generic's destructor
        auto generic = type->data.subtype.generic;
        if (generic && generic->kind == TypeKind::Struct) {
            return get_struct_member(generic, "delete") != nullptr;
        }
        return false;
    }

    // EnumValue: check if base struct or any variant has destructible fields
    if (type->kind == TypeKind::EnumValue) {
        auto enum_ = type->data.enum_value.parent_enum();
        auto bvs = enum_->base_value_type->data.enum_value.resolved_struct;
        if (bvs) {
            for (auto field : bvs->data.struct_.fields) {
                if (type_needs_destruction(field->resolved_type))
                    return true;
            }
        }
        for (auto variant : enum_->variants) {
            if (auto vs = variant->resolved_type->data.enum_value.variant_struct) {
                for (auto field : vs->data.struct_.fields) {
                    if (type_needs_destruction(field->resolved_type))
                        return true;
                }
            }
        }
        return false;
    }

    // Only structs can have destructors or fields needing destruction
    if (type->kind != TypeKind::Struct)
        return false;

    // Has custom destructor
    if (get_struct_member(type, "delete"))
        return true;

    // Check if any field needs destruction
    auto &fields = type->data.struct_.fields;
    for (auto field : fields) {
        if (type_needs_destruction(field->resolved_type)) {
            return true;
        }
    }
    return false;
}

bool Resolver::is_non_copyable(ChiType *type) {
    if (!type)
        return false;
    if (type->kind == TypeKind::Subtype) {
        auto final_type = type->data.subtype.final_type;
        if (final_type)
            return is_non_copyable(final_type);
    }
    if (type->kind != TypeKind::Struct)
        return false;
    for (auto iface : type->data.struct_.interfaces) {
        if (iface->inteface_symbol == IntrinsicSymbol::DisallowCopy)
            return true;
    }
    return false;
}

bool Resolver::should_destroy(ast::Node *node, ChiType *type_override) {
    auto is_managed = has_lang_flag(node->module->get_lang_flags(), LANG_FLAG_MANAGED);
    if (is_managed && node->is_heap_allocated()) {
        return false;
    }
    auto resolved_type = type_override ? type_override : node_get_type(node);
    return type_needs_destruction(resolved_type);
}

bool Resolver::should_resolve_fn_body(ResolveScope &scope) {
    auto parent_struct = scope.parent_struct;
    if (!parent_struct) {
        return !scope.skip_fn_bodies;
    }
    auto &struct_ = parent_struct->data.struct_;
    return struct_.kind != ContainerKind::Interface &&
           struct_.resolve_status >= ResolveStatus::MemberTypesKnown;
}

void Resolver::type_placeholders_sub_each(TypeList *list, ChiTypeSubtype *subs, TypeList *output) {
    for (auto arg : *list) {
        output->add(type_placeholders_sub(arg, subs));
    }
}

void Resolver::type_placeholders_sub_each_selective(TypeList *list, ChiTypeSubtype *subs,
                                                    TypeList *output, ast::Node *source_filter) {
    for (auto arg : *list) {
        output->add(type_placeholders_sub_selective(arg, subs, source_filter));
    }
}

// Visitor function for type placeholder substitution with configurable behavior
template <typename PlaceholderHandler, typename RecursiveCallHandler>
ChiType *Resolver::recursive_type_replace(ChiType *type, ChiTypeSubtype *subs,
                                          PlaceholderHandler handle_placeholder,
                                          RecursiveCallHandler make_recursive_call) {
    if (!type)
        return nullptr;
    switch (type->kind) {
    case TypeKind::Placeholder:
        return handle_placeholder(type, subs);

    case TypeKind::Infer: {
        // For Infer types, return the inferred type if available
        auto inferred = type->data.infer.inferred_type;
        if (inferred) {
            // Recursively substitute in case the inferred type has placeholders
            return make_recursive_call(inferred, subs);
        }
        // If no inferred type, return as-is (should not happen in normal flow)
        return type;
    }

    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef:
    case TypeKind::Optional: {
        auto elem_type = make_recursive_call(type->get_elem(), subs);
        return get_pointer_type(elem_type, type->kind);
    }
    case TypeKind::Array: {
        auto elem_type = make_recursive_call(type->get_elem(), subs);
        return get_array_type(elem_type);
    }
    case TypeKind::Span: {
        auto elem_type = make_recursive_call(type->get_elem(), subs);
        return get_span_type(elem_type, type->data.span.is_mut);
    }

    case TypeKind::Subtype: {
        auto &data = type->data.subtype;
        array<ChiType *> args;
        for (auto arg : data.args) {
            args.add(make_recursive_call(arg, subs));
        }
        return get_subtype(data.generic, &args);
    }

    case TypeKind::Fn: {
        auto &data = type->data.fn;
        ChiType *fn_type = create_type(TypeKind::Fn);
        auto ret = make_recursive_call(data.return_type, subs);
        fn_type->data.fn.return_type = ret;
        for (auto param : data.params) {
            fn_type->data.fn.params.add(make_recursive_call(param, subs));
        }
        // Preserve type parameters that don't belong to the source we're substituting
        for (auto type_param : data.type_params) {
            fn_type->data.fn.type_params.add(type_param);
        }
        fn_type->data.fn.is_variadic = data.is_variadic;
        fn_type->data.fn.container_ref =
            data.container_ref ? make_recursive_call(data.container_ref, subs) : nullptr;
        fn_type->data.fn.is_extern = data.is_extern;
        fn_type->data.fn.is_static = data.is_static;

        // Mark as placeholder if any component is still a placeholder
        // This must be consistent with get_fn_type()
        fn_type->is_placeholder = false;

        // Check params
        for (auto param : fn_type->data.fn.params) {
            if (param->is_placeholder) {
                fn_type->is_placeholder = true;
                break;
            }
        }

        // Check type_params
        if (!fn_type->is_placeholder) {
            for (auto type_param : fn_type->data.fn.type_params) {
                if (type_param->is_placeholder) {
                    fn_type->is_placeholder = true;
                    break;
                }
            }
        }

        // Check return type
        fn_type->is_placeholder = fn_type->is_placeholder || ret->is_placeholder;

        return fn_type;
    }

    case TypeKind::Struct: {
        auto &data = type->data.struct_;

        // If struct has placeholder type_params, substitute them to get a Subtype
        // This handles "specialized structs" created by resolve_subtype for generic
        // instantiations like Promise<T> where T is still a placeholder
        if (data.type_params.len > 0) {
            bool has_placeholder_param = false;
            array<ChiType *> subst_args;
            for (auto tp : data.type_params) {
                auto subst = make_recursive_call(tp, subs);
                subst_args.add(subst);
                if (tp != subst)
                    has_placeholder_param = true;
            }
            if (has_placeholder_param) {
                // Find the base generic struct by looking up through subtypes
                // The base generic has the same node but global_id without type args
                ChiType *base_generic = nullptr;
                if (data.node) {
                    // Look for the base struct in the node's resolved_type
                    auto node_type = data.node->resolved_type;
                    if (node_type && node_type->kind == TypeKind::TypeSymbol) {
                        auto underlying = node_type->data.type_symbol.underlying_type;
                        if (underlying && underlying->kind == TypeKind::Struct) {
                            base_generic = underlying;
                        }
                    }
                }
                if (base_generic) {
                    return get_subtype(base_generic, &subst_args);
                }
            }
        }

        // For placeholder structs (e.g. lambda bind structs with generic fields),
        // create a new struct with substituted field types
        if (!type->is_placeholder)
            return type;
        auto new_struct = create_type(TypeKind::Struct);
        new_struct->display_name = type->display_name;
        auto &new_data = new_struct->data.struct_;
        new_data.kind = data.kind;
        for (auto field : data.fields) {
            auto new_type = make_recursive_call(field->resolved_type, subs);
            auto name = fmt::format("field_{}", new_data.fields.len);
            new_data.add_member(get_allocator(), field->get_name(), get_dummy_var(name), new_type);
        }
        new_struct->is_placeholder = false;
        for (auto field : new_data.fields) {
            if (field->resolved_type->is_placeholder) {
                new_struct->is_placeholder = true;
                break;
            }
        }
        return new_struct;
    }

    case TypeKind::FnLambda: {
        auto &data = type->data.fn_lambda;
        auto fn = data.fn ? make_recursive_call(data.fn, subs) : nullptr;
        auto internal = data.internal ? make_recursive_call(data.internal, subs) : nullptr;
        auto bound_fn = data.bound_fn ? make_recursive_call(data.bound_fn, subs) : nullptr;
        auto bind_struct = data.bind_struct ? make_recursive_call(data.bind_struct, subs) : nullptr;

        auto lambda_type = create_type(TypeKind::FnLambda);
        lambda_type->data.fn_lambda.fn = fn;
        lambda_type->data.fn_lambda.internal = internal;
        lambda_type->data.fn_lambda.bound_fn = bound_fn;
        lambda_type->data.fn_lambda.bind_struct = bind_struct;

        // If internal is null (was deferred), check if we can finalize now
        // __CxLambda is not generic, so use it directly
        if (!internal) {
            auto rt_lambda = m_ctx->rt_lambda_type;
            if (rt_lambda &&
                rt_lambda->data.struct_.resolve_status >= ResolveStatus::MemberTypesKnown) {
                lambda_type->data.fn_lambda.internal = to_value_type(rt_lambda);
            }
        }

        // is_placeholder if fn is placeholder or internal couldn't be resolved
        lambda_type->is_placeholder =
            (fn && fn->is_placeholder) || !lambda_type->data.fn_lambda.internal;
        return lambda_type;
    }

    case TypeKind::EnumValue: {
        auto &ev_data = type->data.enum_value;
        auto new_ev = create_type(TypeKind::EnumValue);
        new_ev->data.enum_value = ev_data;
        new_ev->name = type->name;
        new_ev->display_name = type->display_name;
        // Substitute struct member types for variant_struct and resolved_struct
        auto sub_struct = [&](ChiType *src) -> ChiType * {
            if (!src || src->kind != TypeKind::Struct)
                return src;
            auto dst = create_type(TypeKind::Struct);
            src->clone(dst);
            dst->data.struct_.members = {};
            dst->data.struct_.fields = {};
            dst->data.struct_.member_table = {};
            for (auto member : src->data.struct_.members) {
                // Only substitute field types — methods have container_ref back to
                // the EnumValue which would cause infinite recursion
                auto new_type = member->is_method()
                                    ? member->resolved_type
                                    : make_recursive_call(member->resolved_type, subs);
                auto new_member = dst->data.struct_.add_member(get_allocator(), member->get_name(),
                                                               member->node, new_type);
                // Preserve field_index override for variant fields with parent_member
                if (member->parent_member) {
                    new_member->field_index = member->field_index;
                }
            }
            // Fix up parent_member pointers to reference new members (same order)
            for (size_t i = 0; i < src->data.struct_.members.len; i++) {
                auto orig = src->data.struct_.members[i];
                if (orig->parent_member) {
                    // Find the new parent_member by matching position
                    for (size_t j = 0; j < src->data.struct_.members.len; j++) {
                        if (src->data.struct_.members[j] == orig->parent_member) {
                            dst->data.struct_.members[i]->parent_member =
                                dst->data.struct_.members[j];
                            break;
                        }
                    }
                }
            }
            return dst;
        };
        new_ev->data.enum_value.variant_struct = sub_struct(ev_data.variant_struct);
        new_ev->data.enum_value.resolved_struct = sub_struct(ev_data.resolved_struct);
        return new_ev;
    }

    case TypeKind::This:
    case TypeKind::ThisType: {
        // This and ThisType should be handled by the caller if needed
        return type;
    }

    default:
        return type;
    }
}

// Substitute This types with a concrete type (used for interface implementations)
ChiType *Resolver::substitute_this_type(ChiType *type, ChiType *replacement) {
    // Use recursive_type_replace with custom handlers for This/ThisType
    auto handle_this = [&](ChiType *t, ChiTypeSubtype *) -> ChiType * {
        // This case is not used in this function, but required by template
        return t;
    };

    auto make_recursive_call = [&](ChiType *t, ChiTypeSubtype *) -> ChiType * {
        if (t->kind == TypeKind::This || t->kind == TypeKind::ThisType) {
            // Replace This/ThisType with the implementing type
            return replacement;
        }
        // Recurse for other types
        return substitute_this_type(t, replacement);
    };

    // Handle This/ThisType at the top level
    if (type->kind == TypeKind::This || type->kind == TypeKind::ThisType) {
        return replacement;
    }

    // Use recursive_type_replace for all other cases
    return recursive_type_replace(type, nullptr, handle_this, make_recursive_call);
}

// Selective substitution that only replaces placeholders from a specific source declaration
ChiType *Resolver::type_placeholders_sub_selective(ChiType *type, ChiTypeSubtype *subs,
                                                   ast::Node *source_filter) {
    auto handle_placeholder = [source_filter](ChiType *type, ChiTypeSubtype *subs) -> ChiType * {
        // Only substitute if this placeholder belongs to the source we're targeting
        if (type->data.placeholder.source_decl == source_filter) {
            return subs->args[type->data.placeholder.index];
        } else {
            return type;
        }
    };

    auto make_recursive_call = [this, source_filter](ChiType *type,
                                                     ChiTypeSubtype *subs) -> ChiType * {
        return type_placeholders_sub_selective(type, subs, source_filter);
    };

    return recursive_type_replace(type, subs, handle_placeholder, make_recursive_call);
}

ChiType *Resolver::type_placeholders_sub(ChiType *type, ChiTypeSubtype *subs) {
    auto handle_placeholder = [](ChiType *type, ChiTypeSubtype *subs) -> ChiType * {
        return subs->args[type->data.placeholder.index];
    };

    auto make_recursive_call = [this](ChiType *type, ChiTypeSubtype *subs) -> ChiType * {
        return type_placeholders_sub(type, subs);
    };

    return recursive_type_replace(type, subs, handle_placeholder, make_recursive_call);
}

ChiType *Resolver::type_placeholders_sub_map(ChiType *type, map<ChiType *, ChiType *> *subs) {
    auto handle_placeholder = [subs](ChiType *type, ChiTypeSubtype *) -> ChiType * {
        auto substitution = subs->get(type);
        if (substitution)
            return *substitution;
        // Fallback: match by global_id for cross-module cases where
        // get_subtype caching may produce different Placeholder pointers
        if (!type->global_id.empty()) {
            for (auto &pair : subs->get()) {
                if (pair.first->global_id == type->global_id)
                    return pair.second;
            }
        }
        return type;
    };

    auto make_recursive_call = [this, subs](ChiType *type, ChiTypeSubtype *) -> ChiType * {
        return type_placeholders_sub_map(type, subs);
    };

    return recursive_type_replace(type, nullptr, handle_placeholder, make_recursive_call);
}

// Type inference algorithm using visitor pattern - mirrors recursive_type_replace structure
template <typename UnificationHandler, typename RecursiveCallHandler>
bool Resolver::visit_type_recursive(ChiType *param_type, ChiType *arg_type,
                                    UnificationHandler handle_placeholder,
                                    RecursiveCallHandler make_recursive_call) {
    // Handle different type kinds with same structure as recursive_type_replace
    switch (param_type->kind) {
    case TypeKind::Placeholder: {
        // If arg_type is an Infer type with inferred_type, use the inferred type
        ChiType *concrete_arg = arg_type;
        if (arg_type->kind == TypeKind::Infer && arg_type->data.infer.inferred_type) {
            concrete_arg = arg_type->data.infer.inferred_type;
        }
        return handle_placeholder(param_type, concrete_arg);
    }

    case TypeKind::Infer: {
        // For Infer types, if we have an inferred_type, use it to bind the original placeholder.
        // This allows type inference from lambda return types to propagate to generic type params.
        auto &infer_data = param_type->data.infer;
        if (infer_data.inferred_type && infer_data.placeholder) {
            // Bind the placeholder to the inferred type
            return handle_placeholder(infer_data.placeholder, infer_data.inferred_type);
        }
        // If arg_type is also an Infer with inferred_type, use that
        if (arg_type->kind == TypeKind::Infer) {
            auto &arg_infer = arg_type->data.infer;
            if (arg_infer.inferred_type && infer_data.placeholder) {
                return handle_placeholder(infer_data.placeholder, arg_infer.inferred_type);
            }
        }
        // Otherwise, just try to unify with the arg_type directly
        if (infer_data.placeholder) {
            return handle_placeholder(infer_data.placeholder, arg_type);
        }
        return false;
    }

    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef:
    case TypeKind::Optional:
    case TypeKind::Array:
    case TypeKind::Span: {
        // Must have same wrapper kind and unify element types
        if (param_type->kind != arg_type->kind) {
            return false;
        }
        return make_recursive_call(param_type->get_elem(), arg_type->get_elem());
    }

    case TypeKind::Subtype: {
        // Must be same generic type with unifiable type arguments
        if (arg_type->kind != TypeKind::Subtype) {
            return false;
        }
        auto &param_data = param_type->data.subtype;
        auto &arg_data = arg_type->data.subtype;
        if (param_data.generic != arg_data.generic) {
            return false;
        }
        if (param_data.args.len != arg_data.args.len) {
            return false;
        }
        for (size_t i = 0; i < param_data.args.len; i++) {
            if (!make_recursive_call(param_data.args[i], arg_data.args[i])) {
                return false;
            }
        }
        return true;
    }

    case TypeKind::Fn: {
        // Must be function types with unifiable signatures
        // Also accept FnLambda as argument (extract inner Fn type)
        ChiTypeFn *arg_fn = nullptr;
        if (arg_type->kind == TypeKind::Fn) {
            arg_fn = &arg_type->data.fn;
        } else if (arg_type->kind == TypeKind::FnLambda) {
            arg_fn = &arg_type->data.fn_lambda.fn->data.fn;
        } else {
            return false;
        }
        auto &param_data = param_type->data.fn;
        auto &arg_data = *arg_fn;

        // Check return type
        if (!make_recursive_call(param_data.return_type, arg_data.return_type)) {
            return false;
        }

        // Check parameter count
        if (param_data.params.len != arg_data.params.len) {
            return false;
        }

        // Check each parameter type
        for (size_t i = 0; i < param_data.params.len; i++) {
            if (!make_recursive_call(param_data.params[i], arg_data.params[i])) {
                return false;
            }
        }
        return true;
    }

    case TypeKind::FnLambda: {
        // Must be lambda types with unifiable function signatures
        if (arg_type->kind != TypeKind::FnLambda) {
            return false;
        }
        auto &param_data = param_type->data.fn_lambda;
        auto &arg_data = arg_type->data.fn_lambda;
        return make_recursive_call(param_data.fn, arg_data.fn);
    }

    default:
        // For concrete types, they must be identical
        return param_type == arg_type ||
               (param_type->kind == arg_type->kind && param_type->global_id == arg_type->global_id);
    }
}

bool Resolver::infer_type_arguments(ChiTypeFn *fn, TypeList *arg_types,
                                    map<ChiType *, ChiType *> *inferred_types) {
    // Initialize inferred types map
    inferred_types->clear();

    // Check if we have the right number of arguments
    if (fn->params.len != arg_types->len) {
        return false;
    }

    // Handle placeholder mapping via unification
    auto handle_placeholder = [fn, inferred_types](ChiType *placeholder,
                                                   ChiType *concrete) -> bool {
        // Find the corresponding type parameter
        size_t placeholder_index = placeholder->data.placeholder.index;
        if (placeholder_index >= fn->type_params.len) {
            return false;
        }

        ChiType *corresponding_type_param = fn->type_params[placeholder_index];
        auto existing = inferred_types->get(corresponding_type_param);
        if (existing) {
            // Check consistency
            return *existing == concrete;
        } else {
            // New inference
            (*inferred_types)[corresponding_type_param] = concrete;
            return true;
        }
    };

    // Fully recursive unification with depth guard against cyclic types
    constexpr int MAX_UNIFY_DEPTH = 32;
    int depth = 0;
    std::function<bool(ChiType *, ChiType *)> unify =
        [this, &handle_placeholder, &unify, &depth](ChiType *param, ChiType *arg) -> bool {
        if (++depth > MAX_UNIFY_DEPTH) {
            --depth;
            return false;
        }
        auto result = this->visit_type_recursive(param, arg, handle_placeholder, unify);
        --depth;
        return result;
    };

    // Use visitor pattern to unify each parameter with its argument
    for (size_t i = 0; i < fn->params.len; i++) {
        ChiType *param_type = fn->params[i];
        ChiType *arg_type = (*arg_types)[i];

        // Attempt to unify this parameter with its argument
        if (!visit_type_recursive(param_type, arg_type, handle_placeholder, unify)) {
            return false; // Unification failed
        }
    }

    // Verify that all type parameters have been inferred
    for (auto type_param : fn->type_params) {
        if (!inferred_types->get(type_param)) {
            return false; // Could not infer this type parameter
        }
    }

    return true;
}

// Infer type parameters from expected return type
// This enables bidirectional type inference: type params can be inferred from
// the expected return type, not just from arguments.
// For example: `Promise<Unit> x = promise(func (resolve) { ... })` can infer T=Unit
// from the expected return type Promise<Unit>.
bool Resolver::infer_from_return_type(ChiTypeFn *fn, ChiType *expected_type,
                                      map<ChiType *, ChiType *> *inferred_types) {
    if (!expected_type || !fn->return_type) {
        return false;
    }

    // Use visitor pattern to unify return type with expected type
    auto handle_placeholder = [fn, inferred_types](ChiType *placeholder,
                                                   ChiType *concrete) -> bool {
        size_t placeholder_index = placeholder->data.placeholder.index;
        if (placeholder_index >= fn->type_params.len) {
            return false;
        }

        ChiType *corresponding_type_param = fn->type_params[placeholder_index];
        auto existing = inferred_types->get(corresponding_type_param);
        if (existing) {
            return *existing == concrete;
        } else {
            (*inferred_types)[corresponding_type_param] = concrete;
            return true;
        }
    };

    // Fully recursive unification with depth guard against cyclic types
    constexpr int MAX_UNIFY_DEPTH = 32;
    int depth = 0;
    std::function<bool(ChiType *, ChiType *)> unify =
        [this, &handle_placeholder, &unify, &depth](ChiType *param, ChiType *arg) -> bool {
        if (++depth > MAX_UNIFY_DEPTH) {
            --depth;
            return false;
        }
        auto result = this->visit_type_recursive(param, arg, handle_placeholder, unify);
        --depth;
        return result;
    };

    return visit_type_recursive(fn->return_type, expected_type, handle_placeholder, unify);
}

bool Resolver::is_struct_type(ChiType *type) {
    switch (type->kind) {
    case TypeKind::This:
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef:
        return false;
    default:
        return (bool)resolve_struct_type(type);
    }
}

ChiTypeStruct *Resolver::resolve_struct_type(ChiType *type) {
    auto stype = eval_struct_type(type);
    return stype ? &stype->data.struct_ : nullptr;
}

ChiType *Resolver::eval_struct_type(ChiType *type) {
    if (!type)
        return nullptr;
    auto sty = type;
    if (sty->kind == TypeKind::This) {
        sty = sty->get_elem();
    }
    if (sty->is_pointer_like()) {
        sty = sty->get_elem();
    }
    if (sty->kind == TypeKind::Array) {
        if (!sty->data.array.internal) {
            auto rt_array = m_ctx->rt_array_type;
            assert(rt_array);
            array<ChiType *> args;
            args.add(sty->data.array.elem);
            auto astype = get_subtype(to_value_type(rt_array), &args);
            sty->data.array.internal = astype;
            sty = astype;
        } else {
            sty = sty->data.array.internal;
        }
    } else if (sty->kind == TypeKind::Span) {
        if (!sty->data.span.internal) {
            auto rt_span = m_ctx->rt_span_type;
            assert(rt_span);
            array<ChiType *> args;
            args.add(sty->data.span.elem);
            auto sstype = get_subtype(to_value_type(rt_span), &args);
            sty->data.span.internal = sstype;
            sty = sstype;
        } else {
            sty = sty->data.span.internal;
        }
    } else if (sty->kind == TypeKind::String) {
        auto rt_string = m_ctx->rt_string_type;
        assert(rt_string);
        sty = rt_string;
    } else if (sty->kind == TypeKind::Result) {
        sty = sty->data.result.internal;
    } else if (sty->kind == TypeKind::EnumValue) {
        sty = sty->data.enum_value.resolved_struct;
    } else if (sty->kind == TypeKind::FnLambda) {
        sty = sty->data.fn_lambda.internal;
    } else if (sty->kind == TypeKind::TypeSymbol) {
        if (auto underlying_type = sty->data.type_symbol.underlying_type) {
            sty = underlying_type;
        }
    }
    if (sty->kind == TypeKind::Subtype) {
        sty = resolve_subtype(sty);
    }
    if (sty->kind != TypeKind::Struct) {
        return nullptr;
    }
    return sty;
}

void Resolver::copy_struct_members(ChiType *from, ChiType *to, ChiStructMember *parent_member) {
    assert(from->kind == TypeKind::Struct && to->kind == TypeKind::Struct);
    for (auto member : from->data.struct_.members) {
        auto existing = to->data.struct_.find_member(member->get_name());
        if (existing && member->is_method()) {
            // Override existing member (e.g. custom enum struct overriding __CxEnumBase method)
            existing->node = member->node;
            existing->resolved_type = member->resolved_type;
            if (member->symbol != IntrinsicSymbol::None) {
                existing->symbol = member->symbol;
                to->data.struct_.member_intrinsics[member->symbol] = existing;
            }
            continue;
        }
        auto new_member = to->data.struct_.add_member(get_allocator(), member->get_name(),
                                                      member->node, member->resolved_type);
        if (parent_member && member->is_field()) {
            new_member->parent_member = parent_member;
            new_member->field_index = member->field_index;
        }
        if (member->symbol != IntrinsicSymbol::None) {
            new_member->symbol = member->symbol;
            to->data.struct_.member_intrinsics[member->symbol] = new_member;
        }
    }
}

bool Resolver::is_struct_access_mutable(ChiType *type, ResolveScope *scope) {
    auto sty = type;
    if (sty->kind == TypeKind::This) {
        if (scope && scope->parent_fn_node)
            return scope->parent_fn_node->declspec().is_mutable();
        return false;
    }
    if (sty->is_pointer_like()) {
        return ChiTypeStruct::is_mutable_pointer(sty);
    }
    return true;
}

ChiStructMember *Resolver::get_struct_member(ChiType *struct_type, const string &field_name) {
    if (!struct_type) {
        return nullptr;
    }
    auto sty = resolve_struct_type(struct_type);
    if (!sty) {
        return nullptr;
    }
    return sty->find_member(field_name);
}

ChiStructMember *Resolver::get_struct_member_access(ast::Node *node, ChiType *struct_type,
                                                    const string &field_name, bool is_internal,
                                                    bool is_write, ResolveScope *scope,
                                                    ChiType *access_type) {
    auto mut_check_type = access_type ? access_type : struct_type;
    auto field_member = get_struct_member(struct_type, field_name);
    if (!field_member) {
        error(node, errors::MEMBER_NOT_FOUND, field_name, format_type_display(struct_type));
        return nullptr;
    }
    if (is_write && !is_struct_access_mutable(mut_check_type, scope)) {
        error(node, errors::CANNOT_MODIFY_IMMUTABLE_REFERENCE, format_type_display(struct_type));
        return nullptr;
    }
    if (!field_member->check_access(is_internal, is_write)) {
        if (field_member->get_visibility() == Visibility::Protected) {
            error(node, errors::PROTECTED_MEMBER_NOT_WRITABLE, field_name,
                  format_type_display(struct_type));
        } else {
            error(node, errors::PRIVATE_MEMBER_NOT_ACCESSIBLE, field_name,
                  format_type_display(struct_type));
        }
        return nullptr;
    }

    if (field_member->is_method()) {
        auto is_mutable = field_member->node->declspec_ref().is_mutable();
        if (is_mutable && !is_struct_access_mutable(mut_check_type, scope)) {
            error(node, errors::MUTATING_METHOD_ON_IMMUTABLE_REFERENCE, field_name,
                  format_type_display(struct_type));
            return nullptr;
        }
    }
    return field_member;
}

bool Resolver::is_friend_struct(ChiType *a, ChiType *b) {
    if (a->kind == TypeKind::Array || b->kind == TypeKind::Array || a->kind == TypeKind::String ||
        b->kind == TypeKind::String) {
        return b->kind == a->kind;
    }
    auto a_sty = resolve_struct_type(a);
    auto b_sty = resolve_struct_type(b);
    if (!a_sty || !b_sty || !a_sty->node || !b_sty->node) {
        return false;
    }
    // Use resolve_global_id on the AST node to get the canonical struct ID.
    // This handles generic instantiations correctly - both the base generic
    // and all instantiations share the same node, so they're considered friends.
    return resolve_global_id(a_sty->node) == resolve_global_id(b_sty->node);
}

ChiType *Resolver::resolve_fn_call(ast::Node *node, ResolveScope &scope, ChiTypeFn *fn,
                                   NodeList *args, ast::Node *fn_decl) {
    auto n_args = args->len;
    auto n_params = fn->params.len;

    // Count required parameters (those without defaults, excluding variadic)
    size_t params_required = n_params - (fn->is_variadic ? 1 : 0);
    size_t max_args = params_required;
    if (fn_decl && fn_decl->type == ast::NodeType::FnDef) {
        auto *fn_proto_node = fn_decl->data.fn_def.fn_proto;
        auto &fn_proto = fn_proto_node->data.fn_proto;
        params_required = 0;
        for (size_t i = 0; i < fn_proto.params.len; i++) {
            auto param = fn_proto.params[i];
            if (!param->data.param_decl.default_value && !param->data.param_decl.is_variadic) {
                params_required++;
            }
        }
        max_args = n_params - (fn->is_variadic ? 1 : 0);
    }

    // Validate: n_args must be >= required and <= total (non-variadic) or >= required (variadic)
    bool ok = n_args >= params_required && (fn->is_variadic || n_args <= max_args);
    if (!ok) {
        if (fn_decl && params_required != max_args) {
            error(node, "wrong number of arguments: expected {} to {}, got {}", params_required,
                  max_args, n_args);
        } else {
            error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, params_required, n_args);
        }
        return fn->return_type;
    }

    // Default values for missing arguments are handled by codegen
    // (compile_construction and compile_fn_args inject them at compile time).
    // We do NOT mutate the AST args list here.

    // Check if this is a generic function call that needs explicit type parameters
    if (fn->is_generic()) {
        array<ChiType *> type_args;
        map<ChiType *, ChiType *> type_substitutions;
        bool has_explicit_type_args = false;

        // Check for explicit type arguments first (e.g., promise<Unit>)
        if (node->type == ast::NodeType::FnCallExpr) {
            auto &fn_call_data = node->data.fn_call_expr;
            if (fn_call_data.type_args.len > 0) {
                has_explicit_type_args = true;

                // Check if the number of type parameters matches
                if (fn_call_data.type_args.len != fn->type_params.len) {
                    error(node, "Wrong number of type parameters: expected {}, got {}",
                          fn->type_params.len, fn_call_data.type_args.len);
                    return fn->return_type;
                }

                // Resolve the explicit type parameters and build substitution map
                for (size_t i = 0; i < fn->type_params.len; i++) {
                    auto type_arg = resolve_value(fn_call_data.type_args[i], scope);
                    type_args.add(type_arg);

                    auto type_param = fn->type_params[i];
                    auto lookup_key = type_param;
                    if (type_param->kind == TypeKind::TypeSymbol) {
                        lookup_key = type_param->data.type_symbol.giving_type;
                    }
                    type_substitutions.emplace(lookup_key, type_arg);
                }
            }
        }

        // Try to infer type parameters from expected return type (bidirectional inference)
        // This enables: `Promise<Unit> x = promise(func (resolve) { ... })` to infer T=Unit
        map<ChiType *, ChiType *> return_type_inferred;
        bool has_return_type_inference = false;
        if (!has_explicit_type_args && scope.value_type) {
            if (infer_from_return_type(fn, scope.value_type, &return_type_inferred)) {
                // Check if we inferred all type parameters
                bool all_inferred = true;
                for (auto type_param : fn->type_params) {
                    if (!return_type_inferred.get(type_param)) {
                        all_inferred = false;
                        break;
                    }
                }
                if (all_inferred) {
                    has_return_type_inference = true;
                    // Build type_args and type_substitutions from inferred types
                    for (size_t i = 0; i < fn->type_params.len; i++) {
                        auto type_param = fn->type_params[i];
                        auto inferred = return_type_inferred.get(type_param);
                        type_args.add(*inferred);

                        auto lookup_key = type_param;
                        if (type_param->kind == TypeKind::TypeSymbol) {
                            lookup_key = type_param->data.type_symbol.giving_type;
                        }
                        type_substitutions.emplace(lookup_key, *inferred);
                    }
                }
            }
        }

        // Resolve arguments with substituted parameter types (for lambda type inference)
        array<ChiType *> arg_types;
        for (size_t i = 0; i < n_args; i++) {
            auto arg = args->at(i);
            auto param_type = fn->get_param_at(i);

            // If we have type args (explicit or inferred from return type), substitute
            // placeholders in parameter types for proper lambda parameter inference
            if (has_explicit_type_args || has_return_type_inference) {
                param_type = type_placeholders_sub_map(param_type, &type_substitutions);
            }

            auto arg_scope = scope.set_value_type(param_type).set_move_outlet(nullptr);
            auto arg_type = resolve(arg, arg_scope);
            arg_types.add(arg_type);
        }

        // If no explicit type args and no return type inference, try argument-based inference
        if (!has_explicit_type_args && !has_return_type_inference) {
            if (node->type != ast::NodeType::FnCallExpr) {
                error(node, "Generic function call requires explicit type parameters");
                return fn->return_type;
            }

            map<ChiType *, ChiType *> inferred_types;
            if (!infer_type_arguments(fn, &arg_types, &inferred_types)) {
                error(node, "Failed to infer type parameters for generic function call");
                return fn->return_type;
            }

            // Convert inferred types to type_args array and build substitution map
            for (size_t i = 0; i < fn->type_params.len; i++) {
                auto type_param = fn->type_params[i];
                auto inferred = inferred_types.get(type_param);
                if (!inferred) {
                    error(node, "Could not infer type parameter");
                    return fn->return_type;
                }
                type_args.add(*inferred);

                auto lookup_key = type_param;
                if (type_param->kind == TypeKind::TypeSymbol) {
                    lookup_key = type_param->data.type_symbol.giving_type;
                }
                type_substitutions.emplace(lookup_key, *inferred);
            }
        }

        // Check that type arguments satisfy trait bounds
        for (size_t i = 0; i < fn->type_params.len; i++) {
            auto type_param = fn->type_params[i];
            auto lookup_key = type_param;
            if (type_param->kind == TypeKind::TypeSymbol) {
                lookup_key = type_param->data.type_symbol.giving_type;
            }

            if (lookup_key->kind != TypeKind::Placeholder)
                continue;
            auto type_arg = type_args[i];

            // In safe mode, reject borrowing types (references, structs with ref fields)
            // as generic type arguments — unless T has a lifetime bound (T: 'a)
            if (type_arg && is_borrowing_type(type_arg) &&
                has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_SAFE) &&
                !lookup_key->data.placeholder.lifetime_bound) {
                error(node, "cannot use borrowing type '{}' as type argument for '{}'",
                      format_type_display(type_arg), lookup_key->name.value_or("T"));
                return fn->return_type;
            }

            for (auto trait : lookup_key->data.placeholder.traits) {
                if (!check_trait_bound(type_arg, trait)) {
                    error(node, "Type '{}' does not satisfy trait bound '{}'",
                          format_type_display(type_arg), format_type_display(trait));
                    return fn->return_type;
                }
            }
        }

        // Create or get the specialized function, then delegate to the normal path.
        // This ensures outlets, check_assignment, and track_move_sink are handled uniformly.
        auto &fn_call_data = node->data.fn_call_expr;

        // Path A: static method call on a bare generic struct
        if (fn_call_data.fn_ref_expr->type == NodeType::DotExpr) {
            auto &dot = fn_call_data.fn_ref_expr->data.dot_expr;
            auto dot_lhs_type = node_get_type(dot.expr);
            if (dot_lhs_type && dot_lhs_type->kind == TypeKind::TypeSymbol) {
                auto underlying = dot_lhs_type->data.type_symbol.underlying_type;
                if (underlying->kind == TypeKind::Struct && underlying->data.struct_.is_generic()) {
                    auto subtype = get_subtype(underlying, &type_args);
                    auto resolved = resolve_subtype(subtype);
                    auto member_name = dot.field->get_name();
                    auto member = resolved->data.struct_.find_static_member(member_name);
                    if (member) {
                        dot.resolved_decl = member->node;
                        dot.field->node = member->node;
                        auto &spec_fn = member->resolved_type->data.fn;
                        return resolve_fn_call(node, scope, &spec_fn, args, member->node);
                    }
                }
            }
        }

        // Path B: function-level generics — get specialized variant, delegate to normal path
        auto fn_decl_node = fn_call_data.fn_ref_expr->get_decl();
        assert(fn_decl_node && fn_decl_node->type == ast::NodeType::FnDef);
        auto original_fn_type = node_get_type(fn_decl_node);
        assert(original_fn_type && original_fn_type->kind == TypeKind::Fn);

        auto fn_variant = get_fn_variant(original_fn_type, &type_args, fn_decl_node);
        node->data.fn_call_expr.generated_fn = fn_variant;

        auto &spec_fn = fn_variant->resolved_type->data.fn;
        return resolve_fn_call(node, scope, &spec_fn, args, fn_variant);
    }

    // Normal path — shared by regular calls and delegated-from-generic calls
    {
        for (size_t i = 0; i < n_args; i++) {
            auto param_type = fn->get_param_at(i);
            auto arg = args->at(i);
            // Clear move_outlet for function args - they're passed by value,
            // not written directly to any outer destination
            auto arg_scope = scope.set_value_type(param_type).set_move_outlet(nullptr);
            auto arg_type = resolve(arg, arg_scope);

            // For C variadic functions, param_type is nullptr for variadic args (any type allowed)
            if (param_type) {
                check_assignment(arg, arg_type, param_type);
            }

            // Move tracking for function arguments
            track_move_sink(scope.parent_fn_node, arg, arg_type, node, param_type);

            ensure_temp_owner(arg, arg_type, scope);

            // Mark rvalue/moved args — caller transfers ownership to callee
            // via bitwise pass, so callee skips copy_from and caller
            // skips destroying the temp.
            bool is_explicit_move = arg->type == NodeType::UnaryOpExpr &&
                                    arg->data.unary_op_expr.op_type == TokenType::KW_MOVE;
            if (param_type && !param_type->is_reference() &&
                (!is_addressable(arg) || is_explicit_move) && should_destroy(arg, arg_type)) {
                arg->escape.moved = true;
                // Sink the temp outlet so cleanup doesn't destroy it
                if (arg->resolved_outlet && scope.parent_fn_node) {
                    scope.parent_fn_node->data.fn_def.add_sink_edge(arg->resolved_outlet, node);
                }
            }

            // DisallowCopy: error if passing a named value by value to a non-ref param
            if (param_type && !param_type->is_reference() && is_addressable(arg) &&
                !arg->escape.moved && is_non_copyable(arg_type)) {
                error(arg, errors::TYPE_NOT_COPYABLE, format_type_display(arg_type));
            }
        }
    }

    scope.value_type = nullptr;
    return fn->return_type;
}

ChiType *Resolver::create_pointer_type(ChiType *elem, TypeKind kind) {
    auto pt = create_type(kind);
    pt->data.pointer.elem = elem;
    pt->is_placeholder = elem->is_placeholder;
    return pt;
}

ChiType *Resolver::get_pointer_type(ChiType *elem, TypeKind kind) {
    auto &m = m_ctx->pointer_of[(int)kind];
    if (auto cached = m.get(elem)) {
        return *cached;
    }
    auto pt = create_pointer_type(elem, kind);
    m[elem] = pt;
    pt->is_placeholder = elem->is_placeholder;
    return pt;
}

ChiType *Resolver::get_array_type(ChiType *elem) {
    if (auto cached = m_ctx->array_of.get(elem)) {
        return *cached;
    }

    auto type = create_type(TypeKind::Array);
    type->data.array.elem = elem;
    m_ctx->array_of[elem] = type;
    type->data.array.internal = nullptr;
    type->is_placeholder = elem->is_placeholder;
    // Use to_string for element type since elem->global_id may be empty for anonymous types like
    // lambdas
    auto elem_str = elem->global_id.empty() ? format_type_id(elem) : elem->global_id;
    type->global_id = fmt::format("runtime.Array<{}>", elem_str);
    return type;
}

ChiType *Resolver::get_span_type(ChiType *elem, bool is_mut) {
    auto &cache = is_mut ? m_ctx->mut_span_of : m_ctx->span_of;
    if (auto cached = cache.get(elem))
        return *cached;
    auto type = create_type(TypeKind::Span);
    type->data.span.elem = elem;
    type->data.span.is_mut = is_mut;
    cache[elem] = type;
    type->is_placeholder = elem->is_placeholder;
    auto elem_str = elem->global_id.empty() ? format_type_id(elem) : elem->global_id;
    type->global_id = fmt::format("runtime.__CxSpan<{}>", elem_str);
    return type;
}

ChiType *Resolver::get_fixed_array_type(ChiType *elem, uint32_t size) {
    auto elem_str = elem->global_id.empty() ? format_type_id(elem) : elem->global_id;
    auto key = fmt::format("[{}]{}", size, elem_str);
    if (auto cached = m_ctx->fixed_array_of.get(key)) {
        return *cached;
    }
    auto type = create_type(TypeKind::FixedArray);
    type->data.fixed_array.elem = elem;
    type->data.fixed_array.size = size;
    type->global_id = key;
    type->is_placeholder = elem->is_placeholder;
    m_ctx->fixed_array_of[key] = type;
    return type;
}

ChiType *Resolver::get_promise_type(ChiType *value) {
    if (auto cached = m_ctx->promise_of.get(value)) {
        return *cached;
    }

    // Use the Chi-native Promise<T> struct from runtime.xs
    auto promise = m_ctx->rt_promise_type;
    assert(promise && "Promise struct not found in runtime");

    TypeList args;
    args.add(value);
    auto type = get_subtype(promise, &args);
    m_ctx->promise_of[value] = type;
    return type;
}

bool Resolver::is_promise_type(ChiType *type) {
    if (!m_ctx->rt_promise_type) {
        return false;
    }
    if (type->kind == TypeKind::Subtype) {
        return type->data.subtype.generic == m_ctx->rt_promise_type;
    }
    return false;
}

ChiType *Resolver::get_promise_value_type(ChiType *type) {
    assert(is_promise_type(type));
    return type->data.subtype.args[0];
}

ast::Node *Resolver::get_dummy_var(const string &name, ast::Node *expr) {
    auto node = create_node(NodeType::VarDecl);
    node->name = name;
    node->data.var_decl.is_generated = true;
    node->data.var_decl.expr = expr;
    return node;
}

ast::Node *Resolver::ensure_temp_owner(ast::Node *expr, ChiType *expr_type, ResolveScope &scope) {
    // Whitelist: only node types that can produce a non-addressable temporary value
    switch (expr->type) {
    case NodeType::FnCallExpr:
    case NodeType::ConstructExpr:
    case NodeType::IfExpr:
    case NodeType::SwitchExpr:
    case NodeType::Block:
        break;
    default:
        return nullptr;
    }
    if (is_addressable(expr))
        return nullptr;
    if (expr->resolved_outlet) // already has an outlet (e.g. move_outlet set earlier)
        return nullptr;
    if (expr_type && expr_type->kind == TypeKind::MoveRef) // &move transfers ownership — no temp
        return nullptr;
    if (!should_destroy(expr, expr_type))
        return nullptr;
    if (!scope.block || !scope.parent_fn_node)
        return nullptr;

    auto temp = get_dummy_var("__tmp"); // no expr — just an alloca, filled at call site
    temp->resolved_type = expr_type;
    temp->token = expr->token;
    temp->module = expr->module;
    temp->decl_order = scope.parent_fn_def()->next_decl_order++;

    expr->resolved_outlet = temp;
    scope.block->stmt_temp_vars.add(temp);
    scope.block->cleanup_vars.add(temp);
    scope.parent_fn_def()->has_cleanup = true;
    return temp;
}

static string build_narrowing_path(ast::Node *expr) {
    if (expr->type == NodeType::Identifier) {
        return expr->name;
    }
    if (expr->type == NodeType::DotExpr) {
        auto &data = expr->data.dot_expr;
        auto base = build_narrowing_path(data.expr);
        if (base.empty())
            return "";
        return base + "." + string(data.field->str);
    }
    return "";
}

void Resolver::resolve_destructure_fields(ast::Node *parent, array<ast::Node *> &fields,
                                          ChiType *source_type, ResolveScope &scope,
                                          array<ast::Node *> &generated_vars) {
    auto struct_type = resolve_struct_type(source_type);
    if (!struct_type) {
        error(parent, "cannot destructure non-struct type '{}'", format_type_display(source_type));
        return;
    }

    auto kind = parent->data.destructure_decl.kind;

    for (auto field_node : fields) {
        auto &field_data = field_node->data.destructure_field;
        auto field_name = string(field_data.field_name->str);

        // Look up the field in the struct
        auto member = get_struct_member_access(field_node, source_type, field_name, false, false);
        if (!member)
            continue;
        field_data.resolved_field = member;

        if (field_data.nested) {
            // Nested destructuring: recurse
            auto &nested = field_data.nested->data.destructure_decl;
            resolve_destructure_fields(field_data.nested, nested.fields, member->resolved_type,
                                       scope, nested.generated_vars);
        } else {
            // Create a VarDecl for this binding
            auto binding_name = string(field_data.binding_name->str);
            auto var = get_dummy_var(binding_name);
            var->module = parent->module;
            var->parent_fn = scope.parent_fn_node;
            auto field_type = member->resolved_type;
            var->resolved_type = field_data.sigil == ast::SigilKind::MutRef
                                     ? get_pointer_type(field_type, TypeKind::MutRef)
                                 : field_data.sigil == ast::SigilKind::Reference
                                     ? get_pointer_type(field_type, TypeKind::Reference)
                                     : field_type;
            var->data.var_decl.kind = kind;
            var->data.var_decl.initialized_at = parent;
            var->token = field_data.binding_name;
            var->name = binding_name;

            if (scope.parent_fn_node) {
                var->decl_order = scope.parent_fn_def()->next_decl_order++;
            }

            if (scope.block && scope.block->scope) {
                scope.block->scope->put(binding_name, var);
            }

            if (scope.parent_fn_node && scope.block && should_destroy(var, field_type) &&
                !var->escape.is_capture()) {
                scope.block->cleanup_vars.add(var);
                scope.parent_fn_def()->has_cleanup = true;
            }

            generated_vars.add(var);
        }
    }
}

void Resolver::resolve_array_destructure(ast::Node *parent, array<ast::Node *> &fields,
                                         ChiType *source_type, ResolveScope &scope,
                                         array<ast::Node *> &generated_vars) {
    // Same as IndexExpr resolver: get the internal struct, look up index_mut
    auto struct_type = resolve_struct_type(source_type);
    if (!struct_type) {
        error(parent, "cannot destructure non-indexable type '{}'",
              format_type_display(source_type));
        return;
    }

    auto has_index = has_interface_impl(struct_type, "std.ops.IndexMut");
    if (!has_index) {
        error(parent, "cannot destructure non-indexable type '{}'",
              format_type_display(source_type));
        return;
    }

    auto method_p = struct_type->member_table.get("index_mut");
    assert(method_p);
    auto method = *method_p;
    parent->data.destructure_decl.resolved_index_method = method;

    // Element type from index_mut return type (returns &T)
    auto elem_type = method->resolved_type->data.fn.return_type->get_elem();
    auto kind = parent->data.destructure_decl.kind;

    for (auto field_node : fields) {
        auto &field_data = field_node->data.destructure_field;
        auto binding_name = string(field_data.binding_name->str);

        auto var = get_dummy_var(binding_name);
        var->module = parent->module;
        var->parent_fn = scope.parent_fn_node;
        auto var_type = field_data.sigil == ast::SigilKind::MutRef
                            ? get_pointer_type(elem_type, TypeKind::MutRef)
                        : field_data.sigil == ast::SigilKind::Reference
                            ? get_pointer_type(elem_type, TypeKind::Reference)
                            : elem_type;
        var->resolved_type = var_type;
        var->data.var_decl.kind = kind;
        var->data.var_decl.initialized_at = parent;
        var->token = field_data.binding_name;
        var->name = binding_name;

        if (scope.parent_fn_node) {
            var->decl_order = scope.parent_fn_def()->next_decl_order++;
        }

        if (scope.block && scope.block->scope) {
            scope.block->scope->put(binding_name, var);
        }

        if (scope.parent_fn_node && scope.block && should_destroy(var, var_type) &&
            !var->escape.is_capture()) {
            scope.block->cleanup_vars.add(var);
            scope.parent_fn_def()->has_cleanup = true;
        }

        generated_vars.add(var);
    }
}

ast::Node *Resolver::create_narrowed_var(ast::Node *expr_node, ast::Node *parent_stmt,
                                         ResolveScope &scope, ChiType *narrowed_type) {
    auto type = node_get_type(expr_node);
    string name;
    ast::VarKind kind;
    if (expr_node->type == NodeType::DotExpr) {
        name = build_narrowing_path(expr_node);
        kind = ast::VarKind::Immutable;
    } else {
        name = expr_node->token->get_name();
        if (name.empty() && expr_node->data.identifier.kind == ast::IdentifierKind::This)
            name = "this";
        auto decl = expr_node->data.identifier.decl;
        kind = (decl && decl->type == NodeType::VarDecl) ? decl->data.var_decl.kind
                                                         : ast::VarKind::Immutable;
    }
    ast::Node *var_expr = expr_node;
    if (!narrowed_type) {
        narrowed_type =
            type->kind == TypeKind::Optional ? type->get_elem() : type->data.result.value;
        auto expr = create_node(ast::NodeType::UnaryOpExpr);
        expr->token = expr_node->token;
        expr->data.unary_op_expr.is_suffix = true;
        expr->data.unary_op_expr.op_type = TokenType::LNOT;
        expr->data.unary_op_expr.op1 = expr_node;
        expr->resolved_type = narrowed_type;
        var_expr = expr;
    }
    auto var = get_dummy_var(name, var_expr);
    var->token = expr_node->token;
    var->parent_fn = scope.parent_fn_node;
    var->resolved_type = narrowed_type;
    var->data.var_decl.initialized_at = parent_stmt;
    var->data.var_decl.narrowed_from = expr_node;
    var->data.var_decl.kind = kind;
    return var;
}

bool Resolver::always_terminates(ast::Node *node) {
    switch (node->type) {
    case NodeType::ReturnStmt:
    case NodeType::ThrowStmt:
    case NodeType::BranchStmt:
        return true;
    case NodeType::FnCallExpr: {
        auto type = node_get_type(node);
        return type && type->kind == TypeKind::Never;
    }
    case NodeType::Block: {
        auto &stmts = node->data.block.statements;
        return stmts.len > 0 && always_terminates(stmts[stmts.len - 1]);
    }
    case NodeType::IfExpr: {
        auto &d = node->data.if_expr;
        return d.else_node && always_terminates(d.then_block) && always_terminates(d.else_node);
    }
    default:
        return false;
    }
}

void Resolver::collect_narrowables(ast::Node *expr, bool when_truthy, array<ast::Node *> &out) {
    using namespace ast;

    if (expr->type == NodeType::ParenExpr) {
        collect_narrowables(expr->data.child_expr, when_truthy, out);
        return;
    }

    if (expr->type == NodeType::Identifier) {
        if (when_truthy) {
            auto type = node_get_type(expr);
            if (type && (type->kind == TypeKind::Optional || type->kind == TypeKind::Result)) {
                out.add(expr);
            }
        }
        return;
    }

    if (expr->type == NodeType::DotExpr) {
        if (when_truthy) {
            auto type = node_get_type(expr);
            if (type && (type->kind == TypeKind::Optional || type->kind == TypeKind::Result)) {
                out.add(expr);
            }
        }
        return;
    }

    if (expr->type == NodeType::UnaryOpExpr) {
        auto &data = expr->data.unary_op_expr;
        if (data.op_type == TokenType::LNOT && !data.is_suffix) {
            collect_narrowables(data.op1, !when_truthy, out);
        }
        return;
    }

    if (expr->type == NodeType::BinOpExpr) {
        auto &data = expr->data.bin_op_expr;
        if (data.op_type == TokenType::LAND && when_truthy) {
            collect_narrowables(data.op1, true, out);
            collect_narrowables(data.op2, true, out);
        } else if (data.op_type == TokenType::LOR && !when_truthy) {
            collect_narrowables(data.op1, false, out);
            collect_narrowables(data.op2, false, out);
        }
    }
}

ChiType *Resolver::get_fn_type(ChiType *ret, TypeList *params, bool is_variadic, ChiType *container,
                               bool is_extern, TypeList *type_params) {
    ChiTypeFn fn;
    fn.return_type = ret;
    fn.is_variadic = is_variadic;
    fn.params = *params;
    fn.is_extern = is_extern;
    if (type_params) {
        fn.type_params = *type_params; // Include type parameters if provided
    }
    if (container) {
        fn.container_ref = get_pointer_type(container, TypeKind::Reference);
    }

    auto key = format_type_data(TypeKind::Fn, (ChiType::Data *)&fn);

    // Don't cache function types with placeholder return types - they may be
    // modified during lambda body resolution for type inference
    bool should_cache = !ret->is_placeholder;
    if (should_cache) {
        if (auto cached = m_ctx->composite_types.get(key)) {
            return *cached;
        }
    }

    auto type = create_type(TypeKind::Fn);
    type->data.fn = fn;

    if (should_cache) {
        m_ctx->composite_types[key] = type;
    }
    for (auto param : fn.params) {
        if (param && param->is_placeholder) {
            type->is_placeholder = true;
            break;
        }
    }
    if (type_params && type_params->len) {
        for (auto param : *type_params) {
            if (param->is_placeholder) {
                type->is_placeholder = true;
                break;
            }
        }
    }
    type->is_placeholder = type->is_placeholder || ret->is_placeholder;
    return type;
}

// Helper to recursively finalize a single FnLambda type and its nested lambdas
bool Resolver::finalize_lambda_type_recursive(ChiType *type) {
    if (!type)
        return false;

    bool changed = false;

    if (type->kind == TypeKind::FnLambda) {
        // Finalize this lambda's internal if needed
        // __CxLambda is not generic, so use it directly
        if (type->is_placeholder && !type->data.fn_lambda.internal) {
            auto rt_lambda = m_ctx->rt_lambda_type;
            if (rt_lambda &&
                rt_lambda->data.struct_.resolve_status >= ResolveStatus::MemberTypesKnown) {
                type->data.fn_lambda.internal = to_value_type(rt_lambda);
                type->is_placeholder = false;
                changed = true;
            }
        }

        // Recurse into the lambda's fn type to finalize nested lambdas
        if (type->data.fn_lambda.fn) {
            changed = finalize_lambda_type_recursive(type->data.fn_lambda.fn) || changed;
        }
    } else if (type->kind == TypeKind::Fn) {
        // Recurse into function params to find nested lambdas
        for (auto param : type->data.fn.params) {
            changed = finalize_lambda_type_recursive(param) || changed;
        }
        // Also check return type
        if (type->data.fn.return_type) {
            changed = finalize_lambda_type_recursive(type->data.fn.return_type) || changed;
        }
    }

    return changed;
}

void Resolver::finalize_placeholder_lambda_params(ChiType *fn_type) {
    if (!fn_type || fn_type->kind != TypeKind::Fn) {
        return;
    }

    bool had_placeholder = false;

    // Walk through all function parameters and finalize placeholder lambdas recursively
    for (size_t i = 0; i < fn_type->data.fn.params.len; i++) {
        auto param = fn_type->data.fn.params[i];
        if (finalize_lambda_type_recursive(param)) {
            had_placeholder = true;
        }
    }

    // Also check return type for lambdas
    if (fn_type->data.fn.return_type) {
        if (finalize_lambda_type_recursive(fn_type->data.fn.return_type)) {
            had_placeholder = true;
        }
    }

    // If we finalized any placeholders, recompute the function's placeholder flag
    // Don't just clear it - the function might still be placeholder due to type params or return
    // type
    if (had_placeholder) {
        fn_type->is_placeholder = false;
        // Check if any function type parameter is still a placeholder
        for (auto tp : fn_type->data.fn.type_params) {
            if (tp->is_placeholder) {
                fn_type->is_placeholder = true;
                break;
            }
        }
        // Check if return type is still a placeholder
        if (!fn_type->is_placeholder && fn_type->data.fn.return_type &&
            fn_type->data.fn.return_type->is_placeholder) {
            fn_type->is_placeholder = true;
        }
        // Check if any param is still a placeholder
        if (!fn_type->is_placeholder) {
            for (auto p : fn_type->data.fn.params) {
                if (p->is_placeholder) {
                    fn_type->is_placeholder = true;
                    break;
                }
            }
        }
    }
}

ChiType *Resolver::get_lambda_for_fn(ChiType *fn_type) {
    // Don't cache lambda types - keep them all unique to avoid conflicts
    // Each lambda instance should have its own type even with same signature

    auto lambda = create_type(TypeKind::FnLambda);
    lambda->data.fn_lambda.fn = fn_type;
    lambda->is_placeholder = fn_type->is_placeholder;
    auto &fn_data = fn_type->data.fn;

    // Create or reuse bind_struct
    ChiType *bstruct;

    if (fn_data.container_ref) {
        // Method lambda - create unique bind struct
        bstruct = create_type(TypeKind::Struct);
        bstruct->data.struct_.kind = ContainerKind::Struct;
        bstruct->name = "MethodLambdaBind";

        auto bound_type = create_type(TypeKind::Fn);
        lambda->data.fn_lambda.bound_fn = bound_type;
        auto &bound_fn = bound_type->data.fn;

        bound_fn.params.add(get_system_types()->void_ref);
        for (auto param : fn_data.params) {
            bound_fn.params.add(param);
        }
        bound_fn.return_type = fn_data.return_type;
        bound_fn.is_variadic = fn_data.is_variadic;
        bound_fn.container_ref = fn_data.container_ref;
    } else {
        // Reuse cached empty bind struct for all lambdas with no captures
        if (!m_ctx->rt_empty_bind_type) {
            m_ctx->rt_empty_bind_type = create_type(TypeKind::Struct);
            m_ctx->rt_empty_bind_type->data.struct_.kind = ContainerKind::Struct;
            m_ctx->rt_empty_bind_type->name = "EmptyLambdaBind";
            m_ctx->rt_empty_bind_type->global_id = "EmptyLambdaBind";
            m_ctx->rt_empty_bind_type->data.struct_.resolve_status =
                ResolveStatus::MemberTypesKnown;
        }
        bstruct = m_ctx->rt_empty_bind_type;

        // Always create a bound function with binding struct as first parameter
        // This ensures consistent lambda calling convention
        TypeList bound_params;
        bound_params.add(get_system_types()->void_ref);

        // Add all original function parameters
        for (auto param : fn_data.params) {
            bound_params.add(param);
        }

        // Create the bound function type with all parameters
        auto bound_fn_type = create_type(TypeKind::Fn);
        bound_fn_type->data.fn.return_type = fn_data.return_type;
        bound_fn_type->data.fn.is_variadic = fn_data.is_variadic;
        bound_fn_type->data.fn.params = bound_params;
        bound_fn_type->is_placeholder = fn_type->is_placeholder;
        bound_fn_type->global_id = fmt::format("__lambda_bound_fn_{}", bound_fn_type->id);

        lambda->data.fn_lambda.bound_fn = bound_fn_type;
    }

    lambda->data.fn_lambda.bind_struct = bstruct;

    // Use __CxLambda directly (no longer generic)
    auto rt_lambda = m_ctx->rt_lambda_type;
    assert(rt_lambda && "__CxLambda type not found in runtime");

    // Check if __CxLambda is fully resolved
    if (rt_lambda->data.struct_.resolve_status < ResolveStatus::MemberTypesKnown) {
        // Not resolved yet - defer instantiation by returning placeholder
        // This happens when resolving runtime.xs itself, before __CxLambda is fully resolved
        lambda->data.fn_lambda.internal = nullptr;
        lambda->is_placeholder = true;
        return lambda;
    }

    // Use __CxLambda directly
    lambda->data.fn_lambda.internal = to_value_type(rt_lambda);

    return lambda;
}

ChiType *Resolver::get_result_type(ChiType *value, ChiType *err) {
    if (value->kind == TypeKind::Void) {
        value = m_ctx->rt_unit_type;
    }

    auto key = fmt::format("Result<{},{}>", format_type_id(value), format_type_id(err));
    if (auto cached = m_ctx->composite_types.get(key)) {
        return *cached;
    }

    auto result_type = create_type(TypeKind::Result);
    auto &data = result_type->data.result;
    data.value = value;
    data.error = err;
    result_type->global_id = key;
    m_ctx->composite_types[key] = result_type;

    // create internal struct for accessing the fields
    data.internal = create_type(TypeKind::Struct);
    auto &struct_ = data.internal->data.struct_;
    auto dummy_node = create_node(NodeType::StructDecl);
    dummy_node->name = key;
    dummy_node->resolved_type = result_type;
    struct_.node = dummy_node;
    struct_.kind = ContainerKind::Struct;
    struct_.node = nullptr;

    auto err_optional = get_pointer_type(err, TypeKind::Optional);
    struct_.add_member(get_allocator(), "error", dummy_node, err_optional);
    struct_.add_member(get_allocator(), "value", dummy_node, value);
    return result_type;
}

ChiType *Resolver::create_int_type(int bit_count, bool is_unsigned) {
    auto type = create_type(TypeKind::Int);
    type->data.int_.bit_count = bit_count;
    type->data.int_.is_unsigned = is_unsigned;
    return type;
}

ChiType *Resolver::create_float_type(int bit_count) {
    auto type = create_type(TypeKind::Float);
    type->data.float_.bit_count = bit_count;
    return type;
}

optional<ConstantValue> Resolver::resolve_constant_value(ast::Node *node) {
    switch (node->type) {
    case NodeType::LiteralExpr: {
        auto token = node->token;
        switch (token->type) {
        case TokenType::CHAR:
        case TokenType::INT: {
            return {token->val.i};
        }
        case TokenType::FLOAT: {
            return {token->val.d};
        }
        case TokenType::STRING: {
            return {token->str};
        }
        case TokenType::C_STRING: {
            return {token->str};
        }
        case TokenType::BOOL: {
            return {(int64_t)token->val.b};
        }
        case TokenType::NULLP: {
            return {(int64_t)0};
        }
        case TokenType::KW_UNDEFINED:
        case TokenType::KW_ZEROINIT:
            return {(int64_t)0};
        default:
            break;
        }
        break;
    }

#define _BIN_OP_CAST(op1, op, op2, type) get<type>(op1) op get<type>(op2)
#define _BIN_OP_INT(op1, op, op2) _BIN_OP_CAST(op1, op, op2, const_int_t)

    case NodeType::BinOpExpr: {
        auto &data = node->data.bin_op_expr;
        auto a_val = resolve_constant_value(data.op1);
        auto b_val = resolve_constant_value(data.op2);
        if (!a_val.has_value() || !b_val.has_value()) {
            return std::nullopt;
        }
        auto a = *a_val;
        auto b = *b_val;
        switch (data.op_type) {
        case TokenType::ADD:
            return {_BIN_OP_INT(a, +, b)};
        case TokenType::SUB:
            return {_BIN_OP_INT(a, -, b)};
        case TokenType::LSHIFT:
            return {_BIN_OP_INT(a, <<, b)};
        default:
            break;
        }
    }

    case NodeType::EnumVariant: {
        auto &data = node->data.enum_variant;
        return {data.resolved_value};
    }

    case NodeType::DotExpr: {
        auto &data = node->data.dot_expr;
        if (data.resolved_decl) {
            return resolve_constant_value(data.resolved_decl);
        }
        return std::nullopt;
    }

    case NodeType::ParenExpr: {
        auto child = node->data.child_expr;
        return resolve_constant_value(child);
    }

    case NodeType::UnaryOpExpr: {
        auto &data = node->data.unary_op_expr;
        switch (data.op_type) {
        case TokenType::SUB: {
            auto v = resolve_constant_value(data.op1);
            if (!v.has_value()) {
                return std::nullopt;
            }
            return -get<const_int_t>(*v);
        }
        default:
            break;
        }
    }

    case NodeType::Identifier: {
        auto &data = node->data.identifier;
        if (data.decl) {
            return resolve_constant_value(data.decl);
        }
    }
    case NodeType::VarDecl: {
        auto &data = node->data.var_decl;
        if (data.kind == ast::VarKind::Constant && data.resolved_value.has_value()) {
            return data.resolved_value;
        }
    }
    default:
        break;
    }
    return std::nullopt;
}

bool Resolver::is_same_type(ChiType *a, ChiType *b) {
    a = to_value_type(a);
    b = to_value_type(b);

    // If either type is Infer with inferred_type, use the inferred type
    if (a->kind == TypeKind::Infer && a->data.infer.inferred_type) {
        a = a->data.infer.inferred_type;
    }
    if (b->kind == TypeKind::Infer && b->data.infer.inferred_type) {
        b = b->data.infer.inferred_type;
    }

    // Normalize string types: TypeKind::String -> __CxString struct
    if (a->kind == TypeKind::String && m_ctx->rt_string_type) {
        a = m_ctx->rt_string_type;
    }
    if (b->kind == TypeKind::String && m_ctx->rt_string_type) {
        b = m_ctx->rt_string_type;
    }

    // First check pointer equality
    if (a == b) {
        return true;
    }

    // Special case: null_ptr (*void with is_null flag) is the same as *void
    if (a->kind == TypeKind::Pointer && b->kind == TypeKind::Pointer) {
        auto a_elem = a->get_elem();
        auto b_elem = b->get_elem();
        if (a_elem && b_elem && a_elem->kind == TypeKind::Void && b_elem->kind == TypeKind::Void) {
            // Both are void pointers, consider them the same regardless of is_null flag
            return true;
        }
    }

    // If both types have global_id, compare them
    if (!a->global_id.empty() && !b->global_id.empty()) {
        return a->global_id == b->global_id;
    }

    // Special case for built-in types like string
    if (a->kind == b->kind) {
        if (a->kind == TypeKind::String) {
            return true;
        }
        // Structural comparison for FnLambda types
        if (a->kind == TypeKind::FnLambda) {
            return is_same_type(a->data.fn_lambda.fn, b->data.fn_lambda.fn);
        }
        // Structural comparison for Fn types
        if (a->kind == TypeKind::Fn) {
            auto &a_fn = a->data.fn;
            auto &b_fn = b->data.fn;
            if (a_fn.params.len != b_fn.params.len) {
                return false;
            }
            for (int i = 0; i < a_fn.params.len; i++) {
                if (!is_same_type(a_fn.params[i], b_fn.params[i])) {
                    return false;
                }
            }
            return is_same_type(a_fn.return_type, b_fn.return_type);
        }
        // Structural comparison for pointer-like types
        if (a->kind == TypeKind::Pointer || a->kind == TypeKind::Reference ||
            a->kind == TypeKind::MutRef || a->kind == TypeKind::MoveRef ||
            a->kind == TypeKind::Optional) {
            return is_same_type(a->get_elem(), b->get_elem());
        }
        // Structural comparison for fixed arrays
        if (a->kind == TypeKind::FixedArray) {
            return a->data.fixed_array.size == b->data.fixed_array.size &&
                   is_same_type(a->data.fixed_array.elem, b->data.fixed_array.elem);
        }
    }

    return false;
}

ChiType *Resolver::get_enum_subtype(ChiType *generic, TypeList *type_args) {
    assert(generic->kind == TypeKind::Enum);
    auto &gen = generic->data.enum_;
    for (auto subtype : gen.subtypes) {
        assert(subtype->kind == TypeKind::Subtype);
        auto &subtype_data = subtype->data.subtype;
        if (subtype_data.args.len != type_args->len) {
            continue;
        }
        bool matches = true;
        for (size_t i = 0; i < type_args->len; i++) {
            if (!is_same_type(type_args->at(i), subtype_data.args[i])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return subtype;
        }
    }
    auto sub = create_type(TypeKind::Subtype);
    sub->data.subtype.generic = generic;
    sub->data.subtype.root_node = gen.node;
    sub->global_id = fmt::format("{}<{}>", generic->global_id, format_type_list(type_args));
    for (auto arg : *type_args) {
        sub->data.subtype.args.add(arg);
        if (arg->is_placeholder) {
            sub->is_placeholder = true;
        }
    }
    gen.subtypes.add(sub);

    // Build a concrete enum type using index-based substitution (same as struct subtypes)
    if (!sub->is_placeholder) {
        auto &subtype_data = sub->data.subtype;

        auto concrete_enum = create_type(TypeKind::Enum);
        concrete_enum->name = sub->global_id;
        concrete_enum->display_name = sub->global_id;
        concrete_enum->global_id = sub->global_id;
        concrete_enum->data.enum_.node = gen.node;
        concrete_enum->data.enum_.resolved_generic = generic;
        concrete_enum->data.enum_.discriminator = gen.discriminator;
        concrete_enum->data.enum_.enum_header_struct = gen.enum_header_struct;

        // Substitute base_value_type using index-based substitution
        auto concrete_bvt = type_placeholders_sub(gen.base_value_type, &subtype_data);
        concrete_bvt->data.enum_value.enum_type = concrete_enum;
        concrete_enum->data.enum_.base_value_type = concrete_bvt;

        // Substitute each variant's type — each variant is just a struct
        for (auto variant : gen.variants) {
            auto concrete_vt = type_placeholders_sub(variant->resolved_type, &subtype_data);
            concrete_vt->data.enum_value.enum_type = concrete_enum;

            // Create variant entry without mutating the shared AST node
            auto member = get_allocator()->create_enum_member();
            member->name = variant->name;
            member->node = variant->node;
            member->resolved_type = concrete_vt;
            member->enum_ = &concrete_enum->data.enum_;
            concrete_enum->data.enum_.variants.add(member);
            member->index = concrete_enum->data.enum_.variants.len;
            concrete_enum->data.enum_.variant_table[variant->name] = member;
            concrete_vt->data.enum_value.member = member;
        }

        // Create specialized base_struct with updated method container_refs
        if (gen.base_struct) {
            auto src = gen.base_struct;
            auto dst = create_type(TypeKind::Struct);
            src->clone(dst);
            dst->data.struct_.members = {};
            dst->data.struct_.fields = {};
            dst->data.struct_.member_table = {};

            for (auto member : src->data.struct_.members) {
                if (member->is_method()) {
                    // Clone method type with updated container_ref
                    auto type = m_ctx->allocator->create_type(member->resolved_type->kind);
                    member->resolved_type->clone(type);
                    type->data.fn.container_ref =
                        get_pointer_type(concrete_bvt, TypeKind::Reference);

                    // Clone the FnDef AST node for unique identity
                    auto node = get_allocator()->create_node(ast::NodeType::FnDef);
                    member->node->clone(node);
                    node->name = member->node->name;
                    node->token = member->node->token;
                    node->resolved_type = type;
                    node->root_node = member->node->get_root_node();
                    node->data.fn_def.is_generated = true;

                    auto new_proto = get_allocator()->create_node(ast::NodeType::FnProto);
                    auto orig_proto = member->node->data.fn_def.fn_proto;
                    orig_proto->clone(new_proto);
                    new_proto->resolved_type = type;
                    node->data.fn_def.fn_proto = new_proto;
                    node->data.fn_def.fn_proto->data.fn_proto.fn_def_node = node;

                    dst->data.struct_.add_member(get_allocator(), member->get_name(), node, type);
                } else {
                    // Substitute field types for T → concrete
                    auto new_type = type_placeholders_sub(member->resolved_type, &subtype_data);
                    dst->data.struct_.add_member(get_allocator(), member->get_name(), member->node,
                                                 new_type);
                }
            }
            concrete_enum->data.enum_.base_struct = dst;
        }

        subtype_data.final_type = concrete_enum;
    }

    return sub;
}

ChiType *Resolver::get_subtype(ChiType *generic, TypeList *type_args) {
    assert(generic->kind == TypeKind::Struct);
    auto &gen = generic->data.struct_;
    for (auto subtype : gen.subtypes) {
        assert(subtype->kind == TypeKind::Subtype);
        auto &subtype_data = subtype->data.subtype;
        if (subtype_data.args.len != type_args->len) {
            continue;
        }
        bool matches = true;
        for (size_t i = 0; i < type_args->len; i++) {
            auto a = type_args->at(i);
            auto b = subtype_data.args[i];
            if (!is_same_type(a, b)) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return subtype;
        }
    }
    auto sub = create_type(TypeKind::Subtype);
    sub->data.subtype.generic = generic;
    sub->data.subtype.root_node = gen.node;
    sub->global_id = fmt::format("{}<{}>", gen.global_id, format_type_list(type_args));
    for (auto arg : *type_args) {
        sub->data.subtype.args.add(arg);
        if (arg->is_placeholder) {
            sub->is_placeholder = true;
        }
    }
    gen.subtypes.add(sub);
    // Only resolve concrete subtypes - placeholder subtypes shouldn't be resolved
    // until we have concrete type arguments (the base may not have interfaces yet)
    if (gen.resolve_status >= ResolveStatus::MemberTypesKnown && !sub->is_placeholder) {
        resolve_subtype(sub);
    }

    // Record to monomorphization plan
    if (!sub->is_placeholder) {
        map<ChiType *, ChiType *> subs;
        for (size_t i = 0; i < gen.type_params.len && i < type_args->len; i++) {
            // Use to_value_type to unwrap TypeSymbol wrapper - the actual types in
            // struct members contain raw Placeholder types, not TypeSymbol wrappers
            subs[to_value_type(gen.type_params[i])] = type_args->at(i);
        }
        m_ctx->generics.record_struct(sub->global_id, sub->global_id, generic, subs);
    }

    return sub;
}

ast::Node *Resolver::get_fn_variant(ChiType *generic_fn, TypeList *type_args, ast::Node *fn_node) {
    assert(generic_fn->kind == TypeKind::Fn);
    assert(fn_node->type == NodeType::FnDef);
    auto &gen = generic_fn->data.fn;
    for (auto variant : fn_node->data.fn_def.variants) {
        assert(variant->type == NodeType::GeneratedFn);
        auto subtype = variant->data.generated_fn.fn_subtype;
        auto &subtype_data = subtype->data.subtype;
        if (subtype_data.args.len != type_args->len) {
            continue;
        }
        bool matches = true;
        for (size_t i = 0; i < type_args->len; i++) {
            auto a = type_args->at(i);
            auto b = subtype_data.args[i];
            if (!is_same_type(a, b)) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return variant;
        }
    }

    auto sub = create_type(TypeKind::Subtype);
    sub->data.subtype.generic = generic_fn;
    sub->data.subtype.root_node = fn_node->get_root_node();

    auto generated_fn = create_node(NodeType::GeneratedFn);
    generated_fn->data.generated_fn.fn_subtype = sub;
    generated_fn->data.generated_fn.original_fn = fn_node;
    generated_fn->module = fn_node->module;
    generated_fn->root_node = fn_node->get_root_node();
    generated_fn->token = fn_node->token;
    sub->data.subtype.generated_fn = generated_fn;
    auto orig_proto = fn_node->data.fn_def.fn_proto;
    auto new_proto = create_node(NodeType::FnProto);
    new_proto->data.fn_proto.fn_def_node = generated_fn;
    new_proto->data.fn_proto.return_type = orig_proto->data.fn_proto.return_type;
    generated_fn->data.generated_fn.fn_proto = new_proto;

    for (auto param : orig_proto->data.fn_proto.params) {
        auto new_param = create_node(NodeType::ParamDecl);
        param->clone(new_param);
        new_param->name = param->name;
        new_proto->data.fn_proto.params.add(new_param);
    }

    for (auto arg : *type_args) {
        sub->data.subtype.args.add(arg);
        if (arg->is_placeholder) {
            sub->is_placeholder = true;
        }
    }

    fn_node->data.fn_def.variants.add(generated_fn);
    auto resolved_fn_type = resolve_fn_subtype(sub);

    // Resolve generated nodes
    generated_fn->resolved_type = resolved_fn_type;
    for (int i = 0; i < resolved_fn_type->data.fn.params.len; i++) {
        auto proto_param = new_proto->data.fn_proto.params[i];
        proto_param->resolved_type = resolved_fn_type->data.fn.params[i];
    }

    // Record to monomorphization plan
    if (!sub->is_placeholder) {
        map<ChiType *, ChiType *> subs;

        // Include container struct's type parameters if this is a method
        // Use resolved_fn_type which has the specialized container_ref (e.g., &Container<int>)
        auto container_ref = resolved_fn_type->data.fn.container_ref;
        if (container_ref) {
            auto container = container_ref->get_elem();
            // For specialized methods, look up the container's type_env from struct_envs
            if (container->kind == TypeKind::Struct) {
                auto container_id = container->global_id;
                if (auto entry = m_ctx->generics.struct_envs.get(container_id)) {
                    for (auto &kv : entry->subs.data) {
                        subs[kv.first] = kv.second;
                    }
                }
            } else if (container->kind == TypeKind::Subtype) {
                auto &container_data = container->data.subtype;
                auto &container_base = container_data.generic->data.struct_;
                for (size_t i = 0;
                     i < container_base.type_params.len && i < container_data.args.len; i++) {
                    subs[to_value_type(container_base.type_params[i])] = container_data.args[i];
                }
            }
        }

        // Include the function's own type parameters
        for (size_t i = 0; i < gen.type_params.len && i < type_args->len; i++) {
            // Use to_value_type to unwrap TypeSymbol wrapper - the actual types in
            // params/return contain raw Placeholder types, not TypeSymbol wrappers
            subs[to_value_type(gen.type_params[i])] = type_args->at(i);
        }
        auto fn_name = fn_node->name + "<" + format_type_list(type_args) + ">";
        auto variant_id = resolve_global_id(generated_fn);
        m_ctx->generics.record_fn(variant_id, fn_name, fn_node, generic_fn, subs);
    }

    resolve_fn_lifetimes(generated_fn);
    return generated_fn;
}

ChiType *Resolver::resolve_fn_subtype(ChiType *subtype) {
    assert(subtype->kind == TypeKind::Subtype);
    auto &data = subtype->data.subtype;
    if (data.final_type) {
        return data.final_type;
    }

    auto generic_fn = data.generic;
    assert(generic_fn->kind == TypeKind::Fn);
    auto &generic_fn_data = generic_fn->data.fn;

    // Create type substitution map
    map<ChiType *, ChiType *> type_substitutions;
    for (size_t i = 0; i < generic_fn_data.type_params.len; i++) {
        auto type_param = to_value_type(generic_fn_data.type_params[i]);
        auto concrete_type = data.args[i];

        // Use the underlying placeholder type as the key, not the TypeSymbol wrapper
        auto substitution_key = type_param;

        type_substitutions[substitution_key] = concrete_type;
    }

    // Create specialized function type by substituting type parameters
    auto specialized_return_type =
        type_placeholders_sub_map(generic_fn_data.return_type, &type_substitutions);

    array<ChiType *> specialized_params;
    for (auto param_type : generic_fn_data.params) {
        auto specialized_param = type_placeholders_sub_map(param_type, &type_substitutions);
        specialized_params.add(specialized_param);
    }

    auto container_elem =
        generic_fn_data.container_ref ? generic_fn_data.container_ref->get_elem() : nullptr;
    auto specialized_fn_type =
        get_fn_type(specialized_return_type, &specialized_params, generic_fn_data.is_variadic,
                    container_elem, generic_fn_data.is_extern);

    data.final_type = specialized_fn_type;
    return specialized_fn_type;
}

ChiType *Resolver::resolve_subtype(ChiType *subtype) {
    auto &data = subtype->data.subtype;
    if (data.final_type) {
        return data.final_type;
    }

    if (!data.generic)
        return subtype;
    auto &base = data.generic->data.struct_;
    auto sty = create_type(TypeKind::Struct);
    sty->name = format_type_id(subtype);
    sty->global_id = subtype->global_id;

    auto &scpy = sty->data.struct_;
    scpy.kind = base.kind;
    scpy.node = base.node;
    scpy.display_name = sty->name;
    auto base_symbol = resolve_intrinsic_symbol(base.node);

    for (auto member : base.members) {
        if (!member->resolved_type)
            continue;
        if (!check_where_condition(member->where_condition, &data))
            continue;
        // For promoted methods (from struct embedding), verify the method still exists in
        // the concrete embedded type — where conditions on that type's methods may have
        // filtered it out for this specialization (e.g. Vec<string> should not have
        // resize_fill promoted from Array<string> when resize_fill is only in impl where T: Int).
        if (member->is_method() && member->orig_parent && member->parent_member) {
            auto concrete_orig =
                type_placeholders_sub_selective(member->orig_parent, &data, base.node);
            auto concrete_struct = resolve_struct_type(concrete_orig);
            if (concrete_struct && !concrete_struct->find_member(member->get_name()))
                continue;
        }
        auto type = m_ctx->allocator->create_type(member->resolved_type->kind);
        member->resolved_type->clone(type);
        if (member->is_method()) {
            type->data.fn.container_ref = get_pointer_type(sty, TypeKind::Reference);
        }

        // For built-in enum base types, use complete substitution to resolve all placeholders
        // For user-defined types, use selective substitution to preserve method type parameters
        if (base.node && base.node->name == "__CxEnumBase") {
            // Use complete substitution for enum base to resolve all placeholders
            type = type_placeholders_sub(type, &data);
        } else {
            // Use selective substitution to only replace struct type parameters,
            // preserving method type parameters for later inference
            type = type_placeholders_sub_selective(type, &data, base.node);
        }

        auto node = get_allocator()->create_node(member->node->type);
        member->node->clone(node);
        node->name = member->node->name;
        node->token = member->node->token;
        node->resolved_type = type;
        node->root_node = member->node->get_root_node();

        if (member->node->type == NodeType::FnDef) {
            node->data.fn_def.is_generated = true;

            auto new_proto = get_allocator()->create_node(NodeType::FnProto);
            auto orig_proto = member->node->data.fn_def.fn_proto;
            orig_proto->clone(new_proto);
            new_proto->resolved_type = type;
            node->data.fn_def.fn_proto = new_proto;
            node->data.fn_def.fn_proto->data.fn_proto.fn_def_node = node;
        }

        auto new_member = scpy.add_member(get_allocator(), member->get_name(), node, type,
                                          !member->is_promoted());
        if (member->symbol != IntrinsicSymbol::None) {
            scpy.member_intrinsics[member->symbol] = new_member;
            new_member->symbol = member->symbol;
        }
        member->variants[subtype->id] = new_member;
        new_member->root_variant = member->root_variant ? member->root_variant : member;

        // Propagate struct-embedding metadata so codegen generates forwarding proxies
        // for the concrete specialization (e.g., Vec<int> embedding Array<int>)
        if (member->orig_parent) {
            new_member->orig_parent =
                type_placeholders_sub_selective(member->orig_parent, &data, base.node);
            if (member->parent_member) {
                auto concrete_field = member->parent_member->variants.get(subtype->id);
                new_member->parent_member = concrete_field ? *concrete_field : member->parent_member;
                new_member->field_index = member->field_index;
            }
        }
    }

    // Copy static members into the specialized struct
    for (auto member : base.static_members) {
        if (!member->resolved_type)
            continue;
        if (!check_where_condition(member->where_condition, &data))
            continue;
        auto type = m_ctx->allocator->create_type(member->resolved_type->kind);
        member->resolved_type->clone(type);

        // Set container_ref for unique global ID (e.g., "Box<int>.create" not just "create")
        // Mark is_static so codegen doesn't add a 'this' parameter
        if (type->kind == TypeKind::Fn) {
            type->data.fn.container_ref = get_pointer_type(sty, TypeKind::Reference);
            type->data.fn.is_static = true;
        }

        if (base.node && base.node->name == "__CxEnumBase") {
            type = type_placeholders_sub(type, &data);
        } else {
            type = type_placeholders_sub_selective(type, &data, base.node);
        }

        auto node = get_allocator()->create_node(member->node->type);
        member->node->clone(node);
        node->name = member->node->name;
        node->token = member->node->token;
        node->resolved_type = type;
        node->root_node = member->node->get_root_node();

        if (member->node->type == NodeType::FnDef) {
            node->data.fn_def.is_generated = true;
            auto new_proto = get_allocator()->create_node(NodeType::FnProto);
            auto orig_proto = member->node->data.fn_def.fn_proto;
            orig_proto->clone(new_proto);
            new_proto->resolved_type = type;
            node->data.fn_def.fn_proto = new_proto;
            node->data.fn_def.fn_proto->data.fn_proto.fn_def_node = node;
        }

        auto new_member = scpy.add_member(get_allocator(), member->get_name(), node, type,
                                          !member->is_promoted());
        if (member->symbol != IntrinsicSymbol::None) {
            scpy.member_intrinsics[member->symbol] = new_member;
            new_member->symbol = member->symbol;
        }
        member->variants[subtype->id] = new_member;
        new_member->root_variant = member->root_variant ? member->root_variant : member;
    }

    for (auto type_arg : data.args) {
        if (type_arg->is_placeholder) {
            scpy.type_params.add(type_arg);
        }
    }

    data.final_type = sty;
    // Copy qualifying interfaces into the concrete struct, filtering conditional ones whose
    // where condition isn't satisfied. Also populate interface_table so codegen can look up
    // vtables (interface_table is keyed by the concrete interface type, e.g. Index<uint32, int>
    // not the generic Index<uint32, T>).
    for (auto iface_impl : base.interfaces) {
        if (iface_impl->where_condition &&
            !check_where_condition(iface_impl->where_condition, &data))
            continue;
        scpy.interfaces.add(iface_impl);
        // Populate interface_table with the concrete (specialized) interface type as key.
        ChiType *concrete_iface_type = iface_impl->interface_type;
        if (concrete_iface_type->kind == TypeKind::Subtype) {
            concrete_iface_type = type_placeholders_sub(concrete_iface_type, &data);
        }
        scpy.interface_table[concrete_iface_type] = iface_impl;
    }
    return sty;
}

bool Resolver::builtin_satisfies_intrinsic(ChiType *type, IntrinsicSymbol symbol) {
    switch (symbol) {
    case IntrinsicSymbol::Add:
        return type->is_int_like() || type->kind == TypeKind::Float ||
               type->kind == TypeKind::String;
    case IntrinsicSymbol::Sub:
    case IntrinsicSymbol::Mul:
    case IntrinsicSymbol::Div:
    case IntrinsicSymbol::Rem:
    case IntrinsicSymbol::Neg:
        return type->is_int_like() || type->kind == TypeKind::Float;
    case IntrinsicSymbol::Eq:
    case IntrinsicSymbol::Ord:
        return type->is_int_like() || type->kind == TypeKind::Float ||
               type->kind == TypeKind::String || type->kind == TypeKind::Bool ||
               type->kind == TypeKind::Byte || type->kind == TypeKind::Rune;
    case IntrinsicSymbol::Hash:
        return type->is_int_like() || type->kind == TypeKind::Float ||
               type->kind == TypeKind::String || type->kind == TypeKind::Bool ||
               type->kind == TypeKind::Byte || type->kind == TypeKind::Rune;
    case IntrinsicSymbol::BitAnd:
    case IntrinsicSymbol::BitOr:
    case IntrinsicSymbol::BitXor:
    case IntrinsicSymbol::BitNot:
    case IntrinsicSymbol::Shl:
    case IntrinsicSymbol::Shr:
        return type->is_int_like();
    default:
        return false;
    }
}

void Resolver::check_binary_op(ast::Node *node, TokenType op_type, ChiType *type) {
    if (is_assignment_op(op_type)) {
        op_type = get_assignment_op(op_type);
    }
    if (node->type == ast::NodeType::BinOpExpr && node->data.bin_op_expr.resolved_call) {
        return;
    }

    auto intrinsic = get_operator_intrinsic_symbol(op_type);
    bool ok = (intrinsic != IntrinsicSymbol::None)
        ? builtin_satisfies_intrinsic(type, intrinsic)
        : false;

    // Handle placeholder types with appropriate trait bounds
    if (!ok && type->kind == TypeKind::Placeholder && type->data.placeholder.traits.len > 0) {
        for (auto trait_type : type->data.placeholder.traits) {
            if (ok)
                break;
            if (trait_type->kind == TypeKind::Struct && ChiTypeStruct::is_interface(trait_type)) {
                auto intrinsics = interface_get_intrinsics(trait_type);
                IntrinsicSymbol required_symbol = get_operator_intrinsic_symbol(op_type);
                if (required_symbol != IntrinsicSymbol::None) {
                    for (auto &intrinsic : intrinsics) {
                        if (intrinsic == required_symbol) {
                            ok = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (!ok) {
        error(node, errors::INVALID_OPERATOR, get_token_symbol(op_type), format_type_display(type));
    }
}

ChiType *Resolver::get_system_type(TypeKind kind) {
    auto types = get_system_types();
    switch (kind) {
    case TypeKind::Int:
        return types->int64;
    case TypeKind::Float:
        return types->float64;
    case TypeKind::String:
        return types->string;
    case TypeKind::Array:
        return types->array;
    case TypeKind::Span:
        return types->span;
    case TypeKind::Optional:
        return types->optional;
    case TypeKind::Bool:
        return types->bool_;
    case TypeKind::Byte:
        return types->byte_;
    case TypeKind::Rune:
        return types->rune_;
    case TypeKind::Void:
        return types->void_;
    default:
        panic("unhandled");
        return nullptr;
    }
}

ChiType *Resolver::get_wrapped_type(ChiType *elem, TypeKind kind) {
    switch (kind) {
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef:
    case TypeKind::Optional:
        return get_pointer_type(elem, kind);
    case TypeKind::Array:
        return get_array_type(elem);
    case TypeKind::Span:
        return get_span_type(elem);
    case TypeKind::Promise:
        return get_promise_type(elem);
    default:
        unreachable();
        return {};
    }
}

TypeKind Resolver::get_sigil_type_kind(cx::ast::SigilKind sigil) {
    switch (sigil) {
    case ast::SigilKind::Pointer:
        return TypeKind::Pointer;
    case ast::SigilKind::Reference:
        return TypeKind::Reference;
    case ast::SigilKind::MutRef:
        return TypeKind::MutRef;
    case ast::SigilKind::Move:
        return TypeKind::MoveRef;
    case ast::SigilKind::Optional:
        return TypeKind::Optional;
    default:
        unreachable();
        return {};
    }
}

ast::Node *Resolver::find_root_decl(ast::Node *node) {
    switch (node->type) {
    case NodeType::Identifier: {
        if (node->data.identifier.kind != ast::IdentifierKind::Value) {
            return node;
        }
        return node->data.identifier.decl;
    }
    case NodeType::DotExpr:
        return find_root_decl(node->data.dot_expr.expr);
    case NodeType::ParenExpr:
        return find_root_decl(node->data.child_expr);
    case NodeType::IndexExpr:
        return find_root_decl(node->data.index_expr.expr);
    case NodeType::UnaryOpExpr:
        return find_root_decl(node->data.unary_op_expr.op1);
    case NodeType::FnCallExpr:
    case NodeType::ConstructExpr:
    case NodeType::SliceExpr:
        // Function call/construct/slice results are temporaries, return nullptr to indicate no root
        // decl
        return nullptr;
    default:
        // Literals, casts, etc. — no root decl to track
        return nullptr;
    }
}

void Resolver::resolve_fn_lifetimes(ast::Node *fn_node) {
    auto *fn_type_sym = fn_node->resolved_type;
    if (!fn_type_sym)
        return;
    auto *fn_type = to_value_type(fn_type_sym);
    if (!fn_type || fn_type->kind != TypeKind::Fn)
        return;
    auto &fn = fn_type->data.fn;

    ast::FnProto *proto = nullptr;
    if (fn_node->type == NodeType::FnDef) {
        proto = &fn_node->data.fn_def.fn_proto->data.fn_proto;
    } else if (fn_node->type == NodeType::GeneratedFn) {
        proto = &fn_node->data.generated_fn.fn_proto->data.fn_proto;
    }
    if (!proto)
        return;

    // Extract param lifetimes from resolved param types
    for (size_t i = 0; i < fn.params.len; i++) {
        auto *pt = fn.params[i];
        ChiLifetime *lt = nullptr;
        if (pt && pt->is_reference() && pt->data.pointer.lifetimes.len > 0) {
            lt = pt->data.pointer.lifetimes[0];
        } else if (i < proto->params.len && proto->params[i]->data.param_decl.borrow_lifetime) {
            // Reuse borrow_lifetime set during FnProto resolution
            lt = proto->params[i]->data.param_decl.borrow_lifetime;
        } else if (pt && !pt->is_reference() && is_borrowing_type(pt)) {
            // Borrowing value params resolved after struct members (e.g. Holder with ref fields)
            auto *param_node = (i < proto->params.len) ? proto->params[i] : nullptr;
            string name = param_node ? string(param_node->name) : "fn_param";
            lt = new ChiLifetime{name, LifetimeKind::Param, param_node, nullptr};
            if (param_node)
                param_node->data.param_decl.borrow_lifetime = lt;
        }
        proto->resolved_param_lifetimes.add(lt);
    }

    // Extract return lifetime
    auto *ret = fn.return_type;
    if (ret && ret->is_reference() && ret->data.pointer.lifetimes.len > 0) {
        proto->resolved_return_lifetime = ret->data.pointer.lifetimes[0];
    } else if (ret && is_borrowing_type(ret)) {
        // Struct/value return containing references: elide to min(all ref params)
        array<ChiLifetime *> all_ref_lts;
        for (size_t i = 0; i < proto->resolved_param_lifetimes.len; i++) {
            if (proto->resolved_param_lifetimes[i])
                all_ref_lts.add(proto->resolved_param_lifetimes[i]);
        }
        if (fn.container_ref) {
            auto *container = fn.container_ref->get_elem();
            auto &st = container->data.struct_;
            if (!st.this_lifetime) {
                st.this_lifetime = new ChiLifetime{"this", LifetimeKind::This, nullptr, container};
            }
            all_ref_lts.add(st.this_lifetime);
        }
        if (all_ref_lts.len == 1) {
            proto->resolved_return_lifetime = all_ref_lts[0];
        } else if (all_ref_lts.len > 1) {
            auto *return_lt = new ChiLifetime{"fn", LifetimeKind::Return, nullptr, nullptr};
            for (size_t i = 0; i < all_ref_lts.len; i++) {
                all_ref_lts[i]->outlives.add(return_lt);
            }
            proto->resolved_return_lifetime = return_lt;
        }
    }
}

void Resolver::add_call_borrow_edges(ast::FnDef &fn_def, ast::FnCallExpr &call, ast::Node *target) {
    // Find the callee's FnProto to read resolved lifetime data (direct calls only)
    ast::FnProto *proto = nullptr;
    if (call.generated_fn) {
        proto = &call.generated_fn->data.generated_fn.fn_proto->data.fn_proto;
    } else {
        ast::Node *decl = nullptr;
        if (call.fn_ref_expr->type == NodeType::Identifier) {
            decl = call.fn_ref_expr->data.identifier.decl;
        } else if (call.fn_ref_expr->type == NodeType::DotExpr) {
            decl = call.fn_ref_expr->data.dot_expr.resolved_decl;
        }
        if (decl && decl->type == NodeType::FnDef) {
            proto = &decl->data.fn_def.fn_proto->data.fn_proto;
        } else if (decl && decl->type == NodeType::GeneratedFn) {
            proto = &decl->data.generated_fn.fn_proto->data.fn_proto;
        }
    }

    // Extract return lifetime and per-param lifetimes.
    // Direct calls: read from proto (includes borrowing value params via borrow_lifetime).
    // Indirect calls: read from the function type (ref params only; conservatively
    // assume all borrowing params flow to a borrowing return).
    ChiLifetime *ret_lt = nullptr;
    array<ChiLifetime *> param_lts;
    bool conservative = false;

    if (proto) {
        ret_lt = proto->resolved_return_lifetime;
        param_lts = proto->resolved_param_lifetimes;
        // Method return tied to 'this — borrow from the receiver
        if (ret_lt && ret_lt->kind == LifetimeKind::This &&
            call.fn_ref_expr->type == NodeType::DotExpr) {
            add_borrow_source_edges(fn_def, call.fn_ref_expr->data.dot_expr.expr, target, true);
        }
    } else {
        // Indirect call — extract lifetimes from the callee's function type
        auto *ct = to_value_type(call.fn_ref_expr->resolved_type);
        if (ct && ct->kind == TypeKind::FnLambda)
            ct = to_value_type(ct->data.fn_lambda.fn);
        if (!ct || ct->kind != TypeKind::Fn)
            return;
        auto &fn = ct->data.fn;
        auto *ret = fn.return_type;
        if (ret && ret->is_reference() && ret->data.pointer.lifetimes.len > 0) {
            ret_lt = ret->data.pointer.lifetimes[0];
        }
        conservative = (!ret_lt && ret && is_borrowing_type(ret));
        for (size_t i = 0; i < fn.params.len; i++) {
            auto *pt = fn.params[i];
            ChiLifetime *lt = nullptr;
            if (pt && pt->is_reference() && pt->data.pointer.lifetimes.len > 0)
                lt = pt->data.pointer.lifetimes[0];
            param_lts.add(lt);
        }
    }

    if (!ret_lt && !conservative)
        return;

    for (size_t i = 0; i < param_lts.len && i < call.args.len; i++) {
        if (param_lts[i] && ret_lt && lifetime_outlives(param_lts[i], ret_lt)) {
            add_borrow_source_edges(fn_def, call.args[i], target, true);
        } else if (conservative && !param_lts[i]) {
            // No lifetime info for this param — conservatively assume it flows to return
            add_borrow_source_edges(fn_def, call.args[i], target, true);
        }
    }
}

void Resolver::add_borrow_source_edges(ast::FnDef &fn_def, ast::Node *expr, ast::Node *target,
                                       bool is_ref) {
    if (!expr)
        return;
    // If the expression traces to a root declaration (variable, field, index, etc.):
    // - is_ref=true (reference): add_ref_edge — target depends on root's own lifetime
    // - is_ref=false (by-value): copy_ref_edges — target inherits root's dependencies
    //   but doesn't depend on root itself (the data is copied out)
    auto *root = find_root_decl(expr);
    if (root) {
        if (is_ref) {
            fn_def.add_ref_edge(target, root);
        } else {
            fn_def.copy_ref_edges(target, root);
        }
        return;
    }
    // Otherwise, recurse into compound expressions.
    switch (expr->type) {
    case NodeType::FnCallExpr:
        add_call_borrow_edges(fn_def, expr->data.fn_call_expr, target);
        break;
    case NodeType::ConstructExpr:
        // Field inits already set up edges on the ConstructExpr node during resolution.
        // Transitively copy those leaf edges to the target.
        fn_def.copy_ref_edges(target, expr);
        break;
    case NodeType::TryExpr:
        add_borrow_source_edges(fn_def, expr->data.try_expr.expr, target, is_ref);
        break;
    case NodeType::CastExpr:
        add_borrow_source_edges(fn_def, expr->data.cast_expr.expr, target, is_ref);
        break;
    case NodeType::SwitchExpr:
        for (auto scase : expr->data.switch_expr.cases) {
            if (scase) {
                add_borrow_source_edges(fn_def, scase->data.case_expr.body, target, is_ref);
            }
        }
        break;
    case NodeType::FnDef:
        // Lambda with by-ref captures: capture edges were added during resolution.
        // Transitively copy them to the target.
        fn_def.copy_ref_edges(target, expr);
        break;
    default:
        break;
    }
}

void Resolver::track_move_sink(ast::Node *parent_fn_node, ast::Node *expr, ChiType *expr_type,
                               ast::Node *dest, ChiType *dest_type) {
    if (!parent_fn_node)
        return;

    auto &fn_def = parent_fn_node->data.fn_def;
    ast::Node *moved_src = nullptr;

    if (expr->type == NodeType::UnaryOpExpr &&
        expr->data.unary_op_expr.op_type == TokenType::MOVEREF) {
        // Explicit &move expression: always a move
        moved_src = find_root_decl(expr->data.unary_op_expr.op1);
    } else if (expr->type == NodeType::UnaryOpExpr &&
               expr->data.unary_op_expr.op_type == TokenType::KW_MOVE) {
        // Explicit move expression (value move): always a move
        moved_src = find_root_decl(expr->data.unary_op_expr.op1);
    } else if (expr->type == NodeType::Identifier && expr_type &&
               expr_type->kind == TypeKind::MoveRef && dest_type &&
               dest_type->kind == TypeKind::MoveRef) {
        // &move T identifier passed/assigned to &move destination: natural move
        moved_src = find_root_decl(expr);
    }

    if (moved_src) {
        fn_def.add_sink_edge(moved_src, dest);
        fn_def.copy_ref_edges(dest, moved_src);
    }
}

static string node_label(ast::Node *n) {
    std::ostringstream type_ss;
    type_ss << n->type;
    auto type_str = type_ss.str();
    if (n->name.empty())
        return type_str;
    return fmt::format("{} ({})", n->name, type_str);
}

// Check if a leaf node satisfies a lifetime constraint relative to a terminal.
// Only called on base cases (VarDecl, ParamDecl) — graph construction
// via copy_ref_edges already flattens intermediate nodes to leaves.
static bool satisfies_lifetime_constraint(ChiLifetime *required, ast::Node *terminal,
                                          ast::Node *leaf) {
    if (leaf->type == ast::NodeType::VarDecl) {
        // Intra-function: leaf declared before terminal → leaf outlives terminal (LIFO)
        if (terminal->decl_order >= 0 && leaf->decl_order >= 0) {
            return leaf->decl_order < terminal->decl_order;
        }
        // Function-escape terminal: locals can never escape
        return false;
    }

    if (leaf->type == ast::NodeType::ParamDecl) {
        auto *leaf_type = leaf->resolved_type;
        auto &pdata = leaf->data.param_decl;

        // Determine the param's lifetime: from the reference type or from borrow_lifetime
        ChiLifetime *param_lt = nullptr;
        if (leaf_type && leaf_type->is_reference() && leaf_type->data.pointer.lifetimes.len > 0) {
            param_lt = leaf_type->data.pointer.lifetimes[0];
        } else if (pdata.borrow_lifetime) {
            param_lt = pdata.borrow_lifetime;
        }

        // No lifetime → plain value param (int, bool, etc.) that was captured by-ref.
        // It dies at function exit — fail for returns, pass for intra-function.
        if (!param_lt) {
            return terminal->type != ast::NodeType::ReturnStmt;
        }

        // Has lifetime → check against required (null required = always OK)
        if (!required)
            return true;
        return lifetime_outlives(param_lt, required);
    }

    if (leaf->type == ast::NodeType::Identifier &&
        leaf->data.identifier.kind == ast::IdentifierKind::This) {
        if (!required)
            return true;
        auto *leaf_type = leaf->resolved_type;
        if (!leaf_type)
            return false;
        auto &lifetimes = leaf_type->data.pointer.lifetimes;
        for (size_t i = 0; i < lifetimes.len; i++) {
            if (lifetime_outlives(lifetimes[i], required))
                return true;
        }
        return false;
    }

    return true;
}

// Check lifetime constraints for all terminals in a function.
// DFS from each terminal following ref_edges. For every reachable node,
// satisfies_lifetime_constraint(required, node) determines if the constraint holds.
void Resolver::check_lifetime_constraints(ast::FnDef *fn_def) {
    // Any local with outgoing ref_edges is a terminal (intra-function ordering)
    for (auto &[from, _] : fn_def->ref_edges.data) {
        if (from->decl_order >= 0) {
            fn_def->add_terminal(from);
        }
    }

    if (fn_def->terminals.len == 0 && fn_def->ref_edges.data.size() == 0)
        return;
    bool is_safe = has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_SAFE);
    bool verbose = has_lang_flag(m_ctx->lang_flags, LANG_FLAG_VERBOSE);

    auto fn_name = fn_def->fn_proto ? fn_def->fn_proto->name : "<lambda>";

    if (verbose && fn_def->ref_edges.data.size() > 0) {
        fmt::print("[lifetime] === {} ===\n", fn_name);
        fmt::print("[lifetime] edges:\n");
        for (auto &[from, tos] : fn_def->ref_edges.data) {
            for (auto *to : tos) {
                fmt::print("[lifetime]   {} -> {}\n", node_label(from), node_label(to));
            }
        }
        fmt::print("[lifetime] terminals ({}):\n", fn_def->terminals.len);
        for (size_t i = 0; i < fn_def->terminals.len; i++) {
            fmt::print("[lifetime]   {}\n", node_label(fn_def->terminals[i]));
        }
    }

    for (size_t t = 0; t < fn_def->terminals.len; t++) {
        auto *terminal = fn_def->terminals[t];

        // Extract required lifetime from terminal's type.
        ChiLifetime *required = nullptr;
        if (terminal->type == ast::NodeType::ReturnStmt) {
            // Return terminal: use the pre-calculated return type lifetime from elision
            if (fn_def->fn_proto && fn_def->fn_proto->resolved_type &&
                fn_def->fn_proto->resolved_type->kind == TypeKind::Fn) {
                auto *ret_type = fn_def->fn_proto->resolved_type->data.fn.return_type;
                if (ret_type && ret_type->is_reference() &&
                    ret_type->data.pointer.lifetimes.len > 0) {
                    required = ret_type->data.pointer.lifetimes[0];
                }
            }
            // Also check resolved_return_lifetime for borrowing value returns (func(), structs)
            if (!required && fn_def->fn_proto) {
                auto *proto = fn_def->fn_proto->type == ast::NodeType::FnProto
                                  ? &fn_def->fn_proto->data.fn_proto
                                  : nullptr;
                if (proto && proto->resolved_return_lifetime) {
                    required = proto->resolved_return_lifetime;
                }
            }
        } else {
            // Struct field assignment terminal: resolve to struct and get this_lifetime
            auto *term_type = terminal->resolved_type;
            if (term_type && term_type->kind != TypeKind::Struct) {
                auto *st = resolve_struct_type(term_type);
                if (st)
                    required = st->this_lifetime;
            }
        }

        if (verbose) {
            fmt::print("[lifetime] checking terminal: {} (required: {})\n", node_label(terminal),
                       required ? "'" + required->name : "'fn");
        }

        auto *deps = fn_def->ref_edges.get(terminal);
        if (!deps)
            continue;

        array<ast::Node *> stack;
        map<ast::Node *, bool> visited;
        for (size_t i = 0; i < deps->len; i++)
            stack.add(deps->items[i]);

        while (stack.len > 0) {
            auto *node = stack.last();
            stack.len--;
            if (visited.has_key(node))
                continue;
            visited[node] = true;

            bool satisfied = satisfies_lifetime_constraint(required, terminal, node);
            if (verbose) {
                fmt::print("[lifetime]   leaf {} -> {}\n", node_label(node),
                           satisfied ? "OK" : "VIOLATION");
            }

            if (!satisfied) {
                if (is_safe) {
                    array<Note> notes;
                    notes.add({"referenced here", terminal->token->pos});
                    error_with_notes(node, std::move(notes), "'{}' does not live long enough",
                                     node->name);
                } else {
                    node->escape.escaped = true;
                }
            }

            if (auto *next = fn_def->ref_edges.get(node)) {
                for (size_t i = 0; i < next->len; i++)
                    stack.add(next->items[i]);
            }
        }
    }

    // Separate sink check: for each terminal, check if its CURRENT borrow sources
    // have been sunk (deleted/moved). Uses current_edge_offset to skip stale edges
    // from previous assignments (e.g. s was reassigned from c to r before delete c).
    // Also uses terminal_last_use for NLL-like precision: if the terminal's last use
    // is before the sink point, it's safe (the dangling ref is never dereferenced).
    if (is_safe) {
        for (size_t t = 0; t < fn_def->terminals.len; t++) {
            auto *terminal = fn_def->terminals[t];
            // Skip terminals that were themselves sunk — the direct use-after-delete
            // check at Identifier resolution handles that case
            if (fn_def->is_sunk(terminal))
                continue;
            auto *deps = fn_def->ref_edges.get(terminal);
            if (!deps)
                continue;
            size_t offset = fn_def->current_edge_offset(terminal);
            auto *last_use = fn_def->terminal_last_use.get(terminal);
            for (size_t i = offset; i < deps->len; i++) {
                auto *node = deps->items[i];
                if (fn_def->is_sunk(node)) {
                    auto *sink_target = fn_def->sink_edges[node];
                    // Move ownership: if the sunk node was moved INTO this terminal, skip
                    // (e.g. var b = move a; — b owns the data, a is sunk with dest=b)
                    if (sink_target == terminal)
                        continue;
                    // NLL: if terminal's last use is before the sink point, skip
                    if (last_use && sink_target && sink_target->token) {
                        if (*last_use < sink_target->token->pos.offset)
                            continue;
                    }
                    bool is_delete =
                        sink_target && sink_target->type == NodeType::PrefixExpr &&
                        sink_target->data.prefix_expr.prefix->type == TokenType::KW_DELETE;
                    array<Note> notes;
                    if (sink_target && sink_target->token) {
                        notes.add(
                            {is_delete ? "deleted here" : "moved here", sink_target->token->pos});
                    }
                    notes.add({"referenced here", terminal->token->pos});
                    error_with_notes(terminal, std::move(notes), "'{}' used after {}",
                                     terminal->name, is_delete ? "delete" : "move");
                }
            }
        }
    }
}

bool Resolver::compare_impl_type(ChiType *base, ChiType *impl) {
    if (base == impl) {
        return true;
    }
    if (base->kind == TypeKind::Fn) {
        if (base->data.fn.params.len != impl->data.fn.params.len) {
            return false;
        }
        for (int i = 0; i < base->data.fn.params.len; ++i) {
            if (!compare_impl_type(base->data.fn.params[i], impl->data.fn.params[i])) {
                return false;
            }
        }
        if (!compare_impl_type(base->data.fn.return_type, impl->data.fn.return_type)) {
            return false;
        }
        return true;
    }
    // Allow return type covariance: if base is an interface and impl is a
    // concrete type that implements it, accept the match
    auto base_sty = resolve_struct_type(base);
    auto impl_sty = resolve_struct_type(impl);
    if (base_sty && impl_sty && ChiTypeStruct::is_interface(base_sty)) {
        for (auto &iface : impl_sty->interfaces) {
            auto iface_sty = resolve_struct_type(iface->interface_type);
            if (iface_sty && iface_sty->node == base_sty->node) {
                return true;
            }
        }
    }
    return can_assign(base, impl);
}

Scope *ScopeResolver::push_scope(ast::Node *owner) {
    auto new_scope = m_resolver->get_context()->allocator->create_scope(m_current_scope);
    m_current_scope = new_scope;
    m_current_scope->owner = owner;
    return m_current_scope;
}

void ScopeResolver::pop_scope() { m_current_scope = m_current_scope->parent; }

bool ScopeResolver::declare_symbol(const string &name, ast::Node *node) {
    if (m_current_scope->find_one(name)) {
        return false;
    }
    if (auto builtin = m_resolver->get_builtin(name)) {
        if (builtin->type == NodeType::Primitive || builtin->type == NodeType::StructDecl ||
            builtin->type == NodeType::EnumDecl) {
            return false;
        }
    }
    m_current_scope->put(name, node);
    return true;
}

ast::Node *ScopeResolver::find_symbol(const string &name, Scope *current_scope) {
    if (auto builtin = m_resolver->get_builtin(name)) {
        return builtin;
    }
    auto scope = current_scope ? current_scope : m_current_scope;
    while (scope) {
        if (auto node = scope->find_one(name)) {
            return node;
        }
        scope = scope->parent;
    }
    return nullptr;
}

array<ast::Node *> ScopeResolver::get_all_symbols(Scope *current_scope) {
    auto scope = current_scope ? current_scope : m_current_scope;
    auto list = scope->get_all_recursive();
    for (auto builtin : m_resolver->get_context()->builtins) {
        list.add(builtin);
    }
    return list;
}

ScopeResolver::ScopeResolver(cx::Resolver *resolver) {
    m_resolver = resolver;
    push_scope(nullptr);
}

#define RS_SET_PROP_COPY(prop, value)                                                              \
    auto cpy = *this;                                                                              \
    cpy.prop = value;                                                                              \
    return cpy;

ResolveScope ResolveScope::set_parent_fn(ChiType *fn) const { RS_SET_PROP_COPY(parent_fn, fn); }

ResolveScope ResolveScope::set_parent_struct(ChiType *struct_) const {
    RS_SET_PROP_COPY(parent_struct, struct_);
}

ResolveScope ResolveScope::set_parent_type_symbol(ChiType *symbol) const {
    RS_SET_PROP_COPY(parent_type_symbol, symbol);
}

ResolveScope ResolveScope::set_value_type(ChiType *value_type) const {
    RS_SET_PROP_COPY(value_type, value_type);
}

ResolveScope ResolveScope::set_parent_loop(ast::Node *loop) const {
    RS_SET_PROP_COPY(parent_loop, loop);
}

ResolveScope ResolveScope::set_is_escaping(bool is_escaping) const {
    RS_SET_PROP_COPY(is_escaping, is_escaping);
}

ResolveScope ResolveScope::set_parent_fn_node(ast::Node *fn) const {
    RS_SET_PROP_COPY(parent_fn_node, fn);
}

ResolveScope ResolveScope::set_module(ast::Module *module) const {
    RS_SET_PROP_COPY(module, module);
}

ResolveScope ResolveScope::set_move_outlet(ast::Node *outlet) const {
    RS_SET_PROP_COPY(move_outlet, outlet);
}

ResolveScope ResolveScope::set_block(ast::Block *block) const { RS_SET_PROP_COPY(block, block); }

ResolveScope ResolveScope::set_is_lhs(bool is_lhs) const { RS_SET_PROP_COPY(is_lhs, is_lhs); }

ResolveScope ResolveScope::set_is_fn_call(bool is_fn_call) const {
    RS_SET_PROP_COPY(is_fn_call, is_fn_call);
}

ResolveScope ResolveScope::set_is_unsafe_block(bool is_unsafe) const {
    RS_SET_PROP_COPY(is_unsafe_block, is_unsafe);
}

ChiType *Resolver::try_auto_deref(ast::Node *node, ChiType *stype, const string &field_name,
                                  ResolveScope &scope) {
    auto &struct_data = stype->data.struct_;

    // Prefer DerefMut when available (gives &mut, works for both mut and non-mut access).
    // Fall back to Deref (gives &) for read-only access.
    auto symbol = IntrinsicSymbol::DerefMut;
    auto member_p = struct_data.member_intrinsics.get(symbol);
    if (!member_p) {
        symbol = IntrinsicSymbol::Deref;
        member_p = struct_data.member_intrinsics.get(symbol);
    }
    if (!member_p)
        return nullptr;

    auto deref_member = *member_p;
    auto deref_fn_type = deref_member->resolved_type;
    if (!deref_fn_type || deref_fn_type->kind != TypeKind::Fn)
        return nullptr;

    auto deref_return_type = deref_fn_type->data.fn.return_type;
    if (!deref_return_type || !deref_return_type->is_reference())
        return nullptr;

    // Check that the deref'd target type actually has the member we're looking for
    auto target_type = deref_return_type->get_elem();
    if (!get_struct_member(target_type, field_name))
        return nullptr;

    // Build synthetic call: expr.deref() or expr.deref_mut()
    auto &data = node->data.dot_expr;

    auto dot_node = create_node(ast::NodeType::DotExpr);
    dot_node->token = node->token;
    dot_node->data.dot_expr.expr = data.expr;
    dot_node->data.dot_expr.field = deref_member->node->token;
    dot_node->data.dot_expr.resolved_struct_member = deref_member;
    dot_node->data.dot_expr.resolved_decl = deref_member->node;

    auto call_node = create_node(ast::NodeType::FnCallExpr);
    call_node->token = node->token;
    call_node->data.fn_call_expr.fn_ref_expr = dot_node;
    call_node->data.fn_call_expr.args = {};
    resolve(call_node, scope);

    // Replace the DotExpr's sub-expression with the deref call.
    // Return the deref'd reference type so the caller can redo member lookup.
    data.expr = call_node;
    return deref_return_type;
}

optional<Resolver::OperatorMethodCall>
Resolver::try_resolve_operator_method(IntrinsicSymbol symbol, ChiType *t1, ChiType *t2,
                                      ast::Node *op1, ast::Node *op2, ast::Node *node,
                                      ResolveScope &scope) {
    ChiStructMember *method_member = nullptr;
    ChiType *return_type = nullptr;

    // Try concrete struct type first
    auto stype = eval_struct_type(t1);
    if (stype) {
        auto member_p = stype->data.struct_.member_intrinsics.get(symbol);
        if (member_p && (*member_p)->resolved_type) {
            method_member = *member_p;
            auto method_type = method_member->resolved_type;
            if (method_type && method_type->kind == TypeKind::Fn) {
                auto &fn_data = method_type->data.fn;
                if (t2 == nullptr) {
                    if (fn_data.params.len == 0) {
                        return_type = fn_data.return_type;
                    }
                } else if (fn_data.params.len == 1 && can_assign(t2, fn_data.params[0])) {
                    return_type = fn_data.return_type;
                }
            }
        }
    }

    // Try placeholder type with trait bounds
    if (!method_member && t1->kind == TypeKind::Placeholder &&
        t1->data.placeholder.traits.len > 0) {
        for (auto trait_type : t1->data.placeholder.traits) {
            if (method_member)
                break;
            if (trait_type->kind == TypeKind::Struct && ChiTypeStruct::is_interface(trait_type)) {
                // First try member_intrinsics (populated on implementing structs)
                auto member_p = trait_type->data.struct_.member_intrinsics.get(symbol);
                if (member_p && (*member_p)->is_method()) {
                    method_member = *member_p;
                } else if (trait_type->data.struct_.node) {
                    // For interfaces, check if this trait IS the intrinsic
                    auto global_id = resolve_global_id(trait_type->data.struct_.node);
                    auto intrinsic_p = m_ctx->intrinsic_symbols.get(global_id);
                    if (intrinsic_p && *intrinsic_p == symbol) {
                        for (auto m : trait_type->data.struct_.members) {
                            if (m->is_method()) {
                                method_member = m;
                                break;
                            }
                        }
                    }
                }
                if (method_member) {
                    auto method_type = method_member->resolved_type;
                    if (method_type && method_type->kind == TypeKind::Fn) {
                        auto &fn_data = method_type->data.fn;
                        if (t2 == nullptr ? fn_data.params.len == 0 : fn_data.params.len == 1) {
                            return_type = t1;
                        }
                    }
                }
            }
        }
    }

    // Generate method call if we found a valid method
    if (method_member && return_type) {
        auto call_node = create_node(NodeType::FnCallExpr);
        call_node->token = node->token;
        auto dot_node = create_node(NodeType::DotExpr);
        dot_node->token = node->token;

        // populate generated dot expression
        auto &dot_data = dot_node->data.dot_expr;
        dot_node->data.dot_expr.expr = op1;
        dot_data.field = method_member->node->token;
        dot_data.resolved_struct_member = method_member;
        dot_data.resolved_decl = method_member->node;

        // populate generated call
        auto &call_data = call_node->data.fn_call_expr;
        call_data.fn_ref_expr = dot_node;
        call_data.args = op2 ? array<ast::Node *>{op2} : array<ast::Node *>{};
        resolve(call_node, scope);

        return OperatorMethodCall{call_node, return_type};
    }

    return std::nullopt;
}

array<IntrinsicSymbol> Resolver::interface_get_intrinsics(ChiType *interface_type) {
    array<IntrinsicSymbol> intrinsics;

    if (!interface_type || interface_type->kind != TypeKind::Struct ||
        !ChiTypeStruct::is_interface(interface_type)) {
        return intrinsics;
    }

    auto &struct_data = interface_type->data.struct_;

    // Check the interface itself by its global ID
    if (struct_data.node) {
        string global_id = resolve_global_id(struct_data.node);
        auto intrinsic_p = m_ctx->intrinsic_symbols.get(global_id);
        if (intrinsic_p) {
            intrinsics.add(*intrinsic_p);
        }
    }

    // Check embedded interfaces
    for (auto embed_type : struct_data.embeds) {
        if (embed_type && embed_type->kind == TypeKind::Struct &&
            ChiTypeStruct::is_interface(embed_type)) {
            // Recursively get intrinsics from embedded interface
            auto embed_intrinsics = interface_get_intrinsics(embed_type);
            for (auto &intrinsic : embed_intrinsics) {
                // Add if not already present
                bool found = false;
                for (auto &existing : intrinsics) {
                    if (existing == intrinsic) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    intrinsics.add(intrinsic);
                }
            }
        }
    }

    return intrinsics;
}

bool Resolver::interface_satisfies_trait(ChiType *interface_type, ChiType *required_trait) {
    if (!interface_type || !required_trait) {
        return false;
    }

    // Direct match
    if (interface_type == required_trait) {
        return true;
    }

    // Check if it's an interface
    if (interface_type->kind != TypeKind::Struct || !ChiTypeStruct::is_interface(interface_type)) {
        return false;
    }

    auto &struct_data = interface_type->data.struct_;

    // Check embedded interfaces recursively
    for (auto embed_type : struct_data.embeds) {
        if (interface_satisfies_trait(embed_type, required_trait)) {
            return true;
        }
    }

    return false;
}

bool Resolver::is_constructor_interface_compatible(ChiType *type, ChiType *iface_type) {
    if (!iface_type || iface_type->kind != TypeKind::Struct ||
        !ChiTypeStruct::is_interface(iface_type))
        return false;

    auto &iface_struct = iface_type->data.struct_;

    // Must be a pure constructor interface: only new(), no other methods, no embeds
    if (iface_struct.embeds.len > 0)
        return false;

    ChiStructMember *iface_new = nullptr;
    for (size_t i = 0; i < iface_struct.members.len; i++) {
        auto m = iface_struct.members[i];
        if (m->is_method()) {
            if (iface_new)
                return false; // more than one method
            iface_new = m;
        }
    }
    if (!iface_new || !iface_new->node ||
        iface_new->node->data.fn_def.fn_kind != ast::FnKind::Constructor)
        return false;

    auto *iface_proto = iface_new->node->data.fn_def.fn_proto;
    auto &iface_params = iface_proto->data.fn_proto.params;

    // For non-struct types (built-ins): only zero-arg constructible
    if (!type || type->kind != TypeKind::Struct) {
        return iface_params.len == 0;
    }

    auto *ctor = type->data.struct_.get_constructor();
    if (!ctor || !ctor->node) {
        // No constructor: zero-arg constructible only
        return iface_params.len == 0;
    }

    auto *ctor_proto = ctor->node->data.fn_def.fn_proto;
    auto &ctor_params = ctor_proto->data.fn_proto.params;

    // Count required ctor params (those without defaults)
    size_t required_ctor = 0;
    for (size_t i = 0; i < ctor_params.len; i++) {
        if (!ctor_params[i]->data.param_decl.default_value &&
            !ctor_params[i]->data.param_decl.is_variadic)
            required_ctor++;
    }

    // Interface provides iface_params.len args — must cover required, not exceed total
    if (iface_params.len < required_ctor || iface_params.len > ctor_params.len)
        return false;

    // Check parameter types match (use resolved fn types)
    // Note: 'this' is stored as container_ref, not in params[], so index 0 = first explicit param
    if (iface_params.len > 0) {
        auto *iface_fn_type = iface_new->resolved_type;
        auto *ctor_fn_type = ctor->resolved_type;
        if (!iface_fn_type || !ctor_fn_type)
            return false;

        for (size_t i = 0; i < iface_params.len; i++) {
            auto iface_param = iface_fn_type->data.fn.get_param_at(i);
            auto ctor_param = ctor_fn_type->data.fn.get_param_at(i);
            if (iface_param != ctor_param)
                return false;
        }
    }

    return true;
}

// Returns true if struct_type satisfies iface_type (nominal first, then structural fallback).
// Builds the vtable on-demand if structural match fires.
// Structural fallback only considers methods that were declared inside an impl block
// (is_impl_method == true), preventing accidental satisfaction by coincidentally-named methods.
bool Resolver::struct_satisfies_interface(ChiType *struct_type, ChiType *iface_type) {
    if (iface_type->kind != TypeKind::Struct || !ChiTypeStruct::is_interface(iface_type))
        return false;
    auto &struct_ = struct_type->data.struct_;
    auto &iface = iface_type->data.struct_;
    // 1. Nominal: explicit interface_table entry or embedding chain
    if (struct_.interface_table.get(iface_type)) return true;
    for (auto &impl : struct_.interfaces) {
        if (interface_satisfies_trait(impl->interface_type, iface_type)) return true;
    }
    // 2. Structural fallback: all required (non-default) methods must exist and be impl-declared
    for (auto &iface_member : iface.members) {
        if (!iface_member->is_method()) continue;
        if (iface_member->node && iface_member->node->data.fn_def.body) continue; // skip defaults
        auto struct_member = struct_.find_member(iface_member->get_name());
        if (!struct_member || !struct_member->is_method()) return false;
        if (!struct_member->is_impl_method) return false;
        auto expected = substitute_this_type(iface_member->resolved_type, struct_type);
        if (!compare_impl_type(expected, struct_member->resolved_type)) return false;
    }
    // Structural match: build vtable on-demand so codegen can dispatch normally
    resolve_vtable(iface_type, struct_type, nullptr);
    return true;
}

bool Resolver::check_trait_bound(ChiType *type_arg, ChiType *trait_type) {
    auto check_arg = type_arg;
    if (check_arg->kind == TypeKind::Subtype) {
        check_arg = resolve_subtype(check_arg);
    }

    // Sized is structural: everything is Sized except interfaces
    if (trait_type->kind == TypeKind::Struct && trait_type->data.struct_.node &&
        resolve_intrinsic_symbol(trait_type->data.struct_.node) == IntrinsicSymbol::Sized) {
        return !(check_arg->kind == TypeKind::Struct && ChiTypeStruct::is_interface(check_arg));
    }

    if (check_arg->kind == TypeKind::Struct) {
        if (struct_satisfies_interface(check_arg, trait_type)) return true;
        // Constructor interface: check ctor compatibility
        if (is_constructor_interface_compatible(check_arg, trait_type))
            return true;
    } else {
        // Built-in types: check all required intrinsics are satisfied
        auto required = interface_get_intrinsics(trait_type);
        if (required.len > 0) {
            bool all_satisfied = true;
            for (auto &intrinsic : required) {
                if (!builtin_satisfies_intrinsic(type_arg, intrinsic)) {
                    all_satisfied = false;
                    break;
                }
            }
            if (all_satisfied)
                return true;
        }
        // Constructor interface: built-in types are zero-arg constructible
        if (is_constructor_interface_compatible(check_arg, trait_type))
            return true;
    }
    return false;
}

WhereCondition *Resolver::build_where_condition(ast::ImplementBlockData &impl_data,
                                                ChiTypeStruct *struct_, ResolveScope &scope) {
    if (impl_data.where_clauses.len == 0)
        return nullptr;
    auto *cond = get_allocator()->create_where_condition();
    for (auto &clause : impl_data.where_clauses) {
        auto param_name = clause.param_name->str;
        long param_index = -1;
        for (long i = 0; i < (long)struct_->type_params.len; i++) {
            auto tp = to_value_type(struct_->type_params[i]);
            if (tp->name == param_name) {
                param_index = i;
                break;
            }
        }
        if (param_index < 0) {
            error(clause.bound_type, "unknown type parameter '{}'", param_name);
            continue;
        }
        auto trait_type = resolve_value(clause.bound_type, scope);
        if (!trait_type)
            continue;
        cond->bounds.add({param_index, trait_type});
    }
    return cond;
}

bool Resolver::check_where_condition(WhereCondition *cond, ChiTypeSubtype *subtype_data) {
    if (!cond)
        return true; // No condition = always included

    for (auto &bound : cond->bounds) {
        if (bound.param_index < 0 || bound.param_index >= (long)subtype_data->args.len) {
            return false;
        }

        auto type_arg = subtype_data->args[bound.param_index];

        // If type arg is still a placeholder (partially specialized), keep the member
        if (type_arg->is_placeholder)
            continue;

        if (!check_trait_bound(type_arg, bound.trait))
            return false;
    }

    return true;
}

// ============================================================================
// GenericResolver implementation - tracks generic instantiations and type envs
// ============================================================================

void GenericResolver::record_fn(const string &id, const string &name, ast::Node *node,
                                ChiType *generic_fn, map<ChiType *, ChiType *> subs) {
    if (fn_envs.get(id)) {
        return; // Already recorded
    }
    TypeEnvEntry entry;
    entry.name = name;
    entry.node = node;
    entry.generic_type = generic_fn;
    entry.subs = subs;
    fn_envs[id] = entry;
}

void GenericResolver::record_struct(const string &id, const string &name, ChiType *generic,
                                    map<ChiType *, ChiType *> subs) {
    if (struct_envs.get(id)) {
        return; // Already recorded
    }
    TypeEnvEntry entry;
    entry.name = name;
    entry.node = generic->data.struct_.node;
    entry.generic_type = generic;
    entry.subs = subs;
    struct_envs[id] = entry;
}

void GenericResolver::dump(Resolver *resolver) {
    print("=== Generic Instantiations ===\n\n");

    print("[Functions] ({})\n", fn_envs.size());
    for (auto &pair : fn_envs.get()) {
        auto &id = pair.first;
        auto &entry = pair.second;
        print("  {}\n", entry.name);
        print("    id: {}\n", id);
        if (entry.subs.size() > 0) {
            print("    TypeEnv:\n");
            for (auto &sub_pair : entry.subs.get()) {
                print("      {} → {}\n", resolver->format_type_display(sub_pair.first),
                      resolver->format_type_display(sub_pair.second));
            }
        }
    }

    print("\n[Structs] ({})\n", struct_envs.size());
    for (auto &pair : struct_envs.get()) {
        auto &id = pair.first;
        auto &entry = pair.second;
        print("  {}\n", entry.name);
        print("    id: {}\n", id);
        if (entry.subs.size() > 0) {
            print("    TypeEnv:\n");
            for (auto &sub_pair : entry.subs.get()) {
                print("      {} → {}\n", resolver->format_type_display(sub_pair.first),
                      resolver->format_type_display(sub_pair.second));
            }
        }
    }

    print("\n=== End Generic Instantiations ===\n");
}
