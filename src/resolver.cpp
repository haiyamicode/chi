/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "resolver.h"
#include "ast.h"
#include "enum.h"
#include "errors.h"
#include "fmt/core.h"
#include "lexer.h"
#include "sema.h"
#include "util.h"

using namespace cx;

using ast::NodeType;

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
        panic("primitives already initialized");
        return;
    }
    auto &system_types = m_ctx->system_types;
    system_types.any = create_type(TypeKind::Any);
    system_types.char_ = create_type(TypeKind::Char);
    system_types.uint8 = create_int_type(8, true);
    system_types.int_ = create_int_type(32, false);
    system_types.int32 = create_int_type(32, false);
    system_types.uint32 = create_int_type(32, true);
    system_types.int64 = create_int_type(64, false);
    system_types.float_ = create_float_type(32);
    system_types.float64 = create_float_type(64);
    system_types.void_ = create_type(TypeKind::Void);
    system_types.void_ptr = create_pointer_type(system_types.void_, TypeKind::Pointer);
    system_types.null_ptr = create_pointer_type(system_types.void_, TypeKind::Pointer);
    system_types.null_ptr->data.pointer.is_null = true;
    system_types.void_ref = create_pointer_type(system_types.void_, TypeKind::Reference);
    system_types.bool_ = create_type(TypeKind::Bool);
    system_types.string = create_type(TypeKind::String);
    system_types.str_lit = create_pointer_type(system_types.char_, TypeKind::Pointer);
    system_types.array = create_type(TypeKind::Array);
    system_types.optional = create_type(TypeKind::Optional);
    system_types.box = create_type(TypeKind::Box);
    system_types.result = create_type(TypeKind::Result);
    system_types.error = create_type(TypeKind::Error);
    // Promise is now defined as a Chi-native struct in runtime.xc
    // system_types.promise = create_type(TypeKind::Promise);
    system_types.undefined = create_type(TypeKind::Undefined);

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
    add_primitive("char", system_types.char_);
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

    // non-primitive builtins
    add_primitive("Box", system_types.box);
    add_primitive("Result", system_types.result);
    add_primitive("Error", system_types.error);
    // Promise is now defined as a Chi-native struct in runtime.xc
    // add_primitive("Promise", system_types.promise);

    // intrinsic symbols
    m_ctx->intrinsic_symbols["std.ops.Index"] = IntrinsicSymbol::Index;
    m_ctx->intrinsic_symbols["std.ops.IndexIterable"] = IntrinsicSymbol::IndexInterable;
    m_ctx->intrinsic_symbols["std.ops.CopyFrom"] = IntrinsicSymbol::CopyFrom;
    m_ctx->intrinsic_symbols["std.ops.Display"] = IntrinsicSymbol::Display;
    m_ctx->intrinsic_symbols["std.ops.Add"] = IntrinsicSymbol::Add;
}

ChiType *Resolver::create_type(TypeKind kind) { return m_ctx->allocator->create_type(kind); }

ChiType *Resolver::create_type_symbol(optional<string> name, ChiType *type) {
    auto tysym = create_type(TypeKind::TypeSymbol);
    tysym->name = name;
    tysym->data.type_symbol.giving_type = type;
    tysym->data.type_symbol.underlying_type = type;
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
    case TypeKind::Char:
        return 8;
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

    // If target is larger, it's always safe
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

    if (is_same_type(from_type, to_type)) {
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
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef: {
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
            from_type->kind == TypeKind::MutRef) {

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
            }
        }

        // Int to pointer conversion must be explicit (after pointer/ref checks)
        if (from_type->is_int_like()) {
            return is_explicit;
        }

        return false;
    }
    case TypeKind::Int: {
        // Allow implicit conversion from bool, char, and smaller int types
        if (from_type->kind == TypeKind::Bool || from_type->kind == TypeKind::Char) {
            return is_safe_int_conversion(from_type, to_type);
        }
        if (from_type->kind == TypeKind::Int) {
            return is_safe_int_conversion(from_type, to_type);
        }
        // Float to int requires explicit conversion
        if (from_type->kind == TypeKind::Float) {
            return is_explicit;
        }
        return false;
    }
    case TypeKind::Float: {
        // Allow implicit conversion from any int type
        if (from_type->is_int_like()) {
            return true;
        }
        // Allow implicit conversion from float32 to float64
        if (from_type->kind == TypeKind::Float) {
            return to_type->data.float_.bit_count >= from_type->data.float_.bit_count;
        }
        return false;
    }
    case TypeKind::Char:
        return from_type->kind == TypeKind::Char || (is_explicit && from_type->is_int_like());
    case TypeKind::Any:
        return true;
    case TypeKind::Struct: {
        if (from_type == to_type) {
            return true;
        }
        if (ChiTypeStruct::is_interface(to_type) && ChiTypeStruct::is_pointer_type(from_type)) {
            auto ft = from_type->get_elem();
            if (ft->kind != TypeKind::Struct) {
                return false;
            }
            auto &ss = ft->data.struct_;
            if (ss.kind == ContainerKind::Struct) {
                for (auto &impl : ss.interfaces) {
                    if (is_same_type(impl->interface_type, to_type)) {
                        return true;
                    }
                }
                return false;
            }
        }
        return false;
    }
    case TypeKind::Bool:
        // Allow implicit conversion from any int type to bool
        return from_type->is_int_like() || from_type->kind == TypeKind::Optional ||
               from_type->is_pointer_like() || (is_explicit && from_type->kind == TypeKind::Char);
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
        return from_type->kind == TypeKind::EnumValue &&
               is_same_type(from_type->data.enum_value.enum_type,
                            to_type->data.enum_value.enum_type);
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
    if (type->kind == TypeKind::TypeSymbol) {
        return type->data.type_symbol.giving_type;
    }
    return type;
}

