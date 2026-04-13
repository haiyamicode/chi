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

static ChiType *get_struct_access_root_type(ast::Node *expr);
static bool expr_is_direct_borrow_value(ast::Node *expr);
static ast::Node *get_assignment_expr_node(ast::Node *node);
static bool expr_creates_direct_borrow(Resolver *resolver, ast::Node *expr, ChiType *from_type,
                                       ChiType *to_type, const ResolveScope *scope);
static ast::Node *unwrap_lifetime_copy_intrinsic_arg(ast::Node *expr);

static bool expr_is_narrowed_from_optional(ast::Node *node, ResolveScope &scope);
static bool logical_rhs_uses_truthy_narrowing(TokenType op_type);
static bool is_null_literal(ast::Node *node);
static ast::Node *find_narrowed_optional_var(ast::Node *node, ResolveScope &scope);

static string resolve_intrinsic_id(ast::Node *node) {
    if (!node || !node->module || node->name.empty()) {
        return "";
    }
    return fmt::format("{}.{}", node->module->global_id(), node->name);
}

static bool is_nonowning_alias_decl(ast::Node *node) {
    return node && node->type == NodeType::VarDecl && node->data.var_decl.narrowed_from;
}

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

ast::Module *Resolver::load_module(const string &path, ResolveScope &scope) {
    auto *comp_ctx = static_cast<CompilationContext *>(m_ctx->allocator);
    auto path_info = m_ctx->allocator->find_module_path(path, scope.module ? scope.module->path : "");
    if (!path_info) {
        return nullptr;
    }

    auto abs_path = fs::absolute(path_info->entry_path).string();
    if (auto cached_mod = comp_ctx->source_modules.get(abs_path)) {
        return *cached_mod;
    }

    auto target_package = m_ctx->allocator->get_or_create_package(path_info->package_id_path);
    auto src = io::Buffer::from_file(path_info->entry_path);
    auto module = m_ctx->allocator->process_source(target_package, &src, path_info->entry_path);
    comp_ctx->source_modules[abs_path] = module;
    return module;
}

ast::Module *Resolver::load_and_import_module(const string &path, ResolveScope &scope) {
    auto *module = load_module(path, scope);
    if (!module || !scope.module) {
        return module;
    }

    for (auto import : scope.module->imports) {
        if (import == module) {
            return module;
        }
    }

    scope.module->imports.add(module);
    return module;
}