ChiType *Resolver::resolve_value(ast::Node *node, ResolveScope &scope) {
    auto value_type = to_value_type(resolve(node, scope));
    if (ChiTypeStruct::is_generic(value_type)) {
        error(node, errors::MISSING_TYPE_ARGUMENTS, to_string(value_type));
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
            resolve(data.body, local_fn_scope);

            // resolve captures
            for (auto decl : data.captures) {
                auto type = resolve(decl, scope);
                proto->data.fn_lambda.captures.add(type);
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
                for (int i = 0; i < data.captures.len; i++) {
                    auto capture = data.captures[i];
                    auto name = fmt::format("capture_{}", i);
                    bstruct_data.add_member(
                        get_allocator(), capture->name, get_dummy_var(name),
                        get_pointer_type(capture->resolved_type, TypeKind::Reference));
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
            if (rt_lambda && rt_lambda->data.struct_.resolve_status >= ResolveStatus::MemberTypesKnown) {
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
            resolve(data.body, fn_scope);

            // Add params to cleanup_vars (after body resolution ensures types are resolved)
            auto &proto_data = data.fn_proto->data.fn_proto;
            for (auto param : proto_data.params) {
                if (should_destroy(param) && !param->escape.is_capture()) {
                    data.cleanup_vars.add(param);
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

        // Process type parameters first
        for (auto param : data.type_params) {
            auto type_param = resolve(param, fn_scope);
            type_param_types.add(type_param);
        }

        auto return_type = data.return_type ? resolve_value(data.return_type, fn_scope)
                                            : get_system_types()->void_;

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
        for (int i = 0; i < data.params.len; i++) {
            auto param = data.params[i];
            auto &pdata = param->data.param_decl;
            auto is_last = i == data.params.len - 1;
            if (pdata.is_variadic && !is_last) {
                error(param, errors::VARIADIC_NOT_FINAL, param->name);
                return create_type(TypeKind::Fn);
            }
            auto param_type = resolve_value(param, fn_scope);
            if (pdata.is_variadic) {
                param_type = get_array_type(param_type);
                param->resolved_type = param_type;
                is_variadic = true;
            }
            param_types.add(param_type);
        }

        auto is_extern = node->get_declspec() ? node->get_declspec()->is_extern() : false;
        // Only pass container for method declarations, not for function parameter types
        auto method_container = is_fn_decl ? container : nullptr;
        auto fn_type = get_fn_type(return_type, &param_types, is_variadic, method_container,
                                   is_extern, &type_param_types);

        if (is_lambda || !is_fn_decl) {
            return get_lambda_for_fn(fn_type);
        }
        return fn_type;
    }
    case NodeType::Identifier: {
        auto &data = node->data.identifier;
        if (data.kind == ast::IdentifierKind::This) {
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
            type->data.pointer.elem = get_pointer_type(
                scope.parent_struct, is_mut ? TypeKind::MutRef : TypeKind::Reference);
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
                        captures.add(data.decl);
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

        // Convert function to lambda when used as value (not in call context)
        if (data.decl->type == NodeType::FnDef && !scope.is_fn_call && type->kind == TypeKind::Fn) {
            type = get_lambda_for_fn(type);
        }

        return type;
    }
    case NodeType::TypeSigil: {
        auto &data = node->data.sigil_type;
        auto type = resolve_value(data.type, scope);
        auto final_type = get_pointer_type(type, get_sigil_type_kind(data.sigil));
        return create_type_symbol({}, final_type);
    }
    case NodeType::ParamDecl: {
        auto &data = node->data.param_decl;
        auto result = resolve_value(data.type, scope);
        return result;
    }
    case NodeType::VarDecl: {
        auto &data = node->data.var_decl;
        ChiType *var_type = nullptr;
        if (data.type) {
            var_type = resolve_value(data.type, scope);
        }
        if (data.expr) {
            auto var_scope = var_type ? scope.set_value_type(var_type) : scope;
            var_scope = var_scope.set_move_outlet(node);
            auto expr_type = resolve(data.expr, var_scope);
            if (var_type) {
                if (expr_type->kind == TypeKind::Undefined) {
                    return var_type;
                }
                if (data.expr->type != NodeType::ConstructExpr ||
                    data.expr->data.construct_expr.type) {
                    check_assignment(data.expr, expr_type, var_type);
                }
            } else {
                var_type = expr_type;
            }
        }
        if (!var_type) {
            // Failed to resolve variable type due to malformed expression
            error(node, "failed to resolve variable type");
            return get_system_types()->void_;
        }
        if (var_type->kind == TypeKind::Void) {
            error(node, errors::INVALID_VARIABLE_TYPE, to_string(var_type));
        }
        if (data.is_const) {
            data.resolved_value = resolve_constant_value(data.expr);
            return var_type;
        }
        // Add to cleanup_vars if this variable needs destruction
        if (scope.parent_fn_node && should_destroy(node, var_type) && !node->escape.is_capture()) {
            scope.parent_fn_def()->cleanup_vars.add(node);
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

        ChiType *t2;
        if (is_assignment_op(data.op_type)) {
            auto var = data.op1->get_decl();
            if (var && var->type == NodeType::VarDecl) {
                if (var->data.var_decl.is_const) {
                    error(node, errors::ASSIGNMENT_TO_CONST, var->name);
                }
            }
            if (var && var->type == NodeType::VarDecl && !var->data.var_decl.initialized_at) {
                if (!var->data.var_decl.is_field ||
                    scope.parent_fn->name != "new" && data.op_type == TokenType::ASS) {
                    var->data.var_decl.initialized_at = node;
                }
            }
            auto var_scope = scope.set_value_type(t1).set_move_outlet(data.op1);
            t2 = resolve(data.op2, var_scope);
        } else {
            t2 = resolve(data.op2, scope);
        }

        // For assignment operators, just check assignment validity
        if (is_assignment_op(data.op_type)) {
            check_assignment(data.op2, t2, t1);
            return t1;
        }

        switch (data.op_type) {
        case TokenType::EQ:
        case TokenType::NE:
        case TokenType::LT:
        case TokenType::LE:
        case TokenType::GT:
        case TokenType::GE:
            return get_system_types()->bool_;
        default: {
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
        auto t = resolve(data.op1, scope);
        switch (auto tt = data.op_type) {
        case TokenType::SUB:
        case TokenType::ADD:
        case TokenType::INC:
        case TokenType::DEC:
            check_assignment(data.op1, t,
                             t->kind == TypeKind::Float ? t : get_system_types()->int_);
            return t->kind == TypeKind::Bool ? get_system_types()->int_ : t;
        case TokenType::MUL: {
            error(data.op1, errors::C_STYLE_DEREFERENCE_DEPRECATED);
            break;
        }
        case TokenType::LNOT: {
            if (data.is_suffix) {
                if (ChiTypeStruct::is_pointer_type(t) && t->get_elem()->kind != TypeKind::Void) {
                    if (scope.is_lhs && !ChiTypeStruct::is_mutable_pointer(t)) {
                        error(data.op1, errors::CANNOT_MODIFY_IMMUTABLE_REFERENCE, to_string(t),
                              to_string(t));
                        return nullptr;
                    }
                    return t->get_elem();
                } else if (t->kind == TypeKind::Optional) {
                    return t->get_elem();
                } else if (t->kind == TypeKind::Box) {
                    return t->get_elem();
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
            if (scope.is_escaping) {
                auto decl = find_root_decl(data.op1);
                if (decl) {
                    decl->escape.escaped = decl->can_escape();
                }
                // If decl is null (e.g., function call result), nothing to mark as escaped
            }
            return get_pointer_type(t, data.op_type == TokenType::MUTREF ? TypeKind::MutRef
                                                                         : TypeKind::Reference);
        }
        default:
            unreachable();
        }
    invalid:
        error(data.op1, errors::INVALID_OPERATOR, get_token_symbol(data.op_type), to_string(t));
        return nullptr;
    }
    case NodeType::TryExpr: {
        auto &data = node->data.try_expr;
        auto expr_type = resolve(data.expr, scope);
        if (data.expr->type != NodeType::FnCallExpr) {
            error(data.expr, errors::TRY_NOT_CALL);
        }
        scope.parent_fn_def()->has_try = true;
        return get_result_type(expr_type, get_system_types()->error);
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
            error(node, "await requires a Promise type, got {}", to_string(expr_type));
            return get_system_types()->void_;
        }
        // Return the unwrapped value type
        return get_promise_value_type(expr_type);
    }
    case NodeType::CastExpr: {
        auto &data = node->data.cast_expr;
        auto dest_type = resolve_value(data.dest_type, scope);
        check_cast(node, resolve(data.expr, scope), dest_type);
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
            if (scope.value_type && scope.value_type->kind == TypeKind::Int) {
                return scope.value_type;
            }
            return get_system_types()->int_;
        case TokenType::STRING:
            return get_system_types()->string;
        case TokenType::FLOAT:
            return get_system_types()->float_;
        case TokenType::KW_UNDEFINED:
            return get_system_types()->undefined;
        case TokenType::CHAR:
            return get_system_types()->char_;
        default:
            unreachable();
        }
    }
    case NodeType::ReturnStmt: {
        auto &data = node->data.return_stmt;
        assert(scope.parent_fn);
        auto return_type = scope.parent_fn->data.fn.return_type;

        // For async functions, the value type for the expression is the inner Promise value type
        ChiType *value_type_hint = return_type;
        if (scope.parent_fn_def()->is_async() && is_promise_type(return_type)) {
            value_type_hint = get_promise_value_type(return_type);
        }

        auto expr_scope = scope.set_is_escaping(true).set_value_type(value_type_hint);
        auto expr_type = data.expr ? resolve(data.expr, expr_scope) : get_system_types()->void_;

        // For async functions returning Promise<T>, allow returning T directly
        ChiType *expected_type = return_type;
        if (scope.parent_fn_def()->is_async() && is_promise_type(return_type)) {
            expected_type = get_promise_value_type(return_type);
        }
        check_assignment(data.expr, expr_type, expected_type);
        return return_type;
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
        if (field_name.empty()) {
            return create_type(TypeKind::Unknown);
        }

        if (expr_type->kind == TypeKind::Fn) {
            expr_type = get_lambda_for_fn(expr_type);
        } else if (expr_type->kind == TypeKind::Module) {
            auto symbol = expr_type->data.module.scope->find_export(field_name);
            if (!symbol) {
                error(node, errors::MEMBER_NOT_FOUND, field_name, to_string(expr_type, true));
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
                          to_string(underlying_type, true));
                    return nullptr;
                }
                data.resolved_decl = member->node;
                data.field->node = member->node;
                data.resolved_dot_kind = DotKind::EnumVariant;
                return member->resolved_type;
            }
            case TypeKind::Struct: {
                auto member = underlying_type->data.struct_.find_static_member(field_name);
                if (!member) {
                    error(node, errors::MEMBER_NOT_FOUND, field_name,
                          to_string(underlying_type, true));
                    return nullptr;
                }
                data.resolved_decl = member->node;
                data.field->node = member->node;
                return member->resolved_type;
            }
            default:
                error(node, errors::MEMBER_NOT_FOUND, field_name, to_string(underlying_type, true));
                return nullptr;
            }
        }

        // Check if this is a placeholder type with trait bounds (or reference to one)
        auto check_type = expr_type;
        if (expr_type->is_pointer_like()) {
            check_type = expr_type->get_elem();
        }
        if (check_type->kind == TypeKind::Placeholder && check_type->data.placeholder.trait) {
            auto trait_type = check_type->data.placeholder.trait;
            if (trait_type->kind == TypeKind::Struct && ChiTypeStruct::is_interface(trait_type)) {
                auto trait_struct = &trait_type->data.struct_;
                auto member = trait_struct->find_member(field_name);
                if (member && member->is_method()) {
                    data.resolved_struct_member = member;
                    data.resolved_decl = member->node;
                    data.field->node = member->node;
                    data.resolved_dot_kind = DotKind::TypeTrait;
                    // Substitute ThisType with the concrete placeholder type
                    return substitute_this_type(member->resolved_type, expr_type);
                }
            }
            error(node, errors::MEMBER_NOT_FOUND, field_name, to_string(expr_type, true));
            return nullptr;
        }

        auto stype = eval_struct_type(expr_type);
        if (!stype) {
            error(node, errors::MEMBER_NOT_FOUND, field_name, to_string(expr_type, true));
            return nullptr;
        }
        auto is_internal = scope.parent_struct && is_friend_struct(scope.parent_struct, stype);
        auto member = get_struct_member_access(node, stype, field_name, is_internal, scope.is_lhs);
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
                return resolved_member->resolved_type;
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

        return member->resolved_type;
    }
    case NodeType::ConstructExpr: {
        auto &data = node->data.construct_expr;
        if (scope.move_outlet && !data.is_new) {
            data.resolved_outlet = scope.move_outlet;
            node->escape.moved = true;
        }
        ChiType *value_type;
        ChiType *result_type;
        if (data.type) {
            value_type = resolve_value(data.type, scope);
            result_type =
                data.is_new ? get_pointer_type(value_type, TypeKind::Reference) : value_type;
        } else {
            if (!scope.value_type) {
                error(node, errors::CONSTRUCT_CANNOT_INFER_TYPE);
                return nullptr;
            }
            result_type = scope.value_type;
            if (data.is_new != result_type->is_raw_pointer()) {
                error(node, errors::CONSTRUCT_CANNOT_INFER_TYPE);
            }
            value_type = data.is_new ? result_type->get_elem() : result_type;
        }
        auto struct_type = resolve_struct_type(value_type);
        auto constructor = struct_type ? struct_type->get_constructor() : nullptr;
        if (constructor) {
            auto &fn_type = constructor->resolved_type->data.fn;
            resolve_fn_call(node, scope, &fn_type, &data.items);
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
        }
        return result_type;
    }
    case NodeType::FnCallExpr: {
        auto &data = node->data.fn_call_expr;
        auto fn_ref_scope = scope.set_is_lhs(false).set_is_fn_call(true);
        auto fn_type = resolve(data.fn_ref_expr, fn_ref_scope);
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
        return resolve_fn_call(node, scope, &fn, &data.args);
    }
    case NodeType::IfStmt: {
        auto &data = node->data.if_stmt;
        auto cond_type = resolve(data.condition, scope);
        if (data.condition->type == NodeType::Identifier && cond_type->kind == TypeKind::Optional) {
            auto name = data.condition->token->get_name();
            auto expr = create_node(ast::NodeType::UnaryOpExpr);
            expr->token = data.condition->token;
            expr->data.unary_op_expr.is_suffix = true;
            expr->data.unary_op_expr.op_type = TokenType::LNOT;
            expr->data.unary_op_expr.op1 = data.condition;
            expr->resolved_type = cond_type->get_elem();
            auto var = get_dummy_var(name, expr);
            var->token = data.condition->token;
            var->parent_fn = scope.parent_fn_node;
            var->resolved_type = cond_type->get_elem();
            var->data.var_decl.initialized_at = node;
            auto &block_data = data.then_block->data.block;
            block_data.implicit_vars.add(var);
            block_data.scope->put(name, var);
        }
        check_assignment(data.condition, cond_type, get_system_types()->bool_);
        resolve(data.then_block, scope);
        if (data.else_node) {
            resolve(data.else_node, scope);
        }
        return nullptr;
    }
    case NodeType::WhileStmt: {
        auto &data = node->data.while_stmt;
        if (data.condition) {
            auto cond_type = resolve(data.condition, scope);
            check_assignment(data.condition, cond_type, get_system_types()->bool_);
        }
        auto loop_scope = scope.set_parent_loop(node);
        resolve(data.body, loop_scope);
        return nullptr;
    }
    case NodeType::Block: {
        auto &data = node->data.block;
        auto child_scope = scope.set_block(&data);
        for (auto stmt : data.statements) {
            resolve(stmt, child_scope);
        }
        if (data.return_expr) {
            return resolve(data.return_expr, child_scope);
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
                                          : m_ctx->system_types.int_;

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
            vstruct_data.add_member(
                allocator, "__display_name", get_dummy_var("__display_name"),
                get_pointer_type(get_system_types()->string, TypeKind::Reference));
            enum_type->data.enum_.enum_header_struct = vstruct;

            // clone the enum header to the value struct
            auto vstruct_clone = create_type(TypeKind::Struct);
            vstruct->clone(vstruct_clone);
            vstruct_clone->name = node->name;
            vstruct_clone->display_name = fmt::format("{}.BaseEnumValue", node->name);
            value_type->data.enum_value.resolved_struct = vstruct_clone;

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
                        base_value_struct->add_member(get_allocator(), base_member->get_name(),
                                                      copy_node, base_member->resolved_type);
                    }
                }

                if (data.base_struct) {
                    auto inner_scope = scope.set_parent_struct(enum_data.base_value_type);
                    _resolve(data.base_struct, inner_scope);

                    // copy members from custom base struct to enum value struct
                    auto base_struct = eval_struct_type(data.base_struct->resolved_type);
                    copy_struct_members(base_struct, value_struct);
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
                } else if (node->name == "__CxEnumBase") {
                    m_ctx->rt_enum_base = node;
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
            // second pass
            for (auto member : data.members) {
                resolve_struct_member(struct_type, member, struct_scope);
            }
            struct_->resolve_status = ResolveStatus::MemberTypesKnown;
        } else if (struct_->resolve_status == ResolveStatus::MemberTypesKnown) {
            // third pass
            for (auto member : data.members) {
                if (member->type == NodeType::VarDecl && member->data.var_decl.is_embed) {
                    resolve_struct_embed(struct_type, member, scope);
                }
            }

            for (auto implement : data.implements) {
                // Skip error nodes in implements list
                if (implement->type == NodeType::Error) {
                    continue;
                }

                auto impl_trait = resolve_value(implement, scope);
                auto trait_struct = resolve_struct_type(impl_trait);
                if (!ChiTypeStruct::is_interface(trait_struct)) {
                    error(implement, errors::NON_INTERFACE_IMPL_TYPE, to_string(impl_trait));
                }

                resolve_vtable(impl_trait, struct_type, implement);
                if (struct_->is_generic()) {
                    for (auto subtype : struct_->subtypes) {
                        if (subtype->is_placeholder) {
                            continue;
                        }
                        // For generic struct subtypes, we need to substitute the interface's
                        // type parameters to match the subtype's instantiation
                        ChiType *subtype_impl_trait = impl_trait;
                        if (impl_trait->kind == TypeKind::Subtype) {
                            // Substitute the struct's type parameters in the interface type
                            auto &subtype_data = subtype->data.subtype;
                            subtype_impl_trait = type_placeholders_sub(impl_trait, &subtype_data);
                        }
                        resolve_vtable(subtype_impl_trait, subtype, implement);
                    }
                }
            }
            struct_->resolve_status = ResolveStatus::EmbedsResolved;
        } else {
            // fourth pass
            for (auto member : data.members) {
                if (member->type == NodeType::FnDef) {
                    auto fn_type = node_get_type(member);
                    auto fn_scope = struct_scope.set_parent_fn(fn_type).set_parent_fn_node(member);
                    if (auto body = member->data.fn_def.body) {
                        resolve(body, fn_scope);
                    }
                }
            }

            for (auto member : data.members) {
                if (member->type == NodeType::VarDecl && !member->data.var_decl.initialized_at) {
                    auto not_needed = member->data.var_decl.is_embed &&
                                      (get_struct_member(struct_type, "new") ||
                                       struct_->kind == ContainerKind::Interface);
                    if (!not_needed) {
                        error(member, errors::UNINITIALIZED_FIELD, member->name,
                              to_string(struct_type));
                    }
                }
            }

            struct_->resolve_status = ResolveStatus::Done;
        }
        return type_sym;
    }
    case NodeType::SubtypeExpr: {
        auto &data = node->data.subtype_expr;
        auto type = to_value_type(resolve(data.type, scope));
        if (type->kind == TypeKind::Array || type->kind == TypeKind::Optional ||
            type->kind == TypeKind::Box) {
            if (data.args.len != 1) {
                error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS,
                      to_string(get_system_type(type->kind)), 1, data.args.len);
            }
            auto elem_type = to_value_type(resolve(data.args[0], scope));
            return get_wrapped_type(elem_type, type->kind);
        }
        auto &params = type->data.struct_.type_params;
        if (params.len != data.args.len) {
            error(node, errors::SUBTYPE_WRONG_NUMBER_OF_ARGS, to_string(type), params.len,
                  data.args.len);
            return nullptr;
        }
        array<ChiType *> args;
        for (auto arg : data.args) {
            args.add(resolve_value(arg, scope));
        }
        auto subtype = get_subtype(type, &args);
        return create_type_symbol({}, subtype);
    }
    case NodeType::IndexExpr: {
        auto &data = node->data.index_expr;
        auto expr_type = resolve(data.expr, scope);
        auto subscript_type = resolve(data.subscript, scope);
        if (!expr_type || !subscript_type) {
            return nullptr;
        }

        switch (expr_type->kind) {
        case TypeKind::Pointer:
            check_assignment(data.subscript, subscript_type, get_system_types()->int_);
            break;
        case TypeKind::Struct:
        case TypeKind::Subtype: {
            auto struct_ = resolve_struct_type(expr_type);
            auto has_index = has_interface_impl(struct_, "std.ops.Index");
            if (!has_index) {
                error(node, errors::CANNOT_SUBSCRIPT, to_string(expr_type));
                return nullptr;
            }

            auto method_p = struct_->member_table.get("index");
            assert(method_p);
            auto method = *method_p;

            auto index_type = method->resolved_type->data.fn.get_param_at(0);
            check_assignment(data.subscript, subscript_type, index_type);
            data.resolved_method = method;
            return method->resolved_type->data.fn.return_type->get_elem();
        }
        default:
            error(node, errors::CANNOT_SUBSCRIPT, to_string(expr_type));
            return nullptr;
        }
        return expr_type->get_elem();
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
        if (data.init) {
            resolve(data.init, scope);
        }
        if (data.condition) {
            auto cond_type = resolve(data.condition, scope);
            check_assignment(data.condition, cond_type, get_system_types()->bool_);
        }
        if (data.post) {
            resolve(data.post, scope);
        }
        if (data.expr) {
            auto expr_type = resolve(data.expr, scope);
            auto sty = resolve_struct_type(expr_type);
            if (!sty || !sty->member_intrinsics.get(IntrinsicSymbol::IndexInterable)) {
                error(node, errors::FOR_EXPR_NOT_ITERABLE, to_string(expr_type));
                return nullptr;
            }

            if (data.bind) {
                auto index_fn = sty->member_table.get("index");
                if (!index_fn) {
                    error(node, errors::CANNOT_INDEX, to_string(expr_type));
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

        if (data.type_bound) {
            phty->data.placeholder.trait = to_value_type(resolve(data.type_bound, scope));
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
            auto expr_type = resolve(data.expr, scope);
            if (!expr_type->is_raw_pointer()) {
                error(node, errors::INVALID_OPERATOR, data.prefix->to_string(),
                      to_string(expr_type, true));
            }
            return get_system_types()->void_;
        }
        case TokenType::KW_SIZEOF: {
            auto type = resolve_value(data.expr, scope);
            data.expr->resolved_type = type;
            return get_system_types()->int_;
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
        auto path_info = m_ctx->allocator->find_module_path(data.path->str, scope.module->path);
        if (!path_info) {
            error(node, errors::MODULE_NOT_FOUND, data.path->str);
            return nullptr;
        }

        if (path_info->is_directory && !path_info->entry_path.size()) {
            error(node, errors::MODULE_INDEX_NOT_FOUND, data.path->str);
            return nullptr;
        }

        auto target_package = m_ctx->allocator->get_or_create_package(path_info->package_id_path);
        auto path = path_info->entry_path;
        auto src = io::Buffer::from_file(path);
        auto module = m_ctx->allocator->process_source(target_package, &src, path);
        Resolver resolver(m_ctx);
        resolver.resolve(module);

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
                        assert(item_data.resolved_decl);
                        auto name = item_data.output_name();
                        scope.module->exports.add(item_data.resolved_decl);
                        scope.module->import_scope->put(name, item_data.resolved_decl);
                    }
                }
            }
        } else {
            scope.module->imports.add(module);
            if (data.alias) {
                for (auto item : module->exports) {
                    type->data.module.scope->put(item->name, item);
                }
            } else {
                for (auto symbol : data.symbols) {
                    auto item_type = resolve(symbol, scope);
                    auto &item_data = symbol->data.import_symbol;
                    assert(item_data.resolved_decl);
                    auto name = item_data.output_name();
                    type->data.module.scope->put(name, item_data.resolved_decl);
                }
            }
        }
        return type;
    }
    case NodeType::BindIdentifier: {
        // Add to cleanup_vars if this bind variable needs destruction (for-range by value)
        auto bind_type = scope.value_type;
        if (scope.parent_fn_node && should_destroy(node, bind_type) && !node->escape.is_capture()) {
            scope.parent_fn_def()->cleanup_vars.add(node);
        }
        return bind_type;
    }
    case NodeType::ImportSymbol: {
        auto &data = node->data.import_symbol;
        auto module = data.import->data.import_decl.resolved_module;
        auto decl = module->import_scope->find_one(data.name->get_name());
        if (!decl) {
            error(node, errors::SYMBOL_NOT_FOUND_MODULE, data.name->get_name(), module->path);
            return nullptr;
        }
        data.resolved_decl = decl;
        return decl->resolved_type;
    }
    case NodeType::SwitchExpr: {
        auto &data = node->data.switch_expr;
        if (scope.move_outlet) {
            data.resolved_outlet = scope.move_outlet;
            node->escape.moved = true;
        }
        auto expr_type = resolve(data.expr, scope);
        auto expr_comparator = resolve_comparator(expr_type, scope);

        ChiType *ret_type = scope.value_type;
        for (auto scase : data.cases) {
            auto case_type = resolve(scase, scope);

            if (!scase->data.case_expr.is_else) {
                for (auto clause : scase->data.case_expr.clauses) {
                    auto clause_type = resolve(clause, scope);
                    resolve_constant_value(clause);
                    auto clause_comparator = resolve_comparator(clause_type, scope);
                    check_assignment(clause, clause_comparator, expr_comparator);
                    if (!clause_comparator->is_int_like()) {
                        error(clause, errors::INVALID_SWITCH_TYPE, to_string(clause_type));
                    }
                }
            }

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
    default:
        print("\n");
        panic("unhandled node {}", PRINT_ENUM(node->type));
    }
    return nullptr;
}

ChiType *Resolver::resolve_comparator(ChiType *type, ResolveScope &scope) {
    switch (type->kind) {
    case TypeKind::This:
        return resolve_comparator(type->eval(), scope);
    case TypeKind::EnumValue:
        return type->data.enum_value.discriminator_type;
    case TypeKind::Reference:
    case TypeKind::Pointer:
    case TypeKind::MutRef:
        return resolve_comparator(type->get_elem(), scope);
    default:
        return type;
    }
}

ChiType *Resolver::resolve(ast::Node *node, ResolveScope &scope, uint32_t flags) {
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
    return fmt::format("{}.{}", node->module->global_id(), resolve_qualified_name(node));
}

bool Resolver::has_interface_impl(ChiTypeStruct *struct_type, string interface_id) {
    for (auto &i : struct_type->interfaces) {
        if (i->inteface_symbol == IntrinsicSymbol::Index) {
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
                return fmt::format("{}.{}", to_string(container, true), node->name);
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
        return fmt::format("{}.{}", fn_name, to_string(data.fn_subtype));
    }
    default:
        break;
    }
    return node->name;
}

string Resolver::to_string(ChiType *type, bool for_display) {
    assert(type);
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
        auto name = to_string(type, true);
        return fmt::format("Type:{}:{}", type->id, name);
    }

    switch (type->kind) {
    case TypeKind::This:
        return to_string(type->get_elem(), for_display);
    case TypeKind::Subtype: {
        auto &data = type->data.subtype;
        switch (data.generic->kind) {
        case cx::TypeKind::Fn:
        case cx::TypeKind::FnLambda:
            return to_string(data.final_type, for_display);
        default: {
            std::stringstream ss;
            ss << to_string(data.generic, for_display) << "<";
            for (int i = 0; i < data.args.len; i++) {
                ss << to_string(data.args[i], for_display);
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
        return "*" + to_string(type->get_elem(), for_display);
    case TypeKind::Reference:
        return "&" + to_string(type->get_elem(), for_display);
    case TypeKind::MutRef:
        return "&mut<" + to_string(type->get_elem(), for_display) + ">";
    case TypeKind::Optional:
        return "?" + to_string(type->get_elem(), for_display);
    case TypeKind::Box:
        return "^" + to_string(type->get_elem(), for_display);
    case TypeKind::Array:
        return fmt::format("Array<{}>", to_string(type->get_elem(), for_display));
    case TypeKind::Result:
        return fmt::format("Result<{},{}>", to_string(type->get_elem(), for_display),
                           to_string(type->data.result.error, for_display));
    case TypeKind::Unknown:
        return "unknown";
    case TypeKind::Undefined:
        return "undefined";
    default:
        break;
    }
    assert(type->kind < TypeKind::__COUNT);
    return to_string(type->kind, &type->data, for_display);
}

string Resolver::to_string(TypeList *type_list, bool for_display) {
    std::stringstream ss;
    for (int i = 0; i < type_list->len; i++) {
        ss << to_string(type_list->at(i), for_display);
        if (i < type_list->len - 1) {
            ss << ",";
        }
    }
    return ss.str();
}

string Resolver::to_string(TypeKind kind, ChiType::Data *data, bool for_display) {
    switch (kind) {
    case TypeKind::Struct: {
        auto &struct_ = data->struct_;
        std::stringstream ss;
        ss << "struct ";
        if (struct_.type_params.len > 0) {
            ss << "<";
            for (int i = 0; i < struct_.type_params.len; i++) {
                ss << to_string(struct_.type_params[i], for_display);
                if (i < struct_.type_params.len - 1) {
                    ss << ",";
                }
            }
            ss << ">";
        }
        ss << "{";
        for (int i = 0; i < struct_.members.len; i++) {
            auto &member = struct_.members[i];
            ss << to_string(member->resolved_type, for_display);
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
        if (fn.container_ref) {
            ss << "(";
            ss << to_string(fn.container_ref, for_display);
            ss << ") ";
        }
        ss << "func(";
        for (int i = 0; i < fn.params.len; i++) {
            if (fn.is_variadic && i == fn.params.len - 1) {
                ss << "...";
            }
            ss << to_string(fn.params[i], for_display);
            if (i < fn.params.len - 1) {
                ss << ",";
            }
        }
        ss << ")";
        if (fn.return_type) {
            ss << " " << to_string(fn.return_type, for_display);
        }
        return ss.str();
    }
    case TypeKind::FnLambda: {
        auto &fn_lambda = data->fn_lambda;
        return fmt::format("Lambda<{}>", to_string(fn_lambda.fn, for_display));
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
                ss << "{" << to_string(enum_value.variant_struct, for_display) << "}";
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

    if (!can_assign(from_type, to_type, is_explicit)) {
        if (!is_explicit) {
            auto can_convert_explitcitly = can_assign(from_type, to_type, true);
            if (can_convert_explitcitly) {
                error(value, errors::CANNOT_CONVERT_IMPLICIT, to_string(from_type, true),
                      to_string(to_type, true));
                return;
            }
        }
        error(value, errors::CANNOT_CONVERT, to_string(from_type, true), to_string(to_type, true));
    }
}

bool Resolver::is_addressable(ast::Node *node) {
    switch (node->type) {
    case NodeType::Identifier:
    case NodeType::DotExpr:
    case NodeType::IndexExpr:
        return true;

    case NodeType::ParenExpr:
        return is_addressable(node->data.child_expr);

    case NodeType::UnaryOpExpr: {
        auto &data = node->data.unary_op_expr;
        return data.is_suffix && data.op_type == TokenType::LNOT;
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

void Resolver::resolve_vtable(ChiType *base_type, ChiType *derived_type, ast::Node *base_node) {
    auto &base = *resolve_struct_type(base_type);
    auto &derived = *resolve_struct_type(derived_type);
    InterfaceImpl *iface_impl = nullptr;
    auto trait_symbol = resolve_intrinsic_symbol(base.node);
    if (base.kind == ContainerKind::Interface) {
        iface_impl = derived.add_interface(get_allocator(), base_type, derived_type);
    }

    for (auto &base_member : base.members) {
        auto node = base_member->node;
        if (base_member->is_method()) {
            auto child_method = derived.find_member(node->name);

            if (node->data.fn_def.body) {
                if (!child_method) {
                    child_method = derived.add_member(get_allocator(), node->name, node,
                                                      base_member->resolved_type);
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

            if (!compare_impl_type(base_member_type, child_method->resolved_type)) {
                error(base_node, errors::IMPLEMENT_NOT_MATCH, node->name,
                      to_string(base_type, true));
                break;
            }
            if (iface_impl) {
                assert(child_method);
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
                                                 base_member->resolved_type);
                child_field->orig_parent = base_type;
                child_field->parent_member = base_node->data.var_decl.resolved_field;
                child_field->field_index = base_member->field_index;
            }
        }
    }
    for (auto &impl : base.interfaces) {
        resolve_vtable(impl->interface_type, derived_type, base_node);
    }
}

void Resolver::resolve_struct_embed(ChiType *struct_type, ast::Node *base_node,
                                    ResolveScope &parent_scope) {
    auto &current = struct_type->data.struct_;
    auto em_type = node_get_type(base_node);
    if (em_type->kind != TypeKind::Struct) {
        error(base_node, errors::INVALID_EMBED);
        return;
    }
    auto &base = em_type->data.struct_;
    if (base.kind != ContainerKind::Struct && base.kind != ContainerKind::Interface) {
        error(base_node, errors::INVALID_EMBED);
        return;
    }
    if (current.kind != base.kind) {
        error(base_node, errors::CANNOT_EMBED_INTO, to_string(em_type, true),
              to_string(struct_type, true));
    }
    if (base.resolve_status < ResolveStatus::Done) {
        _resolve(base.node, parent_scope);
    }

    // Add to embeds array if this is an interface
    if (current.kind == ContainerKind::Interface) {
        current.embeds.add(em_type);

        // Copy methods from embedded interface to current interface
        for (auto embed_member : base.members) {
            if (embed_member->is_method()) {
                // Check if method already exists in current interface
                auto existing_member = current.find_member(embed_member->get_name());
                if (!existing_member) {
                    // Add the method to current interface
                    auto copied_member =
                        current.add_member(get_allocator(), embed_member->get_name(),
                                           embed_member->node, embed_member->resolved_type);
                    copied_member->orig_parent = em_type;
                    copied_member->symbol = embed_member->symbol;

                    // Add to member_intrinsics if it has an intrinsic symbol
                    if (copied_member->symbol != IntrinsicSymbol::None) {
                        current.member_intrinsics[copied_member->symbol] = copied_member;
                    }
                }
            }
        }
    }

    // Only resolve vtable for struct implementations, not interface embeds
    if (current.kind != ContainerKind::Interface) {
        resolve_vtable(em_type, struct_type, base_node);
    }
}

// Check if a type needs destruction (has destructor or has fields that need destruction)
bool Resolver::type_needs_destruction(ChiType *type) {
    if (!type) return false;

    // Strings need destruction
    if (type->kind == TypeKind::String) return true;

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

    // Only structs can have destructors or fields needing destruction
    if (type->kind != TypeKind::Struct) return false;

    // Has custom destructor
    if (get_struct_member(type, "delete")) return true;

    // Check if any field needs destruction
    auto &fields = type->data.struct_.fields;
    for (auto field : fields) {
        if (type_needs_destruction(field->resolved_type)) {
            return true;
        }
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
    switch (type->kind) {
    case TypeKind::Placeholder:
        return handle_placeholder(type, subs);

    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::Optional:
    case TypeKind::Box:
    case TypeKind::Array: {
        auto elem_type = make_recursive_call(type->get_elem(), subs);
        return get_wrapped_type(elem_type, type->kind);
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
        fn_type->data.fn.container_ref = data.container_ref;
        fn_type->data.fn.is_extern = data.is_extern;

        // Mark as placeholder if any preserved type parameters are actually placeholders
        fn_type->is_placeholder = false;
        for (auto type_param : fn_type->data.fn.type_params) {
            if (type_param->is_placeholder) {
                fn_type->is_placeholder = true;
                break;
            }
        }
        fn_type->is_placeholder = fn_type->is_placeholder || ret->is_placeholder;

        return fn_type;
    }

    case TypeKind::FnLambda: {
        auto &data = type->data.fn_lambda;
        auto fn = make_recursive_call(data.fn, subs);
        auto internal = make_recursive_call(data.internal, subs);
        auto bound_fn = data.bound_fn ? make_recursive_call(data.bound_fn, subs) : nullptr;
        auto bind_struct = data.bind_struct ? make_recursive_call(data.bind_struct, subs) : nullptr;

        auto lambda_type = create_type(TypeKind::FnLambda);
        lambda_type->data.fn_lambda.fn = fn;
        lambda_type->data.fn_lambda.internal = internal;
        lambda_type->data.fn_lambda.bound_fn = bound_fn;
        lambda_type->data.fn_lambda.bind_struct = bind_struct;
        lambda_type->is_placeholder = fn->is_placeholder;
        return lambda_type;
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
        return substitution ? *substitution : type;
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
    case TypeKind::Placeholder:
        return handle_placeholder(param_type, arg_type);

    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::Optional:
    case TypeKind::Box:
    case TypeKind::Array: {
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
        if (arg_type->kind != TypeKind::Fn) {
            return false;
        }
        auto &param_data = param_type->data.fn;
        auto &arg_data = arg_type->data.fn;

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

    // Use visitor pattern to unify each parameter with its argument
    for (size_t i = 0; i < fn->params.len; i++) {
        ChiType *param_type = fn->params[i];
        ChiType *arg_type = (*arg_types)[i];

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

        // Recursive unification using visitor pattern
        auto make_recursive_call = [this, fn, inferred_types](ChiType *param,
                                                              ChiType *arg) -> bool {
            return this->visit_type_recursive(
                param, arg,
                [fn, inferred_types](ChiType *placeholder, ChiType *concrete) -> bool {
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
                },
                [this, fn, inferred_types](ChiType *param, ChiType *arg) -> bool {
                    return this->visit_type_recursive(
                        param, arg,
                        [fn, inferred_types](ChiType *placeholder, ChiType *concrete) -> bool {
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
                        },
                        [](ChiType *param, ChiType *arg) -> bool {
                            return param == arg ||
                                   (param->kind == arg->kind && param->global_id == arg->global_id);
                        });
                });
        };

        // Attempt to unify this parameter with its argument
        if (!visit_type_recursive(param_type, arg_type, handle_placeholder, make_recursive_call)) {
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

bool Resolver::is_struct_type(ChiType *type) {
    switch (type->kind) {
    case TypeKind::This:
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
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
    auto sty = type;
    if (sty->kind == TypeKind::This) {
        sty = sty->get_elem();
    }
    if (sty->is_pointer_like()) {
        sty = sty->get_elem();
    }
    if (sty->kind == TypeKind::Array) {
        if (!sty->data.array.internal) {
            auto array = m_ctx->rt_array_type;
            assert(array);
            auto sub = create_type(TypeKind::Subtype);
            sub->data.subtype.generic = to_value_type(array);
            sub->data.subtype.args.add(sty->data.array.elem);
            sub->is_placeholder = sty->data.array.elem->is_placeholder;
            sub->global_id = sty->global_id;
            auto astype = resolve_subtype(sub);
            sty->data.array.internal = astype;
            sty = astype;
        } else {
            sty = sty->data.array.internal;
        }
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
        auto new_member = to->data.struct_.add_member(get_allocator(), member->get_name(),
                                                      member->node, member->resolved_type);
        if (parent_member && member->is_field()) {
            new_member->parent_member = parent_member;
            new_member->field_index = member->field_index;
        }
    }
}

bool Resolver::is_struct_access_mutable(ChiType *type) {
    auto sty = type;
    if (sty->kind == TypeKind::This) {
        return is_struct_access_mutable(sty->get_elem());
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
                                                    bool is_write) {
    auto field_member = get_struct_member(struct_type, field_name);
    if (!field_member) {
        error(node, errors::MEMBER_NOT_FOUND, field_name, to_string(struct_type, true));
        return nullptr;
    }
    if (is_write && !is_struct_access_mutable(struct_type)) {
        error(node, errors::CANNOT_MODIFY_IMMUTABLE_REFERENCE, to_string(struct_type, true));
        return nullptr;
    }
    if (!field_member->check_access(is_internal, is_write)) {
        if (field_member->get_visibility() == Visibility::Protected) {
            error(node, errors::PROTECTED_MEMBER_NOT_WRITABLE, field_name,
                  to_string(struct_type, true));
        } else {
            error(node, errors::PRIVATE_MEMBER_NOT_ACCESSIBLE, field_name,
                  to_string(struct_type, true));
        }
        return nullptr;
    }

    if (field_member->is_method()) {
        auto is_mutable = field_member->node->declspec_ref().is_mutable();
        if (is_mutable && !is_struct_access_mutable(struct_type)) {
            error(node, errors::MUTATING_METHOD_ON_IMMUTABLE_REFERENCE, field_name,
                  to_string(struct_type, true));
            return nullptr;
        }
    }
    return field_member;
}

bool Resolver::is_friend_struct(ChiType *a, ChiType *b) {
    if (a->kind == TypeKind::Array || b->kind == TypeKind::Array) {
        return b->kind == a->kind;
    }
    auto a_sty = resolve_struct_type(a);
    auto b_sty = resolve_struct_type(b);
    return a_sty->global_id == b_sty->global_id;
}

ChiType *Resolver::resolve_fn_call(ast::Node *node, ResolveScope &scope, ChiTypeFn *fn,
                                   NodeList *args) {
    auto n_args = args->len;
    auto n_params = fn->params.len;
    auto params_required = n_params - (fn->is_variadic ? 1 : 0);
    bool ok = fn->is_variadic ? n_args >= params_required : n_args == n_params;
    if (!ok) {
        error(node, errors::CALL_WRONG_NUMBER_OF_ARGS, params_required, n_args);
        return fn->return_type;
    }

    // Debug: check type params
    // Check if this is a generic function call that needs explicit type parameters
    if (fn->is_generic()) {
        // Resolve arguments first to get their types
        // Clear move_outlet for function args - they're passed by value
        auto arg_scope = scope.set_move_outlet(nullptr);
        array<ChiType *> arg_types;
        for (size_t i = 0; i < n_args; i++) {
            auto arg = args->at(i);
            auto arg_type = resolve(arg, arg_scope);
            arg_types.add(arg_type);
        }

        // Get explicit type parameters from function call
        array<ChiType *> type_args;
        if (node->type == ast::NodeType::FnCallExpr) {
            auto &fn_call_data = node->data.fn_call_expr;

            // Check if explicit type parameters were provided
            if (fn_call_data.type_args.len == 0) {
                // Try automatic type inference
                map<ChiType *, ChiType *> inferred_types;
                if (!infer_type_arguments(fn, &arg_types, &inferred_types)) {
                    error(node, "Failed to infer type parameters for generic function call");
                    return fn->return_type;
                }

                // Convert inferred types to type_args array for compatibility
                for (auto type_param : fn->type_params) {
                    auto inferred = inferred_types.get(type_param);
                    if (!inferred) {
                        error(node, "Could not infer type parameter");
                        return fn->return_type;
                    }
                    type_args.add(*inferred);
                }
            } else {
                // Check if the number of type parameters matches
                if (fn_call_data.type_args.len != fn->type_params.len) {
                    error(node, "Wrong number of type parameters: expected {}, got {}",
                          fn->type_params.len, fn_call_data.type_args.len);
                    return fn->return_type;
                }

                // Resolve the explicit type parameters
                for (auto type_arg_node : fn_call_data.type_args) {
                    auto type_arg = resolve_value(type_arg_node, scope);
                    type_args.add(type_arg);
                }
            }
        } else {
            // For non-FnCallExpr (like constructors), this shouldn't happen with new logic
            error(node, "Generic function call requires explicit type parameters");
            return fn->return_type;
        }

        // Create type substitution map for parameter type checking
        map<ChiType *, ChiType *> type_substitutions;
        for (size_t i = 0; i < fn->type_params.len; i++) {
            auto type_param = fn->type_params[i];
            auto lookup_key = type_param;
            if (type_param->kind == TypeKind::TypeSymbol) {
                lookup_key = type_param->data.type_symbol.giving_type;
            }
            type_substitutions.emplace(lookup_key, type_args[i]);

            // Check that type argument satisfies trait bounds
            if (lookup_key->kind == TypeKind::Placeholder && lookup_key->data.placeholder.trait) {
                auto trait_type = lookup_key->data.placeholder.trait;
                auto type_arg = type_args[i];

                // Check if type_arg implements the required trait
                bool satisfies_bound = false;

                // Check if it's a struct that implements the interface
                if (type_arg->kind == TypeKind::Struct) {
                    for (auto &impl : type_arg->data.struct_.interfaces) {
                        if (impl->interface_type == trait_type) {
                            satisfies_bound = true;
                            break;
                        }

                        // Check if this implemented interface satisfies the required trait
                        // (including embeds)
                        if (interface_satisfies_trait(impl->interface_type, trait_type)) {
                            satisfies_bound = true;
                            break;
                        }
                    }
                }
                // Check if it's a built-in type that naturally supports the trait
                else {
                    // Get all intrinsics required by the trait
                    auto required_intrinsics = interface_get_intrinsics(trait_type);

                    // For built-in types, check if they naturally support the required operations
                    for (auto &intrinsic : required_intrinsics) {
                        if (intrinsic == IntrinsicSymbol::Add) {
                            // Built-in types that support the + operator satisfy ops.Add
                            satisfies_bound = type_arg->is_int_like() ||
                                              type_arg->kind == TypeKind::Float ||
                                              type_arg->kind == TypeKind::String;
                            break;
                        }
                    }
                    // Add more intrinsic checks here as needed
                }

                if (!satisfies_bound) {
                    error(node, "Type '{}' does not satisfy trait bound '{}'", to_string(type_arg),
                          to_string(trait_type, true));
                    return fn->return_type;
                }
            }
        }

        // Create or get the specialized function type
        // Get the original function declaration node to access its function type
        auto &fn_call_data = node->data.fn_call_expr;
        auto fn_decl_node = fn_call_data.fn_ref_expr->get_decl();
        assert(fn_decl_node && fn_decl_node->type == ast::NodeType::FnDef);
        auto original_fn_type = node_get_type(fn_decl_node);
        assert(original_fn_type && original_fn_type->kind == TypeKind::Fn);

        auto fn_variant = get_fn_variant(original_fn_type, &type_args, fn_decl_node);
        auto generated_fn_type = fn_variant->data.generated_fn.fn_subtype;

        // Store the specialized function for codegen to use
        assert(node->type == ast::NodeType::FnCallExpr);
        node->data.fn_call_expr.generated_fn = fn_variant;

        // Now check types with the explicit concrete types
        for (size_t i = 0; i < n_args; i++) {
            auto param_type = fn->get_param_at(i);
            auto arg_type = arg_types[i];

            // Substitute placeholders with explicit types
            auto concrete_param_type = type_placeholders_sub_map(param_type, &type_substitutions);
            check_assignment(args->at(i), arg_type, concrete_param_type);
        }

        return fn_variant->resolved_type->data.fn.return_type;
    } else {
        // Regular function call - check types normally
        for (size_t i = 0; i < n_args; i++) {
            auto param_type = fn->get_param_at(i);
            auto arg = args->at(i);
            // Clear move_outlet for function args - they're passed by value,
            // not written directly to any outer destination
            auto arg_scope = scope.set_value_type(param_type).set_move_outlet(nullptr);
            auto arg_type = resolve(arg, arg_scope);

            check_assignment(arg, arg_type, param_type);
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
    // Use to_string for element type since elem->global_id may be empty for anonymous types like lambdas
    auto elem_str = elem->global_id.empty() ? to_string(elem) : elem->global_id;
    type->global_id = fmt::format("runtime.Array<{}>", elem_str);
    return type;
}

ChiType *Resolver::get_promise_type(ChiType *value) {
    if (auto cached = m_ctx->promise_of.get(value)) {
        return *cached;
    }

    // Use the Chi-native Promise<T> struct from runtime.xc
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

    auto key = to_string(TypeKind::Fn, (ChiType::Data *)&fn);
    if (auto cached = m_ctx->composite_types.get(key)) {
        return *cached;
    }
    auto type = create_type(TypeKind::Fn);
    type->data.fn = fn;
    m_ctx->composite_types[key] = type;
    for (auto param : fn.params) {
        if (param->is_placeholder) {
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

void Resolver::finalize_placeholder_lambda_params(ChiType *fn_type) {
    if (!fn_type || fn_type->kind != TypeKind::Fn) {
        return;
    }

    bool had_placeholder = false;

    // Walk through all function parameters and finalize placeholder lambdas
    for (size_t i = 0; i < fn_type->data.fn.params.len; i++) {
        auto param = fn_type->data.fn.params[i];
        if (param->kind == TypeKind::FnLambda && param->is_placeholder && !param->data.fn_lambda.internal) {
            // This is a placeholder lambda - finalize it now
            auto bind_struct = param->data.fn_lambda.bind_struct;
            if (!bind_struct) continue;

            auto rt_lambda = m_ctx->rt_lambda_type;
            if (!rt_lambda || rt_lambda->data.struct_.resolve_status < ResolveStatus::MemberTypesKnown) {
                continue; // Still can't finalize
            }

            // Instantiate __CxLambda<BindStruct>
            TypeList type_args;
            type_args.add(bind_struct);
            param->data.fn_lambda.internal = to_value_type(get_subtype(rt_lambda, &type_args));
            param->is_placeholder = false;
            had_placeholder = true;
        }
    }

    // If we finalized any placeholders, clear the function's placeholder flag
    if (had_placeholder) {
        fn_type->is_placeholder = false;
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
            m_ctx->rt_empty_bind_type->data.struct_.resolve_status = ResolveStatus::MemberTypesKnown;
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
        // This happens when resolving runtime.xc itself, before __CxLambda is fully resolved
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
        value = get_system_types()->bool_;
    }

    auto key = fmt::format("Result<{},{}>", to_string(value), to_string(err));
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
    struct_.add_member(get_allocator(), "err", dummy_node, err_optional);
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
        case TokenType::BOOL: {
            return {(int64_t)token->val.b};
        }
        case TokenType::NULLP: {
            return {(int64_t)0};
        }
        case TokenType::KW_UNDEFINED:
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
        if (data.is_const && data.resolved_value.has_value()) {
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
            a->kind == TypeKind::MutRef || a->kind == TypeKind::Optional ||
            a->kind == TypeKind::Box) {
            return is_same_type(a->get_elem(), b->get_elem());
        }
    }

    return false;
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
    sub->global_id = fmt::format("{}<{}>", gen.global_id, to_string(type_args));
    for (auto arg : *type_args) {
        sub->data.subtype.args.add(arg);
        if (arg->is_placeholder) {
            sub->is_placeholder = true;
        }
    }
    gen.subtypes.add(sub);
    if (gen.resolve_status >= ResolveStatus::MemberTypesKnown) {
        resolve_subtype(sub);
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

    auto &base = data.generic->data.struct_;
    auto sty = create_type(TypeKind::Struct);
    sty->name = to_string(subtype);
    sty->global_id = subtype->global_id;

    auto &scpy = sty->data.struct_;
    scpy.kind = base.kind;
    scpy.node = base.node;
    scpy.display_name = sty->name;
    auto base_symbol = resolve_intrinsic_symbol(base.node);

    for (auto member : base.members) {
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

        if (member->get_name() == "filter" &&
            to_string(type) == "(&Array<T>) func(Lambda<func(T) bool>) Array<T>") {
        }

        auto new_member = scpy.add_member(get_allocator(), member->get_name(), node, type);
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
    scpy.interfaces = base.interfaces;
    return sty;
}

void Resolver::check_binary_op(ast::Node *node, TokenType op_type, ChiType *type) {
    if (is_assignment_op(op_type)) {
        return;
    }
    if (node->type == ast::NodeType::BinOpExpr && node->data.bin_op_expr.resolved_call) {
        return;
    }

    bool ok;
    switch (op_type) {
    case TokenType::ADD:
        ok = type->is_int_like() || type->kind == TypeKind::Float || type->kind == TypeKind::String;
        break;
    default:
        ok = type->is_int_like() || type->kind == TypeKind::Float;
        break;
    }

    // Handle placeholder types with appropriate trait bounds
    if (!ok && type->kind == TypeKind::Placeholder && type->data.placeholder.trait) {
        auto trait_type = type->data.placeholder.trait;
        if (trait_type->kind == TypeKind::Struct && ChiTypeStruct::is_interface(trait_type)) {
            // Get all intrinsics supported by this interface
            auto intrinsics = interface_get_intrinsics(trait_type);

            // Map operator to required intrinsic symbol
            IntrinsicSymbol required_symbol = get_operator_intrinsic_symbol(op_type);

            // Check if the interface supports the required intrinsic
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

    if (!ok) {
        error(node, errors::INVALID_OPERATOR, get_token_symbol(op_type), to_string(type, true));
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
    case TypeKind::Optional:
        return types->optional;
    case TypeKind::Box:
        return types->box;
    case TypeKind::Bool:
        return types->bool_;
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
    case TypeKind::Optional:
    case TypeKind::Box:
        return get_pointer_type(elem, kind);
    case TypeKind::Array:
        return get_array_type(elem);
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
    case ast::SigilKind::Optional:
        return TypeKind::Optional;
    case ast::SigilKind::Box:
        return TypeKind::Box;
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
    case NodeType::FnCallExpr:
        // Function call results are temporaries, return nullptr to indicate no root decl
        return nullptr;
    default:
        panic("unhandled find_root_decl {}", PRINT_ENUM(node->type));
    }
    return nullptr;
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
        if (builtin->type == NodeType::Primitive) {
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
                if (fn_data.params.len == 1 && can_assign(t2, fn_data.params[0])) {
                    return_type = fn_data.return_type;
                }
            }
        }
    }

    // Try placeholder type with trait bounds
    if (!method_member && t1->kind == TypeKind::Placeholder && t1->data.placeholder.trait) {
        auto trait_type = t1->data.placeholder.trait;
        if (trait_type->kind == TypeKind::Struct && ChiTypeStruct::is_interface(trait_type)) {
            auto member_p = trait_type->data.struct_.member_intrinsics.get(symbol);
            if (member_p && (*member_p)->is_method()) {
                method_member = *member_p;
                auto method_type = method_member->resolved_type;
                if (method_type && method_type->kind == TypeKind::Fn) {
                    auto &fn_data = method_type->data.fn;
                    if (fn_data.params.len == 1) {
                        return_type = t1; // Return placeholder type for trait bounds
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
        call_data.args = {op2};
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