ast::Node *Resolver::get_reflect_type_decl(ResolveScope &scope) {
    auto *module = load_and_import_module("std/reflect", scope);
    if (!module || !module->scope) {
        return nullptr;
    }

    for (auto decl : module->root->data.root.top_level_decls) {
        if (decl->type == NodeType::StructDecl && decl->name == "Type" &&
            decl->declspec().is_exported()) {
            return decl;
        }
    }
    return nullptr;
}

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
    system_types.null_ = create_type(TypeKind::Null);
    system_types.void_ref = create_pointer_type(system_types.void_, TypeKind::Reference);
    system_types.bool_ = create_type(TypeKind::Bool);
    system_types.string = create_type(TypeKind::String);
    system_types.str_lit = create_pointer_type(system_types.byte_, TypeKind::Pointer);
    system_types.array = create_type(TypeKind::Array);
    system_types.span = create_type(TypeKind::Span);
    system_types.optional = create_type(TypeKind::Optional);
    // Result and Promise are now defined as Chi-native types in runtime.xs
    // system_types.promise = create_type(TypeKind::Promise);
    system_types.undefined = create_type(TypeKind::Undefined);
    system_types.zeroinit = create_type(TypeKind::ZeroInit);
    system_types.never_ = create_type(TypeKind::Never);
    system_types.unit = create_type(TypeKind::Unit);
    system_types.tuple = create_type(TypeKind::Tuple);
    m_ctx->rt_unit_type = system_types.unit;
    m_ctx->static_lifetime = new ChiLifetime{"static", LifetimeKind::Static, nullptr, nullptr};

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
    add_primitive("Unit", system_types.unit);
    add_primitive("Tuple", system_types.tuple);

    // Result and Promise are discovered from runtime.xs declarations

    // intrinsic symbols
    m_ctx->intrinsic_symbols["std.ops.Index"] = IntrinsicSymbol::Index;
    m_ctx->intrinsic_symbols["std.ops.IndexMut"] = IntrinsicSymbol::IndexMut;
    m_ctx->intrinsic_symbols["std.ops.IndexMutIterable"] = IntrinsicSymbol::IndexMutIterable;
    m_ctx->intrinsic_symbols["std.ops.ListInit"] = IntrinsicSymbol::ListInit;
    m_ctx->intrinsic_symbols["std.ops.KvInit"] = IntrinsicSymbol::KvInit;
    m_ctx->intrinsic_symbols["std.mem.AllocInit"] = IntrinsicSymbol::AllocInit;
    m_ctx->intrinsic_symbols["std.mem.annotate_copy"] = IntrinsicSymbol::AnnotateCopy;
    m_ctx->intrinsic_symbols["intrinsics.__copy"] = IntrinsicSymbol::MemCopy;
    m_ctx->intrinsic_symbols["intrinsics.__move"] = IntrinsicSymbol::MemMove;
    m_ctx->intrinsic_symbols["intrinsics.__destroy"] = IntrinsicSymbol::MemDestroy;
    m_ctx->intrinsic_symbols["intrinsics.__atomic_load"] = IntrinsicSymbol::AtomicLoad;
    m_ctx->intrinsic_symbols["intrinsics.__atomic_store"] = IntrinsicSymbol::AtomicStore;
    m_ctx->intrinsic_symbols["intrinsics.__atomic_compare_exchange"] =
        IntrinsicSymbol::AtomicCompareExchange;
    m_ctx->intrinsic_symbols["intrinsics.__atomic_fetch_add"] =
        IntrinsicSymbol::AtomicFetchAdd;
    m_ctx->intrinsic_symbols["intrinsics.__atomic_fetch_sub"] =
        IntrinsicSymbol::AtomicFetchSub;
    m_ctx->intrinsic_symbols["intrinsics.__reflect_dyn_elem"] =
        IntrinsicSymbol::ReflectDynElem;
    m_ctx->intrinsic_symbols["std.mem.__copy"] = IntrinsicSymbol::MemCopy;
    m_ctx->intrinsic_symbols["std.mem.__move"] = IntrinsicSymbol::MemMove;
    m_ctx->intrinsic_symbols["std.mem.__destroy"] = IntrinsicSymbol::MemDestroy;
    m_ctx->intrinsic_symbols["runtime.__copy"] = IntrinsicSymbol::MemCopy;
    m_ctx->intrinsic_symbols["runtime.__move"] = IntrinsicSymbol::MemMove;
    m_ctx->intrinsic_symbols["runtime.__destroy"] = IntrinsicSymbol::MemDestroy;
    m_ctx->intrinsic_symbols["std.json.__move"] = IntrinsicSymbol::MemMove;
    m_ctx->intrinsic_symbols["std.atomic.__atomic_load"] = IntrinsicSymbol::AtomicLoad;
    m_ctx->intrinsic_symbols["std.atomic.__atomic_store"] = IntrinsicSymbol::AtomicStore;
    m_ctx->intrinsic_symbols["std.atomic.__atomic_compare_exchange"] =
        IntrinsicSymbol::AtomicCompareExchange;
    m_ctx->intrinsic_symbols["std.atomic.__atomic_fetch_add"] =
        IntrinsicSymbol::AtomicFetchAdd;
    m_ctx->intrinsic_symbols["std.atomic.__atomic_fetch_sub"] =
        IntrinsicSymbol::AtomicFetchSub;
    m_ctx->intrinsic_symbols["std.reflect.__reflect_dyn_elem"] =
        IntrinsicSymbol::ReflectDynElem;
    m_ctx->intrinsic_symbols["std.ops.Copy"] = IntrinsicSymbol::Copy;
    m_ctx->intrinsic_symbols["std.ops.NoCopy"] = IntrinsicSymbol::NoCopy;
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
    m_ctx->intrinsic_symbols["std.ops.Unsized"] = IntrinsicSymbol::Unsized;
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
    m_ctx->intrinsic_symbols["std.ops.AsTuple"] = IntrinsicSymbol::AsTuple;
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
    if (node->resolved_node && node->resolved_node->resolved_type) {
        return node->resolved_node->resolved_type;
    }
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
    if (!module || module->resolved || module->resolving) {
        return;
    }

    auto *prev_module = m_module;
    module->resolving = true;

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
    if (has_lang_flag(m_ctx->lang_flags, LANG_FLAG_VERBOSE_GENERICS)) {
        m_ctx->generics.dump(this);
    }

    module->resolved = true;
    module->resolving = false;
    m_module = prev_module;
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
    // void-returning functions convert to Unit-returning functions transparently
    bool return_ok = can_assign(from_data.return_type, to_data.return_type, is_explicit) ||
                     (from_data.return_type->kind == TypeKind::Void &&
                      to_data.return_type->kind == TypeKind::Unit);
    if (!return_ok) {
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
        // &mut [T] -> &[T] is allowed (downgrade), &[T] -> &mut [T] is not
        if (to_type->data.span.is_mut && !from_type->data.span.is_mut) return false;
        return true;
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef: {
        if (from_type->kind == TypeKind::Null) {
            return to_type->kind == TypeKind::Pointer;
        }

        // Handle pointer/reference conversions
        if (from_type->is_pointer_like()) {

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
                    // Pointer <-> Reference-like conversions are allowed
                    if (from_type->kind == TypeKind::Pointer ||
                        to_type->kind == TypeKind::Pointer) {
                        return true;
                    }
                    // MoveRef only assigns implicitly to MoveRef. Borrowing from an owner
                    // requires either a managed/unsafe assignment context or an explicit cast.
                    if (from_type->kind == TypeKind::MoveRef) {
                        return to_type->kind == TypeKind::MoveRef ||
                               (is_explicit && to_type->is_borrow_reference());
                    }
                    // MutRef -> Ref is allowed.
                    if (from_type->kind == TypeKind::MutRef &&
                        to_type->kind == TypeKind::Reference) {
                        return true;
                    }
                    // Ref -> MutRef is NOT allowed.
                    if (from_type->kind == TypeKind::Reference &&
                        to_type->kind == TypeKind::MutRef) {
                        return false;
                    }
                    // Ref/MutRef -> MoveRef is NOT allowed.
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
            if (from_type->is_pointer_like()) {
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
        if (from_type->is_int()) {
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
               from_type->kind == TypeKind::Pointer ||
               (is_explicit && from_type->kind == TypeKind::Byte);
    case TypeKind::Optional: {
        // Allow null, same optional type, or implicit T -> ?T wrap
        if (from_type->kind == TypeKind::Null) {
            return true;
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
    case TypeKind::Subtype: {
        if (from_type->kind == TypeKind::EnumValue) {
            return get_enum_root(from_type) == get_enum_root(to_type);
        }
        // Struct subtypes: same generic + compatible type args
        if (from_type->kind == TypeKind::Subtype) {
            auto &from_sub = from_type->data.subtype;
            auto &to_sub = to_type->data.subtype;
            if (from_sub.generic == to_sub.generic && from_sub.args.len == to_sub.args.len) {
                for (int i = 0; i < from_sub.args.len; i++) {
                    if (!can_assign(from_sub.args[i], to_sub.args[i], is_explicit)) {
                        return false;
                    }
                }
                return true;
            }
        }
        return false;
    }
    case TypeKind::Infer: {
        auto &infer_data = to_type->data.infer;
        if (infer_data.inferred_type) {
            return can_assign(from_type, infer_data.inferred_type, is_explicit);
        }
        if (from_type->kind != TypeKind::Placeholder && from_type->kind != TypeKind::Infer) {
            infer_data.inferred_type = from_type;
            to_type->is_placeholder = false;
            return true;
        }
        return false;
    }
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
static string node_label(ast::Node *n);
static ast::FnProto *get_decl_fn_proto(ast::Node *decl);
static ast::FnProto *get_decl_summary_proto(ast::Node *decl);
static ChiLifetime *get_first_ref_lifetime(ChiType *type);
static bool type_has_lifetime_kind(ChiType *type, LifetimeKind kind);
static bool type_may_propagate_borrow_deps(Resolver *resolver, ChiType *type);
static bool is_value_borrowing_type(Resolver *resolver, ChiType *type);
static ChiLifetime *get_param_effective_lifetime(ast::Node *param_node, ChiType *param_type);

static bool is_exclusive_access_borrow_param(ChiType *type, ast::Node *param_node) {
    return type && type->kind == TypeKind::MutRef;
}

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

static void collect_fn_nodes_from_decl(ast::Node *decl, array<ast::Node *> &out);

ChiType *Resolver::_resolve(ast::Node *node, ResolveScope &scope, uint32_t flags) {
    switch (node->type) {
    case NodeType::Root: {
        auto &data = node->data.root;
        auto is_module_var = [](ast::Node *decl) {
            return decl->type == NodeType::VarDecl && !decl->data.var_decl.is_field &&
                   !decl->data.var_decl.is_embed;
        };

        // first pass: skip function/struct bodies and module-level vars
        scope.skip_fn_bodies = true;
        for (auto decl : data.top_level_decls) {
            if (is_module_var(decl)) {
                continue;
            }
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

        // module-level var initializers: deferred until struct types are complete
        for (auto decl : data.top_level_decls) {
            if (is_module_var(decl)) {
                resolve(decl, scope);
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

        // fourth pass: resolve generic subtypes (signatures only, before bodies)
        m_ctx->generics.resolve_pending(this);

        // fifth pass: resolve function and method bodies
        scope.skip_fn_bodies = false;
        for (auto decl : data.top_level_decls) {
            if (decl->type == NodeType::StructDecl || decl->type == NodeType::EnumDecl ||
                decl->type == NodeType::FnDef) {
                _resolve(decl, scope);
            }
        }

        // sixth pass: resolve any new generic subtypes created during body resolution
        m_ctx->generics.resolve_pending(this);

        array<ast::Node *> fns = {};
        for (auto decl : data.top_level_decls) {
            collect_fn_nodes_from_decl(decl, fns);
        }
        for (auto *fn_node : fns) {
            if (fn_node->type == NodeType::FnDef) {
                compute_receiver_copy_edge_summary(fn_node->data.fn_def);
            }
        }
        for (auto *fn_node : fns) {
            if (fn_node->type == NodeType::FnDef) {
                finalize_lifetime_flow(fn_node->data.fn_def);
                check_lifetime_constraints(&fn_node->data.fn_def, fn_node->data.fn_def.flow);
            }
        }

        // effect-summary pass: propagate exclusive-access requirements through the call graph.
        // Only applied to low-level modules (.xs); managed mode (.x) relies on the GC to keep
        // escaped references valid across realloc/rehash, so the exclusive-access rule would
        // reject code that is actually memory-safe.
        if (!has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_MANAGED)) {
            compute_exclusive_access_summaries(data.top_level_decls);
            apply_exclusive_access_effects(data.top_level_decls);
        }

        // final pass: finalize placeholder lambdas
        for (auto decl : data.top_level_decls) {
            if (decl->type == NodeType::FnDef) {
                auto fn_type = to_value_type(decl->resolved_type);
                finalize_placeholder_lambda_params(fn_type);
            }
        }
        return nullptr;
    }
    case NodeType::FnDef: {
        auto &data = node->data.fn_def;
        resolve_intrinsic_symbol(node);
        if (data.decl_spec) {
            if (data.fn_kind == ast::FnKind::Constructor && data.decl_spec->is_static()) {
                error(node, errors::STATIC_CONSTRUCTOR_NOT_ALLOWED);
            }

            if (!data.body && data.decl_spec->is_mutable()) {
                error(node, errors::MUTABLE_FUNCTION_REQUIRES_BODY, "mut");
            }

            // `mut` only makes sense on instance methods (it controls exclusive access
            // to `this`). Reject it on free functions, lambdas, and static methods.
            if (data.decl_spec->is_mutable() && !data.is_instance_method()) {
                error(node, errors::MUT_ONLY_ON_INSTANCE_METHOD);
            }
        }
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
                compute_receiver_copy_edge_summary(data);
                add_fn_body_param_cleanups(node, data.body);
            }

            compute_lambda_capture_move_summary(node);

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

            // Borrow tracking: captures create edges in the function that
            // owns the captured variable, so the lambda is treated as borrowing
            // from that local. Only add edges for variables owned by the immediate
            // enclosing function — deeper captures propagate through the chain.
            // - By-ref captures: direct edge (lambda borrows the variable itself)
            // - By-value captures: copy edges (lambda inherits the variable's
            //   borrow dependencies — e.g., a captured &T still borrows the pointee)
            if (scope.parent_fn_node) {
                auto &parent_fn_def = *scope.parent_fn_def();
                for (auto &cap : data.captures) {
                    if (cap.decl->parent_fn != scope.parent_fn_node)
                        continue;
                    if (cap.mode == ast::CaptureMode::ByRef) {
                        parent_fn_def.add_ref_edge(node, cap.decl);
                    } else {
                        // By-value capture:
                        // - params need an abstract copy-edge so generic borrow-carrying
                        //   values still propagate through lambda summaries
                        // - locals only contribute their current leaf borrow deps; the
                        //   closure must not depend on the local variable itself
                        if (cap.decl->type == NodeType::ParamDecl) {
                            parent_fn_def.copy_ref_edges(node, cap.decl, false);
                        } else {
                            parent_fn_def.flow.copy_existing_ref_edges(node, cap.decl, false);
                        }
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
                // Hidden per-capture drop-flag pointers. These let a lambda move clear the
                // caller's maybe-move flag when a captured by-ref value is actually moved.
                for (int i = 0; i < data.captures.len; i++) {
                    auto name = fmt::format("capture_flag_{}", i);
                    bstruct_data.add_member(get_allocator(), name, get_dummy_var(name),
                                            get_pointer_type(get_system_types()->bool_, TypeKind::Pointer));
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
            compute_receiver_copy_edge_summary(data);
            add_fn_body_param_cleanups(node, data.body);
        }
        return proto;
    }
    case NodeType::FnProto: {
        auto &data = node->data.fn_proto;
        resolve_intrinsic_symbol(node);
        auto is_fn_decl = flags & IS_FN_DECL_PROTO;
        auto is_lambda = flags & IS_FN_LAMBDA;

        // Create a new scope for type parameters
        auto fn_scope = scope;
        TypeList type_param_types;

        // Process explicit lifetime parameters: create shared ChiLifetime objects
        map<string, ChiLifetime *> lifetime_map;
        array<ChiLifetime *> fn_lifetime_list;
        for (auto lt_node : data.lifetime_params) {
            ChiLifetime *lt;
            if (lt_node->name == "static") {
                lt = m_ctx->static_lifetime;
            } else {
                lt = new ChiLifetime{lt_node->name, LifetimeKind::Param, nullptr, nullptr};
            }
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

        // Infer return type from expected function type if not provided.
        // Block lambdas (func (...) { ... }) without explicit return type are always void.
        // Only arrow lambdas (func (...) => expr) infer return type from the body.
        ChiType *return_type = nullptr;
        bool is_arrow_body = data.fn_def_node && data.fn_def_node->data.fn_def.body &&
                             data.fn_def_node->data.fn_def.body->data.block.is_arrow;
        if (data.return_type) {
            return_type = resolve_value(data.return_type, fn_scope);
        } else if (expected_fn && is_arrow_body) {
            auto expected_return = expected_fn->return_type;
            // Arrow lambda: infer return type from body expression.
            if (expected_return && expected_return->kind == TypeKind::Placeholder) {
                auto infer_type = create_type(TypeKind::Infer);
                infer_type->data.infer.placeholder = expected_return;
                infer_type->is_placeholder = true;
                return_type = infer_type;
            } else if (expected_return && expected_return->kind == TypeKind::Infer &&
                       !expected_return->data.infer.inferred_type) {
                return_type = expected_return;
            } else {
                return_type = expected_return;
            }
        } else if (expected_fn) {
            // Block lambda without return type: use expected if concrete, else void.
            auto expected_return = expected_fn->return_type;
            if (expected_return && !expected_return->is_placeholder) {
                return_type = expected_return;
            } else {
                return_type = get_system_types()->void_;
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
        auto container = (is_static || is_lambda) ? nullptr : scope.parent_struct;

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
                param_type = get_span_type(param_type);
                param->resolved_type = param_type;
                is_variadic = true;
            }
            // Each reference-like param gets its own distinct lifetime.
            if (type_needs_first_ref_lifetime(param_type)) {
                auto *lt =
                    new ChiLifetime{string(param->name), LifetimeKind::Param, param, nullptr};
                all_ref_lifetimes.add(lt);
                auto *fresh = with_first_ref_lifetime(param_type, lt);
                param_type = fresh;
                param->resolved_type = param_type;
            } else if (is_value_borrowing_type(this, param_type)) {
                // Borrowing value params get lifetimes so borrows flow through calls.
                ChiLifetime *lt = nullptr;
                // Check for 'static lifetime on func types: func<'static>
                auto *fn_inner = param_type->kind == TypeKind::FnLambda
                                     ? param_type->data.fn_lambda.fn
                                     : (param_type->kind == TypeKind::Fn ? param_type : nullptr);
                if (fn_inner && fn_inner->kind == TypeKind::Fn) {
                    for (auto *flt : fn_inner->data.fn.lifetime_params) {
                        if (flt->kind == LifetimeKind::Static) {
                            lt = flt;
                            break;
                        }
                    }
                }
                // For lifetime-bounded placeholders (T: 'a), use the declared lifetime.
                if (!lt && param_type->kind == TypeKind::Placeholder &&
                    param_type->data.placeholder.lifetime_bound) {
                    lt = param_type->data.placeholder.lifetime_bound;
                }
                if (!lt) {
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
            null_node->resolved_type = get_system_types()->null_;
            pdata.resolved_default_value = null_node;
        }

        // Return type lifetime elision: return borrows from min(all reference-like params)
        if (type_needs_first_ref_lifetime(return_type)) {
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
                return_type = with_first_ref_lifetime(return_type, elided_lt);
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
        ChiType *type_override = nullptr;
        if (data.kind == ast::IdentifierKind::This) {
            // Check for narrowed 'this' (e.g., enum variant narrowing in switch)
            if (scope.block && scope.block->scope) {
                auto narrowed = scope.block->scope->find_one("this", true);
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
        if (data.kind == ast::IdentifierKind::Value && (!data.decl || data.decl_is_provisional)) {
            resolve_contextual_identifier(node, scope, data, &type_override);
        }
        if (!data.decl) {
            error(node, errors::UNDECLARED, node->name);
            return create_type(TypeKind::Unknown);
        }
        if (data.kind == ast::IdentifierKind::Value && scope.block) {
            auto replacement = scope.block->scope->find_one(data.decl->name, true);
            if (replacement && replacement->type == NodeType::VarDecl &&
                replacement->data.var_decl.is_generated && replacement != data.decl) {
                data.decl = replacement;
            }
        }
        auto type = type_override ? type_override : resolve(data.decl, scope);
        if (auto decl_fn = data.decl->parent_fn) {
            if (decl_fn != scope.parent_fn_node) {
                data.decl->analysis.escaped = true;

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
                    node->analysis.capture_path.add(path_entry);
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
            auto &fn_def = *scope.parent_fn_def();
            if (fn_def.is_sunk(data.decl)) {
                auto *target = fn_def.flow.sink_target(data.decl);
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
            auto &fn_def = *scope.parent_fn_def();
            fn_def.flow.terminal_last_use[data.decl] = node->token->pos.offset;
            fn_def.flow.terminal_last_use_node[data.decl] = node;
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
            if (data.lifetime.empty()) {
                return get_span_type(type, data.is_mut);
            }
            auto *lifetime = resolve_named_lifetime(node, scope, data.lifetime);
            if (!lifetime) {
                return create_type(TypeKind::Unknown);
            }
            return get_span_type(type, data.is_mut, lifetime);
        }
        auto kind = get_sigil_type_kind(data.sigil);
        ChiType *final_type;
        if (!data.lifetime.empty()) {
            // Lifetime-annotated ref: create fresh type (not cached) with resolved lifetime
            final_type = create_pointer_type(type, kind);
            auto *lifetime = resolve_named_lifetime(node, scope, data.lifetime);
            if (!lifetime) {
                return create_type(TypeKind::Unknown);
            }
            final_type->data.pointer.lifetimes.add(lifetime);
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
            check_assignment(data.default_value, default_type, result, &scope);
        }
        return result;
    }
    case NodeType::DestructureDecl: {
        auto &data = node->data.destructure_decl;
        auto *expr_node = data.effective_expr();

        // Create temp VarDecl BEFORE resolving — exactly like VarDecl sets move_outlet
        // so ConstructExpr can RVO directly into the temp
        auto temp_var = get_dummy_var("__destructure_tmp", expr_node);
        temp_var->module = node->module;
        temp_var->parent_fn = scope.parent_fn_node;
        temp_var->data.var_decl.kind = ast::VarKind::Immutable;
        temp_var->data.var_decl.initialized_at = node;
        data.temp_var = temp_var;

        // Resolve RHS with move_outlet set to temp — same as VarDecl
        auto expr_scope = scope.set_move_outlet(temp_var);
        auto expr_type = resolve(expr_node, expr_scope);
        if (!expr_type)
            return nullptr;

        temp_var->resolved_type = expr_type;

        if (scope.parent_fn_node) {
            temp_var->decl_order = scope.parent_fn_def()->next_decl_order++;
            if (!scope.is_unsafe_block) {
                bool is_ref =
                    expr_creates_direct_borrow(this, expr_node, expr_type, expr_type, &scope);
                auto &fn_def = *scope.parent_fn_def();
                add_borrow_source_edges(fn_def, expr_node, temp_var, is_ref);
                copy_projection_summaries(fn_def, expr_node, temp_var, expr_type);
            }
        }

        auto *borrow_source = find_root_decl(expr_node) ? expr_node : temp_var;

        // Resolve each field pattern
        resolve_destructure(node, expr_type, scope, borrow_source);

        // Add temp to cleanup if needed
        if (scope.parent_fn_node && scope.block && should_destroy(temp_var, expr_type) &&
            !temp_var->analysis.is_capture()) {
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
            if (!expr_type) {
                return var_type;
            }
            // Copy-edge propagation for local initialization. This is graph-driven:
            // if the initializer carries tracked borrow dependencies they flow into
            // the local; otherwise add_borrow_source_edges is a no-op.
            if (scope.parent_fn_node && !scope.is_unsafe_block) {
                auto *target_type_for_borrows = var_type ? var_type : expr_type;
                bool is_ref = expr_creates_direct_borrow(this, data.expr, expr_type,
                                                         target_type_for_borrows, &scope);
                auto &fn_def = *scope.parent_fn_def();
                add_borrow_source_edges(fn_def, data.expr, node, is_ref);
                copy_projection_summaries(fn_def, data.expr, node,
                                          target_type_for_borrows);
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
                    check_assignment(data.expr, expr_type, var_type, &scope);
                }
            } else {
                var_type = expr_type;
            }
            // NoCopy: error if initializing from a named value
            if (var_type && is_non_copyable(var_type) && is_addressable(data.expr) &&
                !data.expr->analysis.moved && should_destroy(data.expr, expr_type)) {
                error(data.expr, errors::TYPE_NOT_COPYABLE,
                      format_type_display(var_type));
            }
            // RHS is a non-addressable temp: transfer ownership (move, don't copy)
            mark_temp_moved_if_needed(data.expr, var_type);
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
        if (scope.parent_fn_node && scope.block && !is_nonowning_alias_decl(node) &&
            should_destroy(node, var_type) &&
            !node->analysis.is_capture()) {
            scope.block->cleanup_vars.add(node);
            scope.parent_fn_def()->has_cleanup = true;
        }
        return var_type;
    }
    case NodeType::BinOpExpr: {
        auto &data = node->data.bin_op_expr;
        if (data.op_type == TokenType::QUES && scope.move_outlet) {
            node->resolved_outlet = scope.move_outlet;
            node->analysis.moved = true;
        }
        auto op1_scope = scope;
        if (is_assignment_op(data.op_type)) {
            op1_scope = scope.set_is_lhs(true);
        }
        auto t1 = resolve(data.op1, op1_scope);
        if (!t1) {
            return nullptr;
        }

        // `opt! = value` → `opt = value`: retarget the assignment at the whole
        // optional so codegen's normal T → ?T implicit-wrap path handles it,
        // rather than special-casing optional unwrap in the ASS handler.
        // We keep the RHS inference context on the inner T (not ?T) so
        // brace-init shorthand continues to construct the payload type.
        ChiType *rhs_ctx_override = nullptr;
        if (data.op_type == TokenType::ASS &&
            data.op1->type == NodeType::UnaryOpExpr) {
            auto &u = data.op1->data.unary_op_expr;
            if (u.is_suffix && u.op_type == TokenType::LNOT && u.op1 &&
                u.op1->resolved_type &&
                u.op1->resolved_type->kind == TypeKind::Optional &&
                t1 == u.op1->resolved_type->get_elem()) {
                rhs_ctx_override = t1;
                data.op1 = u.op1;
                t1 = u.op1->resolved_type;
            }
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
            auto var_scope = scope.set_value_type(rhs_ctx_override ? rhs_ctx_override : t1);
            // For plain assignment, allow construct-into-target optimization.
            // For compound assignments (+=, -=, etc.), don't set move_outlet because
            // the old LHS value must be read by the operator method before being overwritten.
            if (data.op_type == TokenType::ASS) {
                var_scope = var_scope.set_move_outlet(data.op1);
            }
            t2 = resolve(data.op2, var_scope);
            if (scope.parent_fn_node) {
                auto lhs_decl = find_root_decl(data.op1);
                // `new Foo{...}` produces an owning pointer, not a borrow —
                // skip borrow edges so args inside `new` aren't treated as borrowed by the LHS.
                bool is_new_expr = data.op2->type == NodeType::ConstructExpr &&
                                   data.op2->data.construct_expr.is_new;
                if (lhs_decl && !scope.is_unsafe_block && !is_new_expr) {
                    auto &fn_def = *scope.parent_fn_def();
                    fn_def.bump_edge_offset(lhs_decl);
                    bool is_ref_target =
                        expr_creates_direct_borrow(this, data.op2, t2, t1, &scope);
                    add_borrow_source_edges(fn_def, data.op2, lhs_decl, is_ref_target);
                    copy_projection_summaries(fn_def, data.op2, lhs_decl, t1);
                    if (fn_def.flow.ref_edges.has_key(lhs_decl)) {
                        fn_def.add_terminal(lhs_decl);
                        if (data.op1->type == NodeType::DotExpr) {
                            auto *field_decl = data.op1->data.dot_expr.resolved_decl;
                            if (field_decl && field_decl->type == NodeType::VarDecl &&
                                field_decl->data.var_decl.is_field) {
                                auto *field_type = field_decl->resolved_type;
                                if (field_type &&
                                    type_may_propagate_borrow_deps(this, field_type)) {
                                    auto *root_type = node_get_type(lhs_decl);
                                    if (root_type) {
                                        auto *st = resolve_struct_type(root_type);
                                        if (st && st->this_lifetime) {
                                            fn_def.flow.terminal_lifetimes[lhs_decl] =
                                                st->this_lifetime;
                                        }
                                    }
                                }
                            }
                        }
                    }
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
            if (data.op_type == TokenType::QUES && t1->kind == TypeKind::Optional) {
                ctx_type = t1->get_elem();
            }
            auto op2_scope = scope.set_value_type(ctx_type);
            ast::Block rhs_narrow_block = {};
            if ((data.op_type == TokenType::LAND || data.op_type == TokenType::LOR) && scope.block &&
                scope.block->scope && scope.parent_fn_node) {
                array<ast::Node *> rhs_narrowables;
                collect_narrowables(data.op1, logical_rhs_uses_truthy_narrowing(data.op_type),
                                    rhs_narrowables);
                if (rhs_narrowables.len > 0) {
                    rhs_narrow_block.scope = m_ctx->allocator->create_scope(scope.block->scope);
                    for (auto ident : rhs_narrowables) {
                        auto var = create_narrowed_var(ident, node, scope);
                        rhs_narrow_block.scope->put(var->name, var);
                        data.rhs_narrow_vars.add(var);
                    }
                    op2_scope = op2_scope.set_block(&rhs_narrow_block);
                }
            }
            t2 = resolve(data.op2, op2_scope);
        }

        if (!t2) {
            return nullptr;
        }

        // For plain assignment
        if (data.op_type == TokenType::ASS) {
            check_assignment(data.op2, t2, t1, &scope);
            // NoCopy: error if assigning from a named value
            if (is_non_copyable(t1) && is_addressable(data.op2) &&
                !data.op2->analysis.moved && should_destroy(data.op2, t2)) {
                error(data.op2, errors::TYPE_NOT_COPYABLE, format_type_display(t1));
            }
            // RHS is a non-addressable temp (fn call, construct, etc.):
            // transfer ownership to the LHS — move, don't copy.
            mark_temp_moved_if_needed(data.op2, t2);
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
            check_assignment(data.op2, t2, t1, &scope);
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
            // Handle null in comparisons
            if (data.op_type == TokenType::EQ || data.op_type == TokenType::NE) {
                bool lhs_null = t1->kind == TypeKind::Null;
                bool rhs_null = t2->kind == TypeKind::Null;
                // Optional null check: ?T == null / null == ?T
                if ((lhs_null && t2->kind == TypeKind::Optional) ||
                    (rhs_null && t1->kind == TypeKind::Optional)) {
                    return get_system_types()->bool_;
                }
                if ((lhs_null && t2->is_reference()) || (rhs_null && t1->is_reference())) {
                    error(node, "references cannot be compared to null");
                    return nullptr;
                }
                // Optional-to-optional: reject (only null checks allowed)
                else if (t1->kind == TypeKind::Optional && t2->kind == TypeKind::Optional) {
                    error(node, "cannot compare optional values directly — "
                          "only null checks are allowed (e.g. x == null)");
                    return nullptr;
                }
                // Pointer null check: coerce null to pointer type
                else if (lhs_null && !rhs_null && can_assign(t1, t2)) {
                    data.op1->resolved_type = t2;
                    t1 = t2;
                } else if (rhs_null && !lhs_null && can_assign(t2, t1)) {
                    data.op2->resolved_type = t1;
                    t2 = t1;
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
            if (!data.resolved_call && t1->kind != TypeKind::Optional) {
                check_binary_op(node, data.op_type, t1);
            }
            return get_system_types()->bool_;
        }
        case TokenType::LAND:
        case TokenType::LOR:
            check_assignment(data.op1, t1, get_system_types()->bool_, &scope);
            check_assignment(data.op2, t2, get_system_types()->bool_, &scope);
            return get_system_types()->bool_;
        case TokenType::QUES: {
            if (t1->kind != TypeKind::Optional) {
                error(node, "left operand of ?? must be optional, got {}", format_type_display(t1));
                return nullptr;
            }
            auto elem_type = t1->get_elem();
            bool rhs_optional = t2->kind == TypeKind::Optional &&
                                is_same_type(t2->get_elem(), elem_type);
            auto target_type = rhs_optional ? t1 : elem_type;
            data.op2->analysis.conversion_type = resolve_conversion_type(t2, target_type);
            mark_temp_moved_if_needed(data.op2, t2);
            if (data.op2->analysis.moved) {
                node->analysis.moved = true;
            }
            check_assignment(data.op2, t2, target_type, &scope);
            return target_type;
        }
        default: {
            // Pointer arithmetic (requires unsafe)
            if (data.op_type == TokenType::ADD || data.op_type == TokenType::SUB) {
                bool lhs_ptr = t1->kind == TypeKind::Pointer;
                bool rhs_ptr = t2->kind == TypeKind::Pointer;
                bool lhs_int = t1->is_int();
                bool rhs_int = t2->is_int();

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
            else if (t1->is_int() && t2->is_int()) {
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
            check_assignment(data.op1, t1, result_type, &scope);
            check_assignment(data.op2, t2, result_type, &scope);

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
            if (t->kind != TypeKind::Float && !t->is_int()) {
                error(data.op1, errors::INVALID_OPERATOR, get_token_symbol(tt),
                      format_type_display(t));
                return nullptr;
            }
            return t;
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
            if (!t->is_int()) {
                error(data.op1, errors::INVALID_OPERATOR, get_token_symbol(tt),
                      format_type_display(t));
                return nullptr;
            }
            return t;
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
                }
                if (auto *narrowed = find_narrowed_optional_var(data.op1, scope)) {
                    if (data.op1->type == NodeType::Identifier) {
                        data.op1->data.identifier.decl = narrowed;
                    } else if (data.op1->type == NodeType::DotExpr) {
                        data.op1->data.dot_expr.narrowed_var = narrowed;
                    }
                    return t;
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
                check_assignment(data.op1, t, get_system_types()->bool_, &scope);
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
                    scope.parent_fn_def()->add_ref_edge(outlet, ref_target);
                }
            }
            if (scope.is_escaping) {
                auto decl = find_root_decl(data.op1);
                if (decl) {
                    decl->analysis.escaped = decl->can_escape();
                }
            }
            return get_pointer_type(t->eval(),
                                    data.op_type == TokenType::MUTREF
                                        ? TypeKind::MutRef
                                        : TypeKind::Reference);
        }
        case TokenType::MOVEREF: {
            if (!is_addressable(data.op1)) {
                error(node, errors::CANNOT_GET_REFERENCE_UNADDRESSABLE);
            }
            auto *src_decl = find_root_decl(data.op1);
            if (is_nonowning_alias_decl(src_decl)) {
                error(node, "cannot move from non-owning alias '{}'", src_decl->name);
            }
            if (scope.parent_fn_node && has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_SAFE)) {
                if (src_decl) {
                    auto &fn_def = *scope.parent_fn_def();
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
            auto *src_decl = find_root_decl(data.op1);
            if (is_nonowning_alias_decl(src_decl)) {
                error(node, "cannot move from non-owning alias '{}'", src_decl->name);
            }
            if (scope.parent_fn_node) {
                if (src_decl) {
                    auto &fn_def = *scope.parent_fn_def();
                    if (has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_SAFE) &&
                        fn_def.is_sunk(src_decl)) {
                        error(node, "'{}' used after move", src_decl->name);
                    }
                    // Sink the source at the move site — works for all contexts
                    // (return, assignment, function arg, etc.)
                    fn_def.add_sink_edge(src_decl, node);
                }
            }
            node->analysis.moved = true;
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
        bool is_try_await = contains_await(data.expr);
        auto try_value_type = expr_type;

        // Transform: try { block_with_await } catch [Type] → try await (async { block })() catch [Type]
        // This allows the existing try-await codegen to handle Result wrapping naturally.
        if (is_try_await && !data.catch_block && data.expr->type == NodeType::Block) {
            auto block_type = (expr_type->kind == TypeKind::Void) ? get_system_types()->unit : expr_type;
            auto promise_type = get_promise_type(block_type);

            // Create: async func() Promise<T> { block }
            auto decl_spec = get_allocator()->create_decl_spec();
            decl_spec->flags = ast::DECL_ASYNC;

            auto fn_proto = create_node(NodeType::FnProto);
            fn_proto->token = data.expr->token;
            fn_proto->module = node->module;

            auto fn_def = create_node(NodeType::FnDef);
            fn_def->token = data.expr->token;
            fn_def->module = node->module;
            fn_def->data.fn_def.fn_proto = fn_proto;
            fn_def->data.fn_def.body = data.expr;
            fn_def->data.fn_def.fn_kind = ast::FnKind::Lambda;
            fn_def->data.fn_def.is_generated = true;
            fn_def->data.fn_def.decl_spec = decl_spec;
            fn_def->data.fn_def.has_try = scope.parent_fn_def()->has_try;
            fn_proto->data.fn_proto.fn_def_node = fn_def;
            fn_def->parent_fn = scope.parent_fn_node;
            fn_def->name = fmt::format("{}__try_lambda_{}", scope.parent_fn_node->name, node->id);

            auto fn_type = create_type(TypeKind::Fn);
            fn_type->data.fn.return_type = promise_type;
            fn_def->resolved_type = fn_type;
            fn_proto->resolved_type = fn_type;

            // Create: lambda()
            auto call_expr = create_node(NodeType::FnCallExpr);
            call_expr->token = data.expr->token;
            call_expr->module = node->module;
            call_expr->data.fn_call_expr.fn_ref_expr = fn_def;
            call_expr->resolved_type = promise_type;

            // Create: await lambda()
            auto await_expr = create_node(NodeType::AwaitExpr);
            await_expr->token = data.expr->token;
            await_expr->module = node->module;
            await_expr->data.await_expr.expr = call_expr;
            await_expr->resolved_type = block_type;

            data.resolved_expr = await_expr;
            try_value_type = block_type;
        }
        auto resolve_catch_type = [&]() -> ChiType * {
            if (!data.catch_expr) {
                return nullptr;
            }
            auto catch_type = to_value_type(resolve(data.catch_expr, scope));
            if (catch_type && catch_type->kind == TypeKind::Struct) {
                auto rt_error = m_ctx->rt_error_type;
                if (rt_error && !catch_type->data.struct_.interface_table.get(rt_error)) {
                    error(data.catch_expr, errors::CATCH_NOT_ERROR,
                          format_type_display(catch_type));
                }
                return catch_type;
            }
            error(data.catch_expr, errors::CATCH_NOT_ERROR, format_type_display(catch_type));
            return nullptr;
        };
        if (!is_try_await && data.expr->type != NodeType::FnCallExpr && data.expr->type != NodeType::Block) {
            error(data.expr, errors::TRY_NOT_CALL);
        }
        scope.parent_fn_def()->has_try = true;

        if (data.catch_block) {
            // Catch block mode: try f() catch (...) { block } → yields T
            ChiType *err_var_type = nullptr;
            if (data.catch_expr) {
                auto catch_type = resolve_catch_type();
                if (catch_type) {
                    err_var_type = get_pointer_type(catch_type, TypeKind::Reference);
                }
            } else if (!is_try_await) {
                // catch-all: error binding is &Error (interface reference)
                err_var_type = get_pointer_type(m_ctx->rt_error_type, TypeKind::Reference);
            }

            // Set up error binding var in catch block scope
            if (data.catch_err_var && err_var_type) {
                data.catch_err_var->resolved_type = err_var_type;
                data.catch_err_var->parent_fn = scope.parent_fn_node;
                data.catch_err_var->data.var_decl.is_generated = true;
                data.catch_err_var->data.var_decl.initialized_at = node;
                data.catch_err_var->decl_order = scope.parent_fn_def()->next_decl_order++;
                auto &block_data = data.catch_block->data.block;
                block_data.implicit_vars.add(data.catch_err_var);
                block_data.scope->put(data.catch_err_var->name, data.catch_err_var);
            }

            // Fork flow state for branch-aware analysis
            assert(scope.parent_fn_node);
            auto *fn_def = scope.parent_fn_def();
            auto pre_catch = fn_def->flow.fork();

            resolve(data.catch_block, scope);

            if (always_terminates(data.catch_block)) {
                // Catch always exits: restore pre-catch flow
                fn_def->flow = pre_catch;
            } else {
                // Catch may fall through: merge with pre-catch (no-error path)
                fn_def->flow.merge(pre_catch);
            }
            return try_value_type;
        }

        if (is_try_await) {
            if (data.catch_expr) {
                resolve_catch_type();
            }
            return get_result_type(try_value_type, get_shared_type(m_ctx->rt_error_type));
        }

        if (data.catch_expr) {
            // No block, typed Result mode is a type filter over owned Shared<Error>.
            resolve_catch_type();
        }
        // try f() → Result<T, Shared<Error>>
        return get_result_type(try_value_type, get_shared_type(m_ctx->rt_error_type));
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
        node->analysis.conversion_type = resolve_conversion_type(from_type, dest_type, true);
        ensure_temp_owner(data.expr, from_type, scope);
        if (data.expr) {
            if (node->analysis.conversion_type == ast::ConversionType::OwningCoercion) {
                mark_temp_moved_if_needed(data.expr, from_type);
            }
            if (data.expr->analysis.moved) {
                node->analysis.moved = true;
            }
            if (is_addressable(data.expr) && !data.expr->analysis.moved &&
                is_non_copyable(from_type)) {
                error(data.expr, errors::TYPE_NOT_COPYABLE, format_type_display(from_type));
            }
        }
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
            return get_system_types()->null_;
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
                check_assignment(data.expr, expr_type, expected_type, &scope);
            }
        }

        // Track move in return expression (e.g. return move b)
        if (data.expr && scope.parent_fn_node) {
            track_move_sink(scope.parent_fn_node, data.expr, expr_type, node, return_type);
        }

        // Return-move optimization: transfer ownership instead of copy.
        // For named locals/params, skip their destruction at the return site.
        // For non-addressable temps (e.g. lambda expressions), avoid creating
        // a copy that would leak since the temp has no cleanup.
        if (data.expr && !data.expr->analysis.moved) {
            if (data.expr->type == NodeType::Identifier &&
                data.expr->data.identifier.kind == ast::IdentifierKind::Value) {
                auto *decl = data.expr->data.identifier.decl;
                if (decl &&
                    (decl->type == NodeType::VarDecl || decl->type == NodeType::ParamDecl)) {
                    bool is_field =
                        decl->type == NodeType::VarDecl && decl->data.var_decl.is_field;
                    if (!is_field && !is_nonowning_alias_decl(decl)) {
                        auto *var_type = node_get_type(decl);
                        if (var_type && type_needs_destruction(var_type)) {
                            data.expr->analysis.moved = true;
                        }
                    }
                }
            } else if (expr_type) {
                mark_temp_moved_if_needed(data.expr, expr_type);
            }
        }

        if (data.expr && scope.parent_fn_node && expr_type && !scope.is_unsafe_block) {
            auto &fn_def = *scope.parent_fn_def();
            bool is_ref = expr_type->is_reference();
            add_borrow_source_edges(fn_def, data.expr, node, is_ref);
            if (fn_def.flow.ref_edges.has_key(node) || fn_def.flow.copy_edges.has_key(node)) {
                fn_def.add_terminal(node);
            }
        }

        return return_type;
    }
    case NodeType::ThrowStmt: {
        auto &data = node->data.throw_stmt;
        auto expr_type = resolve(data.expr, scope);
        if (expr_type) {
            // The thrown value must be a reference to a struct implementing Error
            if (!expr_type->is_reference()) {
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
        auto type = resolve(child, scope);
        if (child && child->analysis.moved) {
            node->analysis.moved = true;
        }
        return type;
    }
    case NodeType::UnitExpr: {
        return get_system_types()->unit;
    }
    case NodeType::TupleExpr: {
        auto &data = node->data.tuple_expr;
        if (scope.move_outlet) {
            node->resolved_outlet = scope.move_outlet;
        }
        TypeList elements;
        auto *tuple_owner = scope.move_outlet ? scope.move_outlet : node;
        for (int32_t i = 0; i < data.items.len; i++) {
            auto *item = data.items[i];
            auto elem_type = resolve(item, scope);
            if (!elem_type) return nullptr;
            elements.add(elem_type);
            if (scope.parent_fn_node && !scope.is_unsafe_block) {
                auto &fn_def = *scope.parent_fn_def();
                add_borrow_source_edges(fn_def, item, tuple_owner, false);
                if (type_may_propagate_borrow_deps(this, elem_type)) {
                    auto *projection = get_projection_node(tuple_owner, i, nullptr);
                    add_borrow_source_edges(fn_def, item, projection, false);
                    copy_projection_summaries(fn_def, item, projection, elem_type);
                }
            }
        }
        return get_tuple_type(elements);
    }
    case NodeType::TypeInfoExpr: {
        auto &data = node->data.type_info_expr;
        auto expr_type = resolve(data.expr, scope, flags);
        if (!expr_type) {
            return nullptr;
        }
        ensure_temp_owner(data.expr, expr_type, scope);

        auto reflect_type_decl = get_reflect_type_decl(scope);
        if (!reflect_type_decl) {
            error(node, "failed to load std/reflect.Type");
            return nullptr;
        }

        auto reflect_type = reflect_type_decl->resolved_type;
        if (reflect_type && reflect_type->kind == TypeKind::TypeSymbol) {
            reflect_type = reflect_type->data.type_symbol.underlying_type;
        }
        if (!reflect_type || reflect_type->kind != TypeKind::Struct) {
            error(node, "std/reflect.Type is not a struct type");
            return nullptr;
        }

        ast::Node *ctor_node = nullptr;
        for (auto member : reflect_type_decl->data.struct_decl.members) {
            if (member->type == NodeType::FnDef &&
                member->data.fn_def.fn_kind == ast::FnKind::Constructor) {
                ctor_node = member;
                break;
            }
        }
        if (!ctor_node) {
            error(node, "std/reflect.Type.new(raw: *void) not found");
            return nullptr;
        }

        data.resolved_ctor = ctor_node;
        return reflect_type;
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
                ChiEnumVariant *member = nullptr;
                auto result_type = resolve_enum_member_type(underlying_type, field_name, &member);
                if (!member) {
                    error(node, errors::MEMBER_NOT_FOUND, field_name,
                          format_type_display(underlying_type));
                    return nullptr;
                }
                data.resolved_decl = member->node;
                data.field->node = member->node;
                data.resolved_dot_kind = DotKind::EnumVariant;
                return result_type;
            }
            case TypeKind::Subtype: {
                if (underlying_type->data.subtype.generic &&
                    underlying_type->data.subtype.generic->kind == TypeKind::Enum) {
                    ChiEnumVariant *member = nullptr;
                    auto result_type =
                        resolve_enum_member_type(underlying_type, field_name, &member);
                    if (!member) {
                        error(node, errors::MEMBER_NOT_FOUND, field_name,
                              format_type_display(underlying_type));
                        return nullptr;
                    }
                    data.resolved_decl = member->node;
                    data.field->node = member->node;
                    data.resolved_dot_kind = DotKind::EnumVariant;
                    return result_type;
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
                return finalize_member_fn_type(member, node);
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

                return finalize_fn_type(result_type, node);
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
        if (check_type->kind == TypeKind::Placeholder) {
            auto all_traits = get_placeholder_traits(check_type);
            for (auto trait_type : all_traits) {
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

        if (expr_type->kind == TypeKind::Pointer && !scope.is_unsafe_block) {
            error(node, "raw pointer member access requires unsafe block");
            return nullptr;
        }

        // Tuple field access: expr.0, expr.1, ...
        if (expr_type->kind == TypeKind::Tuple) {
            auto &elems = expr_type->data.tuple.elements;
            // Parse field name as integer index
            char *end;
            long idx = std::strtol(field_name.c_str(), &end, 10);
            if (*end == '\0' && idx >= 0 && idx < elems.len) {
                data.resolved_value = idx;
                data.resolved_dot_kind = DotKind::TupleField;
                if (scope.parent_fn_node && !scope.is_unsafe_block &&
                    type_may_propagate_borrow_deps(this, elems[idx])) {
                    auto *projection_source = data.narrowed_var ? data.narrowed_var : data.expr;
                    auto &fn_def = *scope.parent_fn_def();
                    auto *projection = get_expr_projection_node(node);
                    seed_projection_node(fn_def, projection_source, elems[idx], projection, idx,
                                         nullptr);
                    fn_def.copy_ref_edges(node, projection, false);
                }
                return elems[idx];
            }
            error(node, errors::MEMBER_NOT_FOUND, field_name, format_type_display(expr_type));
            return nullptr;
        }

        auto stype = eval_struct_type(expr_type, node);
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
                stype = eval_struct_type(expr_type, node);
            }
        }
        auto *access_expr = data.effective_expr();
        auto is_internal = scope.parent_struct && is_friend_struct(scope.parent_struct, stype);
        auto access_check_type = get_struct_access_root_type(access_expr);
        if (!access_check_type) {
            access_check_type = expr_type;
        }
        auto member = get_struct_member_access(node, stype, field_name, is_internal, scope.is_lhs,
                                               &scope, access_check_type);
        if (!member) {
            return nullptr;
        }
        data.resolved_struct_member = member;
        data.resolved_decl = member->node;
        data.field->node = member->node;
        auto fn_result_type = finalize_member_fn_type(member, node);
        member->resolved_type = fn_result_type;
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

        // Method accessed as a value (not in a call context): synthesize a
        // lambda that captures the receiver.
        if (member->is_method() && member->resolved_type &&
            member->resolved_type->kind == TypeKind::Fn && !scope.is_fn_call) {
            assert(member->node && "method-as-value requires a method def");
            auto *lambda_def = synthesize_method_lambda(node, member, scope);
            node->resolved_node = lambda_def;
            node->resolved_type = lambda_def->resolved_type;
            return lambda_def->resolved_type;
        }

        auto result_type = member->resolved_type;

        // Check for narrowing redirect (path-based lookup)
        // Skip LHS of assignments — writes should go to the original optional field
        if (scope.block && !member->is_method() && !scope.is_lhs) {
            auto path = build_narrowing_path(node);
            if (!path.empty()) {
                auto narrowed = scope.block->scope->find_one(path, true);
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
        if (scope.parent_fn_node && !scope.is_unsafe_block && !member->is_method() &&
            type_may_propagate_borrow_deps(this, result_type)) {
            auto *projection_source = data.narrowed_var ? data.narrowed_var : data.effective_expr();
            auto &fn_def = *scope.parent_fn_def();
            auto *projection = get_expr_projection_node(node);
            seed_projection_node(fn_def, projection_source, result_type, projection, -1, member);
            fn_def.copy_ref_edges(node, projection, false);
        }
        return result_type;
    }
    case NodeType::ConstructExpr: {
        auto &data = node->data.construct_expr;
        data.use_list_init = false;
        data.use_alloc_init = false;
        data.resolved_type_source = ast::ResolvedTypeSourceKind::None;
        auto dest_type = scope.value_type; // Save before resolve calls modify scope
        if (scope.move_outlet && !data.is_new) {
            node->resolved_outlet = scope.move_outlet;
            node->analysis.moved = true;
        }
        ChiType *value_type;
        ChiType *result_type;
        if (data.type) {
            if (data.type->type == NodeType::Identifier) {
                auto variant = find_expected_enum_variant(data.type->name, scope.value_type);
                if (variant) {
                    auto &ident = data.type->data.identifier;
                    ident.decl = variant->node;
                    ident.kind = ast::IdentifierKind::Value;
                    ident.decl_is_provisional = false;
                    value_type = resolve_expected_enum_variant_type(variant, scope.value_type);
                } else {
                    value_type = resolve_value(data.type, scope);
                }
            } else {
                value_type = resolve_value(data.type, scope);
            }
            if (!value_type) {
                return nullptr;
            }
            if (data.items.len == 0 && value_type->kind == TypeKind::Placeholder) {
                bool has_construct_bound = false;
                for (auto t : get_placeholder_traits(value_type)) {
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
            bool is_contextual = false;
            data.resolved_type_is_ambiguous = false;
            if (dest_type) {
                if (is_same_type(result_type, dest_type)) {
                    is_contextual = true;
                } else if (data.type->type == NodeType::DotExpr) {
                    auto *resolved_decl = data.type->data.dot_expr.resolved_decl;
                    auto *resolved_member =
                        (result_type && result_type->eval() &&
                         result_type->eval()->kind == TypeKind::EnumValue)
                            ? result_type->eval()->data.enum_value.member
                            : nullptr;
                    if (resolved_decl && resolved_decl->type == NodeType::EnumVariant) {
                        resolved_member = resolved_decl->data.enum_variant.resolved_enum_variant;
                    }
                    if (resolved_member) {
                        auto *expected_variant =
                            find_expected_enum_variant(data.type->data.dot_expr.field->str, dest_type);
                        auto *result_enum_root = get_enum_root(result_type);
                        auto *dest_enum_root = get_enum_root(dest_type);
                        bool same_variant = expected_variant && resolved_member &&
                                            expected_variant->name == resolved_member->name &&
                                            result_enum_root && dest_enum_root &&
                                            result_enum_root == dest_enum_root;
                        if (same_variant) {
                            is_contextual = true;
                            data.resolved_type_is_ambiguous = is_contextual_resolution_ambiguous(
                                data.type->data.dot_expr.field->str, resolved_member->node, scope);
                        }
                    }
                }
            }
            data.resolved_type_source = is_contextual ? ast::ResolvedTypeSourceKind::Contextual
                                                      : ast::ResolvedTypeSourceKind::Explicit;
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
                    data.resolved_type_source = ast::ResolvedTypeSourceKind::Inferred;
                } else {
                    error(node, errors::CONSTRUCT_CANNOT_INFER_TYPE);
                    return nullptr;
                }
            } else {
                result_type = scope.value_type;
                data.resolved_type_source = ast::ResolvedTypeSourceKind::Contextual;
                {
                    // Empty construct on unresolved Infer type — cannot determine type
                    if (data.items.len == 0 && result_type->kind == TypeKind::Infer &&
                        !result_type->data.infer.inferred_type) {
                        error(node, errors::CONSTRUCT_CANNOT_INFER_TYPE);
                        return nullptr;
                    }
                    // Empty construct on placeholder (= {}) requires a constructor
                    // interface bound whose new() has zero params
                    if (data.items.len == 0 && result_type->kind == TypeKind::Placeholder) {
                        bool has_construct_bound = false;
                        for (auto t : get_placeholder_traits(result_type)) {
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
        // Empty construct of Unit type → rewrite to UnitExpr for formatter
        if (value_type && value_type->kind == TypeKind::Unit && data.items.len == 0 &&
            !data.field_inits.len && !data.spread_expr) {
            node->type = NodeType::UnitExpr;
            return value_type;
        }

        // FixedArray construct: [N]T{items...} or array literal assigned to [N]T
        if (value_type->kind == TypeKind::FixedArray) {
            auto elem_type = value_type->data.fixed_array.elem;
            auto fa_size = value_type->data.fixed_array.size;
            if ((uint32_t)data.items.len > fa_size) {
                error(node, "too many items for [{}]{}: got {}, max {}", fa_size,
                      format_type_display(elem_type), data.items.len, fa_size);
            }
            for (auto item : data.items) {
                auto item_scope = scope.set_value_type(elem_type);
                item_scope.move_outlet = nullptr; // Items need separate temporaries
                auto item_type = resolve(item, item_scope);
                if (item_type) {
                    check_assignment(item, item_type, elem_type, &scope);
                }
            }
            return result_type;
        }

        auto struct_type = resolve_struct_type(value_type);
        auto *list_init_member_p =
            struct_type ? struct_type->member_intrinsics.get(IntrinsicSymbol::ListInit) : nullptr;
        auto *kv_init_member_p =
            struct_type ? struct_type->member_intrinsics.get(IntrinsicSymbol::KvInit) : nullptr;
        auto *alloc_init_member_p =
            struct_type ? struct_type->member_intrinsics.get(IntrinsicSymbol::AllocInit) : nullptr;
        auto constructor = struct_type ? struct_type->get_constructor() : nullptr;
        bool use_list_init = list_init_member_p && data.items.len > 0 && !data.field_inits.len &&
                             !data.spread_expr;
        // Detect kv init: all field_inits have key_expr (string: value pairs)
        bool has_kv_entries = data.field_inits.len > 0 && data.items.len == 0 &&
                              data.field_inits[0]->data.field_init_expr.key_expr != nullptr;
        bool use_kv_init = kv_init_member_p && has_kv_entries;
        bool use_alloc_init =
            has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_MANAGED) && alloc_init_member_p;
        data.use_list_init = use_list_init;
        data.use_kv_init = use_kv_init;
        data.use_alloc_init = use_alloc_init;
        if (has_kv_entries && !use_kv_init) {
            error(node, "type '{}' does not support key-value initialization (missing KvInit implementation)",
                  format_type_display(value_type));
            return nullptr;
        }
        if (use_list_init) {
            auto *list_init_member = *list_init_member_p;
            assert(list_init_member && "ListInit member missing");
            if (constructor) {
                auto is_internal =
                    scope.parent_struct && is_friend_struct(scope.parent_struct, value_type);
                if (!constructor->check_access(is_internal, false)) {
                    error(node, errors::PRIVATE_MEMBER_NOT_ACCESSIBLE, "new",
                          format_type_display(value_type));
                    return nullptr;
                }
                auto &fn_type = constructor->resolved_type->data.fn;
                NodeList empty_args = {};
                resolve_fn_call(node, scope, &fn_type, &empty_args, constructor->node);
            }

            auto &list_init_fn = list_init_member->resolved_type->data.fn;
            resolve_fn_call(node, scope, &list_init_fn, &data.items, list_init_member->node);

            if (scope.parent_fn_node && !scope.is_unsafe_block) {
                auto &fn_def = *scope.parent_fn_def();
                for (auto item : data.items) {
                    add_borrow_source_edges(fn_def, item, node, false);
                }
            }
            return result_type;
        }
        if (use_kv_init) {
            auto *kv_init_member = *kv_init_member_p;
            assert(kv_init_member && "KvInit member missing");
            if (constructor) {
                auto is_internal =
                    scope.parent_struct && is_friend_struct(scope.parent_struct, value_type);
                if (!constructor->check_access(is_internal, false)) {
                    error(node, errors::PRIVATE_MEMBER_NOT_ACCESSIBLE, "new",
                          format_type_display(value_type));
                    return nullptr;
                }
                auto &fn_type = constructor->resolved_type->data.fn;
                NodeList empty_args = {};
                resolve_fn_call(node, scope, &fn_type, &empty_args, constructor->node);
            }

            auto &kv_init_fn = kv_init_member->resolved_type->data.fn;
            for (auto fi : data.field_inits) {
                auto &fi_data = fi->data.field_init_expr;
                assert(fi_data.key_expr && "kv init entry missing key_expr");
                // Resolve key and value as args to kv_init(key, value)
                NodeList kv_args = {};
                kv_args.add(fi_data.key_expr);
                kv_args.add(fi_data.value);
                resolve_fn_call(fi, scope, &kv_init_fn, &kv_args, kv_init_member->node);
            }

            if (scope.parent_fn_node && !scope.is_unsafe_block) {
                auto &fn_def = *scope.parent_fn_def();
                for (auto fi : data.field_inits) {
                    auto &fi_data = fi->data.field_init_expr;
                    add_borrow_source_edges(fn_def, fi_data.key_expr, node, false);
                    add_borrow_source_edges(fn_def, fi_data.value, node, false);
                }
            }
            return result_type;
        }
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

            // Track copy-edge propagation from constructor arguments into the
            // constructed value using the constructor's saved `this <- param`
            // summary. This handles generic/value params too.
            if (scope.parent_fn_node && !scope.is_unsafe_block) {
                auto &fn_def = *scope.parent_fn_def();
                auto *ctor_proto = get_decl_summary_proto(constructor->node);
                assert(ctor_proto && "constructor summary proto missing");
                if (ctor_proto->copy_edge_summary_valid) {
                    for (auto idx : ctor_proto->this_copy_edge_param_indices) {
                        if (idx >= 0 && idx < static_cast<int32_t>(data.items.len)) {
                            add_borrow_source_edges(
                                fn_def, data.items[static_cast<uint32_t>(idx)], node, false);
                        }
                    }
                }
            }
        } else if (value_type->kind == TypeKind::Placeholder && data.items.len > 0) {
            // Placeholder type with positional args: resolve args against
            // a matching constructor interface bound's new() signature
            ChiStructMember *iface_new = nullptr;
            for (auto t : get_placeholder_traits(value_type)) {
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
            auto payload_fields = get_enum_payload_fields(value_type);
            if (payload_fields.len > 0 && data.items.len > 0) {
                if (data.items.len != payload_fields.len) {
                    error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, payload_fields.len,
                          data.items.len);
                    return nullptr;
                }
                for (uint32_t i = 0; i < data.items.len; i++) {
                    auto item = data.items[i];
                    auto field = payload_fields[i];
                    auto item_scope = scope.set_value_type(field->resolved_type);
                    auto item_type = resolve(item, item_scope, flags);
                    check_assignment(item, item_type, field->resolved_type, &scope);
                    if (scope.parent_fn_node && !scope.is_unsafe_block) {
                        auto &fn_def = *scope.parent_fn_def();
                        auto *construct_owner = scope.move_outlet ? scope.move_outlet : node;
                        add_borrow_source_edges(fn_def, item, construct_owner, false);
                        if (type_may_propagate_borrow_deps(this, field->resolved_type)) {
                            auto *projection = get_projection_node(construct_owner, -1, field);
                            add_borrow_source_edges(fn_def, item, projection, false);
                            copy_projection_summaries(fn_def, item, projection,
                                                      field->resolved_type);
                        }
                    }
                }
            } else if (result_type->kind == TypeKind::Optional) {
                if (data.items.len != 1) {
                    error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, 1, data.items.len);
                    return nullptr;
                }
                auto item = data.items[0];
                auto item_type = resolve(item, scope, flags);
                check_assignment(item, item_type, result_type->get_elem(), &scope);
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
            check_assignment(data.value, init_value_type, field_member->resolved_type, &scope);

            if (scope.parent_fn_node && !scope.is_unsafe_block) {
                auto outlet = scope.move_outlet ? scope.move_outlet : node;
                auto &fn_def = *scope.parent_fn_def();
                add_borrow_source_edges(fn_def, data.value, outlet, false);
                if (type_may_propagate_borrow_deps(this, field_member->resolved_type)) {
                    auto *projection = get_projection_node(outlet, -1, field_member);
                    add_borrow_source_edges(fn_def, data.value, projection, false);
                    copy_projection_summaries(fn_def, data.value, projection,
                                              field_member->resolved_type);
                }
            }
        }

        if (scope.parent_fn_node && data.spread_expr && value_type &&
            value_type->kind == TypeKind::Struct && !scope.is_unsafe_block) {
            std::set<long> provided_fields;
            for (auto *field_init : data.field_inits) {
                auto *member = field_init->data.field_init_expr.resolved_field;
                if (member && member->field_index >= 0) {
                    provided_fields.insert(member->field_index);
                }
            }
            auto &fn_def = *scope.parent_fn_def();
            auto *construct_owner = scope.move_outlet ? scope.move_outlet : node;
            for (auto *field : value_type->data.struct_.own_fields()) {
                if (field->field_index < 0 || provided_fields.count(field->field_index) > 0 ||
                    !type_may_propagate_borrow_deps(this, field->resolved_type)) {
                    continue;
                }
                seed_projection_node(fn_def, data.spread_expr, field->resolved_type,
                                     get_projection_node(construct_owner, -1, field), -1, field);
            }
        }

        // Enforce struct lifetime outlives bounds at construction time.
        // For 'a: 'b, the source assigned to 'a must outlive the source assigned to 'b.
        // Add ref edges from 'b field inits to 'a field inits so 'a sources are checked
        // wherever 'b flows.
        if (scope.parent_fn_node && value_type && value_type->kind == TypeKind::Struct) {
            auto &st = value_type->data.struct_;
            if (st.lifetime_params.len > 0) {
                // Map each lifetime param to its field init source decls
                map<ChiLifetime *, array<ast::Node *>> lt_to_sources;
                for (auto field_init : data.field_inits) {
                    auto &fi = field_init->data.field_init_expr;
                    if (!fi.resolved_field) continue;
                    auto field_type = to_value_type(fi.resolved_field->resolved_type);
                    if (!field_type || !field_type->is_lifetime_reference()) continue;
                    auto *src = find_root_decl(fi.value);
                    if (!src) continue;
                    auto *lifetimes = field_type->get_lifetimes();
                    if (!lifetimes) continue;
                    for (auto *lt : *lifetimes) {
                        lt_to_sources[lt].add(src);
                    }
                }
                // Find the destination variable for 'this lifetime checks
                auto *outlet = scope.move_outlet ? find_root_decl(scope.move_outlet) : nullptr;

                // For 'a: 'b, each 'a source must outlive each 'b source.
                // For 'a: 'this, each 'a source must outlive the struct instance.
                // In LIFO order: 'a source must be declared before 'b source.
                auto &fn_def = *scope.parent_fn_def();
                for (auto *lt_a : st.lifetime_params) {
                    auto *a_sources = lt_to_sources.get(lt_a);
                    if (!a_sources) continue;
                    for (auto *lt_b : lt_a->outlives) {
                        // Resolve the target nodes for this bound
                        array<ast::Node *> this_target;
                        array<ast::Node *> *b_targets;
                        if (lt_b->kind == LifetimeKind::This) {
                            if (!outlet || outlet->decl_order < 0) continue;
                            this_target.add(outlet);
                            b_targets = &this_target;
                        } else {
                            b_targets = lt_to_sources.get(lt_b);
                            if (!b_targets) continue;
                        }
                        for (auto *a_src : *a_sources) {
                            for (auto *b_src : *b_targets) {
                                if (a_src->decl_order >= 0 && b_src->decl_order >= 0 &&
                                    a_src->decl_order >= b_src->decl_order) {
                                    array<Note> notes;
                                    notes.add({"referenced here", node->token->pos});
                                    error_with_notes(a_src, std::move(notes),
                                                     "'{}' does not live long enough",
                                                     a_src->name);
                                }
                            }
                        }
                        // For 'this: add ref edges so the borrow checker tracks reassignment
                        if (lt_b->kind == LifetimeKind::This) {
                            for (auto *a_src : *a_sources) {
                                fn_def.add_ref_edge(outlet, a_src);
                            }
                        }
                    }
                }
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
        // Resolve the inner expression - it should be a homogeneous variadic slice
        auto &data = node->data.pack_expansion;
        auto expr_type = resolve(data.expr, scope);
        if (!expr_type) {
            return nullptr;
        }

        // Variadics inside Chi functions are exposed as &[T], but pack expansion also accepts
        // Array<T> sources for compatibility.
        auto val_type = to_value_type(expr_type);
        if (val_type->kind != TypeKind::Span && val_type->kind != TypeKind::Array) {
            error(node, "pack expansion can only be used on variadic parameters (Array<T> or &[T])");
            return nullptr;
        }

        // The type of the pack expansion is the source sequence type
        return expr_type;
    }
    case NodeType::FnCallExpr: {
        auto &data = node->data.fn_call_expr;
        if (scope.parent_fn_node && !scope.is_unsafe_block) {
            scope.parent_fn_def()->call_sites.add(node);
        }
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
        auto fn_symbol =
            fn_decl ? resolve_intrinsic_symbol(fn_decl) : IntrinsicSymbol::None;

        // Unsafe functions cannot be called in safe mode (unless inside an unsafe block)
        if (fn_decl && fn_decl->type == NodeType::FnDef &&
            fn_decl->data.fn_def.decl_spec->is_unsafe() &&
            has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_SAFE) && !scope.is_unsafe_block) {
            error(node, errors::UNSAFE_CALL_IN_SAFE_MODE, fn_decl->name);
        }

        auto result = resolve_fn_call(node, scope, &fn, &data.args, fn_decl);

        // mem move intrinsic: sink the source variable so it's not destroyed at scope exit
        if (fn_symbol == IntrinsicSymbol::MemMove && scope.parent_fn_node) {
            assert(data.args.len > 1 && "intrinsic mem move missing source argument");
            auto *src_decl = find_root_decl(data.args[1]);
            if (src_decl) {
                scope.parent_fn_def()->add_sink_edge(src_decl, node);
            }
        }

        // std/mem.annotate_copy:
        // compiler-only marker that the owner copies the borrow dependencies of value into itself.
        // No runtime effect.
        if (fn_symbol == IntrinsicSymbol::AnnotateCopy && scope.parent_fn_node) {
            assert(data.args.len > 1 && "intrinsic annotate_copy missing argument");
            auto *owner_expr = unwrap_lifetime_copy_intrinsic_arg(data.args[0]);
            auto *value_expr = unwrap_lifetime_copy_intrinsic_arg(data.args[1]);
            auto *owner_root = owner_expr ? find_root_decl(owner_expr) : nullptr;
            if (owner_root && value_expr) {
                add_borrow_source_edges(*scope.parent_fn_def(), value_expr, owner_root,
                                        false);
            }
        }

        // 'static lifetime params: check callee's param borrow_lifetimes for 'static.
        // Void calls don't trigger add_call_borrow_edges, so we check here.
        if (scope.parent_fn_node && !scope.is_unsafe_block) {
            ast::FnProto *callee_proto =
                data.generated_fn ? get_decl_fn_proto(data.generated_fn) : get_decl_fn_proto(fn_decl);
            if (callee_proto) {
                auto &fn_def = *scope.parent_fn_def();
                for (size_t i = 0; i < callee_proto->params.len && i < data.args.len; i++) {
                    auto *lt = callee_proto->params[i]->data.param_decl.borrow_lifetime;
                    if (lt && lt->kind == LifetimeKind::Static) {
                        auto *arg = data.args[i];
                        if (fn_def.flow.ref_edges.has_key(arg)) {
                            fn_def.add_terminal(arg);
                            fn_def.flow.terminal_lifetimes[arg] = lt;
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
            node->analysis.moved = true;
        }

        auto cond_type = resolve(data.condition, scope);
        if (data.binding_decl) {
            if (data.binding_clause) {
                auto switch_enum = get_enum_type(cond_type);
                if (!switch_enum || switch_enum->kind != TypeKind::Enum) {
                    error(data.condition, "if let enum pattern requires an enum expression, got '{}'",
                          cond_type ? format_type_display(cond_type) : "<unknown>");
                } else {
                    auto clause_scope = scope.set_value_type(cond_type);
                    auto clause_type = resolve(data.binding_clause, clause_scope);
                    if (!clause_type || clause_type->kind != TypeKind::EnumValue) {
                        error(data.binding_clause,
                              "if let enum pattern requires an enum variant clause");
                    } else if (get_enum_root(clause_type) != get_enum_root(cond_type)) {
                        error(data.binding_clause, "enum variant '{}' does not belong to '{}'",
                              format_type_display(clause_type), format_type_display(cond_type));
                    } else if (data.binding_decl->type != NodeType::DestructureDecl) {
                        error(data.binding_decl,
                              "if let enum pattern currently requires destructuring syntax");
                    } else {
                        ast::Node *narrow_source = data.condition;
                        if (auto temp_var = ensure_temp_owner(data.condition, cond_type, scope,
                                                              true)) {
                            auto temp_ident = create_node(ast::NodeType::Identifier);
                            temp_ident->token = temp_var->token;
                            temp_ident->name = temp_var->name;
                            temp_ident->module = node->module;
                            temp_ident->parent_fn = scope.parent_fn_node;
                            temp_ident->data.identifier.decl = temp_var;
                            temp_ident->resolved_type = cond_type;
                            narrow_source = temp_ident;
                        }

                        auto &block_data = data.then_block->data.block;
                        auto binding =
                            create_narrowed_var(narrow_source, node, scope, clause_type);
                        if (binding->name.empty()) {
                            binding->name = "__if_let_variant";
                        }
                        block_data.implicit_vars.add(binding);
                        block_data.scope->put(binding->name, binding);

                        auto ident = create_node(NodeType::Identifier);
                        ident->token = narrow_source->token;
                        ident->module = narrow_source->module;
                        ident->parent_fn = scope.parent_fn_node;
                        ident->name = binding->name;
                        ident->data.identifier.decl = binding;
                        ident->data.identifier.kind = ast::IdentifierKind::Value;
                        ident->resolved_type = clause_type;

                        auto pattern = data.binding_decl;
                        pattern->data.destructure_decl.resolved_expr = ident;

                        auto then_scope = scope.set_block(&block_data);
                        resolve(pattern, then_scope);
                        block_data.implicit_vars.add(pattern);
                    }
                }
            } else if (!cond_type || cond_type->kind != TypeKind::Optional) {
                error(data.condition, "if let/var requires an optional expression, got '{}'",
                      cond_type ? format_type_display(cond_type) : "<unknown>");
            } else {
                ast::Node *narrow_source = data.condition;
                if (auto temp_var = ensure_temp_owner(data.condition, cond_type, scope, true)) {
                    auto temp_ident = create_node(ast::NodeType::Identifier);
                    temp_ident->token = temp_var->token;
                    temp_ident->name = temp_var->name;
                    temp_ident->module = node->module;
                    temp_ident->parent_fn = scope.parent_fn_node;
                    temp_ident->data.identifier.decl = temp_var;
                    temp_ident->resolved_type = cond_type;
                    narrow_source = temp_ident;
                }
                auto &block_data = data.then_block->data.block;
                if (data.binding_decl->type == NodeType::DestructureDecl) {
                    auto pattern = data.binding_decl;
                    auto unwrap_expr = create_node(ast::NodeType::UnaryOpExpr);
                    unwrap_expr->token = narrow_source->token;
                    unwrap_expr->module = node->module;
                    unwrap_expr->parent_fn = scope.parent_fn_node;
                    unwrap_expr->data.unary_op_expr.is_suffix = true;
                    unwrap_expr->data.unary_op_expr.op_type = TokenType::LNOT;
                    unwrap_expr->data.unary_op_expr.op1 = narrow_source;
                    unwrap_expr->resolved_type = cond_type->get_elem();
                    pattern->data.destructure_decl.resolved_expr = unwrap_expr;
                    auto then_scope = scope.set_block(&block_data);
                    resolve(pattern, then_scope);
                    block_data.implicit_vars.add(pattern);
                } else {
                    auto binding =
                        create_narrowed_var(narrow_source, node, scope, cond_type->get_elem());
                    binding->name = data.binding_decl->name;
                    binding->token = data.binding_decl->token;
                    binding->module = data.binding_decl->module;
                    binding->parent_fn = scope.parent_fn_node;
                    binding->data.var_decl.identifier = data.binding_decl->data.var_decl.identifier;
                    binding->data.var_decl.kind = data.binding_decl->data.var_decl.kind;
                    if (binding->data.var_decl.identifier) {
                        binding->data.var_decl.identifier->node = binding;
                    }
                    block_data.implicit_vars.add(binding);
                    block_data.scope->put(binding->name, binding);
                }
            }
        } else {
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
        }

        if (!data.binding_clause) {
            check_assignment(data.condition, cond_type, get_system_types()->bool_, &scope);
        }

        // Fork flow state for branch-aware analysis
        assert(scope.parent_fn_node);
        auto *fn_def = scope.parent_fn_def();
        auto pre_branch = fn_def->flow.fork();

        auto then_type = resolve(data.then_block, scope);
        bool then_terminates = always_terminates(data.then_block);

        ChiType *else_type = nullptr;
        if (data.else_node) {
            auto then_flow = fn_def->flow.fork();
            fn_def->flow = pre_branch;
            else_type = resolve(data.else_node, scope);
            bool else_terminates = always_terminates(data.else_node);
            if (then_terminates && else_terminates) {
                // Both terminate: doesn't matter, nothing runs after
            } else if (then_terminates) {
                // Only then terminates: else flow continues
            } else if (else_terminates) {
                // Only else terminates: then flow continues
                fn_def->flow = then_flow;
            } else {
                // Neither terminates: merge both
                fn_def->flow.merge(then_flow);
            }
        } else {
            if (then_terminates) {
                // Guard clause: then always exits, restore pre-branch
                fn_def->flow = pre_branch;
            } else {
                // Then may fall through: merge with pre-branch (no-else path)
                fn_def->flow.merge(pre_branch);
            }
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

        if (data.else_node && then_type && else_type) {
            bool then_is_value = then_type->kind != TypeKind::Void;
            bool else_is_value = else_type->kind != TypeKind::Void;

            if (then_is_value != else_is_value) {
                error(node,
                      "if expression branches must both produce values or both be void, got "
                      "'{}' and '{}'",
                      format_type_display(then_type), format_type_display(else_type));
                return get_system_types()->void_;
            }

            if (then_is_value) {
                auto result_type =
                    resolve_common_value_type(then_type, else_type, scope.value_type);
                auto *then_value = get_assignment_expr_node(data.then_block);
                auto *else_value = get_assignment_expr_node(data.else_node);
                check_assignment(then_value, then_type, result_type, &scope);
                check_assignment(else_value, else_type, result_type, &scope);
                return result_type;
            }

            return get_system_types()->void_;
        }

        return nullptr;
    }
    case NodeType::WhileStmt: {
        auto &data = node->data.while_stmt;
        if (data.condition) {
            auto cond_type = resolve(data.condition, scope);
            ensure_temp_owner(data.condition, cond_type, scope);
            check_assignment(data.condition, cond_type, get_system_types()->bool_, &scope);
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
        // Snapshot flow state and run lifetime checks at block exit
        assert(scope.parent_fn_node);
        auto snapshot_flow = [&]() {
            auto &fn_def = *scope.parent_fn_def();
            data.exit_flow = fn_def.flow.fork();
            check_lifetime_constraints(&fn_def, data.exit_flow);
        };
        auto stamp_new_stmt_temps = [&](size_t before, int stmt_idx) {
            for (size_t t = before; t < data.stmt_temp_vars.len; t++) {
                data.stmt_temp_vars[t]->data.var_decl.stmt_owner_index = stmt_idx;
            }
        };
        for (int i = 0; i < (int)data.statements.len; i++) {
            auto stmt = data.statements[i];
            size_t temps_before = data.stmt_temp_vars.len;
            auto stmt_type = resolve(stmt, child_scope);
            ensure_temp_owner(stmt, stmt_type, child_scope);
            stamp_new_stmt_temps(temps_before, i);
        }
        if (data.return_expr) {
            size_t temps_before = data.stmt_temp_vars.len;
            auto type = resolve(data.return_expr, child_scope);
            if (!type || type->kind == TypeKind::Void) {
                // Not value-producing (e.g. void if/switch) — reclassify as statement
                ensure_temp_owner(data.return_expr, type, child_scope);
                data.return_expr->index = data.statements.len;
                data.statements.add(data.return_expr);
                data.return_expr = nullptr;
                stamp_new_stmt_temps(temps_before, data.statements.len - 1);
                snapshot_flow();
                return get_system_types()->void_;
            }
            stamp_new_stmt_temps(temps_before, data.statements.len);
            snapshot_flow();
            return type;
        }
        snapshot_flow();
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
            enum_type->is_placeholder = false;
            for (auto param : enum_type->data.enum_.type_params) {
                if (to_value_type(param)->is_placeholder) {
                    enum_type->is_placeholder = true;
                    break;
                }
            }
            value_type->is_placeholder = enum_type->is_placeholder;

            if (data.base_struct) {
                auto base_struct_type = resolve(data.base_struct, scope);
                auto base_struct = eval_struct_type(base_struct_type);
                enum_type->data.enum_.base_struct = base_struct;
            }
            if (scope.module->package->kind == ast::PackageKind::BUILTIN) {
                if (node->name == "Result") {
                    m_ctx->rt_result_type = enum_type;
                }
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
                        auto new_member = base_value_struct->add_member(
                            get_allocator(), base_member->get_name(), base_member->node,
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

                // Determine if this is a plain enum (no variant carries data)
                enum_data.is_plain = true;
                if (data.base_struct) {
                    auto base_struct = eval_struct_type(data.base_struct->resolved_type);
                    for (auto member : base_struct->data.struct_.members) {
                        if (member->is_field()) {
                            enum_data.is_plain = false;
                            break;
                        }
                    }
                }
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
            // Process explicit lifetime params
            for (auto lt_node : data.lifetime_params) {
                ChiLifetime *lt;
                if (lt_node->name == "static") {
                    lt = m_ctx->static_lifetime;
                } else {
                    lt = new ChiLifetime{lt_node->name, LifetimeKind::Param, nullptr, struct_type};
                }
                struct_->lifetime_params.add(lt);
            }
            // Wire up outlives bounds
            for (size_t i = 0; i < data.lifetime_params.len; i++) {
                auto &bound = data.lifetime_params[i]->data.lifetime_param.bound;
                if (!bound.empty()) {
                    for (size_t j = 0; j < struct_->lifetime_params.len; j++) {
                        if (data.lifetime_params[j]->name == bound) {
                            struct_->lifetime_params[i]->outlives.add(struct_->lifetime_params[j]);
                            break;
                        }
                    }
                }
            }
            // Implicit 'this bound: all lifetime params outlive the struct instance
            if (struct_->lifetime_params.len > 0) {
                if (!struct_->this_lifetime) {
                    struct_->this_lifetime =
                        new ChiLifetime{"this", LifetimeKind::This, nullptr, struct_type};
                }
                for (auto *lt : struct_->lifetime_params) {
                    if (lt->kind != LifetimeKind::Static) {
                        lt->outlives.add(struct_->this_lifetime);
                    }
                }
            }
            for (auto param : data.type_params) {
                struct_->type_params.add(resolve(param, scope));
            }

            // Build where conditions on impl blocks early (before member resolution)
            // so that subtypes created during other structs' field resolution can
            // filter members by where condition.
            for (auto member : data.members) {
                if (member->type != NodeType::ImplementBlock)
                    continue;
                auto &impl_data = member->data.implement_block;
                if (impl_data.where_clauses.len == 0)
                    continue;
                impl_data.resolved_where_cond =
                    build_where_condition(impl_data, struct_, scope);
            }

            struct_->resolve_status = ResolveStatus::None;
            type_sym = create_type_symbol(node->name, struct_type);
            auto package_kind = scope.module->package->kind;
            if (package_kind == ast::PackageKind::BUILTIN) {
                if (node->name == "Array") {
                    m_ctx->rt_array_type = struct_type;
                } else if (node->name == "Shared") {
                    m_ctx->rt_shared_type = struct_type;
                } else if (node->name == "Promise") {
                    m_ctx->rt_promise_type = struct_type;
                } else if (node->name == "Result") {
                    m_ctx->rt_result_type = struct_type;
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
                }
            }
            if (package_kind == ast::PackageKind::BUILTIN ||
                package_kind == ast::PackageKind::STDLIB) {
                if (node->name == "Sized") {
                    m_ctx->rt_sized_interface = struct_type;
                } else if (node->name == "Unsized") {
                    m_ctx->rt_allow_unsized_interface = struct_type;
                }
            }
            node->resolved_type = type_sym;
            return type_sym;
        }
        type_sym = node->resolved_type;
        struct_type = type_sym->data.type_symbol.underlying_type;
        struct_ = &struct_type->data.struct_;

        auto struct_scope = scope;
        struct_scope.parent_fn_node = nullptr;
        struct_scope.parent_fn = nullptr;
        struct_scope.value_type = nullptr;
        struct_scope.parent_loop = nullptr;
        struct_scope.is_escaping = false;
        struct_scope.move_outlet = nullptr;
        struct_scope.block = nullptr;
        struct_scope.is_lhs = false;
        struct_scope.is_fn_call = false;
        if (!struct_scope.parent_struct) {
            struct_scope = struct_scope.set_parent_struct(struct_type).set_parent_type_symbol(type_sym);
        }

        // Make struct lifetime params available for field type resolution (&'a T)
        map<string, ChiLifetime *> struct_lifetime_map;
        if (struct_->lifetime_params.len > 0) {
            for (size_t i = 0; i < data.lifetime_params.len && i < struct_->lifetime_params.len; i++) {
                struct_lifetime_map[data.lifetime_params[i]->name] = struct_->lifetime_params[i];
            }
            struct_scope.fn_lifetime_params = &struct_lifetime_map;
        }

        if (struct_->resolve_status == ResolveStatus::None) {
            // second pass - resolve member types

            // Early-register implemented interfaces so they're visible during
            // field type resolution (e.g. is_non_copyable). Full vtable
            // resolution still happens in pass 3.
            for (auto member : data.members) {
                if (member->type != NodeType::ImplementBlock)
                    continue;
                auto &impl_data = member->data.implement_block;
                if (impl_data.interface_types.len == 0 || impl_data.where_clauses.len > 0)
                    continue;
                for (auto iface_node : impl_data.interface_types) {
                    auto impl_trait = resolve_value(iface_node, struct_scope);
                    if (!impl_trait)
                        continue;
                    auto trait_struct = resolve_struct_type(impl_trait);
                    if (!trait_struct)
                        continue;
                    auto iface_impl = struct_->add_interface(
                        get_allocator(), impl_trait, struct_type);
                    iface_impl->inteface_symbol =
                        resolve_intrinsic_symbol(trait_struct->node);
                }
            }

            for (auto member : data.members) {
                if (member->type == NodeType::ImplementBlock) {
                    auto &impl_data = member->data.implement_block;
                    for (auto impl_member : impl_data.members) {
                        auto *sm = resolve_struct_member(struct_type, impl_member, struct_scope);
                        if (sm && impl_data.resolved_where_cond)
                            sm->where_condition = impl_data.resolved_where_cond;
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

                // Skip non-interface where-blocks (members tagged in pass 2)
                if (impl_data.interface_types.len == 0)
                    continue;

                // Use where condition built in pass 1
                auto *impl_where_cond = impl_data.resolved_where_cond;

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


            struct_->resolve_status = ResolveStatus::EmbedsResolved;
        } else if (struct_->resolve_status == ResolveStatus::EmbedsResolved) {
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
                            check_assignment(vd.expr, expr_type, field_type, &scope);
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
                    compute_receiver_copy_edge_summary(fn_member->data.fn_def);
                    add_fn_body_param_cleanups(fn_member, body);
                }
            };
            for (auto member : data.members) {
                if (member->type == NodeType::ImplementBlock) {
                    auto &impl_data = member->data.implement_block;

                    // For where-blocks, set scoped where-clause traits
                    array<ChiType *> where_placeholders;
                    if (impl_data.where_clauses.len > 0) {
                        for (auto &clause : impl_data.where_clauses) {
                            auto param_name = clause.param_name->str;
                            for (auto tp : struct_->type_params) {
                                auto ph = to_value_type(tp);
                                if (ph->kind == TypeKind::Placeholder && ph->name == param_name) {
                                    m_where_traits[ph].add(
                                        resolve_value(clause.bound_type, scope));
                                    bool already_tracked = false;
                                    for (auto p : where_placeholders) {
                                        if (p == ph) { already_tracked = true; break; }
                                    }
                                    if (!already_tracked)
                                        where_placeholders.add(ph);
                                    break;
                                }
                            }
                        }
                    }

                    for (auto impl_member : impl_data.members) {
                        resolve_fn_body(impl_member);
                    }

                    // Clear scoped where-clause traits
                    for (auto ph : where_placeholders) {
                        m_where_traits.data.erase(ph);
                    }
                } else {
                    resolve_fn_body(member);
                }
            }

            for (auto member : data.members) {
                if (member->type == NodeType::VarDecl && !member->data.var_decl.initialized_at) {
                    auto not_needed = member->data.var_decl.is_embed &&
                                      struct_->kind == ContainerKind::Interface;
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

            // Rule of Three: struct with func delete() must implement Copy
            // or NoCopy (not required in managed mode — GC handles lifecycle)
            if (struct_->kind == ContainerKind::Struct &&
                !has_lang_flag(node->module->get_lang_flags(), LANG_FLAG_MANAGED) &&
                struct_->find_member("delete") &&
                !struct_->member_intrinsics.has_key(IntrinsicSymbol::Copy) &&
                !is_non_copyable(struct_type)) {
                auto name = format_type_display(struct_type);
                error(node, errors::DESTRUCTOR_WITHOUT_COPY, name);
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

        if (type->kind == TypeKind::Tuple) {
            // Tuple<> → Unit, Tuple<A, B, ...> → Tuple type
            if (data.args.len == 0) {
                return create_type_symbol({}, get_system_types()->unit);
            }
            TypeList elements;
            for (auto arg : data.args) {
                // Handle ...T spread: expand Tuple elements into args
                if (arg->type == NodeType::PackExpansion) {
                    auto inner = resolve_value(arg->data.pack_expansion.expr, scope);
                    if (!inner) return nullptr;
                    // If inner is a variadic placeholder, Tuple<...T> is just T
                    if (inner->kind == TypeKind::Placeholder && inner->data.placeholder.is_variadic) {
                        if (data.args.len == 1) {
                            return create_type_symbol({}, inner);
                        }
                    }
                    // If inner is a concrete Tuple, expand its elements
                    if (inner->kind == TypeKind::Tuple) {
                        for (auto elem : inner->data.tuple.elements) {
                            elements.add(elem);
                        }
                        continue;
                    }
                    error(arg, "cannot spread non-tuple type '{}' in type arguments",
                          format_type_display(inner));
                    return nullptr;
                }
                auto resolved = resolve_value(arg, scope);
                if (!resolved) return nullptr;
                elements.add(resolved);
            }
            return create_type_symbol({}, get_tuple_type(elements));
        }
        if (type->kind == TypeKind::Array || type->kind == TypeKind::Optional) {
            if (data.args.len != 1) {
                error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS,
                      format_type_display(get_system_type(type->kind)), 1, data.args.len);
            }
            auto elem_type = to_value_type(resolve(data.args[0], scope));
            return get_wrapped_type(elem_type, type->kind);
        }
        // Handle lifetime-only SubtypeExpr: Holder<'static>{...}
        // Lifetime args don't change the struct type — just constrain borrow checking
        if (data.lifetime_args.len > 0 && data.args.len == 0) {
            if (type->kind == TypeKind::Struct) {
                auto &struct_ = type->data.struct_;
                if (struct_.type_params.len == 0) {
                    // Resolve lifetime args and store on the node for borrow checking
                    for (auto lt_arg : data.lifetime_args) {
                        // Validate that the lifetime matches a struct lifetime param
                        // For now, just accept it — borrow checker will use it
                    }
                    return create_type_symbol({}, type);
                }
            }
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

        // Check if the last type param is variadic
        bool has_variadic = decl_params.len > 0 &&
                            decl_params[decl_params.len - 1]->data.type_param.is_variadic;
        size_t non_variadic_count = has_variadic ? params.len - 1 : params.len;

        if (!has_variadic && data.args.len > params.len) {
            error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS, format_type_display(type), params.len,
                  data.args.len);
            return nullptr;
        }
        if (has_variadic && data.args.len < non_variadic_count) {
            error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS, format_type_display(type),
                  non_variadic_count, data.args.len);
            return nullptr;
        }
        // Check that missing non-variadic args all have defaults
        if (!has_variadic) {
            for (auto i = data.args.len; i < params.len; i++) {
                if (!decl_params[i]->data.type_param.default_type) {
                    error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS, format_type_display(type),
                          params.len, data.args.len);
                    return nullptr;
                }
            }
        }
        // Resolve all provided type args
        array<ChiType *> resolved_args;
        for (size_t i = 0; i < data.args.len; i++) {
            auto resolved = resolve_value(data.args[i], scope);
            resolved_args.add(resolved);
        }
        // Build final args, packing variadic excess into a Tuple
        array<ChiType *> args;
        if (has_variadic) {
            for (size_t i = 0; i < non_variadic_count && i < resolved_args.len; i++) {
                args.add(resolved_args[i]);
            }
            TypeList variadic_elements;
            for (size_t i = non_variadic_count; i < resolved_args.len; i++) {
                variadic_elements.add(resolved_args[i]);
            }
            args.add(get_tuple_type(variadic_elements));
        } else {
            for (auto a : resolved_args) args.add(a);
        }
        // Fill in defaults for missing non-variadic type args
        if (!has_variadic) {
            for (auto i = data.args.len; i < params.len; i++) {
                auto resolved = resolve_value(decl_params[i]->data.type_param.default_type, scope);
                args.add(resolved);
            }
        }

        // Validate trait bounds on type arguments
        for (size_t i = 0; i < args.len && i < params.len; i++) {
            auto param_type = to_value_type(params[i]);
            auto type_arg = args[i];
            if (!type_arg)
                continue;

            // NoCopy types require explicit NoCopy bound on the type param
            if (is_non_copyable(type_arg) && !is_non_copyable(param_type)) {
                error(node, errors::TYPE_NOT_COPYABLE,
                      format_type_display(type_arg));
                return nullptr;
            }

            if (!param_type)
                continue;
            auto param_traits = get_placeholder_traits(param_type);
            if (param_traits.len == 0)
                continue;
            for (auto trait : param_traits) {
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
        case TypeKind::Reference:
        case TypeKind::MutRef:
        case TypeKind::MoveRef:
        case TypeKind::This:
        case TypeKind::Struct:
        case TypeKind::Subtype:
        case TypeKind::Array:
        case TypeKind::Span: {
            auto base_type = expr_type;
            if (base_type && base_type->is_reference()) {
                base_type = base_type->get_elem();
            }
            // Block writes on immutable spans
            if (scope.is_lhs && base_type && base_type->kind == TypeKind::Span &&
                !base_type->data.span.is_mut) {
                error(node, errors::CANNOT_WRITE_IMMUTABLE_SPAN,
                      format_type_display(base_type),
                      format_type_display(base_type->data.span.elem));
                return nullptr;
            }
            auto struct_ = resolve_struct_type(base_type);
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

        check_assignment(data.subscript, subscript_type, expected_index_type, &scope);

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
            check_assignment(data.start, start_type, uint32_type, &scope);
        }
        if (data.end) {
            auto end_scope = scope.set_value_type(uint32_type);
            auto end_type = resolve(data.end, end_scope);
            if (!end_type)
                return nullptr;
            check_assignment(data.end, end_type, uint32_type, &scope);
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
            enum_value->is_placeholder =
                is_enum_value_placeholder(enum_value->data.enum_value.enum_type);
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
                check_assignment(data.value, value_type, get_system_types()->int_, &scope);
                auto value = resolve_constant_value(data.value);
                if (value.has_value() && holds_alternative<const_int_t>(*value)) {
                    data.resolved_value = get<const_int_t>(*value);
                    scope.next_enum_value = data.resolved_value + 1;
                } else {
                    data.resolved_value = scope.next_enum_value++;
                }
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
        auto body_block_scope = scope.set_block(&data.body->data.block);

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
            check_assignment(data.condition, cond_type, get_system_types()->bool_, &scope);
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
                check_assignment(range.end, end_type, start_type, &scope);
                data.resolved_kind = ast::ForLoopKind::IntRange;
                if (data.bind) {
                    auto bind_scope = body_block_scope.set_value_type(start_type);
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
                        auto bind_scope = body_block_scope.set_value_type(value_type);
                        resolve(data.bind, bind_scope);
                    }
                    if (data.index_bind) {
                        auto idx_scope =
                            body_block_scope.set_value_type(get_system_types()->uint32);
                        resolve(data.index_bind, idx_scope);
                    }
                    auto loop_scope = scope.set_parent_loop(node);
                    resolve(data.body, loop_scope);
                    return nullptr;
                }

                if (expr_type && expr_type->kind == TypeKind::Span) {
                    if (data.bind) {
                        auto elem_type = expr_type->data.span.elem;
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
                        auto bind_scope = body_block_scope.set_value_type(value_type);
                        resolve(data.bind, bind_scope);
                    }
                    if (data.index_bind) {
                        auto idx_scope =
                            body_block_scope.set_value_type(get_system_types()->uint32);
                        resolve(data.index_bind, idx_scope);
                    }
                    auto loop_scope = scope.set_parent_loop(node);
                    resolve(data.body, loop_scope);
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
                        auto bind_scope = body_block_scope.set_value_type(value_type);
                        resolve(data.bind, bind_scope);
                    }
                    if (data.index_bind) {
                        auto idx_scope =
                            body_block_scope.set_value_type(get_system_types()->uint32);
                        resolve(data.index_bind, idx_scope);
                    }
                } else if (sty->member_intrinsics.get(IntrinsicSymbol::MutIterable)) {
                    // Iterator-based iteration (MutIterable)
                    data.resolved_kind = ast::ForLoopKind::Iter;
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
                        auto bind_scope = body_block_scope.set_value_type(value_type);
                        resolve(data.bind, bind_scope);
                    }
                    if (data.index_bind) {
                        auto idx_scope =
                            body_block_scope.set_value_type(get_system_types()->uint32);
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

        // Implicit Sized bound: all type params get Sized unless Unsized
        if (m_ctx->rt_sized_interface) {
            bool has_allow_unsized = false;
            bool has_sized = false;
            for (auto t : phty->data.placeholder.traits) {
                if (t && t->kind == TypeKind::Struct && t->data.struct_.node) {
                    auto sym = resolve_intrinsic_symbol(t->data.struct_.node);
                    if (sym == IntrinsicSymbol::Unsized)
                        has_allow_unsized = true;
                    if (sym == IntrinsicSymbol::Sized)
                        has_sized = true;
                }
            }
            if (has_allow_unsized) {
                // Remove Unsized from the list — it's an opt-out, not a real bound
                array<ChiType *> filtered;
                for (auto t : phty->data.placeholder.traits) {
                    if (t && t->kind == TypeKind::Struct && t->data.struct_.node &&
                        resolve_intrinsic_symbol(t->data.struct_.node) ==
                            IntrinsicSymbol::Unsized)
                        continue;
                    filtered.add(t);
                }
                phty->data.placeholder.traits = filtered;
            } else if (!has_sized) {
                // No Unsized and no explicit Sized: add implicit Sized
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
        phty->data.placeholder.is_variadic = data.is_variadic;

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
            bool is_pointer = expr_type->is_raw_pointer() || expr_type->kind == TypeKind::MoveRef;
            bool is_value = !is_pointer && type_needs_destruction(expr_type);
            if (!is_pointer && !is_value) {
                error(node, errors::INVALID_OPERATOR, data.prefix->to_string(),
                      format_type_display(expr_type));
            }
            if (!scope.is_unsafe_block) {
                if (expr_type->is_raw_pointer()) {
                    error(node, "'delete' on raw pointer requires unsafe block");
                } else {
                    error(node, "'delete' requires unsafe block");
                }
                return nullptr;
            }
            // delete sinks the variable and its current borrow leaves
            if (scope.parent_fn_node) {
                auto *deleted_decl = find_root_decl(data.expr);
                if (deleted_decl) {
                    auto &fn_def = *scope.parent_fn_def();
                    fn_def.add_sink_edge(deleted_decl, node);
                    // Propagate sink to the data this variable currently points to
                    auto *edges = fn_def.flow.ref_edges.get(deleted_decl);
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
            // Check for direct module import into non-std package
            auto *comp_ctx = static_cast<CompilationContext *>(m_ctx->allocator);
            auto [pkg_path, mod_path] = comp_ctx->parse_import_path(data.path->str);
            if (pkg_path != "std" && mod_path.size() && !is_relative_path(data.path->str)) {
                error(node, errors::DIRECT_MODULE_IMPORT, data.path->str, pkg_path);
                return nullptr;
            }

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
                // When the runtime module imports/exports a sub-module,
                // make all currently-resolved runtime exports available as builtins
                // so the sub-module can reference types like Array, panic, etc.
                if (scope.module->package->kind == ast::PackageKind::BUILTIN) {
                    context_init_builtins(scope.module);
                }
                auto target_package =
                    m_ctx->allocator->get_or_create_package(path_info->package_id_path);
                auto src = io::Buffer::from_file(path);
                module = m_ctx->allocator->process_source(target_package, &src, path);
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
            !node->analysis.is_capture()) {
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
            node->analysis.moved = true;
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

            // Fork flow state for branch-aware analysis
            assert(scope.parent_fn_node);
            auto *fn_def = scope.parent_fn_def();
            auto pre_branch = fn_def->flow.fork();

            ChiType *ret_type = (scope.value_type && scope.value_type->kind != TypeKind::Any)
                                     ? scope.value_type
                                     : nullptr;
            array<ast::FlowState> case_flows;
            bool all_terminate = true;
            bool has_else = false;
            int covered_count = 0;

            for (auto scase : data.cases) {
                if (!scase)
                    continue;

                if (scase->data.case_expr.is_else)
                    has_else = true;
                else
                    covered_count += scase->data.case_expr.clauses.len;

                if (!scase->data.case_expr.is_else) {
                    for (auto clause : scase->data.case_expr.clauses) {
                        auto clause_scope = scope.set_value_type(expr_type);
                    auto clause_type = resolve(clause, clause_scope);
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

                // Restore pre-branch flow before resolving each case
                fn_def->flow = pre_branch;
                auto case_type = resolve(scase, scope);

                if (!always_terminates(scase->data.case_expr.body)) {
                    case_flows.add(fn_def->flow.fork());
                    all_terminate = false;
                }

                if (ret_type) {
                    auto result_type =
                        resolve_common_value_type(ret_type, case_type, scope.value_type);
                    check_assignment(get_assignment_expr_node(scase->data.case_expr.body), case_type,
                                     result_type, &scope);
                    ret_type = result_type;
                    scase->resolved_type = ret_type;
                } else {
                    ret_type = case_type;
                }
            }

            // Determine exhaustiveness for type switch
            // (type switches on interfaces are generally not exhaustive unless has_else)
            bool exhaustive = has_else;

            // Merge flow states
            if (all_terminate && exhaustive) {
                // All cases terminate and switch is exhaustive: nothing runs after
            } else if (case_flows.len == 0) {
                // All cases terminate but not exhaustive: no-match path continues
                fn_def->flow = pre_branch;
            } else {
                // Start with first non-terminating case's flow
                fn_def->flow = case_flows[0];
                for (int i = 1; i < case_flows.len; i++) {
                    fn_def->flow.merge(case_flows[i]);
                }
                if (!exhaustive) {
                    // Non-exhaustive: also merge with pre-branch (no-match path)
                    fn_def->flow.merge(pre_branch);
                }
            }

            return ret_type ? ret_type : get_system_types()->void_;
        }

        auto expr_comparator = resolve_comparator(expr_type, scope);

        // Fork flow state for branch-aware analysis
        assert(scope.parent_fn_node);
        auto *fn_def = scope.parent_fn_def();
        auto pre_branch = fn_def->flow.fork();

        ChiType *ret_type = (scope.value_type && scope.value_type->kind != TypeKind::Any)
                                 ? scope.value_type
                                 : nullptr;
        array<ast::FlowState> case_flows;
        bool all_terminate = true;
        bool has_else = false;
        int covered_count = 0;

        for (auto scase : data.cases) {
            // Skip null case expressions
            if (!scase)
                continue;

            if (scase->data.case_expr.is_else)
                has_else = true;
            else
                covered_count += scase->data.case_expr.clauses.len;

            // Resolve clauses BEFORE body so we can inject narrowed vars
            if (!scase->data.case_expr.is_else) {
                for (auto clause : scase->data.case_expr.clauses) {
                    auto clause_scope = scope.set_value_type(expr_type);
                    if (clause->type == NodeType::Identifier) {
                        auto variant = find_expected_enum_variant(clause->name, expr_type);
                        if (variant) {
                            auto &ident = clause->data.identifier;
                            ident.decl = variant->node;
                            ident.kind = ast::IdentifierKind::Value;
                            ident.decl_is_provisional = true;
                        }
                    }
                    auto clause_type = resolve(clause, clause_scope);
                    if (clause->type == NodeType::DotExpr) {
                        auto &dot = clause->data.dot_expr;
                        auto *resolved_decl = dot.resolved_decl;
                        if (resolved_decl && resolved_decl->type == NodeType::EnumVariant) {
                            auto expected_variant =
                                find_expected_enum_variant(dot.field->str, expr_type);
                            if (expected_variant ==
                                resolved_decl->data.enum_variant.resolved_enum_variant) {
                                dot.resolved_can_shorthand = true;
                                dot.resolved_shorthand_is_ambiguous =
                                    is_contextual_resolution_ambiguous(dot.field->str,
                                                                       resolved_decl, clause_scope);
                            }
                        }
                    }
                    resolve_constant_value(clause);
                    auto clause_comparator = resolve_comparator(clause_type, scope);

                    // Only check assignment if both comparators are valid
                    if (clause_comparator && expr_comparator) {
                        check_assignment(clause, clause_comparator, expr_comparator, &scope);

                        if (!clause_comparator->is_int_like()) {
                            error(clause, errors::INVALID_SWITCH_TYPE,
                                  format_type_display(clause_type));
                        }
                    }
                }

                // Enum variant narrowing: single-clause case matching a specific variant
                if (scase->data.case_expr.clauses.len == 1) {
                    auto switch_enum = get_enum_type(expr_type);
                    if (switch_enum && switch_enum->kind == TypeKind::Enum) {
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

                                auto pattern = scase->data.case_expr.destructure_pattern;
                                if (pattern) {
                                    auto ident = create_node(NodeType::Identifier);
                                    ident->token = switch_expr->token;
                                    ident->module = switch_expr->module;
                                    ident->name = var->name;
                                    ident->data.identifier.decl = var;
                                    ident->data.identifier.kind = ast::IdentifierKind::Value;
                                    ident->data.identifier.decl_is_provisional = false;

                                    pattern->data.destructure_decl.resolved_expr = ident;

                                    auto body_scope = scope.set_block(&block_data);
                                    resolve(pattern, body_scope);
                                    block_data.implicit_vars.add(pattern);
                                }
                            } else if (scase->data.case_expr.destructure_pattern) {
                                error(scase->data.case_expr.destructure_pattern,
                                      "switch destructuring requires an identifier or field access");
                            }
                        } else if (scase->data.case_expr.destructure_pattern) {
                            error(scase->data.case_expr.destructure_pattern,
                                  "switch destructuring requires an enum variant clause");
                        }
                    } else if (scase->data.case_expr.destructure_pattern) {
                        error(scase->data.case_expr.destructure_pattern,
                              "switch destructuring requires an enum switch expression");
                    }
                } else if (scase->data.case_expr.destructure_pattern) {
                    error(scase->data.case_expr.destructure_pattern,
                          "switch destructuring requires exactly one clause");
                }
            }

            // Restore pre-branch flow before resolving each case
            fn_def->flow = pre_branch;
            auto case_type = resolve(scase, scope);

            if (!always_terminates(scase->data.case_expr.body)) {
                case_flows.add(fn_def->flow.fork());
                all_terminate = false;
            }

            if (ret_type) {
                auto result_type =
                    resolve_common_value_type(ret_type, case_type, scope.value_type);
                check_assignment(get_assignment_expr_node(scase->data.case_expr.body), case_type,
                                 result_type, &scope);
                ret_type = result_type;
                scase->resolved_type = ret_type;
            } else if (case_type && case_type->kind != TypeKind::Void) {
                ret_type = case_type;
            }
        }

        // Determine exhaustiveness
        bool exhaustive = has_else;
        if (!exhaustive) {
            auto enum_type = get_enum_type(expr_type);
            if (enum_type && enum_type->kind == TypeKind::Enum) {
                auto enum_ = &enum_type->data.enum_;
                if (covered_count >= enum_->variants.len) {
                    exhaustive = true;
                }
            }
            if (!exhaustive && ret_type && ret_type->kind != TypeKind::Void) {
                error(node, errors::SWITCH_EXPR_MUST_HAVE_ELSE);
            }
        }

        // Merge flow states
        if (all_terminate && exhaustive) {
            // All cases terminate and switch is exhaustive: nothing runs after
        } else if (case_flows.len == 0) {
            // All cases terminate but not exhaustive: no-match path continues
            fn_def->flow = pre_branch;
        } else {
            // Start with first non-terminating case's flow
            fn_def->flow = case_flows[0];
            for (int i = 1; i < case_flows.len; i++) {
                fn_def->flow.merge(case_flows[i]);
            }
            if (!exhaustive) {
                // Non-exhaustive: also merge with pre-branch (no-match path)
                fn_def->flow.merge(pre_branch);
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

    if (auto enum_type = get_enum_type(type)) {
        auto base_value_type = enum_type->data.enum_.base_value_type;
        if (base_value_type && base_value_type->kind == TypeKind::EnumValue) {
            return base_value_type->data.enum_value.discriminator_type;
        }
    }

    switch (type->kind) {
    case TypeKind::This:
        return resolve_comparator(type->eval(), scope);
    case TypeKind::Reference:
    case TypeKind::Pointer:
    case TypeKind::MutRef:
    case TypeKind::MoveRef:
        return resolve_comparator(type->get_elem(), scope);
    default:
        return type;
    }
}

static bool has_conditional_compile_attrs(ast::Node *node);

ChiType *Resolver::resolve(ast::Node *node, ResolveScope &scope, uint32_t flags) {
    if (!node)
        return nullptr;
    auto cached = node_get_type(node);
    if (cached) {
        return cached;
    }

    if (has_conditional_compile_attrs(node)) {
        if (node->type != NodeType::Block) {
            error(node, "conditional compilation attributes are only supported on blocks");
            node->analysis.is_enabled = false;
            node->resolved_type = get_system_types()->void_;
            return node->resolved_type;
        }

        bool is_enabled = true;
        for (auto attr : node->attributes) {
            auto enabled =
                resolve_conditional_platform_term(attr ? attr->data.decl_attribute.term : nullptr);
            if (!enabled.has_value()) {
                error(attr ? attr : node, "invalid conditional compilation attribute");
                node->analysis.is_enabled = false;
                node->resolved_type = get_system_types()->void_;
                return node->resolved_type;
            }
            is_enabled = is_enabled && *enabled;
        }

        node->analysis.is_enabled = is_enabled;
        if (!is_enabled) {
            node->resolved_type = get_system_types()->void_;
            return node->resolved_type;
        }
    }

    auto result = _resolve(node, scope, flags);
    // If _resolve installed a synthesized resolved_node, it has already
    // pre-seeded node->resolved_type to the type the synthesized body wants
    // (e.g. method-as-value pre-seeds the raw fn type so the inner call can
    // dispatch). Don't clobber it with the outer wrapper type.
    if (!node->resolved_node) {
        node->resolved_type = result;
    }
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

static string qualify_base_name(const string &local_name, ast::Module *module,
                                const string &relative_module_id) {
    if (!module || relative_module_id.empty() || module->global_id() == relative_module_id) {
        return local_name;
    }
    return fmt::format("{}.{}", module->global_id(), local_name);
}

static string format_container_name(Resolver *resolver, const string &base_name,
                                     TypeList *type_params, const string &module_id,
                                     ast::Module *module = nullptr) {
    auto qualified = qualify_base_name(base_name, module, module_id);
    if (!type_params || type_params->len == 0) {
        return qualified;
    }

    std::stringstream ss;
    ss << qualified << "<";
    for (int i = 0; i < type_params->len; i++) {
        if (i > 0) ss << ",";
        ss << resolver->format_type_qualified_name((*type_params)[i], module_id);
    }
    ss << ">";
    return ss.str();
}

static string format_container_display_name(Resolver *resolver, const string &base_name,
                                            TypeList *type_params) {
    if (!type_params || type_params->len == 0) {
        return base_name;
    }

    std::stringstream ss;
    ss << base_name << "<";
    for (int i = 0; i < type_params->len; i++) {
        if (i > 0) ss << ",";
        ss << resolver->format_type_display((*type_params)[i]);
    }
    ss << ">";
    return ss.str();
}

static string format_container_qualified_name(Resolver *resolver, const string &base_name,
                                              ast::Module *module, TypeList *type_params,
                                              const string &module_id) {
    auto qualified = qualify_base_name(base_name, module, module_id);
    if (!type_params || type_params->len == 0) {
        return qualified;
    }

    std::stringstream ss;
    ss << qualified << "<";
    for (int i = 0; i < type_params->len; i++) {
        if (i > 0) ss << ",";
        ss << resolver->format_type_qualified_name((*type_params)[i], module_id);
    }
    ss << ">";
    return ss.str();
}

string Resolver::format_type_qualified_name(ChiType *type, const string &module_id) {
    if (!type) {
        return "<invalid-type>";
    }

    switch (type->kind) {
    case TypeKind::This:
        return format_type_qualified_name(type->eval(), module_id);
    case TypeKind::TypeSymbol:
        if (type->name) {
            return *type->name;
        }
        if (type->display_name) {
            return *type->display_name;
        }
        return format_type_display(type);
    case TypeKind::Subtype: {
        auto &data = type->data.subtype;
        if (data.generic->kind == TypeKind::Fn || data.generic->kind == TypeKind::FnLambda) {
            return format_type_qualified_name(data.final_type, module_id);
        }
        std::stringstream ss;
        // Use the generic's base name without its own type_params — the Subtype's
        // args replace those params, so including them would double-count.
        if (data.generic->kind == TypeKind::Struct && data.generic->data.struct_.node) {
            ss << qualify_base_name(data.generic->data.struct_.node->name,
                                    data.generic->data.struct_.node->module, module_id);
        } else if (data.generic->kind == TypeKind::Enum && data.generic->data.enum_.node) {
            ss << qualify_base_name(data.generic->data.enum_.node->name,
                                    data.generic->data.enum_.node->module, module_id);
        } else {
            ss << format_type_qualified_name(data.generic, module_id);
        }
        ss << "<";
        for (int i = 0; i < data.args.len; i++) {
            if (i > 0) ss << ",";
            ss << format_type_qualified_name(data.args[i], module_id);
        }
        ss << ">";
        return ss.str();
    }
    case TypeKind::Struct: {
        auto &sty = type->data.struct_;
        // For specialized structs from resolve_subtype, delegate to the original
        // Subtype for correct module-relative naming at each nesting level.
        if (!type->global_id.empty()) {
            auto entry = m_ctx->generics.struct_envs.get(type->global_id);
            if (entry && entry->subtype && entry->subtype->kind == TypeKind::Subtype) {
                return format_type_qualified_name(entry->subtype, module_id);
            }
        }
        auto base_name = type->name.value_or(sty.node ? sty.node->name : string("<anon>"));
        return format_container_qualified_name(this, base_name,
                                               sty.node ? sty.node->module : nullptr,
                                               &sty.type_params, module_id);
    }
    case TypeKind::Enum: {
        auto &ety = type->data.enum_;
        // Same as Struct: delegate to Subtype for specialized enum types.
        if (!type->global_id.empty()) {
            auto entry = m_ctx->generics.struct_envs.get(type->global_id);
            if (entry && entry->subtype && entry->subtype->kind == TypeKind::Subtype) {
                return format_type_qualified_name(entry->subtype, module_id);
            }
        }
        auto base_name = type->name.value_or(ety.node ? ety.node->name : string("<anon>"));
        return format_container_qualified_name(this, base_name,
                                               ety.node ? ety.node->module : nullptr,
                                               &ety.type_params, module_id);
    }
    default:
        return format_type_id(type);
    }
}

string Resolver::resolve_qualified_name(ast::Node *node) {
    switch (node->type) {
    case NodeType::FnDef:
        if (node->resolved_type && node->resolved_type->kind == TypeKind::Fn) {
            auto container_ref = node->resolved_type->data.fn.container_ref;
            if (container_ref) {
                auto container = container_ref->get_elem();
                return fmt::format("{}.{}",
                                   format_type_qualified_name(container, node->module->global_id()),
                                   node->name);
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

string Resolver::resolve_display_name(ast::Node *node) {
    if (!node) {
        return "<anon>";
    }

    switch (node->type) {
    case NodeType::FnDef:
        if (node->resolved_type && node->resolved_type->kind == TypeKind::Fn) {
            auto container_ref = node->resolved_type->data.fn.container_ref;
            if (container_ref) {
                auto container = container_ref->get_elem();
                return fmt::format("{}.{}", format_type_display(container), node->name);
            }
        }
        return node->name;
    case NodeType::GeneratedFn: {
        auto &data = node->data.generated_fn;
        auto base_name = resolve_display_name(data.original_fn);
        if (!data.fn_subtype || data.fn_subtype->kind != TypeKind::Subtype) {
            return base_name;
        }

        std::stringstream ss;
        ss << base_name << "<";
        auto &args = data.fn_subtype->data.subtype.args;
        for (int i = 0; i < args.len; i++) {
            ss << format_type_display(args[i]);
            if (i < args.len - 1) {
                ss << ", ";
            }
        }
        ss << ">";
        return ss.str();
    }
    default:
        return node->name;
    }
}

string Resolver::format_type(ChiType *type, bool for_display) {
    if (!type) {
        return "<invalid-type>";
    }
    if (for_display) {
        if (type->display_name) {
            return *type->display_name;
        }
        if (type->kind == TypeKind::Struct) {
            auto &sty = type->data.struct_;
            auto base_name = sty.node ? sty.node->name : type->name.value_or(string("<anon>"));
            return format_container_display_name(this, base_name, &sty.type_params);
        }
        if (type->kind == TypeKind::Enum) {
            auto &ety = type->data.enum_;
            auto base_name = ety.node ? ety.node->name : type->name.value_or(string("<anon>"));
            return format_container_display_name(this, base_name, &ety.type_params);
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
    case TypeKind::Unit:
        return "Unit";
    case TypeKind::Tuple: {
        std::stringstream ss;
        ss << "Tuple<";
        auto &elems = type->data.tuple.elements;
        for (int i = 0; i < elems.len; i++) {
            ss << format_type(elems[i], for_display);
            if (i < elems.len - 1) ss << ", ";
        }
        ss << ">";
        return ss.str();
    }
    case TypeKind::Infer: {
        auto inferred = type->data.infer.inferred_type;
        if (inferred) {
            return format_type(inferred, for_display);
        }
        return for_display ? "Infer" : fmt::format("Type:{}:Infer", type->id);
    }
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
            bool first = true;
            for (int i = 0; i < data.args.len; i++) {
                auto arg = data.args[i];
                // Unpack variadic Tuple arg for display
                bool is_variadic_param = false;
                if (data.generic->kind == TypeKind::Struct) {
                    auto &dp = data.generic->data.struct_.node->data.struct_decl.type_params;
                    if (i < dp.len && dp[i]->data.type_param.is_variadic)
                        is_variadic_param = true;
                } else if (data.generic->kind == TypeKind::Enum) {
                    auto &dp = data.generic->data.enum_.node->data.enum_decl.type_params;
                    if (i < dp.len && dp[i]->data.type_param.is_variadic)
                        is_variadic_param = true;
                }
                if (is_variadic_param && arg->kind == TypeKind::Tuple) {
                    for (auto elem : arg->data.tuple.elements) {
                        if (!first) ss << ",";
                        first = false;
                        ss << format_type(elem, for_display);
                    }
                } else {
                    if (!first) ss << ",";
                    first = false;
                    ss << format_type(arg, for_display);
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
    case TypeKind::Span: {
        string lt_name =
            type->data.span.lifetimes.len > 0 && type->data.span.lifetimes[0]
                ? type->data.span.lifetimes[0]->name
                : "";
        string prefix = format_span_prefix(type->data.span.is_mut, lt_name);
        return fmt::format("{}[{}]", prefix, format_type(type->data.span.elem, for_display));
    }
    case TypeKind::FixedArray:
        return fmt::format("[{}]{}", type->data.fixed_array.size,
                           format_type(type->data.fixed_array.elem, for_display));
    case TypeKind::Unknown:
        return "unknown";
    case TypeKind::Undefined:
        return "undefined";
    case TypeKind::ZeroInit:
        return "zeroinit";
    case TypeKind::Never:
        return "never";
    case TypeKind::Null:
        return "null";
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
                auto *param = fn.get_variadic_elem_type();
                if (param) {
                    ss << format_type(param, for_display);
                } else {
                    ss << format_type(fn.params[i], for_display);
                }
                if (i < fn.params.len - 1) {
                    ss << ",";
                }
                continue;
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

static bool allows_move_ref_borrow_coercion(Resolver *resolver, ast::Node *value,
                                            ChiType *from_type, ChiType *to_type,
                                            const ResolveScope *scope) {
    if (!from_type || !to_type || from_type->kind != TypeKind::MoveRef ||
        !to_type->is_borrow_reference()) {
        return false;
    }

    auto *from_elem = from_type->get_elem();
    auto *to_elem = to_type->get_elem();
    if (from_elem && from_elem->kind == TypeKind::Subtype && from_elem->data.subtype.final_type) {
        from_elem = from_elem->data.subtype.final_type;
    }
    if (to_elem && to_elem->kind == TypeKind::Subtype && to_elem->data.subtype.final_type) {
        to_elem = to_elem->data.subtype.final_type;
    }
    if (!from_elem || !to_elem || from_elem != to_elem) {
        return false;
    }

    if (scope && scope->is_unsafe_block) {
        return true;
    }
    if (scope && scope->module &&
        has_lang_flag(scope->module->get_lang_flags(), LANG_FLAG_MANAGED)) {
        return true;
    }
    if (value && value->module &&
        has_lang_flag(value->module->get_lang_flags(), LANG_FLAG_MANAGED)) {
        return true;
    }
    return false;
}

static bool expr_is_direct_borrow_value(ast::Node *expr) {
    if (!expr) {
        return false;
    }

    switch (expr->type) {
    case ast::NodeType::BinOpExpr: {
        auto op = expr->data.bin_op_expr.op_type;
        return op == TokenType::AND || op == TokenType::MUTREF;
    }
    case ast::NodeType::CastExpr: {
        auto *inner = expr->data.cast_expr.expr;
        auto *inner_type = inner ? inner->resolved_type : nullptr;
        return inner_type && inner_type->kind == TypeKind::MoveRef && expr->resolved_type &&
               expr->resolved_type->is_borrow_reference();
    }
    case ast::NodeType::TryExpr:
        return expr_is_direct_borrow_value(expr->data.try_expr.expr);
    default:
        return false;
    }
}

static ast::Node *get_assignment_expr_node(ast::Node *node) {
    if (!node) {
        return nullptr;
    }
    if (node->type == ast::NodeType::Block) {
        if (node->data.block.return_expr) {
            return node->data.block.return_expr;
        }
    }
    return node;
}

static bool expr_can_fallback_to_borrow_source(ast::Node *expr) {
    if (!expr) {
        return false;
    }
    auto *type = expr->resolved_type;
    if (type && type->is_borrow_reference()) {
        return true;
    }
    if (expr->type == ast::NodeType::ParamDecl) {
        return get_param_effective_lifetime(expr, type) != nullptr;
    }
    return false;
}

static ast::Node *unwrap_lifetime_copy_intrinsic_arg(ast::Node *expr) {
    while (expr) {
        switch (expr->type) {
        case ast::NodeType::ParenExpr:
            expr = expr->data.child_expr;
            continue;
        case ast::NodeType::TryExpr:
            expr = expr->data.try_expr.expr;
            continue;
        case ast::NodeType::CastExpr:
            expr = expr->data.cast_expr.expr;
            continue;
        case ast::NodeType::UnaryOpExpr: {
            auto op = expr->data.unary_op_expr.op_type;
            if (op == TokenType::AND || op == TokenType::MUTREF ||
                op == TokenType::MOVEREF) {
                expr = expr->data.unary_op_expr.op1;
                continue;
            }
            return expr;
        }
        default:
            return expr;
        }
    }
    return nullptr;
}

static bool expr_creates_direct_borrow(Resolver *resolver, ast::Node *expr, ChiType *from_type,
                                       ChiType *to_type, const ResolveScope *scope) {
    if (!expr || !to_type || !to_type->is_borrow_reference()) {
        return false;
    }

    if (expr_is_direct_borrow_value(expr)) {
        return true;
    }
    return allows_move_ref_borrow_coercion(resolver, expr, from_type, to_type, scope);
}

void Resolver::check_assignment(ast::Node *value, ChiType *from_type, ChiType *to_type,
                                const ResolveScope *scope, bool is_explicit) {
    // If from_type is null (failed to resolve), skip assignment check
    if (!from_type || !to_type) {
        return;
    }

    if (!is_explicit && value && value->type != NodeType::CastExpr) {
        auto conversion_type = resolve_conversion_type(from_type, to_type);
        if (conversion_type == ast::ConversionType::OwningCoercion) {
            value->analysis.conversion_type = conversion_type;
        }
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
        if (allows_move_ref_borrow_coercion(this, value, from_type, to_type, scope)) {
            return;
        }
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
        if (data.op_type == TokenType::MUL || data.op_type == TokenType::KW_MOVE) {
            return true;
        }
        if (data.op_type == TokenType::LNOT && data.is_suffix) {
            return data.op1 && is_addressable(data.op1);
        }
        return false;
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
    check_assignment(value, from_type, to_type, nullptr, true);
}

void Resolver::context_init_builtins(ast::Module *builtin_module) {
    for (auto &decl : builtin_module->exports) {
        m_ctx->builtins.add(decl);
    }
}

void Resolver::register_intrinsic_decls(ast::Module *module) {
    if (!module || !module->root) {
        return;
    }

    auto register_decl = [&](ast::Node *decl) {
        if (!decl || decl->type != NodeType::FnDef) {
            return;
        }
        auto symbol = resolve_intrinsic_symbol(decl);
        if (symbol == IntrinsicSymbol::None) {
            return;
        }
        m_ctx->intrinsic_decls[symbol] = decl;
    };

    for (auto *decl : module->root->data.root.top_level_decls) {
        if (!decl) {
            continue;
        }
        if (decl->type == NodeType::ExternDecl) {
            for (auto *member : decl->data.extern_decl.members) {
                register_decl(member);
            }
            continue;
        }
        register_decl(decl);
    }
}

ast::Node *Resolver::get_intrinsic_decl(IntrinsicSymbol symbol) {
    auto decl_p = m_ctx->intrinsic_decls.get(symbol);
    return decl_p ? *decl_p : nullptr;
}

string Resolver::resolve_term_string(ast::Node *term) {
    switch (term->type) {
    case NodeType::Identifier:
        return term->name;
    case NodeType::DotExpr:
        return resolve_term_string(term->data.dot_expr.effective_expr()) + "." +
               term->data.dot_expr.field->get_name();
    default:
        return "";
    }
    return "";
}

optional<bool> Resolver::resolve_conditional_platform_term(ast::Node *term) {
    if (!term || term->type != NodeType::FnCallExpr) {
        return std::nullopt;
    }

    auto &call = term->data.fn_call_expr;
    if (!call.fn_ref_expr || call.fn_ref_expr->type != NodeType::Identifier ||
        call.fn_ref_expr->name != "if" || call.args.len != 1) {
        return std::nullopt;
    }

    auto platform_term = resolve_term_string(call.args[0]);
    if (platform_term.empty()) {
        return std::nullopt;
    }

    auto &tags = m_ctx->allocator->get_platform_tags();
    auto enabled_p = tags.get(platform_term);
    if (!enabled_p) {
        return std::nullopt;
    }

    return {*enabled_p};
}

static bool has_conditional_compile_attrs(ast::Node *node) {
    if (!node) {
        return false;
    }
    for (auto attr : node->attributes) {
        if (!attr || attr->type != NodeType::DeclAttribute) {
            continue;
        }
        auto term = attr->data.decl_attribute.term;
        if (term && term->type == NodeType::FnCallExpr &&
            term->data.fn_call_expr.fn_ref_expr &&
            term->data.fn_call_expr.fn_ref_expr->type == NodeType::Identifier &&
            term->data.fn_call_expr.fn_ref_expr->name == "if") {
            return true;
        }
    }
    return false;
}

static void add_unique_node(array<ast::Node *> &nodes, ast::Node *node) {
    if (!node) {
        return;
    }
    for (auto *existing : nodes) {
        if (existing == node) {
            return;
        }
    }
    nodes.add(node);
}

static ast::Node *find_narrowed_optional_var(ast::Node *node, ResolveScope &scope) {
    if (!node || !scope.block || !scope.block->scope) {
        return nullptr;
    }
    ast::Node *narrowed = nullptr;
    if (node->type == NodeType::DotExpr) {
        auto path = build_narrowing_path(node);
        if (!path.empty()) {
            narrowed = scope.block->scope->find_one(path, true);
        }
    } else if (node->type == NodeType::Identifier) {
        string name = node->name;
        if (name.empty() && node->data.identifier.kind == ast::IdentifierKind::This) {
            name = "this";
        }
        if (!name.empty()) {
            narrowed = scope.block->scope->find_one(name, true);
        }
    }
    if (!narrowed || narrowed->type != NodeType::VarDecl) {
        return nullptr;
    }
    auto *source = narrowed->data.var_decl.narrowed_from;
    auto *source_type = source ? source->resolved_type : nullptr;
    if (!source_type || source_type->kind != TypeKind::Optional) {
        return nullptr;
    }
    return narrowed;
}

static bool expr_is_narrowed_from_optional(ast::Node *node, ResolveScope &scope) {
    return find_narrowed_optional_var(node, scope) != nullptr;
}

static bool is_null_literal(ast::Node *node) {
    return node && node->type == ast::NodeType::LiteralExpr && node->token &&
           node->token->type == TokenType::NULLP;
}

static void append_leaf_borrow_roots(ast::FlowState &flow, ast::Node *source,
                                     array<ast::Node *> &out,
                                     bool fallback_to_source = true) {
    if (!source) {
        return;
    }
    auto *deps = flow.ref_edges.get(source);
    size_t offset = flow.current_edge_offset(source);
    if (!deps || deps->len <= offset) {
        if (fallback_to_source) {
            add_unique_node(out, source);
        }
        return;
    }

    array<ast::Node *> stack;
    map<ast::Node *, bool> visited;
    for (size_t i = offset; i < deps->len; i++) {
        stack.add(deps->items[i]);
    }

    for (;;) {
        if (stack.len == 0) {
            break;
        }
        auto *node = stack[stack.len - 1];
        stack.len -= 1;
        if (visited.has_key(node)) {
            continue;
        }
        visited[node] = true;
        auto *next = flow.ref_edges.get(node);
        size_t next_offset = flow.current_edge_offset(node);
        if (next && next->len > next_offset) {
            for (size_t i = next_offset; i < next->len; i++) {
                stack.add(next->items[i]);
            }
        } else {
            add_unique_node(out, node);
        }
    }
}

static void collect_expr_borrow_roots(Resolver *resolver, ast::FlowState &flow, ast::Node *expr,
                                      array<ast::Node *> &out) {
    if (!expr) {
        return;
    }
    if (auto *root = resolver->find_root_decl(expr)) {
        append_leaf_borrow_roots(flow, root, out);
        return;
    }
    switch (expr->type) {
    case ast::NodeType::ParenExpr:
        collect_expr_borrow_roots(resolver, flow, expr->data.child_expr, out);
        break;
    case ast::NodeType::TryExpr:
        collect_expr_borrow_roots(resolver, flow, expr->data.try_expr.expr, out);
        break;
    case ast::NodeType::CastExpr:
        collect_expr_borrow_roots(resolver, flow, expr->data.cast_expr.expr, out);
        break;
    default:
        append_leaf_borrow_roots(flow, expr, out, false);
        break;
    }
}

static string describe_exclusive_access_source(ast::Node *n);

static ast::Node *get_call_decl(ast::FnCallExpr &call) {
    if (call.generated_fn) {
        return call.generated_fn;
    }
    if (!call.fn_ref_expr) {
        return nullptr;
    }
    if (call.fn_ref_expr->type == ast::NodeType::Identifier) {
        return call.fn_ref_expr->data.identifier.decl;
    }
    if (call.fn_ref_expr->type == ast::NodeType::DotExpr) {
        return call.fn_ref_expr->data.dot_expr.resolved_decl;
    }
    return nullptr;
}

static ast::FnProto *get_decl_fn_proto(ast::Node *decl) {
    if (!decl) {
        return nullptr;
    }
    if (decl->type == ast::NodeType::FnDef) {
        return &decl->data.fn_def.fn_proto->data.fn_proto;
    }
    if (decl->type == ast::NodeType::GeneratedFn) {
        return &decl->data.generated_fn.fn_proto->data.fn_proto;
    }
    return nullptr;
}

static ChiLifetime *get_first_ref_lifetime(ChiType *type) {
    if (!type) {
        return nullptr;
    }
    if (type->kind == TypeKind::Subtype) {
        auto *final_type = type->data.subtype.final_type;
        if (final_type && final_type != type) {
            if (auto *lt = get_first_ref_lifetime(final_type)) {
                return lt;
            }
        }
    }
    if (type->kind == TypeKind::Optional) {
        return get_first_ref_lifetime(type->get_elem());
    }
    auto *lifetimes = type->get_lifetimes();
    if (!type->is_lifetime_reference() || !lifetimes || lifetimes->len == 0) {
        return nullptr;
    }
    return (*lifetimes)[0];
}


bool Resolver::type_needs_first_ref_lifetime(ChiType *type) {
    if (!type) {
        return false;
    }
    if (type->kind == TypeKind::Subtype) {
        auto *final_type = type->data.subtype.final_type;
        if (final_type && final_type != type) {
            return type_needs_first_ref_lifetime(final_type);
        }
    }
    if (type->kind == TypeKind::Optional) {
        return type_needs_first_ref_lifetime(type->get_elem());
    }
    auto *lifetimes = type->get_lifetimes();
    return type->is_lifetime_reference() && lifetimes && lifetimes->len == 0;
}

ChiType *Resolver::with_first_ref_lifetime(ChiType *type, ChiLifetime *lt) {
    if (!type || !lt) {
        return type;
    }
    if (type->kind == TypeKind::Subtype) {
        auto *final_type = type->data.subtype.final_type;
        if (final_type && final_type != type) {
            return with_first_ref_lifetime(final_type, lt);
        }
        return type;
    }
    if (type->kind == TypeKind::Optional) {
        auto *elem_type = type->get_elem();
        auto *fresh_elem = with_first_ref_lifetime(elem_type, lt);
        if (fresh_elem == elem_type) {
            return type;
        }
        return get_wrapped_type(fresh_elem, TypeKind::Optional);
    }
    if (type->kind == TypeKind::Span) {
        return get_span_type(type->data.span.elem, type->data.span.is_mut, lt);
    }
    if (type->is_reference()) {
        auto *fresh = create_pointer_type(type->data.pointer.elem, type->kind);
        fresh->data.pointer.lifetimes.add(lt);
        return fresh;
    }
    return type;
}

static bool type_has_lifetime_kind(ChiType *type, LifetimeKind kind) {
    if (!type) {
        return false;
    }
    if (type->kind == TypeKind::Subtype) {
        auto *final_type = type->data.subtype.final_type;
        if (final_type && final_type != type && type_has_lifetime_kind(final_type, kind)) {
            return true;
        }
    }
    if (type->kind == TypeKind::Optional) {
        return type_has_lifetime_kind(type->get_elem(), kind);
    }
    auto *lifetimes = type->get_lifetimes();
    if (!type->is_lifetime_reference() || !lifetimes) {
        return false;
    }
    for (auto *lt : *lifetimes) {
        if (lt && lt->kind == kind) {
            return true;
        }
    }
    return false;
}

static bool is_value_borrowing_type(Resolver *resolver, ChiType *type) {
    return resolver && type && !type->is_reference() && type->kind != TypeKind::Span &&
           resolver->is_borrowing_type(type);
}

static bool type_may_propagate_borrow_deps(Resolver *resolver, ChiType *type) {
    if (!resolver || !type) {
        return false;
    }
    if (resolver->is_borrowing_type(type)) {
        return true;
    }
    type = resolver->to_value_type(type);
    if (!type) {
        return false;
    }
    switch (type->kind) {
    case TypeKind::Subtype: {
        auto *final_type = type->data.subtype.final_type;
        if (final_type) {
            return type_may_propagate_borrow_deps(resolver, final_type);
        }
        auto *generic = type->data.subtype.generic;
        return generic && type_may_propagate_borrow_deps(resolver, generic);
    }
    case TypeKind::Placeholder:
        return true;
    default:
        return false;
    }
}

static ChiLifetime *get_param_effective_lifetime(ast::Node *param_node, ChiType *param_type) {
    if (auto *lt = get_first_ref_lifetime(param_type)) {
        return lt;
    }
    if (param_node && param_node->type == ast::NodeType::ParamDecl) {
        return param_node->data.param_decl.borrow_lifetime;
    }
    return nullptr;
}

static bool is_this_identifier_node(ast::Node *node) {
    return node && node->type == ast::NodeType::Identifier &&
           node->data.identifier.kind == ast::IdentifierKind::This;
}

static void add_unique_int32(array<int32_t> &out, int32_t value) {
    size_t insert_at = out.len;
    for (size_t i = 0; i < out.len; i++) {
        auto existing = out[i];
        if (existing == value) {
            return;
        }
        if (value < existing) {
            insert_at = i;
            break;
        }
    }
    out.add({});
    for (size_t i = out.len - 1; i > insert_at; i--) {
        out[i] = out[i - 1];
    }
    out[insert_at] = value;
}

static ast::FnProto::ProjectionCopySummary *find_projection_copy_summary(
    array<ast::FnProto::ProjectionCopySummary> &items, int32_t index) {
    for (size_t i = 0; i < items.len; i++) {
        if (items[i].index == index) {
            return &items[i];
        }
    }
    return nullptr;
}

static void collect_copy_edge_reachable_params(ast::FlowState &flow, array<ast::Node *> &params,
                                               ast::Node *start, array<int32_t> *out_params,
                                               bool *out_reaches_this) {
    array<ast::Node *> stack = {};
    map<ast::Node *, bool> visited = {};
    if (start) {
        stack.add(start);
    }
    for (;;) {
        if (stack.len == 0) {
            break;
        }
        auto *node = stack[stack.len - 1];
        stack.len -= 1;
        if (!node || visited.has_key(node)) {
            continue;
        }
        visited[node] = true;

        if (out_reaches_this && is_this_identifier_node(node)) {
            *out_reaches_this = true;
        }
        if (out_params) {
            for (size_t i = 0; i < params.len; i++) {
                if (params[i] == node) {
                    add_unique_int32(*out_params, static_cast<int32_t>(i));
                    break;
                }
            }
        }

        if (auto *next = flow.copy_edges.get(node)) {
            for (size_t i = 0; i < next->len; i++) {
                stack.add(next->items[i]);
            }
        }
    }
}

static ast::Node *get_summary_decl(ast::Node *decl) {
    if (!decl) {
        return nullptr;
    }
    if (decl->type == ast::NodeType::GeneratedFn) {
        auto *original = decl->data.generated_fn.original_fn;
        decl = original ? original : decl;
    }
    if (decl->type == ast::NodeType::FnDef && decl->data.fn_def.is_generated) {
        auto *root = decl->get_root_node();
        if (root && root->type == ast::NodeType::FnDef) {
            return root;
        }
    }
    if (decl->type == ast::NodeType::VarDecl) {
        auto *expr = decl->data.var_decl.expr;
        if (expr && expr->type == ast::NodeType::FnDef) {
            return expr;
        }
        if (expr && expr->resolved_node) {
            return expr->resolved_node;
        }
    }
    return decl;
}

static ast::Node *get_call_summary_decl(ast::FnCallExpr &call) {
    return get_summary_decl(get_call_decl(call));
}

static ast::FnProto *get_call_effect_proto(ast::FnCallExpr &call) {
    return get_decl_fn_proto(get_call_summary_decl(call));
}

static ast::FnProto *get_decl_summary_proto(ast::Node *decl) {
    return get_decl_fn_proto(get_summary_decl(decl));
}

static ast::FnProto *get_call_signature_proto(ast::FnCallExpr &call) {
    return get_decl_fn_proto(get_call_decl(call));
}

static bool call_has_instance_receiver(ast::FnCallExpr &call, ast::FnProto *proto) {
    if (!proto || call.fn_ref_expr->type != ast::NodeType::DotExpr) {
        return false;
    }
    auto *fn_type = call.fn_ref_expr->resolved_type;
    if (!fn_type) {
        return false;
    }
    fn_type = fn_type->eval();
    if (!fn_type || fn_type->kind == TypeKind::FnLambda) {
        fn_type = fn_type && fn_type->kind == TypeKind::FnLambda ? fn_type->data.fn_lambda.fn : fn_type;
    }
    return fn_type && fn_type->kind == TypeKind::Fn && fn_type->data.fn.container_ref &&
           !fn_type->data.fn.is_static;
}

static void collect_fn_nodes_from_decl(ast::Node *decl, array<ast::Node *> &out) {
    if (!decl) {
        return;
    }
    if (decl->resolved_node) {
        collect_fn_nodes_from_decl(decl->resolved_node, out);
    }
    if (decl->type == ast::NodeType::FnDef) {
        for (auto *existing : out) {
            if (existing == decl) {
                return;
            }
        }
        add_unique_node(out, decl);
        if (decl->data.fn_def.body) {
            visit_async_children(decl->data.fn_def.body, true, [&](ast::Node *child) {
                collect_fn_nodes_from_decl(child, out);
                return false;
            });
        }
        return;
    }
    if (decl->type == ast::NodeType::StructDecl) {
        for (auto member : decl->data.struct_decl.members) {
            collect_fn_nodes_from_decl(member, out);
        }
        return;
    }
    if (decl->type == ast::NodeType::EnumDecl) {
        for (auto variant : decl->data.enum_decl.variants) {
            collect_fn_nodes_from_decl(variant, out);
        }
        if (decl->data.enum_decl.base_struct) {
            collect_fn_nodes_from_decl(decl->data.enum_decl.base_struct, out);
        }
        return;
    }
    if (decl->type == ast::NodeType::ImplementBlock) {
        for (auto impl_member : decl->data.implement_block.members) {
            collect_fn_nodes_from_decl(impl_member, out);
        }
        return;
    }
    visit_async_children(decl, true, [&](ast::Node *child) {
        collect_fn_nodes_from_decl(child, out);
        return false;
    });
}

static bool roots_overlap(array<ast::Node *> &a, array<ast::Node *> &b, ast::Node **shared = nullptr) {
    for (auto *lhs : a) {
        for (auto *rhs : b) {
            if (lhs == rhs) {
                if (shared) {
                    *shared = lhs;
                }
                return true;
            }
        }
    }
    return false;
}

IntrinsicSymbol Resolver::resolve_intrinsic_symbol(ast::Node *node) {
    if (node->symbol != IntrinsicSymbol::None) {
        return node->symbol;
    }

    auto sym_p = m_ctx->intrinsic_symbols.get(resolve_intrinsic_id(node));
    if (sym_p) {
        node->symbol = *sym_p;
        if (node->type == NodeType::FnDef && !m_ctx->intrinsic_decls.get(*sym_p)) {
            m_ctx->intrinsic_decls[*sym_p] = node;
        }
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

ast::Node *Resolver::synthesize_method_lambda(ast::Node *dot_node, ChiStructMember *member,
                                              ResolveScope &scope) {
    auto &dot = dot_node->data.dot_expr;
    auto &orig_proto = member->node->data.fn_def.fn_proto->data.fn_proto;
    auto &method_fn = member->resolved_type->data.fn;

    auto make = [&](ast::NodeType type, ast::Node *src = nullptr) {
        auto *n = create_node(type);
        n->token = src ? src->token : dot_node->token;
        n->module = dot_node->module;
        return n;
    };

    auto *lambda_proto = make(ast::NodeType::FnProto);
    auto *lambda_def = make(ast::NodeType::FnDef);
    lambda_def->name = fmt::format("__method_lambda_{}_{}", member->get_name(), dot_node->id);
    lambda_def->parent_fn = scope.parent_fn_node;
    lambda_def->data.fn_def.fn_proto = lambda_proto;
    lambda_def->data.fn_def.fn_kind = ast::FnKind::Lambda;
    lambda_def->data.fn_def.is_generated = true;
    lambda_def->data.fn_def.decl_spec = get_allocator()->create_decl_spec();
    lambda_proto->data.fn_proto.fn_def_node = lambda_def;
    lambda_proto->data.fn_proto.return_type = orig_proto.return_type;

    // Build params with pre-seeded resolved types — for generic methods the
    // original type expression refers to type params (T) that wouldn't
    // substitute in the lambda's scope; pre-seeding short-circuits resolution.
    array<ast::Node *> arg_idents;
    for (size_t i = 0; i < orig_proto.params.len; i++) {
        auto *orig = orig_proto.params[i];
        auto *param = make(ast::NodeType::ParamDecl, orig);
        param->name = orig->name;
        param->parent_fn = lambda_def;
        param->data.param_decl.is_variadic = orig->data.param_decl.is_variadic;
        param->resolved_type = i < method_fn.params.len ? method_fn.params[i] : nullptr;
        lambda_proto->data.fn_proto.params.add(param);

        auto *ident = make(ast::NodeType::Identifier, orig);
        ident->name = orig->name;
        ident->parent_fn = lambda_def;
        ident->data.identifier.kind = ast::IdentifierKind::Value;
        ident->data.identifier.decl = param;
        arg_idents.add(ident);
    }

    // Body: fresh DotExpr sharing the receiver subtree (so capture registration
    // runs during re-resolution), wrapped in a fresh FnCallExpr. A new dot node
    // is used so it doesn't carry a resolved_node redirect back to the lambda.
    auto clear_cache = [](ast::Node *root) {
        auto impl = [](auto &self, ast::Node *n) -> void {
            if (!n) return;
            n->resolved_type = nullptr;
            n->analysis = {};
            visit_async_children(n, true, [&](ast::Node *child) {
                self(self, child);
                return false;
            });
        };
        impl(impl, root);
    };
    clear_cache(dot.expr);

    auto *inner_dot = make(ast::NodeType::DotExpr, dot_node);
    inner_dot->name = dot_node->name;
    inner_dot->parent_fn = lambda_def;
    inner_dot->data.dot_expr.expr = dot.expr;
    inner_dot->data.dot_expr.field = dot.field;
    inner_dot->data.dot_expr.is_optional_chain = dot.is_optional_chain;

    auto *inner_call = make(ast::NodeType::FnCallExpr);
    inner_call->parent_fn = lambda_def;
    inner_call->data.fn_call_expr.fn_ref_expr = inner_dot;
    for (auto *a : arg_idents) {
        inner_call->data.fn_call_expr.args.add(a);
    }

    auto *body = make(ast::NodeType::Block);
    body->data.block.scope =
        m_ctx->allocator->create_scope(scope.block ? scope.block->scope : nullptr);
    body->data.block.is_arrow = true;
    if (method_fn.return_type && method_fn.return_type->kind != TypeKind::Void) {
        auto *ret = make(ast::NodeType::ReturnStmt);
        ret->parent_fn = lambda_def;
        ret->data.return_stmt.expr = inner_call;
        body->data.block.statements.add(ret);
    } else {
        body->data.block.statements.add(inner_call);
    }
    lambda_def->data.fn_def.body = body;

    resolve(lambda_def, scope);
    return lambda_def;
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
        return true;
    case TypeKind::Optional:
    case TypeKind::Array:
    case TypeKind::Span:
        return is_borrowing_type(type->get_elem());
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
        if (st.member_intrinsics.has_key(IntrinsicSymbol::Copy)) {
            // Copy handles the container's own data, but if any type parameter
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

ast::ConversionType Resolver::resolve_conversion_type(ChiType *from_type, ChiType *to_type,
                                                      bool is_explicit_cast) {
    if (!from_type || !to_type) {
        return ast::ConversionType::None;
    }
    if (is_same_type(from_type, to_type)) {
        return is_explicit_cast ? ast::ConversionType::NoOp : ast::ConversionType::None;
    }
    if (is_explicit_cast) {
        if (use_implicit_owning_coercion(from_type, to_type)) {
            return ast::ConversionType::OwningCoercion;
        }
        return ast::ConversionType::ValueCast;
    }
    if (use_implicit_owning_coercion(from_type, to_type)) {
        return ast::ConversionType::OwningCoercion;
    }
    return ast::ConversionType::None;
}

bool Resolver::can_forward_variadic_pack_directly(ChiType *arg_type, ChiType *param_type) {
    if (!param_type || param_type->kind != TypeKind::Span) {
        return false;
    }

    auto *value_type = to_value_type(arg_type);
    if (!value_type || value_type->kind != TypeKind::Span) {
        return false;
    }

    auto *arg_elem = value_type->get_elem();
    auto *param_elem = param_type->get_elem();
    if (!arg_elem || !param_elem || !is_same_type(arg_elem, param_elem)) {
        return false;
    }

    return !param_type->data.span.is_mut || value_type->data.span.is_mut;
}

ChiType *Resolver::resolve_common_value_type(ChiType *left_type, ChiType *right_type,
                                             ChiType *preferred_type) {
    if (!left_type || !right_type) {
        return left_type ? left_type : right_type;
    }

    bool left_is_null = left_type->kind == TypeKind::Null;
    bool right_is_null = right_type->kind == TypeKind::Null;
    bool left_is_optional = left_type->kind == TypeKind::Optional;
    bool right_is_optional = right_type->kind == TypeKind::Optional;
    bool has_preferred = preferred_type && preferred_type->kind != TypeKind::Void &&
                         preferred_type->kind != TypeKind::Any;
    bool left_to_preferred = has_preferred && can_assign(left_type, preferred_type, false);
    bool right_to_preferred = has_preferred && can_assign(right_type, preferred_type, false);

    if (left_to_preferred && right_to_preferred) {
        return preferred_type;
    }

    if (left_is_null && right_is_null) {
        return left_type;
    }
    if (left_is_null && right_type->kind != TypeKind::Void) {
        return right_is_optional ? right_type : get_wrapped_type(right_type, TypeKind::Optional);
    }
    if (right_is_null && left_type->kind != TypeKind::Void) {
        return left_is_optional ? left_type : get_wrapped_type(left_type, TypeKind::Optional);
    }

    bool right_to_left = can_assign(right_type, left_type, false);
    bool left_to_right = can_assign(left_type, right_type, false);

    if (right_to_left && !left_to_right) {
        return left_type;
    }
    if (left_to_right && !right_to_left) {
        return right_type;
    }
    if (right_to_left && left_to_right && left_is_optional != right_is_optional) {
        return left_is_optional ? left_type : right_type;
    }

    return left_type;
}

// Check if a type needs destruction (has destructor or has fields that need destruction)
bool Resolver::type_needs_destruction(ChiType *type) {
    if (!type)
        return false;

    // Placeholders conservatively always need destruction — the concrete type may have a destructor
    if (type->kind == TypeKind::Placeholder)
        return true;

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
        auto generic = type->data.subtype.generic;
        if (generic && generic->kind == TypeKind::Struct) {
            return get_struct_member(generic, "delete") != nullptr;
        }
        if (generic && generic->kind == TypeKind::Enum) {
            return enum_needs_destruction(&generic->data.enum_);
        }
        return false;
    }

    if (type->kind == TypeKind::Enum) {
        return enum_needs_destruction(&type->data.enum_);
    }

    // EnumValue: check if base struct or any variant has destructible fields
    if (type->kind == TypeKind::EnumValue) {
        return enum_needs_destruction(type->data.enum_value.parent_enum());
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

array<ChiType *> Resolver::get_placeholder_traits(ChiType *ph) {
    auto traits = ph->data.placeholder.traits;
    auto it = m_where_traits.get(ph);
    if (it) {
        for (auto t : *it)
            traits.add(t);
    }
    return traits;
}

bool Resolver::is_non_copyable(ChiType *type) {
    if (!type)
        return false;
    if (type->kind == TypeKind::Subtype) {
        auto final_type = type->data.subtype.final_type;
        if (final_type)
            return is_non_copyable(final_type);
    }
    if (type->kind == TypeKind::Optional) {
        return is_non_copyable(type->get_elem());
    }
    if (type->kind == TypeKind::Placeholder) {
        bool has_nocopy = false;
        for (auto t : get_placeholder_traits(type)) {
            if (!t || t->kind != TypeKind::Struct || !t->data.struct_.node)
                continue;
            auto sym = resolve_intrinsic_symbol(t->data.struct_.node);
            if (sym == IntrinsicSymbol::Copy)
                return false;
            if (sym == IntrinsicSymbol::NoCopy)
                has_nocopy = true;
        }
        return has_nocopy;
    }
    if (type->kind != TypeKind::Struct)
        return false;
    for (auto iface : type->data.struct_.interfaces) {
        if (iface->inteface_symbol == IntrinsicSymbol::NoCopy)
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

static bool can_move_unaddressable_expr(ast::Node *expr) {
    if (!expr) {
        return false;
    }
    switch (expr->type) {
    case NodeType::ParenExpr:
        return can_move_unaddressable_expr(expr->data.child_expr);
    case NodeType::CastExpr:
        return can_move_unaddressable_expr(expr->data.cast_expr.expr);
    case NodeType::DotExpr:
    case NodeType::IndexExpr:
        return false;
    case NodeType::UnaryOpExpr: {
        auto &data = expr->data.unary_op_expr;
        switch (data.op_type) {
        case TokenType::MUL:
        case TokenType::AND:
        case TokenType::MUTREF:
        case TokenType::MOVEREF:
        case TokenType::KW_MOVE:
            return false;
        case TokenType::LNOT:
            return !data.is_suffix;
        default:
            return true;
        }
    }
    default:
        return true;
    }
}

bool Resolver::should_move_temp_expr(ast::Node *expr, ChiType *type) {
    if (!expr || expr->analysis.moved || is_addressable(expr) || !should_destroy(expr, type)) {
        return false;
    }
    return can_move_unaddressable_expr(expr);
}

void Resolver::mark_temp_moved_if_needed(ast::Node *expr, ChiType *type) {
    if (!should_move_temp_expr(expr, type)) {
        return;
    }
    expr->analysis.moved = true;
}

ast::Node *Resolver::get_moved_expr(ast::Node *expr) {
    if (!expr || !expr->analysis.moved) {
        return expr;
    }
    switch (expr->type) {
    case NodeType::ParenExpr:
        return get_moved_expr(expr->data.child_expr);
    case NodeType::CastExpr:
        return get_moved_expr(expr->data.cast_expr.expr);
    case NodeType::BinOpExpr: {
        auto &data = expr->data.bin_op_expr;
        if (data.op_type == TokenType::QUES && data.op2 && data.op2->analysis.moved) {
            return get_moved_expr(data.op2);
        }
        return expr;
    }
    default:
        return expr;
    }
}

bool Resolver::use_implicit_owning_coercion(ChiType *from_type, ChiType *to_type) {
    if (!from_type || !to_type) {
        return false;
    }
    if (to_type->kind == TypeKind::Optional &&
        from_type->kind != TypeKind::Optional &&
        from_type->kind != TypeKind::Null) {
        return true;
    }
    if (to_type->kind == TypeKind::Any &&
        from_type->kind != TypeKind::Any &&
        from_type->kind != TypeKind::Undefined &&
        from_type->kind != TypeKind::ZeroInit) {
        return true;
    }
    return false;
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
        // No inferred type yet — try substituting the inner placeholder directly.
        // This handles the case where wrap_placeholders_with_infer() created an Infer(T)
        // but can_assign never filled in inferred_type (e.g. Array<Infer(T)> used as
        // value_type for an array literal whose element type was resolved independently).
        if (type->data.infer.placeholder) {
            return handle_placeholder(type->data.infer.placeholder, subs);
        }
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
        auto *lt = type->data.span.lifetimes.len > 0 ? type->data.span.lifetimes[0] : nullptr;
        return get_span_type(elem_type, type->data.span.is_mut, lt);
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

        fn_type->is_placeholder = fn_type_has_placeholder(fn_type);
        return fn_type;
    }

    case TypeKind::Tuple: {
        TypeList elements;
        for (auto elem : type->data.tuple.elements) {
            elements.add(make_recursive_call(elem, subs));
        }
        return get_tuple_type(elements);
    }

    case TypeKind::Enum: {
        auto &data = type->data.enum_;

        if (data.type_params.len > 0) {
            bool has_placeholder_param = false;
            array<ChiType *> subst_args;
            for (auto tp : data.type_params) {
                auto subst = make_recursive_call(to_value_type(tp), subs);
                subst_args.add(subst);
                if (to_value_type(tp) != subst)
                    has_placeholder_param = true;
            }
            if (has_placeholder_param) {
                ChiType *base_generic = type;
                if (data.resolved_generic) {
                    base_generic = data.resolved_generic;
                }
                return get_enum_subtype(base_generic, &subst_args);
            }
        }

        return type;
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
        auto enum_type = ev_data.enum_type;

        if (enum_type && enum_type->kind == TypeKind::Subtype) {
            enum_type = make_recursive_call(enum_type, subs);
        } else if (enum_type && enum_type->kind == TypeKind::Enum &&
                   enum_type->data.enum_.type_params.len > 0) {
            bool has_placeholder_param = false;
            array<ChiType *> subst_args;
            for (auto tp : enum_type->data.enum_.type_params) {
                auto value_tp = to_value_type(tp);
                auto subst = make_recursive_call(value_tp, subs);
                subst_args.add(subst);
                if (value_tp != subst) {
                    has_placeholder_param = true;
                }
            }
            if (has_placeholder_param) {
                auto base_generic = enum_type->data.enum_.resolved_generic
                    ? enum_type->data.enum_.resolved_generic
                    : enum_type;
                enum_type = get_enum_subtype(base_generic, &subst_args);
            }
        }

        if (!(enum_type && enum_type->kind == TypeKind::Subtype &&
              !enum_type->data.subtype.final_type)) {
            enum_type = resolve_enum_value_parent_type(enum_type);
        }
        if (enum_type && enum_type->kind == TypeKind::Enum &&
            !is_enum_value_placeholder(enum_type)) {
            if (ev_data.member) {
                if (auto concrete_member = enum_type->data.enum_.find_member(ev_data.member->name)) {
                    return concrete_member->resolved_type;
                }
            }
            return enum_type->data.enum_.base_value_type;
        }

        auto new_ev = create_type(TypeKind::EnumValue);
        new_ev->data.enum_value = ev_data;
        new_ev->name = type->name;
        new_ev->display_name = type->display_name;

        if (enum_type && (enum_type->kind == TypeKind::Enum ||
                          enum_type->kind == TypeKind::Subtype)) {
            new_ev->data.enum_value.enum_type = enum_type;
        }
        update_enum_value_member(new_ev, ev_data.member);
        new_ev->is_placeholder = is_enum_value_placeholder(new_ev->data.enum_value.enum_type);

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
                auto new_type = member->is_method()
                                    ? member->resolved_type
                                    : make_recursive_call(member->resolved_type, subs);
                auto new_member = dst->data.struct_.add_member(get_allocator(), member->get_name(),
                                                               member->node, new_type);
                if (member->parent_member) {
                    new_member->field_index = member->field_index;
                }
            }
            for (size_t i = 0; i < src->data.struct_.members.len; i++) {
                auto orig = src->data.struct_.members[i];
                if (orig->parent_member) {
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

ChiType *Resolver::resolve_concrete_subtypes(ChiType *type, ast::Node *origin) {
    if (!type || !type->has_unresolved_subtype()) {
        return type;
    }

    auto handle_placeholder = [](ChiType *type, ChiTypeSubtype *) -> ChiType * {
        return type;
    };

    auto make_recursive_call = [this, origin](ChiType *type, ChiTypeSubtype *) -> ChiType * {
        return resolve_concrete_subtypes(type, origin);
    };

    auto resolved = recursive_type_replace(type, nullptr, handle_placeholder, make_recursive_call);
    if (resolved->kind == TypeKind::Subtype && !resolved->is_placeholder &&
        !resolved->data.subtype.final_type) {
        return resolve_subtype(resolved, origin);
    }

    return resolved;
}

ChiType *Resolver::finalize_fn_type(ChiType *type, ast::Node *origin) {
    if (!type || type->kind != TypeKind::Fn || type->is_placeholder ||
        !type->has_unresolved_subtype()) {
        return type;
    }

    auto resolved = resolve_concrete_subtypes(type, origin);
    assert(!resolved || resolved->kind == TypeKind::Fn);
    return resolved;
}

ChiType *Resolver::finalize_fn_decl_type(ast::Node *decl, ast::Node *origin) {
    if (!decl || !decl->resolved_type) {
        return decl ? decl->resolved_type : nullptr;
    }

    auto resolved = finalize_fn_type(decl->resolved_type, origin);
    if (resolved == decl->resolved_type) {
        return resolved;
    }

    decl->resolved_type = resolved;

    ast::Node *proto = nullptr;
    if (decl->type == NodeType::FnDef) {
        proto = decl->data.fn_def.fn_proto;
    } else if (decl->type == NodeType::GeneratedFn) {
        proto = decl->data.generated_fn.fn_proto;
    }

    if (proto) {
        proto->resolved_type = resolved;
        auto &params = resolved->data.fn.params;
        auto &proto_params = proto->data.fn_proto.params;
        size_t count = params.len < proto_params.len ? params.len : proto_params.len;
        for (size_t i = 0; i < count; i++) {
            proto_params[i]->resolved_type = params[i];
        }
    }

    return resolved;
}

ChiType *Resolver::finalize_member_fn_type(ChiStructMember *member, ast::Node *origin) {
    if (!member || !member->resolved_type) {
        return member ? member->resolved_type : nullptr;
    }

    if (member->node && member->node->resolved_type == member->resolved_type) {
        auto resolved = finalize_fn_decl_type(member->node, origin);
        member->resolved_type = resolved;
        return resolved;
    }

    auto resolved = finalize_fn_type(member->resolved_type, origin);
    member->resolved_type = resolved;
    return resolved;
}

ChiType *Resolver::wrap_placeholders_with_infer(ChiType *type) {
    map<ChiType *, ChiType *> infer_map;
    return wrap_placeholders_with_infer(type, infer_map);
}

ChiType *Resolver::wrap_placeholders_with_infer(ChiType *type,
                                                 map<ChiType *, ChiType *> &infer_map) {
    auto handle_placeholder = [this, &infer_map](ChiType *placeholder,
                                                  ChiTypeSubtype *) -> ChiType * {
        if (auto existing = infer_map.get(placeholder)) {
            return *existing;
        }
        auto infer_type = create_type(TypeKind::Infer);
        infer_type->data.infer.placeholder = placeholder;
        infer_type->is_placeholder = true;
        infer_map[placeholder] = infer_type;
        return infer_type;
    };

    auto make_recursive_call = [this, &infer_map](ChiType *type,
                                                   ChiTypeSubtype *) -> ChiType * {
        return wrap_placeholders_with_infer(type, infer_map);
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

    // Type inference only needs the arguments that were actually provided.
    // Trailing defaulted parameters may be omitted at the call site.
    if (arg_types->len > fn->params.len) {
        return false;
    }

    // Handle placeholder mapping via unification
    auto handle_placeholder = [this, fn, inferred_types](ChiType *placeholder,
                                                         ChiType *concrete) -> bool {
        // If the arg type is the same placeholder, it provides no new information
        // (e.g. lambda param inherited its type from the expected placeholder context).
        // Just skip — don't record or check consistency.
        if (concrete->kind == TypeKind::Placeholder && concrete == placeholder) {
            return true;
        }

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
            // New inference — void is not a value type, so when a placeholder
            // is inferred from a void context (e.g. void-returning callback),
            // bind to Unit instead.
            if (concrete->kind == TypeKind::Void) {
                concrete = m_ctx->rt_unit_type;
            }
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
    for (size_t i = 0; i < arg_types->len; i++) {
        ChiType *param_type = fn->params[i];
        ChiType *arg_type = (*arg_types)[i];

        // Skip deferred lambda args (null) — they couldn't be resolved without
        // knowing the type params first
        if (!arg_type) continue;

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

ChiType *Resolver::eval_struct_type(ChiType *type, ast::Node *origin) {
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
            if (!rt_array) {
                return nullptr;
            }
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
            if (!rt_span) {
                return nullptr;
            }
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
    } else if (sty->kind == TypeKind::FnLambda) {
        sty = sty->data.fn_lambda.internal;
    } else if (sty->kind == TypeKind::TypeSymbol) {
        if (auto underlying_type = sty->data.type_symbol.underlying_type) {
            sty = underlying_type;
        }
    }
    if (sty->kind == TypeKind::Subtype) {
        sty = resolve_subtype(sty, origin);
    }
    if (sty->kind == TypeKind::Enum) {
        sty = sty->data.enum_.base_value_type;
    }
    if (sty->kind == TypeKind::EnumValue) {
        sty = sty->data.enum_value.resolved_struct;
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


static ChiType *get_struct_access_root_type(ast::Node *expr) {
    if (!expr) {
        return nullptr;
    }
    switch (expr->type) {
    case ast::NodeType::DotExpr: {
        auto *member = expr->data.dot_expr.resolved_struct_member;
        if (member && member->is_field()) {
            return get_struct_access_root_type(expr->data.dot_expr.effective_expr());
        }
        return expr->resolved_type;
    }
    case ast::NodeType::ParenExpr:
    case ast::NodeType::PackExpansion:
        return get_struct_access_root_type(expr->data.child_expr);
    default:
        return expr->resolved_type;
    }
}

void Resolver::add_cleanup_var(ast::Block *block, ast::Node *var) {
    if (!block || !var) {
        return;
    }
    for (auto existing : block->cleanup_vars) {
        if (existing == var) {
            return;
        }
    }
    block->cleanup_vars.add(var);
}

void Resolver::add_fn_body_param_cleanups(ast::Node *fn_node, ast::Node *body) {
    if (!fn_node || !body || body->type != NodeType::Block) {
        return;
    }

    ast::FnProto *proto = nullptr;
    bool *has_cleanup = nullptr;

    if (fn_node->type == NodeType::FnDef) {
        proto = get_decl_fn_proto(fn_node);
        has_cleanup = &fn_node->data.fn_def.has_cleanup;
    } else if (fn_node->type == NodeType::GeneratedFn) {
        proto = get_decl_fn_proto(fn_node);
    } else {
        return;
    }

    for (auto param : proto->params) {
        if (should_destroy(param) && !param->analysis.is_capture()) {
            add_cleanup_var(&body->data.block, param);
            if (has_cleanup) {
                *has_cleanup = true;
            }
        }
    }
}

ChiType *Resolver::get_enum_type(ChiType *type) {
    if (!type) {
        return nullptr;
    }

    auto current = type->eval();
    switch (current->kind) {
    case TypeKind::TypeSymbol:
        return get_enum_type(current->data.type_symbol.underlying_type);
    case TypeKind::This:
        return get_enum_type(current->eval());
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef:
    case TypeKind::Optional:
        return get_enum_type(current->get_elem());
    case TypeKind::Enum:
        return current;
    case TypeKind::EnumValue:
        return get_enum_type(current->data.enum_value.enum_type);
    case TypeKind::Subtype: {
        auto &subtype = current->data.subtype;
        if (subtype.final_type) {
            return get_enum_type(subtype.final_type);
        }
        auto resolved = resolve_subtype(current);
        if (resolved && resolved != current) {
            return get_enum_type(resolved);
        }
        if (subtype.generic && subtype.generic->kind == TypeKind::Enum) {
            return subtype.generic;
        }
        return nullptr;
    }
    default:
        return nullptr;
    }
}

ChiType *Resolver::get_enum_root(ChiType *type) {
    auto enum_type = get_enum_type(type);
    if (!enum_type || enum_type->kind != TypeKind::Enum) {
        return nullptr;
    }
    return enum_type->data.enum_.resolved_generic ? enum_type->data.enum_.resolved_generic : enum_type;
}

ChiType *Resolver::resolve_enum_value_parent_type(ChiType *enum_type) {
    if (!enum_type || enum_type->kind != TypeKind::Subtype) {
        return enum_type;
    }

    auto &enum_subtype = enum_type->data.subtype;
    if (!enum_subtype.generic || enum_subtype.generic->kind != TypeKind::Enum) {
        return enum_type;
    }
    if (enum_subtype.final_type) {
        return enum_subtype.final_type;
    }
    if (!enum_type->is_placeholder) {
        auto resolved_enum = resolve_subtype(enum_type);
        if (resolved_enum && resolved_enum->kind == TypeKind::Enum) {
            return resolved_enum;
        }
    }
    return enum_type;
}

void Resolver::update_enum_value_member(ChiType *enum_value_type, ChiEnumVariant *member) {
    if (!member || !enum_value_type || enum_value_type->kind != TypeKind::EnumValue) {
        return;
    }

    auto enum_type = enum_value_type->data.enum_value.enum_type;
    if (!enum_type || enum_type->kind != TypeKind::Enum) {
        return;
    }
    if (auto concrete_member = enum_type->data.enum_.find_member(member->name)) {
        enum_value_type->data.enum_value.member = concrete_member;
    }
}

bool Resolver::enum_needs_destruction(ChiTypeEnum *enum_type) {
    if (!enum_type) {
        return false;
    }

    auto base_value_type = enum_type->base_value_type;
    auto base_struct = base_value_type ? base_value_type->data.enum_value.resolved_struct : nullptr;
    if (base_struct) {
        for (auto field : base_struct->data.struct_.fields) {
            if (type_needs_destruction(field->resolved_type)) {
                return true;
            }
        }
    }

    for (auto variant : enum_type->variants) {
        auto variant_struct = variant->resolved_type->data.enum_value.variant_struct;
        if (!variant_struct) {
            continue;
        }
        for (auto field : variant_struct->data.struct_.fields) {
            if (type_needs_destruction(field->resolved_type)) {
                return true;
            }
        }
    }

    return false;
}

bool Resolver::is_enum_value_placeholder(ChiType *enum_type) {
    if (!enum_type) {
        return false;
    }
    if (enum_type->is_placeholder) {
        return true;
    }
    if (enum_type->kind != TypeKind::Enum) {
        return false;
    }
    for (auto param : enum_type->data.enum_.type_params) {
        if (to_value_type(param)->is_placeholder) {
            return true;
        }
    }
    return false;
}

void Resolver::record_specialized_fn_env(ast::Node *node,
                                         map<ChiType *, ChiType *> *base_subs) {
    if (!node || node->type != ast::NodeType::FnDef || !node->resolved_type ||
        node->resolved_type->kind != TypeKind::Fn) {
        return;
    }

    map<ChiType *, ChiType *> subs;
    if (base_subs) {
        for (auto &pair : base_subs->get()) {
            subs[pair.first] = pair.second;
        }
    }

    auto root = node->get_root_node();
    auto generic_type = root && root != node ? root->resolved_type : nullptr;
    auto concrete_type = node->resolved_type;

    std::function<void(ChiType *, ChiType *)> collect = [&](ChiType *generic_type,
                                                            ChiType *concrete_type) {
        if (!generic_type || !concrete_type) {
            return;
        }
        if (generic_type->kind == TypeKind::TypeSymbol) {
            collect(generic_type->data.type_symbol.underlying_type, concrete_type);
            return;
        }
        if (concrete_type->kind == TypeKind::TypeSymbol) {
            collect(generic_type, concrete_type->data.type_symbol.underlying_type);
            return;
        }
        if (generic_type->kind == TypeKind::Infer && generic_type->data.infer.placeholder) {
            collect(generic_type->data.infer.placeholder, concrete_type);
            return;
        }
        if (generic_type->kind == TypeKind::Placeholder) {
            if (!subs.get(generic_type)) {
                subs[generic_type] = concrete_type;
            }
            return;
        }
        if (generic_type->kind != concrete_type->kind) {
            return;
        }

        switch (generic_type->kind) {
        case TypeKind::Pointer:
        case TypeKind::Reference:
        case TypeKind::MutRef:
        case TypeKind::MoveRef:
        case TypeKind::Optional:
        case TypeKind::Array:
        case TypeKind::Span:
            collect(generic_type->get_elem(), concrete_type->get_elem());
            return;
        case TypeKind::Subtype: {
            auto &generic_sub = generic_type->data.subtype;
            auto &concrete_sub = concrete_type->data.subtype;
            if (generic_sub.args.len != concrete_sub.args.len) {
                return;
            }
            for (size_t i = 0; i < generic_sub.args.len; i++) {
                collect(generic_sub.args[i], concrete_sub.args[i]);
            }
            return;
        }
        case TypeKind::Fn: {
            auto &generic_fn = generic_type->data.fn;
            auto &concrete_fn = concrete_type->data.fn;
            collect(generic_fn.return_type, concrete_fn.return_type);
            for (size_t i = 0; i < generic_fn.params.len && i < concrete_fn.params.len; i++) {
                collect(generic_fn.params[i], concrete_fn.params[i]);
            }
            if (generic_fn.container_ref && concrete_fn.container_ref) {
                collect(generic_fn.container_ref, concrete_fn.container_ref);
            }
            return;
        }
        case TypeKind::FnLambda:
            if (generic_type->data.fn_lambda.fn && concrete_type->data.fn_lambda.fn) {
                collect(generic_type->data.fn_lambda.fn, concrete_type->data.fn_lambda.fn);
            }
            return;
        default:
            return;
        }
    };

    if (generic_type && generic_type->kind == TypeKind::Fn) {
        collect(generic_type, concrete_type);
    }

    if (subs.size() == 0) {
        return;
    }

    auto fn_id = resolve_global_id(node);
    auto source_node = root ? root : node;
    auto source_type = generic_type ? generic_type : concrete_type;
    m_ctx->generics.record_fn(fn_id, fn_id, source_node, source_type, subs);
}

void Resolver::ensure_enum_subtype_final_type(ChiType *generic, ChiType *subtype) {
    assert(generic && generic->kind == TypeKind::Enum);
    assert(subtype && subtype->kind == TypeKind::Subtype);

    auto prev_resolving = m_resolving_subtype;
    m_resolving_subtype = subtype;

    auto &gen = generic->data.enum_;
    auto &subtype_data = subtype->data.subtype;

    if (subtype->is_placeholder || gen.resolve_status < ResolveStatus::BodiesResolved) {
        m_resolving_subtype = prev_resolving;
        return;
    }

    auto final_type = subtype_data.final_type;
    auto needs_materialization = !final_type;
    if (!needs_materialization && final_type->kind == TypeKind::Enum) {
        needs_materialization = final_type->data.enum_.variants.len != gen.variants.len;
    }
    if (!needs_materialization) {
        m_resolving_subtype = prev_resolving;
        return;
    }

    auto concrete_env = m_ctx->generics.struct_envs.get(subtype->global_id);
    array<ast::Node *> specialized_method_nodes;

    auto concrete_enum = create_type(TypeKind::Enum);
    concrete_enum->name = subtype->global_id;
    concrete_enum->display_name = subtype->global_id;
    concrete_enum->global_id = subtype->global_id;
    concrete_enum->data.enum_.node = gen.node;
    concrete_enum->data.enum_.resolved_generic = generic;
    concrete_enum->data.enum_.discriminator = gen.discriminator;
    concrete_enum->data.enum_.enum_header_struct = gen.enum_header_struct;

    auto make_concrete_enum_method = [&](ChiStructMember *member, ChiType *container_type) {
        auto type = m_ctx->allocator->create_type(member->resolved_type->kind);
        member->resolved_type->clone(type);
        type->data.fn.container_ref = get_pointer_type(container_type, TypeKind::Reference);
        type = type_placeholders_sub_selective(type, &subtype_data, gen.node);
        type->is_placeholder = fn_type_has_placeholder(type);

        auto node = get_allocator()->create_node(ast::NodeType::FnDef);
        member->node->clone(node);
        node->name = member->node->name;
        node->token = member->node->token;
        node->module = member->node->module;
        node->resolved_type = type;
        node->root_node = member->node->get_root_node();
        node->data.fn_def.is_generated = true;

        auto new_proto = get_allocator()->create_node(ast::NodeType::FnProto);
        auto orig_proto = member->node->data.fn_def.fn_proto;
        orig_proto->clone(new_proto);
        new_proto->module = node->module;
        new_proto->resolved_type = type;
        node->data.fn_def.fn_proto = new_proto;
        node->data.fn_def.fn_proto->data.fn_proto.fn_def_node = node;
        specialized_method_nodes.add(node);

        return std::make_pair(node, type);
    };

    auto specialize_struct_methods = [&](ChiType *src_struct, ChiType *dst_struct,
                                         ChiType *container_type) {
        if (!src_struct || !dst_struct || src_struct->kind != TypeKind::Struct ||
            dst_struct->kind != TypeKind::Struct) {
            return;
        }
        for (auto member : src_struct->data.struct_.members) {
            if (!member->is_method()) {
                continue;
            }
            auto dst_member = dst_struct->data.struct_.find_member(member->get_name());
            if (!dst_member) {
                continue;
            }
            auto [node, type] = make_concrete_enum_method(member, container_type);
            dst_member->node = node;
            dst_member->resolved_type = type;
            if (member->symbol != IntrinsicSymbol::None) {
                dst_member->symbol = member->symbol;
                dst_struct->data.struct_.member_intrinsics[member->symbol] = dst_member;
            }
        }
    };

    auto concrete_bvt = type_placeholders_sub(gen.base_value_type, &subtype_data);
    concrete_bvt->name = subtype->global_id;
    concrete_bvt->display_name = subtype->global_id;
    concrete_bvt->global_id = subtype->global_id;
    concrete_bvt->data.enum_value.enum_type = concrete_enum;
    concrete_bvt->is_placeholder = false;
    concrete_enum->data.enum_.base_value_type = concrete_bvt;
    specialize_struct_methods(gen.base_value_type->data.enum_value.resolved_struct,
                              concrete_bvt->data.enum_value.resolved_struct, concrete_bvt);

    for (auto variant : gen.variants) {
        auto concrete_vt = type_placeholders_sub(variant->resolved_type, &subtype_data);
        auto variant_id = fmt::format("{}.{}", subtype->global_id, variant->name);
        concrete_vt->name = variant_id;
        concrete_vt->display_name = variant_id;
        concrete_vt->global_id = variant_id;
        concrete_vt->data.enum_value.enum_type = concrete_enum;
        concrete_vt->is_placeholder = false;
        specialize_struct_methods(gen.base_value_type->data.enum_value.resolved_struct,
                                  concrete_vt->data.enum_value.resolved_struct, concrete_bvt);

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

    if (gen.base_struct) {
        auto src = gen.base_struct;
        auto dst = create_type(TypeKind::Struct);
        src->clone(dst);
        dst->data.struct_.members = {};
        dst->data.struct_.fields = {};
        dst->data.struct_.member_table = {};

        for (auto member : src->data.struct_.members) {
            if (member->is_method()) {
                auto type = m_ctx->allocator->create_type(member->resolved_type->kind);
                member->resolved_type->clone(type);
                type->data.fn.container_ref =
                    get_pointer_type(concrete_bvt, TypeKind::Reference);
                type = type_placeholders_sub_selective(type, &subtype_data, gen.node);
                type->is_placeholder = fn_type_has_placeholder(type);

                auto node = get_allocator()->create_node(ast::NodeType::FnDef);
                member->node->clone(node);
                node->name = member->node->name;
                node->token = member->node->token;
                node->module = member->node->module;
                node->resolved_type = type;
                node->root_node = member->node->get_root_node();
                node->data.fn_def.is_generated = true;

                auto new_proto = get_allocator()->create_node(ast::NodeType::FnProto);
                auto orig_proto = member->node->data.fn_def.fn_proto;
                orig_proto->clone(new_proto);
                new_proto->module = node->module;
                new_proto->resolved_type = type;
                node->data.fn_def.fn_proto = new_proto;
                node->data.fn_def.fn_proto->data.fn_proto.fn_def_node = node;
                specialized_method_nodes.add(node);

                dst->data.struct_.add_member(get_allocator(), member->get_name(), node, type);
            } else {
                auto new_type = type_placeholders_sub(member->resolved_type, &subtype_data);
                dst->data.struct_.add_member(get_allocator(), member->get_name(), member->node,
                                             new_type);
            }
        }
        concrete_enum->data.enum_.base_struct = dst;
    }

    subtype_data.final_type = concrete_enum;
    for (auto node : specialized_method_nodes) {
        record_specialized_fn_env(node, concrete_env ? &concrete_env->subs : nullptr);
    }
    m_resolving_subtype = prev_resolving;
}

ChiEnumVariant *Resolver::find_expected_enum_variant(const string &name, ChiType *expected_type) {
    auto enum_type = get_enum_type(expected_type);
    if (!enum_type || enum_type->kind != TypeKind::Enum) {
        return nullptr;
    }
    return enum_type->data.enum_.find_member(name);
}

bool Resolver::is_contextual_resolution_ambiguous(const string &name, ast::Node *resolved_decl,
                                                  ResolveScope &scope) {
    if (!resolved_decl) {
        return false;
    }

    if (auto builtin = get_builtin(name)) {
        if (builtin != resolved_decl) {
            return true;
        }
    }

    for (auto current_scope = scope.block ? scope.block->scope : nullptr; current_scope;
         current_scope = current_scope->parent) {
        if (auto symbol = current_scope->find_one(name)) {
            return symbol != resolved_decl;
        }
    }
    return false;
}

void Resolver::resolve_contextual_identifier(ast::Node *node, ResolveScope &scope,
                                             ast::Identifier &data, ChiType **type_override) {
    ChiEnumVariant *variant = nullptr;
    if (scope.value_type) {
        auto expected_type = scope.value_type->eval();
        switch (expected_type ? expected_type->kind : TypeKind::Unknown) {
        case TypeKind::TypeSymbol:
        case TypeKind::This:
        case TypeKind::Enum:
        case TypeKind::EnumValue:
        case TypeKind::Subtype:
            variant = find_expected_enum_variant(node->name, scope.value_type);
            break;
        default:
            break;
        }
    }

    if (variant) {
        auto ambiguous = data.decl_is_provisional && data.decl && data.decl != variant->node;
        data.decl = variant->node;
        data.decl_is_provisional = false;
        data.resolved_decl_source = ast::ResolvedDeclSourceKind::Contextual;
        data.resolved_decl_is_ambiguous = ambiguous;
        if (type_override) {
            *type_override = resolve_expected_enum_variant_type(variant, scope.value_type);
        }
    } else if (data.decl_is_provisional) {
        if (!data.decl) {
            assert(false && "provisional identifier was not resolved");
        }
        data.decl_is_provisional = false;
        data.resolved_decl_source = ast::ResolvedDeclSourceKind::Lexical;
        data.resolved_decl_is_ambiguous = false;
    }
}

ChiType *Resolver::resolve_enum_member_type(ChiType *enum_owner_type, const string &field_name,
                                            ChiEnumVariant **member_out) {
    if (member_out) {
        *member_out = nullptr;
    }
    if (!enum_owner_type) {
        return nullptr;
    }

    auto owner_type = enum_owner_type->eval();
    if (!owner_type) {
        return nullptr;
    }

    if (owner_type->kind == TypeKind::Enum) {
        auto member = owner_type->data.enum_.find_member(field_name);
        if (member_out) {
            *member_out = member;
        }
        return member ? member->resolved_type : nullptr;
    }

    if (owner_type->kind == TypeKind::Subtype) {
        auto &subtype_data = owner_type->data.subtype;
        if (!(subtype_data.generic && subtype_data.generic->kind == TypeKind::Enum)) {
            return nullptr;
        }

        auto concrete_enum = subtype_data.final_type;
        if (!concrete_enum && !owner_type->is_placeholder) {
            concrete_enum = resolve_subtype(owner_type);
        }

        auto member = concrete_enum && concrete_enum->kind == TypeKind::Enum
                          ? concrete_enum->data.enum_.find_member(field_name)
                          : subtype_data.generic->data.enum_.find_member(field_name);
        if (member_out) {
            *member_out = member;
        }
        if (!member) {
            return nullptr;
        }

        auto result_type = concrete_enum && concrete_enum->kind == TypeKind::Enum
                               ? member->resolved_type
                               : type_placeholders_sub(member->resolved_type, &subtype_data);
        if (result_type && result_type->kind == TypeKind::EnumValue) {
            result_type->data.enum_value.enum_type =
                concrete_enum && concrete_enum->kind == TypeKind::Enum ? concrete_enum
                                                                       : owner_type;
            result_type->is_placeholder = result_type->is_placeholder || owner_type->is_placeholder;
        }
        return result_type;
    }

    return nullptr;
}

ChiType *Resolver::resolve_expected_enum_variant_type(ChiEnumVariant *variant,
                                                      ChiType *expected_type) {
    return variant ? (resolve_enum_member_type(expected_type, variant->name) ?: variant->resolved_type)
                   : nullptr;
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

array<ChiStructMember *> Resolver::get_enum_payload_fields(ChiType *type) {
    array<ChiStructMember *> fields;
    if (!type || type->kind != TypeKind::EnumValue) {
        return fields;
    }
    auto variant_struct = type->data.enum_value.variant_struct;
    if (!variant_struct) {
        return fields;
    }
    for (auto field : variant_struct->data.struct_.fields) {
        auto promoted = get_struct_member(type, field->get_name());
        if (promoted) {
            fields.add(promoted);
        }
    }
    return fields;
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
        auto &member_spec = field_member->node->declspec_ref();
        auto is_mutable = member_spec.has_flag(ast::DECL_MUTABLE);
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
    if (!fn || !args) {
        return nullptr;
    }
    auto n_args = args->len;
    auto n_params = fn->params.len;
    auto intrinsic_symbol = fn_decl ? resolve_intrinsic_symbol(fn_decl) : IntrinsicSymbol::None;

    // Count required parameters (those without defaults, excluding variadic)
    size_t params_required = n_params - (fn->is_variadic ? 1 : 0);
    size_t max_args = params_required;
    if (auto *fn_proto = get_decl_fn_proto(fn_decl)) {
        params_required = 0;
        for (size_t i = 0; i < fn_proto->params.len; i++) {
            auto param = fn_proto->params[i];
            if (!param->data.param_decl.effective_default_value() &&
                !param->data.param_decl.is_variadic) {
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

    if (intrinsic_symbol != IntrinsicSymbol::None) {
        auto *intrinsic_decl = get_intrinsic_decl(intrinsic_symbol);
        auto *intrinsic_type = intrinsic_decl ? intrinsic_decl->resolved_type : nullptr;
        auto *callee_type = fn_decl ? fn_decl->resolved_type : nullptr;
        if (intrinsic_type && callee_type) {
            assert(can_assign_fn(callee_type, intrinsic_type) &&
                   can_assign_fn(intrinsic_type, callee_type) &&
                   "intrinsic call does not match canonical prototype");
        }
    }

    // Default values for missing arguments are handled by codegen
    // (compile_construction and compile_fn_args inject them at compile time).
    // We do NOT mutate the AST args list here.

    auto stamp_pack_forward_decision = [&](ast::Node *arg, ChiType *arg_type, ChiType *param_type) {
        if (!arg || arg->type != NodeType::PackExpansion) {
            return;
        }
        arg->data.pack_expansion.can_forward_directly =
            can_forward_variadic_pack_directly(arg_type, param_type);
    };
    auto get_call_param_type = [&](ChiTypeFn *call_fn, size_t index, ast::Node *arg) -> ChiType * {
        if (arg && arg->type == NodeType::PackExpansion && call_fn->is_variadic && !call_fn->is_extern) {
            return call_fn->get_variadic_span_param();
        }
        return call_fn->get_param_at(index);
    };

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

                    // NoCopy types require explicit NoCopy bound on the type param
                    auto param_type = to_value_type(type_param);
                    if (is_non_copyable(type_arg) && !is_non_copyable(param_type)) {
                        error(node, errors::TYPE_NOT_COPYABLE,
                              format_type_display(type_arg));
                        return fn->return_type;
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
            auto param_type = get_call_param_type(fn, i, arg);

            // If we have type args (explicit or inferred from return type), substitute
            // placeholders in parameter types for proper lambda parameter inference
            if (has_explicit_type_args || has_return_type_inference) {
                param_type = type_placeholders_sub_map(param_type, &type_substitutions);
            } else if (param_type && param_type->is_placeholder) {
                // No type args yet — wrap placeholders with Infer so lambda bodies
                // can fill in concrete types for argument-based inference.
                // Lambda args resolved this way will have Infer-wrapped types cached
                // on their nodes; the dispatch path (Path A/B) handles this by
                // returning the specialized return type directly.
                param_type = wrap_placeholders_with_infer(param_type);
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

            for (auto trait : get_placeholder_traits(lookup_key)) {
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

        // When args were resolved with Infer-wrapped placeholders (no explicit type args,
        // no return type inference), we must NOT call resolve_fn_call because it would
        // re-resolve args that already have cached Infer types. Instead, we directly
        // check assignments against the specialized param types and return.
        bool used_infer_wrapping = !has_explicit_type_args && !has_return_type_inference;

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
                        auto spec_fn_type = finalize_member_fn_type(member, node);
                        auto &spec_fn = spec_fn_type->data.fn;

                        if (used_infer_wrapping) {
                            for (size_t i = 0; i < n_args; i++) {
                                auto arg = args->at(i);
                                auto concrete_param = get_call_param_type(&spec_fn, i, arg);
                                auto arg_type = node_get_type(arg);
                                auto concrete_arg_type =
                                    type_placeholders_sub_map(arg_type, &type_substitutions);
                                arg->resolved_type = concrete_arg_type;
                                stamp_pack_forward_decision(arg, concrete_arg_type,
                                                            fn->get_variadic_span_param());
                                if (concrete_param) {
                                    check_assignment(arg, concrete_arg_type, concrete_param, &scope);
                                }
                                track_move_sink(scope.parent_fn_node, arg, concrete_arg_type,
                                                node, concrete_param);
                                ensure_temp_owner(arg, concrete_arg_type, scope);

                                // Mark rvalue/moved args — same logic as the normal path
                                bool is_explicit_move = arg->type == NodeType::UnaryOpExpr &&
                                                        arg->data.unary_op_expr.op_type == TokenType::KW_MOVE;
                                if (concrete_param && !concrete_param->is_reference() &&
                                    (is_explicit_move || should_move_temp_expr(arg, concrete_arg_type)) &&
                                    should_destroy(arg, concrete_arg_type)) {
                                    arg->analysis.moved = true;
                                    if (arg->resolved_outlet && scope.parent_fn_node) {
                                        scope.parent_fn_def()->add_sink_edge(arg->resolved_outlet, node);
                                    }
                                }

                                // NoCopy: error if passing a named value by value to a non-ref param
                                if (concrete_param && !concrete_param->is_reference() && is_addressable(arg) &&
                                    !arg->analysis.moved && is_non_copyable(concrete_arg_type)) {
                                    error(arg, errors::TYPE_NOT_COPYABLE, format_type_display(concrete_arg_type));
                                }
                            }
                            if (scope.parent_fn_node && node->type == NodeType::FnCallExpr) {
                                apply_call_capture_move_effects(*scope.parent_fn_def(),
                                                                node->data.fn_call_expr, node);
                            }
                            return spec_fn.return_type;
                        }

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

        if (used_infer_wrapping) {
            for (size_t i = 0; i < n_args; i++) {
                auto arg = args->at(i);
                auto concrete_param = get_call_param_type(&spec_fn, i, arg);
                auto arg_type = node_get_type(arg);
                auto concrete_arg_type = type_placeholders_sub_map(arg_type, &type_substitutions);
                arg->resolved_type = concrete_arg_type;
                stamp_pack_forward_decision(arg, concrete_arg_type,
                                            fn->get_variadic_span_param());
                if (concrete_param) {
                    check_assignment(arg, concrete_arg_type, concrete_param, &scope);
                }
                track_move_sink(scope.parent_fn_node, arg, concrete_arg_type, node, concrete_param);
                ensure_temp_owner(arg, concrete_arg_type, scope);

                // Mark rvalue/moved args — same logic as the normal path
                bool is_explicit_move = arg->type == NodeType::UnaryOpExpr &&
                                        arg->data.unary_op_expr.op_type == TokenType::KW_MOVE;
                if (concrete_param && !concrete_param->is_reference() &&
                    (is_explicit_move || should_move_temp_expr(arg, concrete_arg_type)) &&
                    should_destroy(arg, concrete_arg_type)) {
                    arg->analysis.moved = true;
                    if (arg->resolved_outlet && scope.parent_fn_node) {
                        scope.parent_fn_def()->add_sink_edge(arg->resolved_outlet, node);
                    }
                }

                // NoCopy: error if passing a named value by value to a non-ref param
                if (concrete_param && !concrete_param->is_reference() && is_addressable(arg) &&
                    !arg->analysis.moved && is_non_copyable(concrete_arg_type)) {
                    error(arg, errors::TYPE_NOT_COPYABLE, format_type_display(concrete_arg_type));
                }
            }
            if (scope.parent_fn_node && node->type == NodeType::FnCallExpr) {
                apply_call_capture_move_effects(*scope.parent_fn_def(),
                                                node->data.fn_call_expr, node);
            }
            return spec_fn.return_type;
        }

        return resolve_fn_call(node, scope, &spec_fn, args, fn_variant);
    }

    // Normal path — shared by regular calls and delegated-from-generic calls
    {
        for (size_t i = 0; i < n_args; i++) {
            auto arg = args->at(i);
            auto param_type = get_call_param_type(fn, i, arg);
            // Clear move_outlet for function args - they're passed by value,
            // not written directly to any outer destination
            auto arg_scope = scope.set_value_type(param_type).set_move_outlet(nullptr);
            auto arg_type = resolve(arg, arg_scope);
            stamp_pack_forward_decision(arg, arg_type, fn->get_variadic_span_param());

            // For C variadic functions, param_type is nullptr for variadic args (any type allowed)
            if (param_type) {
                check_assignment(arg, arg_type, param_type, &scope);
            }

            // Move tracking for function arguments
            track_move_sink(scope.parent_fn_node, arg, arg_type, node, param_type);

            ensure_temp_owner(arg, arg_type, scope);

            // Mark rvalue/moved args — caller transfers ownership to callee
            // via bitwise pass, so callee skips copy and caller
            // skips destroying the temp.
            bool is_explicit_move = arg->type == NodeType::UnaryOpExpr &&
                                    arg->data.unary_op_expr.op_type == TokenType::KW_MOVE;
            if (param_type && !param_type->is_reference() &&
                (is_explicit_move || should_move_temp_expr(arg, arg_type)) &&
                should_destroy(arg, arg_type)) {
                arg->analysis.moved = true;
                // Sink the temp outlet so cleanup doesn't destroy it
                if (arg->resolved_outlet && scope.parent_fn_node) {
                    scope.parent_fn_def()->add_sink_edge(arg->resolved_outlet, node);
                }
            }

            // NoCopy: error if passing a named value by value to a non-ref param
            if (param_type && !param_type->is_reference() && is_addressable(arg) &&
                !arg->analysis.moved && is_non_copyable(arg_type)) {
                error(arg, errors::TYPE_NOT_COPYABLE, format_type_display(arg_type));
            }
        }
        if (scope.parent_fn_node && node->type == NodeType::FnCallExpr) {
            apply_call_capture_move_effects(*scope.parent_fn_def(),
                                            node->data.fn_call_expr, node);
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
    type->is_placeholder = elem->is_placeholder;
    // Use to_string for element type since elem->global_id may be empty for anonymous types like
    // lambdas
    auto elem_str = elem->global_id.empty() ? format_type_id(elem) : elem->global_id;
    type->global_id = fmt::format("runtime.Array<{}>", elem_str);
    // Eagerly create the internal subtype so it's tracked in struct_envs
    if (m_ctx->rt_array_type && !type->is_placeholder) {
        array<ChiType *> args;
        args.add(elem);
        type->data.array.internal = get_subtype(to_value_type(m_ctx->rt_array_type), &args);
    } else {
        type->data.array.internal = nullptr;
    }
    return type;
}

ChiType *Resolver::get_span_type(ChiType *elem, bool is_mut, ChiLifetime *lifetime) {
    if (lifetime) {
        auto type = create_type(TypeKind::Span);
        type->data.span.elem = elem;
        type->data.span.is_mut = is_mut;
        type->data.span.lifetimes.add(lifetime);
        type->is_placeholder = elem->is_placeholder;
        auto elem_str = elem->global_id.empty() ? format_type_id(elem) : elem->global_id;
        type->global_id = fmt::format("runtime.__CxSpan<{}>", elem_str);
        if (m_ctx->rt_span_type && !type->is_placeholder) {
            array<ChiType *> args;
            args.add(elem);
            type->data.span.internal = get_subtype(to_value_type(m_ctx->rt_span_type), &args);
        } else {
            type->data.span.internal = nullptr;
        }
        return type;
    }
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
    // Eagerly create the internal subtype so it's tracked in struct_envs
    if (m_ctx->rt_span_type && !type->is_placeholder) {
        array<ChiType *> args;
        args.add(elem);
        type->data.span.internal = get_subtype(to_value_type(m_ctx->rt_span_type), &args);
    } else {
        type->data.span.internal = nullptr;
    }
    return type;
}

ChiLifetime *Resolver::resolve_named_lifetime(ast::Node *node, ResolveScope &scope,
                                              const string &name) {
    if (name == "static") {
        return m_ctx->static_lifetime;
    }
    if (name == "this" && scope.parent_struct) {
        auto *struct_type = scope.parent_struct;
        auto &st = struct_type->data.struct_;
        if (!st.this_lifetime) {
            st.this_lifetime = new ChiLifetime{"this", LifetimeKind::This, nullptr, struct_type};
        }
        return st.this_lifetime;
    }
    if (scope.fn_lifetime_params) {
        auto *lt = scope.fn_lifetime_params->get(name);
        if (lt) {
            return *lt;
        }
    }
    error(node, "unknown lifetime '{}'", name);
    return nullptr;
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

ChiType *Resolver::get_shared_type(ChiType *value) {
    auto found = m_ctx->shared_of.get(value);
    if (found) {
        return *found;
    }

    auto shared = m_ctx->rt_shared_type;
    assert(shared && "Shared struct not found in runtime");

    TypeList args;
    args.add(value);
    auto type = get_subtype(shared, &args);
    m_ctx->shared_of[value] = type;
    return type;
}

ChiTypeSubtype *Resolver::get_promise_subtype(ChiType *type) {
    if (!m_ctx->rt_promise_type || !type) {
        return nullptr;
    }
    if (type->kind == TypeKind::Subtype && type->data.subtype.generic == m_ctx->rt_promise_type) {
        return &type->data.subtype;
    }
    if (type->kind == TypeKind::Struct) {
        if (auto entry = m_ctx->generics.struct_envs.get(type->global_id)) {
            if (entry->generic_type == m_ctx->rt_promise_type && entry->subtype &&
                entry->subtype->kind == TypeKind::Subtype) {
                return &entry->subtype->data.subtype;
            }
        }
    }
    return nullptr;
}

bool Resolver::is_promise_type(ChiType *type) {
    return get_promise_subtype(type) != nullptr;
}

ChiType *Resolver::get_promise_value_type(ChiType *type) {
    auto subtype = get_promise_subtype(type);
    assert(subtype);
    return subtype->args[0];
}

ast::Node *Resolver::get_dummy_var(const string &name, ast::Node *expr) {
    auto node = create_node(NodeType::VarDecl);
    node->name = name;
    node->data.var_decl.is_generated = true;
    node->data.var_decl.expr = expr;
    return node;
}

ast::Node *Resolver::ensure_temp_owner(ast::Node *expr, ChiType *expr_type, ResolveScope &scope,
                                       bool force_addressable) {
    if (!force_addressable) {
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
    }
    if (is_addressable(expr))
        return nullptr;
    if (expr->resolved_outlet) // already has an outlet (e.g. move_outlet set earlier)
        return expr->resolved_outlet;
    if (!force_addressable && expr_type &&
        expr_type->kind == TypeKind::MoveRef) // &move transfers ownership — no temp
        return nullptr;
    if (!force_addressable && !should_destroy(expr, expr_type))
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
    // Also tracked in cleanup_vars for interleaved LIFO destruction with regular
    // locals. Early-return cleanup skips temps whose owning stmt hasn't been
    // reached yet via `stmt_owner_index` (see compile_block_cleanup).
    add_cleanup_var(scope.block, temp);
    if (should_destroy(temp, expr_type)) {
        scope.parent_fn_def()->has_cleanup = true;
    }
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

void Resolver::resolve_destructure(ast::Node *pattern, ChiType *source_type,
                                   ResolveScope &scope, ast::Node *borrow_source) {
    auto &data = pattern->data.destructure_decl;
    if (data.is_tuple) {
        resolve_tuple_destructure(pattern, data.fields, source_type, scope,
                                  data.generated_vars, borrow_source);
    } else if (data.is_array) {
        resolve_array_destructure(pattern, data.fields, source_type, scope,
                                  data.generated_vars, borrow_source);
    } else {
        resolve_destructure_fields(pattern, data.fields, source_type, scope,
                                   data.generated_vars, borrow_source);
    }
}

static void attach_destructure_binding(ast::Node *field_node, ast::Node *var) {
    auto binding_name = field_node->data.destructure_field.binding_name;
    if (!binding_name || !var) {
        return;
    }
    var->token = binding_name;
    binding_name->node = var;
}

void Resolver::resolve_destructure_fields(ast::Node *parent, array<ast::Node *> &fields,
                                          ChiType *source_type, ResolveScope &scope,
                                          array<ast::Node *> &generated_vars,
                                          ast::Node *borrow_source) {
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
            auto *source = borrow_source ? borrow_source : parent->data.destructure_decl.temp_var;
            auto *nested_source =
                get_projection_node(find_projection_owner(source), -1, member, false);
            resolve_destructure(field_data.nested, member->resolved_type, scope,
                               nested_source ? nested_source : source);
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
            var->name = binding_name;
            attach_destructure_binding(field_node, var);

            if (scope.parent_fn_node) {
                var->decl_order = scope.parent_fn_def()->next_decl_order++;
            }

            if (scope.block && scope.block->scope) {
                scope.block->scope->put(binding_name, var);
            }

            if (scope.parent_fn_node && !scope.is_unsafe_block) {
                auto *source = borrow_source ? borrow_source : parent->data.destructure_decl.temp_var;
                bool is_ref = field_data.sigil == ast::SigilKind::Reference ||
                              field_data.sigil == ast::SigilKind::MutRef;
                add_projection_source_edges(*scope.parent_fn_def(), source,
                                            field_data.resolved_field->resolved_type, var, is_ref,
                                            -1, field_data.resolved_field);
            }

            if (scope.parent_fn_node && scope.block && should_destroy(var, field_type) &&
                !var->analysis.is_capture()) {
                scope.block->cleanup_vars.add(var);
                scope.parent_fn_def()->has_cleanup = true;
            }

            generated_vars.add(var);
        }
    }
}

void Resolver::resolve_array_destructure(ast::Node *parent, array<ast::Node *> &fields,
                                         ChiType *source_type, ResolveScope &scope,
                                         array<ast::Node *> &generated_vars,
                                         ast::Node *borrow_source) {
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

    bool has_rest = false;
    for (auto field_node : fields) {
        if (field_node->data.destructure_field.is_rest) {
            has_rest = true;
            break;
        }
    }
    if (has_rest) {
        auto slice_method_p = struct_type->member_table.get("slice");
        if (!slice_method_p) {
            error(parent, "cannot use rest array destructure on type '{}'",
                  format_type_display(source_type));
            return;
        }
        parent->data.destructure_decl.resolved_slice_method = *slice_method_p;
    }

    // Element type from index_mut return type (returns &T)
    auto elem_type = method->resolved_type->data.fn.return_type->get_elem();
    auto kind = parent->data.destructure_decl.kind;

    for (auto field_node : fields) {
        auto &field_data = field_node->data.destructure_field;
        auto binding_name = string(field_data.binding_name->str);

        auto var = get_dummy_var(binding_name);
        var->module = parent->module;
        var->parent_fn = scope.parent_fn_node;
        auto var_type = field_data.is_rest
                            ? get_array_type(elem_type)
                        : field_data.sigil == ast::SigilKind::MutRef
                            ? get_pointer_type(elem_type, TypeKind::MutRef)
                        : field_data.sigil == ast::SigilKind::Reference
                            ? get_pointer_type(elem_type, TypeKind::Reference)
                            : elem_type;
        var->resolved_type = var_type;
        var->data.var_decl.kind = kind;
        var->data.var_decl.initialized_at = parent;
        var->name = binding_name;
        attach_destructure_binding(field_node, var);

        if (scope.parent_fn_node) {
            var->decl_order = scope.parent_fn_def()->next_decl_order++;
        }

        if (scope.block && scope.block->scope) {
            scope.block->scope->put(binding_name, var);
        }

        if (scope.parent_fn_node && !scope.is_unsafe_block) {
            auto *source = borrow_source ? borrow_source : parent->data.destructure_decl.temp_var;
            bool is_ref = field_data.sigil == ast::SigilKind::Reference ||
                          field_data.sigil == ast::SigilKind::MutRef;
            add_borrow_source_edges(*scope.parent_fn_def(), source, var, is_ref);
        }

        if (scope.parent_fn_node && scope.block && should_destroy(var, var_type) &&
            !var->analysis.is_capture()) {
            scope.block->cleanup_vars.add(var);
            scope.parent_fn_def()->has_cleanup = true;
        }

        generated_vars.add(var);
    }
}

void Resolver::resolve_tuple_destructure(ast::Node *parent, array<ast::Node *> &fields,
                                         ChiType *source_type, ResolveScope &scope,
                                         array<ast::Node *> &generated_vars,
                                         ast::Node *borrow_source) {
    array<ChiStructMember *> tuple_like_fields;
    TypeList tuple_like_elems;

    // If source is not a Tuple, check for a tuple-like struct first, then AsTuple intrinsic
    if (source_type->kind != TypeKind::Tuple) {
        auto stype = resolve_struct_type(source_type);
        bool tuple_like_struct = false;
        ChiStructMember *as_tuple_member = nullptr;
        if (stype) {
            for (int i = 0;; i++) {
                auto field = stype->find_member(std::to_string(i));
                if (!field || !field->is_field()) {
                    tuple_like_struct = i > 0;
                    break;
                }
                tuple_like_fields.add(field);
                tuple_like_elems.add(field->resolved_type);
            }
            if (!tuple_like_struct) {
                auto member_p = stype->member_intrinsics.get(IntrinsicSymbol::AsTuple);
                if (member_p) as_tuple_member = *member_p;
            }
        }
        if (!tuple_like_struct && !as_tuple_member) {
            error(parent, "cannot tuple-destructure non-tuple type '{}'",
                  format_type_display(source_type));
            return;
        }
        if (!tuple_like_struct) {
            borrow_source = parent->data.destructure_decl.temp_var;
            // Get the return type of as_tuple() — should be a Tuple
            auto method_type = as_tuple_member->resolved_type;
            if (!method_type || method_type->kind != TypeKind::Fn ||
                method_type->data.fn.return_type->kind != TypeKind::Tuple) {
                error(parent, "as_tuple() must return a Tuple type");
                return;
            }
            source_type = method_type->data.fn.return_type;
            parent->data.destructure_decl.resolved_as_tuple = as_tuple_member;
            parent->data.destructure_decl.as_tuple_result_type = source_type;
        }
    }

    TypeList &elems = source_type->kind == TypeKind::Tuple ? source_type->data.tuple.elements
                                                           : tuple_like_elems;

    // Check if there's a rest pattern
    bool has_rest = false;
    int rest_index = -1;
    for (int i = 0; i < (int)fields.len; i++) {
        if (fields[i]->data.destructure_field.is_rest) {
            has_rest = true;
            rest_index = i;
            break;
        }
    }

    if (!has_rest && (int)fields.len > elems.len) {
        error(parent, "too many bindings in tuple destructure: expected at most {}, got {}",
              elems.len, fields.len);
        return;
    }
    if (has_rest && (int)fields.len - 1 > elems.len) {
        error(parent, "too many bindings in tuple destructure: expected at most {}, got {}",
              elems.len, (int)fields.len - 1);
        return;
    }

    auto kind = parent->data.destructure_decl.kind;

    for (int i = 0; i < (int)fields.len; i++) {
        auto field_node = fields[i];
        auto &field_data = field_node->data.destructure_field;
        auto binding_name = string(field_data.binding_name->str);

        ChiType *elem_type;
        if (field_data.is_rest) {
            // Collect remaining elements into a Tuple type
            TypeList rest_elems;
            for (int j = i; j < elems.len; j++) {
                rest_elems.add(elems[j]);
            }
            elem_type = get_tuple_type(rest_elems);
        } else {
            elem_type = elems[i];
        }

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
        var->name = binding_name;
        attach_destructure_binding(field_node, var);

        if (scope.parent_fn_node) {
            var->decl_order = scope.parent_fn_def()->next_decl_order++;
        }

        if (scope.block && scope.block->scope) {
            scope.block->scope->put(binding_name, var);
        }

        if (scope.parent_fn_node && !scope.is_unsafe_block) {
            auto *source = borrow_source ? borrow_source : parent->data.destructure_decl.temp_var;
            bool is_ref = var->resolved_type->is_reference();
            add_projection_source_edges(*scope.parent_fn_def(), source, elem_type, var,
                                        is_ref, field_data.is_rest ? -1 : i, nullptr);
        }

        if (scope.parent_fn_node && scope.block && should_destroy(var, var_type) &&
            !var->analysis.is_capture()) {
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
        narrowed_type = type->get_elem();
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
    if (scope.parent_fn_node) {
        var->decl_order = scope.parent_fn_def()->next_decl_order++;
    }
    if (scope.parent_fn_node && !scope.is_unsafe_block) {
        auto &fn_def = *scope.parent_fn_def();
        if (auto *root = find_root_decl(expr_node)) {
            fn_def.add_ref_edge(var, root);
        } else {
            add_borrow_source_edges(fn_def, expr_node, var, false);
        }
    }
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
            if (type && type->kind == TypeKind::Optional) {
                out.add(expr);
            }
        }
        return;
    }

    if (expr->type == NodeType::DotExpr) {
        if (when_truthy) {
            auto type = node_get_type(expr);
            if (type && type->kind == TypeKind::Optional) {
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
        if (data.op_type == TokenType::EQ || data.op_type == TokenType::NE) {
            ast::Node *candidate = nullptr;
            if (is_null_literal(data.op1) && data.op2) {
                candidate = data.op2;
            } else if (is_null_literal(data.op2) && data.op1) {
                candidate = data.op1;
            }
            if (candidate) {
                bool narrows = (data.op_type == TokenType::NE && when_truthy) ||
                               (data.op_type == TokenType::EQ && !when_truthy);
                if (narrows) {
                    collect_narrowables(candidate, true, out);
                }
            }
            return;
        }
        if (data.op_type == TokenType::LAND && when_truthy) {
            collect_narrowables(data.op1, true, out);
            collect_narrowables(data.op2, true, out);
        } else if (data.op_type == TokenType::LOR && !when_truthy) {
            collect_narrowables(data.op1, false, out);
            collect_narrowables(data.op2, false, out);
        }
    }
}

static bool logical_rhs_uses_truthy_narrowing(TokenType op_type) {
    return op_type == TokenType::LAND;
}

bool Resolver::fn_type_has_placeholder(ChiType *fn_type) {
    if (!fn_type || fn_type->kind != TypeKind::Fn) {
        return false;
    }

    auto &data = fn_type->data.fn;
    for (auto param : data.params) {
        if (param && param->is_placeholder) {
            return true;
        }
    }

    for (auto type_param : data.type_params) {
        if (type_param && type_param->is_placeholder) {
            return true;
        }
    }

    if (data.return_type && data.return_type->is_placeholder) {
        return true;
    }

    return data.container_ref && data.container_ref->is_placeholder;
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
    type->is_placeholder = fn_type_has_placeholder(type);

    if (should_cache) {
        m_ctx->composite_types[key] = type;
    }
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
        fn_type->is_placeholder = fn_type_has_placeholder(fn_type);
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

    // Use the Chi-native Result<T, E> type from runtime.xs
    auto result = m_ctx->rt_result_type;
    assert(result && "Result type not found in runtime");

    TypeList args;
    args.add(value);
    args.add(err);
    return get_subtype(result, &args);
}

bool Resolver::is_result_type(ChiType *type) {
    if (!m_ctx->rt_result_type) {
        return false;
    }
    if (type->kind == TypeKind::Subtype) {
        return type->data.subtype.generic == m_ctx->rt_result_type;
    }
    return false;
}

bool Resolver::contains_await(ast::Node *node) {
    if (!node) {
        return false;
    }
    if (node->type == NodeType::AwaitExpr) {
        return true;
    }
    return visit_async_children(node, false, [&](ast::Node *child) {
        return contains_await(child);
    });
}

ast::Node *Resolver::find_await_expr(ast::Node *node) {
    if (!node) {
        return nullptr;
    }
    if (node->type == NodeType::AwaitExpr) {
        return node;
    }

    ast::Node *found = nullptr;
    visit_async_children(node, false, [&](ast::Node *child) {
        found = find_await_expr(child);
        return found != nullptr;
    });
    return found;
}

AwaitSite Resolver::find_await_site(ast::Node *node) {
    if (!node) {
        return {};
    }
    if (node->type == NodeType::AwaitExpr) {
        return {node, node};
    }

    if (node->type == NodeType::TryExpr) {
        auto try_expr = node->data.try_expr.resolved_expr ? node->data.try_expr.resolved_expr : node->data.try_expr.expr;
        auto site = find_await_site(try_expr);
        if (site.await_expr && site.resume_expr &&
            site.resume_expr->type != NodeType::TryExpr) {
            site.resume_expr = node;
        }
        return site;
    }

    AwaitSite site = {};
    visit_async_children(node, false, [&](ast::Node *child) {
        site = find_await_site(child);
        return site.await_expr != nullptr;
    });
    return site;
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

    // Both are void pointers — same type regardless of identity
    if (a->kind == TypeKind::Pointer && b->kind == TypeKind::Pointer) {
        auto a_elem = a->get_elem();
        auto b_elem = b->get_elem();
        if (a_elem && b_elem && a_elem->kind == TypeKind::Void && b_elem->kind == TypeKind::Void) {
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
        if (a->is_pointer_like() || a->kind == TypeKind::Optional) {
            return is_same_type(a->get_elem(), b->get_elem());
        }
        // Structural comparison for fixed arrays
        if (a->kind == TypeKind::FixedArray) {
            return a->data.fixed_array.size == b->data.fixed_array.size &&
                   is_same_type(a->data.fixed_array.elem, b->data.fixed_array.elem);
        }
        // Structural comparison for tuples
        if (a->kind == TypeKind::Tuple) {
            auto &a_elems = a->data.tuple.elements;
            auto &b_elems = b->data.tuple.elements;
            if (a_elems.len != b_elems.len) return false;
            for (int i = 0; i < a_elems.len; i++) {
                if (!is_same_type(a_elems[i], b_elems[i])) return false;
            }
            return true;
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
            if (m_resolving_subtype != subtype) {
                ensure_enum_subtype_final_type(generic, subtype);
            }
            return subtype;
        }
    }
    auto sub = create_type(TypeKind::Subtype);
    sub->data.subtype.generic = generic;
    sub->data.subtype.root_node = gen.node;
    if (m_subtype_origin) sub->data.subtype.origin_node = m_subtype_origin;
    sub->global_id = fmt::format("{}<{}>", generic->global_id, format_type_list(type_args));
    for (auto arg : *type_args) {
        sub->data.subtype.args.add(arg);
        if (arg->is_placeholder) {
            sub->is_placeholder = true;
        }
    }
    gen.subtypes.add(sub);

    if (!sub->is_placeholder) {
        bool is_method_sig = m_resolving_subtype &&
                             m_resolving_subtype->data.subtype.generic == generic &&
                             sub->subtype_depth() > m_resolving_subtype->subtype_depth();
        map<ChiType *, ChiType *> subs;
        for (size_t i = 0; i < gen.type_params.len && i < type_args->len; i++) {
            subs[to_value_type(gen.type_params[i])] = type_args->at(i);
        }
        m_ctx->generics.record_struct(sub->global_id, sub->global_id, generic, sub, subs,
                                      is_method_sig);
    }

    ensure_enum_subtype_final_type(generic, sub);
    return sub;
}

ChiType *Resolver::get_subtype(ChiType *generic, TypeList *type_args) {
    if (generic->kind == TypeKind::Enum) {
        return get_enum_subtype(generic, type_args);
    }

    assert(generic->kind == TypeKind::Struct);
    auto &gen = generic->data.struct_;

    // Check if the requested type_args are all concrete (no placeholders)
    bool args_concrete = true;
    for (auto arg : *type_args) {
        if (arg->is_placeholder) {
            args_concrete = false;
            break;
        }
    }

    for (auto subtype : gen.subtypes) {
        assert(subtype->kind == TypeKind::Subtype);
        auto &subtype_data = subtype->data.subtype;
        if (subtype_data.args.len != type_args->len) {
            continue;
        }
        // Skip placeholder subtypes when looking for a concrete instantiation.
        // is_same_type follows Infer types, so a placeholder subtype with
        // Infer(T) args (where inferred_type=int) would falsely match [int].
        if (args_concrete && subtype->is_placeholder) {
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
    if (m_subtype_origin) sub->data.subtype.origin_node = m_subtype_origin;
    sub->global_id = fmt::format("{}<{}>", gen.global_id, format_type_list(type_args));
    for (auto arg : *type_args) {
        sub->data.subtype.args.add(arg);
        if (arg->is_placeholder) {
            sub->is_placeholder = true;
        }
    }
    gen.subtypes.add(sub);

    // Record to monomorphization plan (struct_envs tracks both the subtype and its type env)
    if (!sub->is_placeholder) {
        bool is_method_sig = m_resolving_subtype &&
                             m_resolving_subtype->data.subtype.generic == generic &&
                             sub->subtype_depth() > m_resolving_subtype->subtype_depth();
        map<ChiType *, ChiType *> subs;
        for (size_t i = 0; i < gen.type_params.len && i < type_args->len; i++) {
            // Use to_value_type to unwrap TypeSymbol wrapper - the actual types in
            // struct members contain raw Placeholder types, not TypeSymbol wrappers
            subs[to_value_type(gen.type_params[i])] = type_args->at(i);
        }
        m_ctx->generics.record_struct(sub->global_id, sub->global_id, generic, sub, subs,
                                      is_method_sig);
    }

    return sub;
}

ast::Node *Resolver::get_fn_variant(ChiType *generic_fn, TypeList *type_args, ast::Node *fn_node) {
    assert(generic_fn->kind == TypeKind::Fn);
    assert(fn_node->type == NodeType::FnDef);
    auto &gen = generic_fn->data.fn;

    int max_arg_depth = 0;
    for (auto arg : *type_args) {
        int d = arg->subtype_depth();
        if (d > max_arg_depth) max_arg_depth = d;
    }
    if (max_arg_depth > MAX_GENERIC_DEPTH) {
        auto fn_display = fn_node->name + "<" + format_type_list(type_args) + ">";
        error(fn_node, errors::GENERIC_DEPTH_EXCEEDED, fn_display, MAX_GENERIC_DEPTH);
        if (fn_node->data.fn_def.variants.len > 0) {
            return fn_node->data.fn_def.variants[0];
        }
        auto stub_sub = create_type(TypeKind::Subtype);
        stub_sub->data.subtype.generic = generic_fn;
        stub_sub->data.subtype.root_node = fn_node->get_root_node();
        stub_sub->is_placeholder = true;
        auto stub_fn = create_node(NodeType::GeneratedFn);
        stub_fn->data.generated_fn.fn_subtype = stub_sub;
        stub_fn->data.generated_fn.original_fn = fn_node;
        stub_fn->module = fn_node->module;
        stub_fn->root_node = fn_node->get_root_node();
        stub_fn->token = fn_node->token;
        stub_fn->resolved_type = generic_fn;
        stub_sub->data.subtype.generated_fn = stub_fn;
        return stub_fn;
    }

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
    specialized_fn_type = finalize_fn_type(specialized_fn_type,
                                           data.generated_fn ? data.generated_fn : data.origin_node);

    data.final_type = specialized_fn_type;
    return specialized_fn_type;
}

ChiType *Resolver::resolve_subtype(ChiType *subtype, ast::Node *origin) {
    auto &data = subtype->data.subtype;
    auto effective_origin = origin ? origin : (m_subtype_origin ? m_subtype_origin : data.origin_node);
    if (effective_origin && !data.origin_node) {
        data.origin_node = effective_origin;
    }
    auto prev_origin = m_subtype_origin;
    m_subtype_origin = effective_origin;
    if (data.final_type) {
        m_subtype_origin = prev_origin;
        return data.final_type;
    }

    if (subtype->subtype_depth() > MAX_GENERIC_DEPTH) {
        auto err_node = origin ? origin : (data.origin_node ? data.origin_node : data.root_node);
        error(err_node, errors::GENERIC_DEPTH_EXCEEDED,
              format_type_display(subtype), MAX_GENERIC_DEPTH);
        m_subtype_origin = prev_origin;
        return subtype;
    }

    if (!data.generic) {
        m_subtype_origin = prev_origin;
        return subtype;
    }

    if (data.generic->kind == TypeKind::Enum) {
        auto enum_subtype = get_enum_subtype(data.generic, &data.args);
        m_subtype_origin = prev_origin;
        return enum_subtype->data.subtype.final_type ? enum_subtype->data.subtype.final_type
                                                     : enum_subtype;
    }

    // Ensure this subtype is recorded in struct_envs and not deferred
    if (!subtype->is_placeholder) {
        auto existing = m_ctx->generics.struct_envs.get(subtype->global_id);
        if (existing) {
            // Clear deferred flag — this subtype is actively being resolved
            existing->from_method_sig = false;
        } else {
            auto &gen = data.generic->data.struct_;
            map<ChiType *, ChiType *> subs;
            for (size_t i = 0; i < gen.type_params.len && i < data.args.len; i++) {
                subs[to_value_type(gen.type_params[i])] = data.args[i];
            }
            m_ctx->generics.record_struct(subtype->global_id, subtype->global_id,
                                          data.generic, subtype, subs);
        }
    }

    auto prev_resolving = m_resolving_subtype;
    m_resolving_subtype = subtype;

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
        node->module = member->node->module;
        node->resolved_type = type;
        node->root_node = member->node->get_root_node();

        if (member->node->type == NodeType::FnDef) {
            node->data.fn_def.is_generated = true;

            auto new_proto = get_allocator()->create_node(NodeType::FnProto);
            auto orig_proto = member->node->data.fn_def.fn_proto;
            orig_proto->clone(new_proto);
            new_proto->module = node->module;
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
    m_resolving_subtype = prev_resolving;
    m_subtype_origin = prev_origin;
    return sty;
}

bool Resolver::builtin_satisfies_intrinsic(ChiType *type, IntrinsicSymbol symbol) {
    switch (symbol) {
    case IntrinsicSymbol::Add:
        return type->is_int() || type->kind == TypeKind::Float;
    case IntrinsicSymbol::Sub:
    case IntrinsicSymbol::Mul:
    case IntrinsicSymbol::Div:
    case IntrinsicSymbol::Rem:
    case IntrinsicSymbol::Neg:
        return type->is_int() || type->kind == TypeKind::Float;
    case IntrinsicSymbol::Eq:
    case IntrinsicSymbol::Ord:
        return type->is_int_like() || type->kind == TypeKind::Float ||
               type->kind == TypeKind::Bool ||
               type->kind == TypeKind::Byte || type->kind == TypeKind::Rune ||
               type->is_pointer_like() ||
               (type->kind == TypeKind::EnumValue && type->data.enum_value.parent_enum()->is_plain);
    case IntrinsicSymbol::Hash:
        return type->is_int_like() || type->kind == TypeKind::Float ||
               type->kind == TypeKind::Bool ||
               type->kind == TypeKind::Byte || type->kind == TypeKind::Rune;
    case IntrinsicSymbol::BitAnd:
    case IntrinsicSymbol::BitOr:
    case IntrinsicSymbol::BitXor:
    case IntrinsicSymbol::BitNot:
    case IntrinsicSymbol::Shl:
    case IntrinsicSymbol::Shr:
        return type->is_int();
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
    if (!ok && type->kind == TypeKind::Placeholder) {
        for (auto trait_type : get_placeholder_traits(type)) {
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

ChiType *Resolver::get_tuple_type(TypeList &elements) {
    // Build cache key from element type IDs
    std::stringstream key;
    key << "Tuple<";
    for (int i = 0; i < elements.len; i++) {
        key << format_type(elements[i]);
        if (i < elements.len - 1) key << ",";
    }
    key << ">";
    auto key_str = key.str();
    auto cached = m_ctx->tuple_types.get(key_str);
    if (cached) return *cached;

    auto tuple_type = create_type(TypeKind::Tuple);
    tuple_type->data.tuple.elements = elements;
    m_ctx->tuple_types[key_str] = tuple_type;
    return tuple_type;
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
    case NodeType::VarDecl:
    case NodeType::ParamDecl:
        return node;
    case NodeType::Identifier: {
        if (node->data.identifier.kind != ast::IdentifierKind::Value) {
            return node;
        }
        return node->data.identifier.decl;
    }
    case NodeType::DotExpr:
        return find_root_decl(node->data.dot_expr.effective_expr());
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

ast::Node *Resolver::find_projection_owner(ast::Node *node) {
    if (!node) {
        return nullptr;
    }
    if (is_this_identifier_node(node) && node->parent_fn) {
        return node->parent_fn;
    }
    if (auto *root = find_root_decl(node)) {
        if (is_this_identifier_node(root) && root->parent_fn) {
            return root->parent_fn;
        }
        return root;
    }
    if (node->resolved_outlet) {
        return node->resolved_outlet;
    }
    return node;
}

ast::Node *Resolver::create_projection_node(ast::Node *owner, string name) {
    auto *node = get_dummy_var(name);
    node->module = owner->module;
    node->parent_fn = owner->parent_fn;
    node->token = owner->token;
    node->name = std::move(name);
    return node;
}

ast::Node *Resolver::get_expr_projection_node(ast::Node *expr, bool create) {
    if (!expr || expr->type != NodeType::DotExpr) {
        return nullptr;
    }
    auto &data = expr->data.dot_expr;
    auto *projection_source = data.narrowed_var ? data.narrowed_var : data.effective_expr();
    auto *owner = find_projection_owner(projection_source);
    if (!owner) {
        return nullptr;
    }
    auto *field_member =
        data.resolved_struct_member && !data.resolved_struct_member->is_method()
            ? data.resolved_struct_member
            : nullptr;
    auto tuple_index = data.resolved_dot_kind == DotKind::TupleField
                           ? static_cast<int32_t>(data.resolved_value)
                           : -1;
    return get_projection_node(owner, tuple_index, field_member, create);
}

ast::Node *Resolver::get_projection_node(ast::Node *owner, int32_t tuple_index,
                                         ChiStructMember *field_member, bool create) {
    if (tuple_index >= 0) {
        return get_tuple_projection_node(owner, tuple_index, create);
    }
    if (field_member) {
        return get_field_projection_node(owner, field_member, create);
    }
    return nullptr;
}

ast::Node *Resolver::get_tuple_projection_node(ast::Node *owner, int32_t index, bool create) {
    if (!owner || index < 0) {
        return nullptr;
    }
    auto *items = m_tuple_projection_nodes.get(owner);
    if (items) {
        auto *existing = items->get(index);
        if (existing) {
            return *existing;
        }
    }
    if (!create) {
        return nullptr;
    }
    auto *node = create_projection_node(owner, fmt::format("__tuple_proj_{}", index));
    m_tuple_projection_nodes[owner][index] = node;
    return node;
}

ast::Node *Resolver::get_field_projection_node(ast::Node *owner, ChiStructMember *member,
                                               bool create) {
    if (!owner || !member || member->field_index < 0) {
        return nullptr;
    }
    auto *items = m_field_projection_nodes.get(owner);
    if (items) {
        auto *existing = items->get(member);
        if (existing) {
            return *existing;
        }
    }
    if (!create) {
        return nullptr;
    }
    auto *node = create_projection_node(owner, fmt::format("__field_proj_{}", member->get_name()));
    m_field_projection_nodes[owner][member] = node;
    return node;
}

void Resolver::seed_projection_node(ast::FnDef &fn_def, ast::Node *source,
                                    ChiType *projection_type, ast::Node *projection,
                                    int32_t tuple_index, ChiStructMember *field_member) {
    if (!projection) {
        return;
    }
    add_projection_source_edges(fn_def, source, projection_type, projection, false, tuple_index,
                                field_member);
    copy_projection_summaries(fn_def, source, projection, projection_type);
}

void Resolver::add_projection_source_edges(ast::FnDef &fn_def, ast::Node *expr,
                                           ChiType *projected_type, ast::Node *target, bool is_ref,
                                           int32_t tuple_index,
                                           ChiStructMember *field_member) {
    if (!expr || !target) {
        return;
    }

    switch (expr->type) {
    case NodeType::ParenExpr:
        add_projection_source_edges(fn_def, expr->data.child_expr, projected_type, target, is_ref,
                                    tuple_index, field_member);
        return;
    case NodeType::TryExpr:
        add_projection_source_edges(fn_def, expr->data.try_expr.expr, projected_type, target, is_ref,
                                    tuple_index, field_member);
        return;
    case NodeType::CastExpr:
        add_projection_source_edges(fn_def, expr->data.cast_expr.expr, projected_type, target,
                                    is_ref, tuple_index, field_member);
        return;
    default:
        break;
    }

    if (!is_ref && !type_may_propagate_borrow_deps(this, projected_type)) {
        return;
    }

    if (expr->type == NodeType::DotExpr) {
        auto *expr_projection = get_expr_projection_node(expr, false);
        if (expr->data.dot_expr.resolved_dot_kind == DotKind::TupleField ||
            (expr->data.dot_expr.resolved_struct_member &&
             !expr->data.dot_expr.resolved_struct_member->is_method())) {
            if (!expr_projection) {
                auto &data = expr->data.dot_expr;
                auto *projection_source = data.narrowed_var ? data.narrowed_var : data.effective_expr();
                auto *projection_type = node_get_type(expr);
                expr_projection = get_expr_projection_node(expr);
                auto tuple_index = data.resolved_dot_kind == DotKind::TupleField
                                       ? static_cast<int32_t>(data.resolved_value)
                                       : -1;
                auto *field_member =
                    data.resolved_dot_kind == DotKind::TupleField ? nullptr : data.resolved_struct_member;
                seed_projection_node(fn_def, projection_source, projection_type, expr_projection,
                                     tuple_index, field_member);
                fn_def.copy_ref_edges(expr, expr_projection, false);
            }
            assert(expr_projection && "missing projection node for projected dot expression");
            auto *nested_projection =
                get_projection_node(expr_projection, tuple_index, field_member, false);
            assert(nested_projection && "missing nested projection summary");
            if (is_ref) {
                fn_def.add_ref_edge(target, nested_projection);
            } else {
                fn_def.copy_ref_edges(target, nested_projection, false);
            }
            return;
        }
    }

    auto *owner = find_projection_owner(expr);
    if (!is_ref && owner) {
        auto *source_projection = get_projection_node(owner, tuple_index, field_member, false);
        if (source_projection && source_projection != target) {
            fn_def.copy_ref_edges(target, source_projection, false);
            return;
        }
        if (source_projection == target &&
            (fn_def.flow.ref_edges.has_key(source_projection) ||
             fn_def.flow.copy_edges.has_key(source_projection))) {
            return;
        }
    }

    if (is_ref) {
        if (auto *root = find_root_decl(expr)) {
            fn_def.add_ref_edge(target, root);
            return;
        }
        if (expr->resolved_outlet) {
            fn_def.add_ref_edge(target, expr->resolved_outlet);
            return;
        }
    }

    switch (expr->type) {
    case NodeType::TupleExpr: {
        if (tuple_index < 0 || tuple_index >= expr->data.tuple_expr.items.len) {
            return;
        }
        add_borrow_source_edges(fn_def, expr->data.tuple_expr.items[tuple_index], target, false);
        return;
    }
    case NodeType::ConstructExpr: {
        auto &data = expr->data.construct_expr;
        if (field_member) {
            for (auto *field_init : data.field_inits) {
                auto &field_data = field_init->data.field_init_expr;
                if (field_data.resolved_field == field_member) {
                    add_borrow_source_edges(fn_def, field_data.value, target, false);
                    return;
                }
            }

            auto *value_type = to_value_type(node_get_type(expr));
            auto payload_fields = value_type ? get_enum_payload_fields(value_type) : array<ChiStructMember *>{};
            for (uint32_t i = 0; i < payload_fields.len && i < data.items.len; i++) {
                if (payload_fields[i] == field_member) {
                    add_borrow_source_edges(fn_def, data.items[i], target, false);
                    return;
                }
            }

            if (data.spread_expr) {
                add_projection_source_edges(fn_def, data.spread_expr, projected_type, target, false,
                                            tuple_index, field_member);
                return;
            }
        }
        return;
    }
    case NodeType::FnCallExpr: {
        auto &call = expr->data.fn_call_expr;
        auto *summary_proto = get_call_effect_proto(call);
        if (summary_proto && summary_proto->copy_edge_summary_valid) {
            int32_t summary_index = tuple_index >= 0
                                        ? tuple_index
                                        : field_member ? static_cast<int32_t>(field_member->field_index)
                                                       : -1;
            if (summary_index >= 0) {
                auto *summary = find_projection_copy_summary(
                    summary_proto->return_projection_copy_summaries, summary_index);
                if (summary) {
                    if (summary->from_this && call.fn_ref_expr->type == NodeType::DotExpr) {
                        add_borrow_source_edges(
                            fn_def, call.fn_ref_expr->data.dot_expr.effective_expr(), target,
                            false);
                    }
                    for (auto idx : summary->param_indices) {
                        if (idx >= 0 && idx < static_cast<int32_t>(call.args.len)) {
                            add_borrow_source_edges(fn_def, call.args[static_cast<uint32_t>(idx)],
                                                    target, false);
                        }
                    }
                    return;
                }
            }
        }
        break;
    }
    default:
        break;
    }

    auto *root = find_root_decl(expr);
    if (!is_ref && root) {
        fn_def.copy_ref_edges(target, root, expr_can_fallback_to_borrow_source(expr));
        return;
    }
    if (is_ref) {
        add_borrow_source_edges(fn_def, expr, target, true);
    } else {
        add_borrow_source_edges(fn_def, expr, target, false);
    }
}

void Resolver::copy_projection_summaries(ast::FnDef &fn_def, ast::Node *expr, ast::Node *target,
                                         ChiType *target_type) {
    if (!expr || !target || !target_type) {
        return;
    }
    if (find_projection_owner(expr) == target) {
        return;
    }
    target_type = to_value_type(target_type);
    if (!target_type) {
        return;
    }

    if (target_type->kind == TypeKind::Tuple) {
        auto &elems = target_type->data.tuple.elements;
        for (int32_t i = 0; i < elems.len; i++) {
            if (!type_may_propagate_borrow_deps(this, elems[i])) {
                continue;
            }
            seed_projection_node(fn_def, expr, elems[i],
                                 get_projection_node(target, i, nullptr), i, nullptr);
        }
        return;
    }

    auto *struct_type = resolve_struct_type(target_type);
    if (!struct_type) {
        return;
    }
    for (auto *field : struct_type->own_fields()) {
        auto *field_type = to_value_type(field->resolved_type);
        if (!type_may_propagate_borrow_deps(this, field_type)) {
            continue;
        }
        seed_projection_node(fn_def, expr, field_type,
                             get_projection_node(target, -1, field), -1, field);
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

    ast::FnProto *proto = get_decl_fn_proto(fn_node);
    if (!proto)
        return;

    proto->resolved_param_lifetimes = {};
    proto->resolved_return_lifetime = nullptr;

    // Extract only explicit param lifetimes from resolved param types.
    for (size_t i = 0; i < fn.params.len; i++) {
        auto *pt = fn.params[i];
        auto *param_node = (i < proto->params.len) ? proto->params[i] : nullptr;
        ChiLifetime *lt = get_param_effective_lifetime(param_node, pt);
        proto->resolved_param_lifetimes.add(lt);
    }

    // Extract only explicit return lifetime from the resolved return type.
    auto *ret = fn.return_type;
    if (auto *ret_lt = get_first_ref_lifetime(ret)) {
        proto->resolved_return_lifetime = ret_lt;
    }
}

void Resolver::compute_lambda_capture_move_summary(ast::Node *fn_node) {
    if (!fn_node || fn_node->type != ast::NodeType::FnDef) {
        return;
    }
    auto &fn_def = fn_node->data.fn_def;
    if (fn_def.fn_kind != ast::FnKind::Lambda || !fn_def.fn_proto) {
        return;
    }
    auto &proto = fn_def.fn_proto->data.fn_proto;
    proto.moved_capture_roots = {};
    proto.moved_capture_sources = {};
    proto.moved_capture_kinds = {};

    auto add_capture_move = [&](ast::Node *root, ast::Node *source, ast::SinkKind kind) {
        if (!root) {
            return;
        }
        auto *capture_idx = fn_def.capture_map.get(root);
        if (!capture_idx || *capture_idx < 0 || *capture_idx >= fn_def.captures.len) {
            return;
        }
        if (fn_def.captures[*capture_idx].mode != ast::CaptureMode::ByRef) {
            return;
        }
        for (size_t i = 0; i < proto.moved_capture_roots.len; i++) {
            if (proto.moved_capture_roots[i] == root) {
                if (proto.moved_capture_kinds[i] == ast::SinkKind::Maybe ||
                    kind == ast::SinkKind::Maybe) {
                    proto.moved_capture_kinds[i] = ast::SinkKind::Maybe;
                }
                if (source) {
                    proto.moved_capture_sources[i] = source;
                }
                return;
            }
        }
        proto.moved_capture_roots.add(root);
        proto.moved_capture_sources.add(source);
        proto.moved_capture_kinds.add(kind);
    };

    for (auto &[root, edge] : fn_def.flow.sink_edges.data) {
        add_capture_move(root, edge.target, edge.kind);
    }
}

void Resolver::compute_receiver_copy_edge_summary(ast::FnDef &fn_def) {
    if (!fn_def.fn_proto) {
        return;
    }
    auto &proto = fn_def.fn_proto->data.fn_proto;
    proto.this_copy_edge_param_indices = {};
    proto.return_copy_edge_param_indices = {};
    proto.return_projection_copy_summaries = {};
    proto.return_copy_edge_from_this = false;
    if (fn_def.flow.copy_edges.size() > 0) {
        for (auto &[node, _] : fn_def.flow.copy_edges.data) {
            if (is_this_identifier_node(node)) {
                collect_copy_edge_reachable_params(fn_def.flow, proto.params, node,
                                                   &proto.this_copy_edge_param_indices, nullptr);
            }
        }

        for (auto *terminal : fn_def.flow.terminals) {
            if (!terminal || terminal->type != ast::NodeType::ReturnStmt) {
                continue;
            }
            collect_copy_edge_reachable_params(fn_def.flow, proto.params, terminal,
                                               &proto.return_copy_edge_param_indices,
                                               &proto.return_copy_edge_from_this);
        }
    }

    compute_return_projection_copy_summaries(fn_def);

    proto.copy_edge_summary_valid = true;
}

void Resolver::compute_return_projection_copy_summaries(ast::FnDef &fn_def) {
    if (!fn_def.fn_proto) {
        return;
    }
    auto &proto = fn_def.fn_proto->data.fn_proto;
    proto.return_projection_copy_summaries = {};

    auto add_projection_summary = [&](int32_t index, ast::Node *source) {
        if (!source) {
            return;
        }
        auto *summary = find_projection_copy_summary(proto.return_projection_copy_summaries, index);
        if (!summary) {
            proto.return_projection_copy_summaries.add({});
            summary = &proto.return_projection_copy_summaries[proto.return_projection_copy_summaries.len - 1];
            summary->index = index;
        }
        collect_copy_edge_reachable_params(fn_def.flow, proto.params, source,
                                           &summary->param_indices, &summary->from_this);
    };

    for (auto *terminal : fn_def.flow.terminals) {
        if (!terminal || terminal->type != ast::NodeType::ReturnStmt ||
            !terminal->data.return_stmt.expr) {
            continue;
        }

        auto *expr = terminal->data.return_stmt.expr;
        auto *owner = find_projection_owner(expr);
        auto *type = to_value_type(node_get_type(expr));
        if (!type) {
            continue;
        }

        if (type->kind == TypeKind::Tuple) {
            auto &elems = type->data.tuple.elements;
            for (int32_t i = 0; i < elems.len; i++) {
                if (!type_may_propagate_borrow_deps(this, elems[i])) {
                    continue;
                }
                add_projection_summary(i, get_projection_node(owner, i, nullptr, false));
            }
            continue;
        }

        auto *struct_type = resolve_struct_type(type);
        if (!struct_type) {
            continue;
        }
        for (auto *field : struct_type->own_fields()) {
            auto *field_type = to_value_type(field->resolved_type);
            if (!type_may_propagate_borrow_deps(this, field_type)) {
                continue;
            }
            add_projection_summary(static_cast<int32_t>(field->field_index),
                                   get_projection_node(owner, -1, field, false));
        }
    }
}

void Resolver::compute_exclusive_access_summaries(array<ast::Node *> &top_level_decls) {
    array<ast::Node *> fns;
    for (auto decl : top_level_decls) {
        collect_fn_nodes_from_decl(decl, fns);
    }

    for (auto *fn_node : fns) {
        if (fn_node->type != ast::NodeType::FnDef) {
            continue;
        }
        auto &fn_def = fn_node->data.fn_def;
        auto *proto_node = fn_def.fn_proto;
        if (!proto_node) {
            continue;
        }
        auto &proto = proto_node->data.fn_proto;
        proto.requires_exclusive_capture_roots = {};
        proto.requires_exclusive_capture_sources = {};
    }

    for (int fi = (int)fns.len - 1; fi >= 0; fi--) {
        auto *fn_node = fns[fi];
        if (fn_node->type != ast::NodeType::FnDef) {
            continue;
        }
        auto &fn_def = fn_node->data.fn_def;
        auto *proto_node = fn_def.fn_proto;
        if (!proto_node) {
            continue;
        }
        auto &proto = proto_node->data.fn_proto;

        auto add_capture_root = [&](ast::Node *root, ast::Node *source) {
            if (!root) {
                return;
            }
            if (!fn_def.capture_map.has_key(root)) {
                return;
            }
            for (size_t i = 0; i < proto.requires_exclusive_capture_roots.len; i++) {
                if (proto.requires_exclusive_capture_roots[i] == root) {
                    return;
                }
            }
            proto.requires_exclusive_capture_roots.add(root);
            proto.requires_exclusive_capture_sources.add(source);
        };

        auto add_capture_roots_from_expr = [&](ast::Node *expr, ast::Node *source) {
            array<ast::Node *> roots;
            collect_expr_borrow_roots(this, fn_def.flow, expr, roots);
            for (auto *root : roots) {
                add_capture_root(root, source);
            }
        };

        for (auto *call_node : fn_def.call_sites) {
            auto &call = call_node->data.fn_call_expr;
            auto *callee_proto = get_call_effect_proto(call);
            if (!callee_proto) {
                continue;
            }
            auto *callee_decl = get_call_summary_decl(call);

            auto explicit_exclusive_this =
                call.fn_ref_expr->type == ast::NodeType::DotExpr && callee_decl &&
                callee_decl->type == ast::NodeType::FnDef && callee_decl->data.fn_def.decl_spec &&
                callee_decl->data.fn_def.decl_spec->is_mutable();

            if (explicit_exclusive_this) {
                add_capture_roots_from_expr(call.fn_ref_expr->data.dot_expr.effective_expr(),
                                            call_node);
            }

            auto *fn_type = call.fn_ref_expr->resolved_type;
            if (fn_type && fn_type->kind == TypeKind::FnLambda) {
                fn_type = fn_type->data.fn_lambda.fn;
            }
            for (size_t i = 0; i < call.args.len; i++) {
                auto *param_type =
                    (fn_type && fn_type->kind == TypeKind::Fn) ? fn_type->data.fn.get_param_at(i) : nullptr;
                auto *param_node = i < callee_proto->params.len ? callee_proto->params[i] : nullptr;
                if (is_exclusive_access_borrow_param(param_type, param_node)) {
                    add_capture_roots_from_expr(call.args[i], call_node);
                }
            }

            for (size_t i = 0; i < callee_proto->requires_exclusive_capture_roots.len; i++) {
                add_capture_root(
                    callee_proto->requires_exclusive_capture_roots[i],
                    i < callee_proto->requires_exclusive_capture_sources.len &&
                            callee_proto->requires_exclusive_capture_sources[i]
                        ? callee_proto->requires_exclusive_capture_sources[i]
                        : call_node);
            }
        }
    }
}

void Resolver::apply_exclusive_access_effects(array<ast::Node *> &top_level_decls) {
    array<ast::Node *> fns;
    for (auto decl : top_level_decls) {
        collect_fn_nodes_from_decl(decl, fns);
    }

    for (auto *fn_node : fns) {
        if (fn_node->type != ast::NodeType::FnDef) {
            continue;
        }
        auto &fn_def = fn_node->data.fn_def;
        for (auto *call_node : fn_def.call_sites) {
            apply_call_exclusive_access_effects(fn_def, call_node->data.fn_call_expr, call_node);
        }
        check_terminal_flow_constraints(&fn_def, fn_def.flow, false, true);
    }
}

bool Resolver::apply_call_receiver_copy_edge_effects(ast::FnDef &fn_def, ast::FnCallExpr &call,
                                                     ast::Node *call_node) {
    (void)call_node;
    auto *summary_decl = get_call_summary_decl(call);
    auto *callee_proto = get_call_effect_proto(call);
    if (callee_proto && !callee_proto->copy_edge_summary_valid) {
        if (summary_decl && summary_decl->type == ast::NodeType::FnDef &&
            summary_decl->data.fn_def.body) {
            assert(false && "call summary must be finalized before applying receiver copy effects");
        }
        return false;
    }
    auto has_receiver = callee_proto && call_has_instance_receiver(call, callee_proto);
    if (!callee_proto || callee_proto->this_copy_edge_param_indices.len == 0 || !has_receiver ||
        call.fn_ref_expr->type != ast::NodeType::DotExpr) {
        return false;
    }

    auto *receiver_decl = find_root_decl(call.fn_ref_expr->data.dot_expr.effective_expr());
    if (!receiver_decl) {
        return false;
    }

    bool changed = false;
    for (auto idx : callee_proto->this_copy_edge_param_indices) {
        if (idx < 0 || idx >= (int32_t)call.args.len) {
            continue;
        }
        auto *before = fn_def.flow.ref_edges.get(receiver_decl);
        size_t before_len = before ? before->len : 0;
        add_borrow_source_edges(fn_def, call.args[static_cast<uint32_t>(idx)], receiver_decl,
                                false);
        auto *after = fn_def.flow.ref_edges.get(receiver_decl);
        size_t after_len = after ? after->len : 0;
        if (after_len != before_len) {
            changed = true;
        }
    }

    return changed;
}

bool Resolver::apply_receiver_copy_edge_effects(ast::FnDef &fn_def) {
    bool changed = false;
    for (size_t i = fn_def.applied_receiver_copy_effect_call_count; i < fn_def.call_sites.len; i++) {
        auto *call_node = fn_def.call_sites[i];
        changed |=
            apply_call_receiver_copy_edge_effects(fn_def, call_node->data.fn_call_expr, call_node);
    }
    fn_def.applied_receiver_copy_effect_call_count = fn_def.call_sites.len;
    return changed;
}

void Resolver::finalize_lifetime_flow(ast::FnDef &fn_def) {
    if (fn_def.fn_proto && (fn_def.applied_receiver_copy_effect_call_count < fn_def.call_sites.len ||
                            !fn_def.fn_proto->data.fn_proto.copy_edge_summary_valid)) {
        apply_receiver_copy_edge_effects(fn_def);
        compute_receiver_copy_edge_summary(fn_def);
    }
}

void Resolver::apply_call_capture_move_effects(ast::FnDef &fn_def, ast::FnCallExpr &call,
                                               ast::Node *call_node) {
    auto *callee_proto = get_call_effect_proto(call);
    if (!callee_proto) {
        return;
    }
    for (size_t i = 0; i < callee_proto->moved_capture_roots.len; i++) {
        auto *root = callee_proto->moved_capture_roots[i];
        auto kind = i < callee_proto->moved_capture_kinds.len
                        ? callee_proto->moved_capture_kinds[i]
                        : ast::SinkKind::Definite;
        fn_def.add_sink_edge(root, call_node, kind);
    }
}

void Resolver::apply_call_exclusive_access_effects(ast::FnDef &fn_def, ast::FnCallExpr &call,
                                                   ast::Node *call_node) {
    struct CallSlot {
        ast::Node *expr = nullptr;
        array<ast::Node *> roots = {};
        bool is_borrow = false;
        bool is_exclusive = false;
        ast::Node *exclusive_source = nullptr;
    };
    auto *callee_proto = get_call_effect_proto(call);
    if (!callee_proto) {
        return;
    }

    array<CallSlot> slots;

    if (call_has_instance_receiver(call, callee_proto)) {
        CallSlot slot;
        slot.expr = call.fn_ref_expr->data.dot_expr.effective_expr();
        auto *receiver_type = node_get_type(slot.expr);
        if (receiver_type && !receiver_type->is_borrow_reference() &&
            receiver_type->kind != TypeKind::MoveRef) {
            if (auto *receiver_root = find_root_decl(slot.expr)) {
                slot.roots.add(receiver_root);
            } else {
                collect_expr_borrow_roots(this, fn_def.flow, slot.expr, slot.roots);
            }
        } else {
            collect_expr_borrow_roots(this, fn_def.flow, slot.expr, slot.roots);
        }
        slot.is_borrow = true;
        auto *callee_decl = get_call_summary_decl(call);
        slot.is_exclusive = callee_decl && callee_decl->type == ast::NodeType::FnDef &&
                            callee_decl->data.fn_def.decl_spec &&
                            callee_decl->data.fn_def.decl_spec->is_mutable();
        slot.exclusive_source = slot.is_exclusive ? callee_decl : nullptr;
        slots.add(slot);
    }

    auto *fn_type = call.fn_ref_expr->resolved_type;
    if (fn_type && fn_type->kind == TypeKind::FnLambda) {
        fn_type = fn_type->data.fn_lambda.fn;
    }

    for (size_t i = 0; i < call.args.len; i++) {
        CallSlot slot;
        slot.expr = call.args[i];
        collect_expr_borrow_roots(this, fn_def.flow, slot.expr, slot.roots);
        auto *param_type =
            (fn_type && fn_type->kind == TypeKind::Fn) ? fn_type->data.fn.get_param_at(i) : nullptr;
        auto *param_node = i < callee_proto->params.len ? callee_proto->params[i] : nullptr;
        slot.is_borrow = is_exclusive_access_borrow_param(param_type, param_node) ||
                         type_may_propagate_borrow_deps(this, param_type);
        slot.is_exclusive = is_exclusive_access_borrow_param(param_type, param_node);
        slot.exclusive_source = slot.is_exclusive ? param_node : nullptr;
        slots.add(slot);
    }

    for (size_t i = 0; i < callee_proto->requires_exclusive_capture_roots.len; i++) {
        CallSlot slot;
        slot.expr = call_node;
        slot.roots.add(callee_proto->requires_exclusive_capture_roots[i]);
        slot.is_exclusive = true;
        slot.exclusive_source =
            i < callee_proto->requires_exclusive_capture_sources.len
                ? callee_proto->requires_exclusive_capture_sources[i]
                : nullptr;
        slots.add(slot);
    }

    bool had_conflict = false;
    for (size_t i = 0; i < slots.len; i++) {
        if (!slots[i].is_exclusive) {
            continue;
        }
        auto *exclusive_terminal = find_root_decl(slots[i].expr);
        for (auto *root : slots[i].roots) {
            fn_def.flow.add_invalidate_edge(root, call_node);
            if (exclusive_terminal && slots[i].is_borrow) {
                fn_def.flow.add_invalidate_exempt_terminal(root, exclusive_terminal);
            }
        }
        for (size_t j = 0; j < slots.len; j++) {
            if (i == j || !slots[j].is_borrow) {
                continue;
            }
            ast::Node *shared_root = nullptr;
            if (!roots_overlap(slots[i].roots, slots[j].roots, &shared_root)) {
                continue;
            }
            array<Note> notes;
            notes.add({errors::EXCLUSIVE_ACCESS_CALL_NOTE, call_node->token->pos});
            if (slots[i].exclusive_source && slots[i].exclusive_source->token &&
                slots[i].exclusive_source != call_node) {
                notes.add({describe_exclusive_access_source(slots[i].exclusive_source),
                           slots[i].exclusive_source->token->pos});
            }
            if (slots[j].expr && slots[j].expr->token) {
                notes.add({errors::EXCLUSIVE_ACCESS_BORROW_NOTE, slots[j].expr->token->pos});
            }
            error_with_notes(call_node, std::move(notes), errors::EXCLUSIVE_ACCESS_CALL_CONFLICT,
                             shared_root ? node_label(shared_root) : "<borrow>");
            had_conflict = true;
            break;
        }
        if (had_conflict) {
            break;
        }
    }
}

void Resolver::add_call_borrow_edges(ast::FnDef &fn_def, ast::FnCallExpr &call,
                                     ast::Node *call_node, ast::Node *target) {
    // Signature data comes from the actual resolved callee (including generated specializations).
    ast::FnProto *proto = get_call_signature_proto(call);
    // Body-derived summaries come from the root/original function body.
    ast::FnProto *summary_proto = get_call_effect_proto(call);

    // Borrow edges added here represent the call's return-value borrowing into its inputs.
    // Mark them as created at (just after) the call's source position so the exclusive-access
    // validation correctly treats them as fresh borrows produced by this call, not pre-existing
    // borrows that the call would invalidate.
    long call_edge_offset = (call_node && call_node->token)
                                ? static_cast<long>(call_node->token->pos.offset) + 1
                                : -1;

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
            add_borrow_source_edges(fn_def, call.fn_ref_expr->data.dot_expr.effective_expr(),
                                    target, true, call_edge_offset);
        }
        // By-value return copy summary: result inherits borrow dependencies from `this`
        // and/or parameters through assignments/temps in the callee body.
        if (summary_proto && summary_proto->copy_edge_summary_valid &&
            summary_proto->return_copy_edge_from_this &&
            call.fn_ref_expr->type == NodeType::DotExpr) {
            add_borrow_source_edges(fn_def, call.fn_ref_expr->data.dot_expr.effective_expr(),
                                    target, false, call_edge_offset);
        }
        if (summary_proto && summary_proto->copy_edge_summary_valid) {
            for (auto idx : summary_proto->return_copy_edge_param_indices) {
                if (idx >= 0 && idx < (int32_t)call.args.len) {
                    add_borrow_source_edges(fn_def, call.args[static_cast<uint32_t>(idx)], target,
                                            false, call_edge_offset);
                }
            }
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
        ret_lt = get_first_ref_lifetime(ret);
        conservative = (!ret_lt && type_may_propagate_borrow_deps(this, ret));
        for (size_t i = 0; i < fn.params.len; i++) {
            param_lts.add(get_first_ref_lifetime(fn.params[i]));
        }
    }

    // 'static params: add arg as terminal (value must not contain local borrows)
    for (size_t i = 0; i < param_lts.len && i < call.args.len; i++) {
        if (param_lts[i] && param_lts[i]->kind == LifetimeKind::Static) {
            auto *arg = call.args[i];
            if (fn_def.flow.ref_edges.has_key(arg)) {
                fn_def.add_terminal(arg);
                fn_def.flow.terminal_lifetimes[arg] = param_lts[i];
            }
        }
    }

    if (!ret_lt && !conservative)
        return;

    for (size_t i = 0; i < param_lts.len && i < call.args.len; i++) {
        if (param_lts[i] && param_lts[i]->kind == LifetimeKind::Static)
            continue; // handled above as terminal
        if (param_lts[i] && ret_lt && lifetime_outlives(param_lts[i], ret_lt)) {
            add_borrow_source_edges(fn_def, call.args[i], target, true);
        } else if (conservative && !param_lts[i]) {
            // No direct lifetime info for this by-value param — conservatively propagate any
            // borrow deps it carries into the return value, but do not treat the parameter root
            // itself as directly borrowed.
            add_borrow_source_edges(fn_def, call.args[i], target, false);
        }
    }
}

void Resolver::add_borrow_source_edges(ast::FnDef &fn_def, ast::Node *expr, ast::Node *target,
                                       bool is_ref, long edge_offset) {
    if (!expr)
        return;
    if (expr->resolved_node) {
        add_borrow_source_edges(fn_def, expr->resolved_node, target, is_ref, edge_offset);
        return;
    }
    if (expr->type == NodeType::DotExpr) {
        auto &data = expr->data.dot_expr;
        auto *projection_source = data.narrowed_var ? data.narrowed_var : data.effective_expr();
        if (data.resolved_dot_kind == DotKind::TupleField) {
            add_projection_source_edges(fn_def, projection_source, node_get_type(expr), target,
                                        is_ref, static_cast<int32_t>(data.resolved_value),
                                        nullptr);
            return;
        }
        if (data.resolved_struct_member && !data.resolved_struct_member->is_method()) {
            add_projection_source_edges(fn_def, projection_source, node_get_type(expr), target,
                                        is_ref, -1, data.resolved_struct_member);
            return;
        }
    }
    // If the expression traces to a root declaration (variable, field, index, etc.):
    // - is_ref=true (reference): add_ref_edge — target depends on root's own lifetime
    // - is_ref=false (by-value): copy_ref_edges — target inherits root's dependencies
    //   but doesn't depend on root itself (the data is copied out)
    auto *root = find_root_decl(expr);
    if (root) {
        if (!is_ref && is_nonowning_alias_decl(root) &&
            !type_may_propagate_borrow_deps(this, node_get_type(expr))) {
            return;
        }
        if (is_ref) {
            fn_def.add_ref_edge(target, root, edge_offset);
        } else {
            fn_def.copy_ref_edges(target, root, expr_can_fallback_to_borrow_source(expr),
                                  edge_offset);
        }
        return;
    }
    // Otherwise, recurse into compound expressions.
    switch (expr->type) {
    case NodeType::FnCallExpr:
        add_call_borrow_edges(fn_def, expr->data.fn_call_expr, expr, target);
        break;
    case NodeType::ConstructExpr:
        // Field inits already set up edges on the ConstructExpr node during resolution.
        // Transitively copy those leaf edges to the target.
        // A value construction is not itself a direct borrow source, so if the
        // construct carries no borrow deps this must remain a no-op.
        fn_def.copy_ref_edges(target, expr, false, edge_offset);
        break;
    case NodeType::TupleExpr:
        fn_def.copy_ref_edges(target, expr, false, edge_offset);
        break;
    case NodeType::TryExpr:
        add_borrow_source_edges(fn_def, expr->data.try_expr.expr, target, is_ref, edge_offset);
        break;
    case NodeType::CastExpr:
        add_borrow_source_edges(fn_def, expr->data.cast_expr.expr, target, is_ref, edge_offset);
        break;
    case NodeType::SwitchExpr:
        for (auto scase : expr->data.switch_expr.cases) {
            if (scase) {
                add_borrow_source_edges(fn_def, scase->data.case_expr.body, target, is_ref,
                                        edge_offset);
            }
        }
        break;
    case NodeType::FnDef:
        // Lambda with by-ref captures: capture edges were added during resolution.
        // Transitively copy them to the target.
        // A lambda value is not itself a direct borrow source; only its captured
        // borrow deps should propagate.
        fn_def.copy_ref_edges(target, expr, false, edge_offset);
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
    ast::Node *move_expr = get_moved_expr(expr);

    if (move_expr && move_expr != expr && move_expr->resolved_outlet) {
        auto *outlet_root = find_root_decl(move_expr->resolved_outlet);
        if (!outlet_root) {
            outlet_root = move_expr->resolved_outlet;
        }
        auto *dest_root = find_root_decl(dest);
        if (!dest_root) {
            dest_root = dest;
        }
        if (outlet_root != dest_root) {
            moved_src = move_expr->resolved_outlet;
        }
    }

    if (!moved_src && move_expr && move_expr->type == NodeType::UnaryOpExpr &&
        move_expr->data.unary_op_expr.op_type == TokenType::MOVEREF) {
        // Explicit &move expression: always a move
        moved_src = find_root_decl(move_expr->data.unary_op_expr.op1);
    } else if (!moved_src && move_expr && move_expr->type == NodeType::UnaryOpExpr &&
               move_expr->data.unary_op_expr.op_type == TokenType::KW_MOVE) {
        // Explicit move expression (value move): always a move
        moved_src = find_root_decl(move_expr->data.unary_op_expr.op1);
    } else if (!moved_src && expr->type == NodeType::Identifier && expr_type &&
               expr_type->kind == TypeKind::MoveRef && dest_type &&
               dest_type->kind == TypeKind::MoveRef) {
        // &move T identifier passed/assigned to &move destination: natural move
        moved_src = find_root_decl(expr);
    }

    if (moved_src) {
        fn_def.add_sink_edge(moved_src, dest);
        // A value move transfers the moved value's existing borrow dependencies,
        // but it does not make the destination borrow from the source variable itself.
        fn_def.copy_ref_edges(dest, moved_src, false);
    }
}

static string node_label(ast::Node *n) {
    auto type_str = ast::node_type_display_name(n->type);
    if (n->name.empty())
        return type_str;
    return fmt::format("{} ({})", n->name, type_str);
}

static string debug_node_label(ast::Node *n) {
    if (!n) {
        return "<null>";
    }
    auto label = node_label(n);
    if (n->name.rfind("__tuple_proj_", 0) == 0 || n->name.rfind("__field_proj_", 0) == 0) {
        return fmt::format("{}@{:p}", label, static_cast<void *>(n));
    }
    return label;
}

static string describe_exclusive_access_source(ast::Node *n) {
    if (!n) {
        return "this mut call";
    }
    if (n->type == ast::NodeType::FnCallExpr) {
        auto &call = n->data.fn_call_expr;
        auto *decl = get_call_decl(call);
        if (decl && !decl->name.empty()) {
            if (call.fn_ref_expr && call.fn_ref_expr->type == ast::NodeType::DotExpr) {
                return fmt::format("call to mut method '{}' requires exclusive access",
                                   decl->name);
            }
            if (decl->type == ast::NodeType::FnDef || decl->type == ast::NodeType::GeneratedFn) {
                return fmt::format("call to mut function '{}' requires exclusive access",
                                   decl->name);
            }
            if (decl->type == ast::NodeType::VarDecl && decl->data.var_decl.expr &&
                decl->data.var_decl.expr->type == ast::NodeType::FnDef) {
                return fmt::format(
                    "call to lambda '{}' requires exclusive access because it captures a borrowed value",
                    decl->name);
            }
        }
        return "this mut call requires exclusive access";
    }
    if (n->type == ast::NodeType::FnDef && !n->name.empty()) {
        return fmt::format("function '{}' is declared mut here", n->name);
    }
    return fmt::format("exclusive access originates from {}", node_label(n));
}

static ChiLifetime *get_terminal_required_lifetime(Resolver *resolver, ast::FnDef *fn_def,
                                                   ast::FlowState &flow, ast::Node *terminal) {
    if (auto *explicit_lt = flow.terminal_lifetimes.get(terminal)) {
        return *explicit_lt;
    }

    if (terminal->type == ast::NodeType::ReturnStmt) {
        // Return terminal: use the pre-calculated return type lifetime from elision
        if (fn_def->fn_proto && fn_def->fn_proto->resolved_type &&
            fn_def->fn_proto->resolved_type->kind == TypeKind::Fn) {
            auto *ret_type = fn_def->fn_proto->resolved_type->data.fn.return_type;
            auto *ret_lifetimes = ret_type ? ret_type->get_lifetimes() : nullptr;
            if (ret_type && (ret_type->is_reference() || ret_type->kind == TypeKind::Span) &&
                ret_lifetimes && ret_lifetimes->len > 0) {
                return (*ret_lifetimes)[0];
            }
        }
        // Also check resolved_return_lifetime for borrowing value returns (func(), structs)
        if (fn_def->fn_proto) {
            auto *proto = fn_def->fn_proto->type == ast::NodeType::FnProto
                              ? &fn_def->fn_proto->data.fn_proto
                              : nullptr;
            if (proto && proto->resolved_return_lifetime) {
                return proto->resolved_return_lifetime;
            }
        }
        return nullptr;
    }

    return nullptr;
}

static bool return_summary_allows_leaf(ast::FnDef *fn_def, ast::Node *leaf) {
    if (!fn_def || !fn_def->fn_proto || fn_def->fn_proto->type != ast::NodeType::FnProto) {
        return false;
    }
    auto &proto = fn_def->fn_proto->data.fn_proto;
    if (!proto.copy_edge_summary_valid) {
        return false;
    }
    if (leaf->type == ast::NodeType::Identifier && is_this_identifier_node(leaf)) {
        return proto.return_copy_edge_from_this;
    }
    if (leaf->type != ast::NodeType::ParamDecl) {
        return false;
    }
    for (size_t i = 0; i < proto.params.len; i++) {
        if (proto.params[i] != leaf) {
            continue;
        }
        for (auto idx : proto.return_copy_edge_param_indices) {
            if (idx == static_cast<int32_t>(i)) {
                return true;
            }
        }
        return false;
    }
    return false;
}

// Check if a leaf node satisfies a lifetime constraint relative to a terminal.
// Only called on base cases (VarDecl, ParamDecl) — graph construction
// via copy_ref_edges already flattens intermediate nodes to leaves.
static bool satisfies_lifetime_constraint(ast::FnDef *fn_def, ChiLifetime *required,
                                          ast::Node *terminal, ast::Node *leaf) {
    // 'static required: no local variable or non-static param can satisfy it
    if (required && required->kind == LifetimeKind::Static) {
        if (leaf->type == ast::NodeType::VarDecl)
            return false;
        if (leaf->type == ast::NodeType::ParamDecl) {
            auto *leaf_type = leaf->resolved_type;
            ChiLifetime *param_lt = get_param_effective_lifetime(leaf, leaf_type);
            return param_lt && param_lt->kind == LifetimeKind::Static;
        }
        return false;
    }

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
        // Determine the param's lifetime: from the reference type or from borrow_lifetime
        ChiLifetime *param_lt = get_param_effective_lifetime(leaf, leaf_type);

        // No lifetime → plain value param (int, bool, etc.) that was captured by-ref.
        // For return terminals, body-derived copy-edge summaries describe which params
        // may legally flow into the return value.
        if (!param_lt) {
            if (terminal->type == ast::NodeType::ReturnStmt) {
                return return_summary_allows_leaf(fn_def, leaf);
            }
            return true;
        }

        // Has lifetime → check against required (null required = always OK)
        if (!required)
            return true;
        return lifetime_outlives(param_lt, required);
    }

    if (leaf->type == ast::NodeType::Identifier &&
        leaf->data.identifier.kind == ast::IdentifierKind::This) {
        if (!required && terminal->type == ast::NodeType::ReturnStmt) {
            return return_summary_allows_leaf(fn_def, leaf);
        }
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
// Wrapper: checks lifetime constraints on the function's current flow state
void Resolver::check_lifetime_constraints(ast::FnDef *fn_def) {
    check_lifetime_constraints(fn_def, fn_def->flow);
}

// DFS from each terminal following ref_edges. For every reachable node,
// satisfies_lifetime_constraint(required, node) determines if the constraint holds.
void Resolver::check_lifetime_constraints(ast::FnDef *fn_def, ast::FlowState &flow) {
    // Any local with outgoing ref_edges is a terminal (intra-function ordering)
    for (auto &[from, _] : flow.ref_edges.data) {
        if (from->decl_order >= 0) {
            flow.add_terminal(from);
        }
    }
    for (auto &[from, _] : flow.copy_edges.data) {
        if (from->decl_order >= 0) {
            flow.add_terminal(from);
        }
    }

    if (flow.terminals.len == 0 && flow.ref_edges.data.size() == 0 && flow.copy_edges.data.size() == 0)
        return;
    bool is_safe = has_lang_flag(m_module->get_lang_flags(), LANG_FLAG_SAFE);
    bool verbose = has_lang_flag(m_ctx->lang_flags, LANG_FLAG_VERBOSE_LIFETIMES);

    auto fn_name = fn_def->fn_proto ? fn_def->fn_proto->name : "<lambda>";

    if (verbose && (flow.ref_edges.data.size() > 0 || flow.copy_edges.data.size() > 0)) {
        fmt::print("[lifetime] === {} ===\n", fn_name);
        fmt::print("[lifetime] edges:\n");
        for (auto &[from, tos] : flow.ref_edges.data) {
            for (auto *to : tos) {
                fmt::print("[lifetime]   borrow {} -> {}\n", debug_node_label(from),
                           debug_node_label(to));
            }
        }
        for (auto &[from, tos] : flow.copy_edges.data) {
            for (auto *to : tos) {
                fmt::print("[lifetime]   copy {} -> {}\n", debug_node_label(from),
                           debug_node_label(to));
            }
        }
        fmt::print("[lifetime] terminals ({}):\n", flow.terminals.len);
        for (size_t i = 0; i < flow.terminals.len; i++) {
            fmt::print("[lifetime]   {}\n", debug_node_label(flow.terminals[i]));
        }
    }

    for (size_t t = 0; t < flow.terminals.len; t++) {
        auto *terminal = flow.terminals[t];

        auto *required = get_terminal_required_lifetime(this, fn_def, flow, terminal);

        if (verbose) {
            fmt::print("[lifetime] checking terminal: {} (required: {})\n", node_label(terminal),
                       required ? "'" + required->name : "'fn");
        }

        array<ast::Node *> leaves;
        flow.collect_borrow_leaves(terminal, leaves);
        for (auto *node : leaves) {
            bool satisfied = satisfies_lifetime_constraint(fn_def, required, terminal, node);
            if (verbose) {
                fmt::print("[lifetime]   leaf {} -> {}\n", node_label(node),
                           satisfied ? "OK" : "VIOLATION");
            }

            if (!satisfied) {
                // If this leaf is explicitly moved/deleted later, let the sink checker
                // produce the more precise diagnostic based on actual last use.
                if (flow.is_sunk(node)) {
                    continue;
                }
                if (is_safe) {
                    array<Note> notes;
                    notes.add({"referenced here", terminal->token->pos});
                    error_with_notes(node, std::move(notes), "'{}' does not live long enough",
                                     node->name);
                } else {
                    node->analysis.escaped = true;
                }
            }
        }
    }

    check_terminal_flow_constraints(fn_def, flow, is_safe, true);
}

void Resolver::check_terminal_flow_constraints(ast::FnDef *fn_def, ast::FlowState &flow,
                                               bool check_sinks, bool check_invalidations) {
    if (check_sinks) {
        for (size_t t = 0; t < flow.terminals.len; t++) {
            auto *terminal = flow.terminals[t];
            if (flow.is_sunk(terminal))
                continue;
            auto *deps = flow.ref_edges.get(terminal);
            if (!deps)
                continue;
            size_t offset = flow.current_edge_offset(terminal);
            auto *last_use = flow.terminal_last_use.get(terminal);
            auto *last_use_node = flow.terminal_last_use_node.get(terminal);
            if ((terminal->type == NodeType::VarDecl || terminal->type == NodeType::ParamDecl) &&
                (!last_use_node || !*last_use_node)) {
                continue;
            }
            for (size_t i = offset; i < deps->len; i++) {
                auto *node = deps->items[i];
                if (flow.is_sunk(node)) {
                    auto *sink_target = flow.sink_target(node);
                    if (sink_target == terminal)
                        continue;
                    if (last_use_node && *last_use_node == sink_target) {
                        continue;
                    }
                    if (last_use && sink_target && sink_target->token) {
                        if (*last_use <= sink_target->token->pos.offset)
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
                    auto *error_node = last_use_node && *last_use_node ? *last_use_node : terminal;
                    assert(error_node && error_node->token &&
                           "lifetime sink violation missing diagnostic node");
                    notes.add({"referenced here", error_node->token->pos});
                    error_with_notes(error_node, std::move(notes), "'{}' used after {}",
                                     terminal->name, is_delete ? "delete" : "move");
                }
            }
        }
    }

    if (check_invalidations) {
        for (size_t t = 0; t < flow.terminals.len; t++) {
            auto *terminal = flow.terminals[t];
            if (flow.is_sunk(terminal)) {
                continue;
            }
            auto *deps = flow.ref_edges.get(terminal);
            if (!deps) {
                continue;
            }
            size_t offset = flow.current_edge_offset(terminal);
            auto *last_use = flow.terminal_last_use.get(terminal);
            auto *last_use_node = flow.terminal_last_use_node.get(terminal);
            if ((terminal->type == NodeType::VarDecl || terminal->type == NodeType::ParamDecl) &&
                (!last_use_node || !*last_use_node)) {
                continue;
            }
            auto *edge_offsets = flow.ref_edge_offsets.get(terminal);
            for (size_t i = offset; i < deps->len; i++) {
                auto *root = deps->items[i];
                auto *invalid = flow.invalidate_edges.get(root);
                if (!invalid) {
                    continue;
                }
                if (auto *exempt = flow.invalidate_exempt_terminals.get(root)) {
                    bool skip = false;
                    for (auto *item : *exempt) {
                        if (item == terminal) {
                            skip = true;
                            break;
                        }
                    }
                    if (skip) {
                        continue;
                    }
                }
                auto *target = invalid->target;
                long edge_created_at =
                    edge_offsets && i < edge_offsets->len ? edge_offsets->items[i] : -1;
                if (edge_created_at < 0 && terminal->token) {
                    edge_created_at = terminal->token->pos.offset;
                }
                if (target && target->token && edge_created_at >= 0 &&
                    edge_created_at > target->token->pos.offset) {
                    continue;
                }
                if (last_use && target && target->token && *last_use <= target->token->pos.offset) {
                    continue;
                }
                array<Note> notes;
                if (target && target->token) {
                    notes.add({fmt::format(errors::EXCLUSIVE_ACCESS_INVALIDATED_NOTE,
                                           describe_exclusive_access_source(target)),
                               target->token->pos});
                }
                auto *error_node = last_use_node && *last_use_node ? *last_use_node : terminal;
                assert(error_node && error_node->token &&
                       "lifetime invalidation violation missing diagnostic node");
                notes.add({errors::EXCLUSIVE_ACCESS_BORROW_REFERENCED, error_node->token->pos});
                error_with_notes(error_node, std::move(notes),
                                 errors::EXCLUSIVE_ACCESS_BORROW_USED_AFTER, node_label(root));
                break;
            }
        }
    }
}

bool Resolver::compare_impl_type(ChiType *base, ChiType *impl) {
    if (base == impl) {
        return true;
    }
    // Unwrap matching wrapper types and compare inner types
    if (base->kind == impl->kind && base->is_pointer_like()) {
        return compare_impl_type(base->get_elem(), impl->get_elem());
    }
    // A generic struct Foo and its self-referencing Subtype Foo<T> (where T is its own
    // placeholder) are equivalent — this arises when &This in an interface method gets
    // substituted with the raw struct, but the impl method resolves &This to the Subtype.
    if (base->kind == TypeKind::Struct && impl->kind == TypeKind::Subtype &&
        impl->data.subtype.generic == base) {
        return true;
    }
    if (impl->kind == TypeKind::Struct && base->kind == TypeKind::Subtype &&
        base->data.subtype.generic == impl) {
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
    auto symbols = scope.is_lhs ? array<IntrinsicSymbol>{IntrinsicSymbol::DerefMut,
                                                         IntrinsicSymbol::Deref}
                                : array<IntrinsicSymbol>{IntrinsicSymbol::Deref,
                                                         IntrinsicSymbol::DerefMut};

    ChiStructMember *deref_member = nullptr;
    ChiType *deref_return_type = nullptr;
    ChiStructMember *target_member = nullptr;

    auto candidate_is_usable = [&](ChiStructMember *member, ChiType *return_type) {
        if (!member || !return_type || !return_type->is_reference()) {
            return false;
        }
        auto *target = return_type->get_elem();
        auto *resolved_member = get_struct_member(target, field_name);
        if (!resolved_member) {
            return false;
        }
        if (scope.is_lhs && !ChiTypeStruct::is_mutable_pointer(return_type)) {
            return false;
        }
        if (resolved_member->is_method()) {
            auto &spec = resolved_member->node->declspec_ref();
            if (spec.is_mutable() && !ChiTypeStruct::is_mutable_pointer(return_type)) {
                return false;
            }
        }
        target_member = resolved_member;
        return true;
    };

    for (auto symbol : symbols) {
        auto member_p = struct_data.member_intrinsics.get(symbol);
        if (!member_p) {
            continue;
        }
        auto *candidate_member = *member_p;
        auto *candidate_fn_type = candidate_member->resolved_type;
        if (!candidate_fn_type || candidate_fn_type->kind != TypeKind::Fn) {
            continue;
        }
        auto *candidate_return_type = candidate_fn_type->data.fn.return_type;
        if (!candidate_return_type || !candidate_return_type->is_reference()) {
            continue;
        }
        if (!candidate_is_usable(candidate_member, candidate_return_type)) {
            continue;
        }
        deref_member = candidate_member;
        deref_return_type = candidate_return_type;
        break;
    }
    if (!deref_member || !deref_return_type) {
        return nullptr;
    }

    // Build synthetic call: expr.deref() or expr.deref_mut()
    auto &data = node->data.dot_expr;

    auto dot_node = create_node(ast::NodeType::DotExpr);
    dot_node->token = node->token;
    dot_node->data.dot_expr.expr = data.effective_expr();
    dot_node->data.dot_expr.field = deref_member->node->token;
    dot_node->data.dot_expr.resolved_struct_member = deref_member;
    dot_node->data.dot_expr.resolved_decl = deref_member->node;

    auto call_node = create_node(ast::NodeType::FnCallExpr);
    call_node->token = node->token;
    call_node->data.fn_call_expr.fn_ref_expr = dot_node;
    call_node->data.fn_call_expr.args = {};
    resolve(call_node, scope);

    // Preserve the parsed AST and keep the synthetic deref call as resolved-only data.
    data.resolved_expr = call_node;
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
                } else if (fn_data.params.len == 1) {
                    auto param = fn_data.params[0];
                    // Unwrap reference for operator params (e.g. &This → This)
                    auto param_elem = param->kind == TypeKind::Reference ? param->get_elem() : param;
                    if (can_assign(t2, param_elem)) {
                        return_type = fn_data.return_type;
                    }
                }
            }
        }
    }

    // Try placeholder type with trait bounds
    if (!method_member && t1->kind == TypeKind::Placeholder) {
        for (auto trait_type : get_placeholder_traits(t1)) {
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
        call_node->module = node->module;
        auto dot_node = create_node(NodeType::DotExpr);
        dot_node->token = node->token;
        dot_node->module = node->module;

        // populate generated dot expression
        auto &dot_data = dot_node->data.dot_expr;
        dot_node->data.dot_expr.expr = op1;
        dot_data.field = method_member->node->token;
        dot_data.resolved_struct_member = method_member;
        dot_data.resolved_decl = method_member->node;

        // populate generated call
        auto &call_data = call_node->data.fn_call_expr;
        call_data.fn_ref_expr = dot_node;
        if (op2) {
            auto method_type = method_member->resolved_type;
            auto param = method_type->data.fn.params[0];
            // If the param is a reference (e.g. &This), wrap op2 in a take-address node
            if (param->kind == TypeKind::Reference) {
                auto ref_node = create_node(NodeType::UnaryOpExpr);
                ref_node->token = op2->token;
                ref_node->module = op2->module;
                ref_node->data.unary_op_expr.op1 = op2;
                ref_node->data.unary_op_expr.op_type = TokenType::AND;
                ref_node->resolved_type = get_pointer_type(t2, TypeKind::Reference);
                call_data.args = {ref_node};
            } else {
                call_data.args = {op2};
            }
        } else {
            call_data.args = {};
        }
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
        if (!ctor_params[i]->data.param_decl.effective_default_value() &&
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

    // Placeholder: check declared traits directly
    if (check_arg->kind == TypeKind::Placeholder) {
        for (auto t : get_placeholder_traits(check_arg)) {
            if (t == trait_type)
                return true;
        }
        return false;
    }

    // For subtypes, use the generic struct directly — no need to resolve
    if (check_arg->kind == TypeKind::Subtype && check_arg->data.subtype.generic) {
        check_arg = check_arg->data.subtype.generic;
    }

    // Sized is structural: everything is Sized except interfaces and void.
    if (trait_type->kind == TypeKind::Struct && trait_type->data.struct_.node &&
        resolve_intrinsic_symbol(trait_type->data.struct_.node) == IntrinsicSymbol::Sized) {
        return check_arg->kind != TypeKind::Void &&
               !(check_arg->kind == TypeKind::Struct && ChiTypeStruct::is_interface(check_arg));
    }

    // NoCopy bound: allows all types through (both copyable and non-copyable)
    if (trait_type->kind == TypeKind::Struct && trait_type->data.struct_.node &&
        resolve_intrinsic_symbol(trait_type->data.struct_.node) == IntrinsicSymbol::NoCopy) {
        return true;
    }

    // Unsized bound is an opt-out of the implicit Sized bound, not a concrete requirement.
    if (trait_type->kind == TypeKind::Struct && trait_type->data.struct_.node &&
        resolve_intrinsic_symbol(trait_type->data.struct_.node) == IntrinsicSymbol::Unsized) {
        return true;
    }

    // Copy is structural: all types satisfy Copy by default, except types implementing NoCopy
    if (trait_type->kind == TypeKind::Struct && trait_type->data.struct_.node &&
        resolve_intrinsic_symbol(trait_type->data.struct_.node) == IntrinsicSymbol::Copy) {
        return !is_non_copyable(check_arg);
    }

    // Resolve built-in types backed by structs (e.g. string → __CxString)
    // so their Chi impl blocks are checked for trait satisfaction
    auto resolved_struct = eval_struct_type(check_arg);
    if (resolved_struct && resolved_struct->kind == TypeKind::Struct) {
        check_arg = resolved_struct;
    }

    if (check_arg->kind == TypeKind::Struct) {
        if (struct_satisfies_interface(check_arg, trait_type)) return true;
        // Constructor interface: check ctor compatibility
        if (is_constructor_interface_compatible(check_arg, trait_type))
            return true;
    }
    // Built-in types without struct backing: check intrinsics
    if (check_arg->kind != TypeKind::Struct) {
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
                                    ChiType *subtype, map<ChiType *, ChiType *> subs,
                                    bool from_method_sig) {
    if (struct_envs.get(id)) {
        return; // Already recorded
    }
    TypeEnvEntry entry;
    entry.name = name;
    entry.node = generic->data.struct_.node;
    entry.generic_type = generic;
    entry.subtype = subtype;
    entry.subs = subs;
    entry.from_method_sig = from_method_sig;
    struct_envs[id] = entry;
}

void GenericResolver::resolve_pending(Resolver *resolver) {
    bool made_progress = true;
    while (made_progress) {
        made_progress = false;
        array<string> pending_ids;
        for (auto &pair : struct_envs.data) {
            pending_ids.add(pair.first);
        }
        for (auto &id : pending_ids) {
            auto entry = struct_envs.get(id);
            if (!entry || !entry->subtype || entry->subtype->data.subtype.final_type) {
                continue;
            }
            if (entry->subtype->subtype_depth() > MAX_GENERIC_DEPTH)
                continue;
            if (entry->from_method_sig)
                continue;
            bool was_resolved = entry->subtype->data.subtype.final_type != nullptr;
            resolver->resolve_subtype(entry->subtype);
            if (!was_resolved && entry->subtype->data.subtype.final_type) {
                made_progress = true;
            }
        }
    }
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
