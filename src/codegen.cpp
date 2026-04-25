#include "codegen.h"
#include "ast.h"
#include "context.h"
#include "enum.h"
#include "fmt/core.h"
#include "resolver.h"
#include "sema.h"
#include "util.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Instrumentation/AddressSanitizer.h"
#include <set>

namespace cx {
namespace codegen {

typedef llvm::ArrayRef<llvm::Type *> TypeArray;

static ast::Node *find_imported_module_member(ast::Module *module, const string &module_id,
                                              const string &member_name) {
    auto module_matches = [&](ast::Module *candidate) {
        if (!candidate)
            return false;
        auto gid = candidate->global_id();
        if (gid == module_id)
            return true;
        auto dotted = "." + module_id;
        return gid.size() > dotted.size() &&
               gid.compare(gid.size() - dotted.size(), dotted.size(), dotted) == 0;
    };

    if (!module)
        return nullptr;
    if (module_matches(module) && module->scope) {
        return module->scope->find_one(member_name);
    }
    for (auto *imported : module->imports) {
        if (!imported || !imported->scope)
            continue;
        if (module_matches(imported)) {
            return imported->scope->find_one(member_name);
        }
    }
    return nullptr;
}

static ast::Node *find_gc_allocator_decl(ast::Module *module) {
    return find_imported_module_member(module, "mem", "GC_ALLOCATOR");
}

static ast::Node *unwrap_cast_exprs(ast::Node *node) {
    while (node && node->type == ast::NodeType::CastExpr) {
        node = node->data.cast_expr.expr;
    }
    return node;
}

static bool is_ref_unary_op(TokenType op_type) {
    return op_type == TokenType::AND || op_type == TokenType::MUTREF ||
           op_type == TokenType::MOVEREF;
}

void Compiler::emit_default_field_initializer(Function *fn, llvm::Value *dest,
                                              ChiType *container_type,
                                              ChiStructMember *field) {
    if (!field || !field->node) {
        return;
    }
    auto &var_decl = field->node->data.var_decl;
    auto default_expr = var_decl.expr;
    if (default_expr) {
        auto field_gep = compile_dot_access(fn, dest, container_type, field);
        compile_assignment_to_ptr(fn, default_expr, field_gep, field->resolved_type);
        return;
    }

    // Embedded struct field: recursively initialize via its own constructor.
    if (var_decl.is_embed && var_decl.is_field) {
        auto field_gep = compile_dot_access(fn, dest, container_type, field);
        emit_construct_init(fn, field_gep, field->resolved_type);
    }
}

bool Compiler::struct_needs_managed_constructor(ChiType *struct_type,
                                                std::set<ChiType *> &visiting) {
    auto resolved_type = get_resolver()->eval_struct_type(struct_type);
    if (!resolved_type || resolved_type->kind != TypeKind::Struct) {
        return false;
    }

    if (auto cached = m_ctx->managed_constructor_needed_table.get(resolved_type)) {
        return *cached;
    }
    if (visiting.count(resolved_type)) {
        return false;
    }

    visiting.insert(resolved_type);
    bool needs_managed_ctor = false;
    for (auto field : resolved_type->data.struct_.fields) {
        if (!field->node) {
            continue;
        }
        auto *default_expr = field->node->data.var_decl.expr;
        if (!default_expr || default_expr->type != ast::NodeType::ConstructExpr) {
            continue;
        }

        auto nested_type = get_resolver()->eval_struct_type(default_expr->resolved_type);
        if (!nested_type || nested_type->kind != TypeKind::Struct) {
            continue;
        }

        if (nested_type->data.struct_.member_intrinsics.get(IntrinsicSymbol::AllocInit) ||
            struct_needs_managed_constructor(nested_type, visiting)) {
            needs_managed_ctor = true;
            break;
        }
    }

    visiting.erase(resolved_type);
    m_ctx->managed_constructor_needed_table[resolved_type] = needs_managed_ctor;
    return needs_managed_ctor;
}

bool Compiler::type_needs_managed_constructor(ChiType *type) {
    std::set<ChiType *> visiting;
    return struct_needs_managed_constructor(type, visiting);
}

ast::Module *Compiler::get_codegen_context_module(Function *fn, ast::Module *fallback) {
    auto *module = fn ? fn->module : fallback;
    if (!module && fn && fn->node) {
        module = fn->node->module;
    }
    return module;
}

bool Compiler::should_use_managed_constructor_variant(Function *fn, ast::Module *context_module,
                                                      ChiType *type) {
    auto managed_module = get_codegen_context_module(fn, context_module);
    return managed_module && has_lang_flag(managed_module->get_lang_flags(), LANG_FLAG_MANAGED) &&
           type_needs_managed_constructor(type);
}

void Compiler::emit_alloc_init(Function *fn, llvm::Value *dest, ChiType *struct_type) {
    auto *alloc_init_member_p =
        struct_type->data.struct_.member_intrinsics.get(IntrinsicSymbol::AllocInit);
    if (!alloc_init_member_p) {
        return;
    }

    auto managed_module = get_codegen_context_module(fn);
    if (!managed_module || !has_lang_flag(managed_module->get_lang_flags(), LANG_FLAG_MANAGED)) {
        return;
    }

    auto allocator_module = struct_type->data.struct_.node ? struct_type->data.struct_.node->module
                                                           : nullptr;
    if (!allocator_module) {
        allocator_module = managed_module;
    }

    auto allocator_node = find_gc_allocator_decl(allocator_module);
    assert(allocator_node && "GC_ALLOCATOR not found");

    auto alloc_init_node = get_variant_member_node(*alloc_init_member_p, std::nullopt);
    auto alloc_init_type = get_chitype(alloc_init_node);
    auto alloc_init_id = get_resolver()->resolve_global_id(alloc_init_node);
    auto alloc_init_entry = m_ctx->function_table.get(alloc_init_id);
    assert(alloc_init_entry && "alloc_init method not compiled");

    auto alloc_init_fn = *alloc_init_entry;
    auto alloc_init_type_l = (llvm::FunctionType *)compile_type(alloc_init_type);
    auto allocator_ref = compile_expr_ref(fn, allocator_node);
    auto allocator_param_type = alloc_init_type->data.fn.get_param_at(0);
    assert(allocator_param_type && "alloc_init allocator param type missing");
    auto concrete_ref_type =
        get_resolver()->get_pointer_type(get_chitype(allocator_node), TypeKind::Reference);
    auto allocator_arg = compile_conversion(fn, allocator_ref.address, concrete_ref_type,
                                            allocator_param_type);
    m_ctx->llvm_builder->CreateCall(alloc_init_type_l, alloc_init_fn->llvm_fn,
                                    {dest, allocator_arg});
}

void Compiler::emit_construct_init(Function *fn, llvm::Value *dest, ChiType *type,
                                   ast::Module *context_module) {
    auto struct_type = get_resolver()->eval_struct_type(type);
    if (!struct_type || struct_type->kind != TypeKind::Struct) {
        return;
    }

    auto managed_module = get_codegen_context_module(fn, context_module);
    bool use_managed_ctor =
        should_use_managed_constructor_variant(fn, context_module, struct_type);

    auto generated_ctor = generate_constructor(struct_type, nullptr, use_managed_ctor,
                                               managed_module);
    if (generated_ctor) {
        auto &builder = *m_ctx->llvm_builder;
        // The call must carry a !dbg if the surrounding function has debug
        // info. Synthesize a zero-line location anchored to the current
        // function scope when the caller hasn't set one.
        auto saved_dbg = builder.getCurrentDebugLocation();
        if (!saved_dbg && m_ctx->dbg_scopes.size()) {
            builder.SetCurrentDebugLocation(
                llvm::DILocation::get(*m_ctx->llvm_ctx, 0, 0, m_ctx->dbg_scopes.last(), nullptr));
        }
        builder.CreateCall(generated_ctor->llvm_fn, {dest});
        builder.SetCurrentDebugLocation(saved_dbg);
    }

    emit_alloc_init(fn, dest, struct_type);
}

CodegenContext::~CodegenContext() {}
CodegenContext::CodegenContext(CompilationContext *compilation_ctx)
    : compilation_ctx(compilation_ctx), resolver(&compilation_ctx->resolve_ctx) {
    init_llvm();
}

ScopedCodegenState::ScopedCodegenState(Compiler *compiler) : compiler(compiler) {
    assert(compiler && "scoped codegen state requires compiler");
    saved_fn = compiler->m_fn;
    saved_fn_eval_subtype = compiler->m_fn_eval_subtype;
    auto &builder = *compiler->m_ctx->llvm_builder.get();
    saved_block = builder.GetInsertBlock();
    if (saved_block) {
        saved_point = builder.GetInsertPoint();
    }
    saved_dbg = builder.getCurrentDebugLocation();
}

ScopedCodegenState::~ScopedCodegenState() {
    if (!compiler) {
        return;
    }
    compiler->m_fn = saved_fn;
    compiler->m_fn_eval_subtype = saved_fn_eval_subtype;
    auto &builder = *compiler->m_ctx->llvm_builder.get();
    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }
    builder.SetCurrentDebugLocation(saved_dbg);
}

Function *CodegenContext::add_fn(ast::Node *node, Function *fn) {
    functions.emplace(fn)->get();
    auto id = resolver.resolve_global_id(node);
    function_table[id] = fn;
    return fn;
}

llvm::StructType *CodegenContext::get_caught_result_type() {
    auto &builder = *llvm_builder;
    llvm::Type *field_types[] = {builder.getPtrTy(), builder.getInt32Ty()};
    return llvm::StructType::get(*llvm_ctx, TypeArray(field_types));
}

Function::Function(CodegenContext *ctx, llvm::Function *llvm_fn, ast::Node *node)
    : ctx(ctx), llvm_fn(llvm_fn), node(node) {
    if (node) {
        qualified_name = ctx->resolver.resolve_qualified_name(node);
    }
}

label_t *Function::new_label(const string &name, label_t *insert_before) {
    auto &builder = *ctx->llvm_builder.get();
    auto bb = llvm::BasicBlock::Create(*ctx->llvm_ctx, name, llvm_fn, insert_before);
    return bb;
}

void Function::use_label(label_t *bb) {
    auto &builder = *ctx->llvm_builder.get();
    builder.SetInsertPoint(bb);
}

void Function::insn_noop() {
    auto &builder = *ctx->llvm_builder.get();
    builder.CreateIntrinsic(llvm::Type::getVoidTy(*ctx->llvm_ctx), llvm::Intrinsic::donothing, {});
}

llvm::AllocaInst *Function::entry_alloca(llvm::Type *ty, const string &name) {
    auto &block = llvm_fn->getEntryBlock();
    llvm::IRBuilder<> tmp(&block, block.begin());
    auto var = tmp.CreateAlloca(ty, 0, name);
    return var;
}

void CodegenContext::init_llvm() {
    llvm_ctx = std::make_unique<llvm::LLVMContext>();
    llvm_module = std::make_unique<llvm::Module>("main", *llvm_ctx);
    llvm_builder = std::make_unique<llvm::IRBuilder<>>(*llvm_ctx);
    dbg_builder = std::make_unique<llvm::DIBuilder>(*llvm_module);
}

Compiler::Compiler(CodegenContext *ctx) : m_ctx(ctx) {}

ChiType *Compiler::eval_type(ChiType *type) {
    // Handle Infer types - extract the inferred concrete type
    if (type->kind == TypeKind::Infer && type->data.infer.inferred_type) {
        return eval_type(type->data.infer.inferred_type);
    }

    // Use TypeEnv from GenericResolver (set by compile_fn_proto)
    if (m_fn && m_fn->type_env && type->is_placeholder) {
        type = get_resolver()->type_placeholders_sub_map(type, m_fn->type_env);
    }

    // Handle m_fn_eval_subtype (for function proto compilation, before m_fn exists)
    if (type->is_placeholder && m_fn_eval_subtype && m_fn_eval_subtype->kind == TypeKind::Subtype) {
        type = get_resolver()->type_placeholders_sub(type, &m_fn_eval_subtype->data.subtype);
    }

    // Resolve special type kinds
    if (type->kind == TypeKind::Subtype) {
        if (!type->data.subtype.final_type && !type->is_placeholder) {
            auto resolved = get_resolver()->resolve_subtype(type);
            if (resolved && resolved != type) {
                return resolved;
            }
        }
        assert(type->data.subtype.final_type && "eval_type encountered unresolved subtype");
        return type->data.subtype.final_type;
    }
    if (type->kind == TypeKind::This && m_fn) {
        auto cr = m_fn->fn_type->data.fn.container_ref;
        return cr ? cr->get_elem() : type;
    }
    return type;
}

llvm::Type *Compiler::get_llvm_ptr_type() {
    auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
    return llvm::Type::getInt8PtrTy(llvm_ctx);
}

ChiType *Compiler::get_chitype(ast::Node *node) {
    if (m_fn && node->orig_type && node->orig_type->kind == TypeKind::This) {
        auto cr = m_fn->fn_type->data.fn.container_ref;
        return cr ? cr->get_elem() : eval_type(node->resolved_type);
    }
    return eval_type(node->resolved_type);
}

ast::Node *Compiler::unwrap_cast_exprs(ast::Node *node) {
    while (node && node->type == ast::NodeType::CastExpr) {
        node = node->data.cast_expr.expr;
    }
    return node;
}

ChiType *Compiler::find_nonvoid_pointee_type(ast::Node *node) {
    for (auto *current = node; current; ) {
        auto *type = get_chitype(current);
        if (type && type->is_pointer_like()) {
            auto *elem = type->get_elem();
            if (elem && elem->kind != TypeKind::Void) {
                return elem;
            }
        }

        if (current->type != ast::NodeType::CastExpr) {
            break;
        }
        current = current->data.cast_expr.expr;
    }
    return nullptr;
}

ChiType *Compiler::get_dyn_elem_value_type(ast::Node *node) {
    auto *value_expr = unwrap_cast_exprs(node);
    if (value_expr && value_expr->type == ast::NodeType::UnaryOpExpr) {
        auto &unary = value_expr->data.unary_op_expr;
        if (is_ref_unary_op(unary.op_type)) {
            value_expr = unary.op1;
        }
    }

    return value_expr ? eval_type(get_chitype(value_expr)) : nullptr;
}

llvm::DICompileUnit *Compiler::get_module_cu(ast::Module *module) {
    auto id = module->global_id();
    auto entry = m_ctx->module_cu_table.get(id);
    if (entry) {
        return *entry;
    }
    // Create compile unit on-demand for cross-module function compilation
    auto module_cu = m_ctx->dbg_builder->createCompileUnit(
        llvm::dwarf::DW_LANG_C,
        m_ctx->dbg_builder->createFile(module->filename, module->path, std::nullopt, std::nullopt),
        "Chi Compiler", 0, "", 0);
    m_ctx->module_cu_table[id] = module_cu;
    return module_cu;
}

void Compiler::compile_module(ast::Module *module) {
    // Skip if module already compiled
    if (m_ctx->compiled_modules.count(module)) {
        return;
    }
    m_ctx->compiled_modules.insert(module);

    for (auto import : module->imports) {
        compile_module(import);
    }

    // Skip debug info for virtual modules (C interop)
    bool is_virtual = module->path.size() >= 8 && module->path.substr(0, 8) == "<virtual";
    llvm::DICompileUnit *module_cu = nullptr;

    if (!is_virtual) {
        module_cu = m_ctx->dbg_builder->createCompileUnit(
            llvm::dwarf::DW_LANG_C,
            m_ctx->dbg_builder->createFile(module->filename, module->path, std::nullopt,
                                           std::nullopt),
            "Chi Compiler", 0, "", 0);
        m_ctx->module_cu_table[module->global_id()] = module_cu;
        m_ctx->dbg_cu = module_cu;
    }

    m_ctx->pending_fns.clear();
    auto &root = module->root->data.root;
    for (auto decl : root.top_level_decls) {
        switch (decl->type) {
        case ast::NodeType::FnDef: {
            auto proto = decl->data.fn_def.fn_proto;
            auto fn_type = get_chitype(decl);
            if (fn_type && fn_type->is_placeholder) {
                // This is a generic function - compile all its instantiated subtypes
                for (auto variant : decl->data.fn_def.variants) {
                    if (variant->resolved_type->is_placeholder) {
                        continue;
                    }

                    // Compile specialized version of the function
                    auto specialized_fn = compile_fn_proto(proto, variant, "");
                    m_ctx->pending_fns.add(specialized_fn);
                }
            } else {
                auto fn = compile_fn_proto(proto, decl);
                m_ctx->pending_fns.add(fn);
            }
            break;
        }
        case ast::NodeType::StructDecl:
            compile_struct(decl);
            break;
        case ast::NodeType::ExternDecl:
            compile_extern(decl);
            break;
        case ast::NodeType::ImportDecl:
            compile_module(decl->data.import_decl.resolved_module);
            break;
        case ast::NodeType::ExportDecl:
            compile_module(decl->data.export_decl.resolved_module);
            break;
        case ast::NodeType::EnumDecl:
            compile_enum(decl);
            break;
        case ast::NodeType::VarDecl: {
            // Top-level declarations (used for C macros, global constants)
            auto &var_data = decl->data.var_decl;
            if (var_data.kind == ast::VarKind::Constant) {
                // Compile-time constants are inlined at use sites
                // No code generation needed here - just skip
                break;
            }
            if (var_data.kind == ast::VarKind::Immutable) {
                // Module-level let: create global variable, defer initialization to main
                auto var_type_l = compile_type_of(decl);
                auto initial = llvm::Constant::getNullValue(var_type_l);
                auto global_name = "__chi_" + module->id_path + "_" + decl->name;
                auto global = new llvm::GlobalVariable(*m_ctx->llvm_module, var_type_l, false,
                                                       llvm::GlobalValue::InternalLinkage, initial,
                                                       global_name);
                add_var(decl, global);
                m_ctx->pending_global_inits.add({decl, global});
                break;
            }
            panic("global mutable variables not supported");
            break;
        }
        case ast::NodeType::TypedefDecl:
            // Typedefs are type aliases, no code generation needed
            break;
        default:
            panic("not implemented: {}", PRINT_ENUM(decl->type));
        }
    }

    while (m_ctx->pending_fns.size()) {
        auto list = m_ctx->pending_fns;
        m_ctx->pending_fns.clear();
        for (auto fn : list) {
            // Skip method bodies whose fn signature reaches the generic depth
            // limit — their bodies reference types beyond what the resolver created
            if (fn->fn_type && fn->fn_type->subtype_depth() >= MAX_GENERIC_DEPTH) {
                // Emit a safe stub body — these may be called at runtime by
                // destructor chains of deep generic types (e.g. Shared<T>)
                auto &builder = *m_ctx->llvm_builder.get();
                auto bb = llvm::BasicBlock::Create(*m_ctx->llvm_ctx, "entry", fn->llvm_fn);
                builder.SetInsertPoint(bb);
                auto ret_type = fn->llvm_fn->getReturnType();
                if (ret_type->isVoidTy()) {
                    builder.CreateRetVoid();
                } else {
                    builder.CreateRet(llvm::Constant::getNullValue(ret_type));
                }
                continue;
            }
            m_fn = fn;
            compile_fn_def(fn->node, fn);
            m_fn = nullptr;
        }
    }

    finalize_pending_typeinfos();
}

llvm::Value *Compiler::compile_reflection_vtable() {
    auto &builder = *m_ctx->llvm_builder.get();
    auto &llvm_ctx = *m_ctx->llvm_ctx.get();
    auto &llvm_module = *m_ctx->llvm_module;

    auto count = m_ctx->reflection_vtable.size();
    if (!count) {
        return nullptr;
    }
    auto vtable_type_l = llvm::ArrayType::get(get_llvm_ptr_type(), count);
    auto vtable_data_l = llvm::ConstantArray::get(vtable_type_l, m_ctx->reflection_vtable);
    return new llvm::GlobalVariable(*m_ctx->llvm_module, vtable_type_l, true,
                                    llvm::GlobalValue::ExternalLinkage, vtable_data_l,
                                    "internal_vtable");
}

inline llvm::Type *Compiler::compile_type_of(cx::ast::Node *node) {
    return compile_type(get_chitype(node));
}



void Compiler::compile_struct(ast::Node *node) {
    auto struct_type = get_resolver()->to_value_type(get_chitype(node));
    if (ChiTypeStruct::is_interface(struct_type)) {
        return;
    }
    auto &type_data = struct_type->data.struct_;
    if (ChiTypeStruct::is_generic(struct_type)) {
        for (auto subtype : type_data.subtypes) {
            if (subtype->is_placeholder) {
                continue;
            }
            if (!subtype->data.subtype.final_type) {
                continue;
            }
            // Track this struct specialization for comparison with GenericResolver
            m_ctx->compiled_generic_structs.insert(subtype->global_id);
            _compile_struct(node, subtype);
        }

    } else {
        _compile_struct(node, struct_type);
    }
}

void Compiler::_compile_struct(ast::Node *node, ChiType *type) {
    auto subtype = type->kind == TypeKind::Subtype ? type : nullptr;
    auto struct_type = type->kind == TypeKind::Subtype ? type->data.subtype.final_type : type;

    // Collect all FnDef members, including those inside ImplementBlock nodes
    cx::NodeList fn_members;
    for (auto member : node->data.struct_decl.members) {
        if (member->type == ast::NodeType::FnDef) {
            fn_members.add(member);
        } else if (member->type == ast::NodeType::ImplementBlock) {
            for (auto impl_member : member->data.implement_block.members) {
                fn_members.add(impl_member);
            }
        }
    }

    for (auto member : fn_members) {
        if (member->type == ast::NodeType::FnDef) {
            auto fn_node = member;
            if (subtype) {
                auto subtype_member = struct_type->data.struct_.find_member(member->name);
                if (!subtype_member) {
                    subtype_member = struct_type->data.struct_.find_static_member(member->name);
                }
                if (!subtype_member)
                    continue;
                fn_node = subtype_member->node;
            }

            auto fn_type = get_chitype(fn_node);
            if (fn_type->has_unresolved_subtype()) {
                continue;
            }

            // For generic methods, compile their specialized versions instead
            if (fn_type->is_placeholder) {
                // Skip the generic version but compile all specialized versions
                for (auto variant : fn_node->data.fn_def.variants) {
                    if (!variant->resolved_type->is_placeholder) {
                        auto fn = compile_fn_proto(fn_node->data.fn_def.fn_proto, variant);
                        if (subtype) {
                            fn->container_subtype = &subtype->data.subtype;
                            fn->container_type = subtype;
                        } else {
                            fn->container_type = type;
                        }
                        // For method variants, look up the function's TypeEnv from fn_envs
                        // This includes both struct type params AND method type params
                        auto fn_id = get_resolver()->resolve_global_id(variant);
                        if (auto entry = get_resolver()->get_generics()->fn_envs.get(fn_id)) {
                            fn->type_env = &entry->subs;
                        } else if (subtype) {
                            // Fallback to struct's type_env if no function-specific entry
                            if (auto entry = get_resolver()->get_generics()->struct_envs.get(
                                    subtype->global_id)) {
                                fn->type_env = &entry->subs;
                            }
                        }
                        m_ctx->pending_fns.add(fn);
                    }
                }
                continue;
            }

            auto fn = compile_fn_proto(fn_node->data.fn_def.fn_proto, fn_node);
            if (subtype) {
                fn->container_subtype = &subtype->data.subtype;
                fn->container_type = subtype;
                // Look up the TypeEnv for this struct specialization
                if (auto entry =
                        get_resolver()->get_generics()->struct_envs.get(subtype->global_id)) {
                    fn->type_env = &entry->subs;
                } else if (is_verbose_generics()) {
                    print("WARNING: No TypeEnv found for struct method, struct: {}\n",
                          subtype->global_id);
                }
            } else {
                fn->container_type = type;
            };
            m_ctx->pending_fns.add(fn);
        }
    }

    // Compile inherited/promoted methods
    for (auto member : struct_type->data.struct_.members) {
        if (!member->is_method() || !member->orig_parent || !member->node->data.fn_def.body)
            continue;
        // Skip members whose embed origin is still a placeholder (unspecialized generic)
        if (member->orig_parent->is_placeholder)
            continue;
        // Skip generic methods (e.g. map<U>) — they can't be concretely proxied
        if (member->resolved_type->is_placeholder)
            continue;
        // Skip if the struct already has its own implementation (override)
        bool has_own_impl = false;
        for (auto m : fn_members) {
            if (m->name == member->node->name) {
                has_own_impl = true;
                break;
            }
        }
        if (has_own_impl)
            continue;

        auto fn_node = member->node;
        auto struct_name =
            get_resolver()->format_type_qualified_name(type, fn_node->module->global_id());
        auto name = struct_name + "." + fn_node->name;
        // For embedding proxies, look up the orig_fn via the concrete embedded struct's
        // own member node (not the clone) so the function_table key matches correctly.
        Function *orig_fn = nullptr;
        if (member->parent_member && member->orig_parent) {
            auto orig_sty = get_resolver()->resolve_struct_type(member->orig_parent);
            if (orig_sty) {
                auto orig_method = orig_sty->find_member(member->node->name);
                if (orig_method)
                    orig_fn = get_fn(orig_method->node);
            }
        }
        auto fn =
            compile_fn_proto(fn_node->data.fn_def.fn_proto, fn_node, name, member->resolved_type);
        fn->container_type = type;
        auto key = fn_node->module->global_id() + "." + struct_name + "." + fn_node->name;
        m_ctx->function_table[key] = fn;

        if (member->parent_member) {
            generate_embed_proxy(fn, orig_fn, member, struct_type);
        } else {
            // Interface default method: recompile body with concrete struct dispatch
            fn->default_method_struct = struct_type;
            m_ctx->pending_fns.add(fn);
        }
    }

    // Generate __delete and __copy before vtables (vtable needs these fn ptrs)
    if (get_resolver()->type_needs_destruction(type)) {
        generate_destructor(type, nullptr);
    }
    generate_copier(type);

    if (struct_type->data.struct_.interfaces.size()) {
        // For generic instantiations (subtype != null), pass the subtype as the lookup_type
        // so that the dtor/copier are found in destructor_table/copier_table (which are
        // keyed by the subtype, not the resolved concrete struct).
        compile_struct_vtables(struct_type, subtype);
    }

    for (auto member : struct_type->data.struct_.members) {
        if (member->is_method() && !member->resolved_type->is_placeholder &&
            !member->resolved_type->has_unresolved_subtype()) {
            auto method_fn = get_fn(member->node, type);
            member->vtable_index = m_ctx->reflection_vtable.size();
            m_ctx->reflection_vtable.push_back(method_fn->llvm_fn);
        }
    }
}

void Compiler::compile_enum(ast::Node *node) {
    auto enum_type = get_chitype(node)->data.type_symbol.underlying_type;
    assert(enum_type->kind == TypeKind::Enum);
    if (enum_type->data.enum_.is_generic()) {
        return;
    }
    if (enum_type->data.enum_.compiled_data_size >= 0) {
        return;
    }

    int data_size = 0;
    for (auto variant : node->data.enum_decl.variants) {
        auto enum_value_type = variant->resolved_type;
        assert(enum_value_type->kind == TypeKind::EnumValue);
        if (auto inner_struct = enum_value_type->data.enum_value.variant_struct) {
            data_size = std::max(data_size, (int)llvm_type_size(compile_type(inner_struct)));
        }
    }
    enum_type->data.enum_.compiled_data_size = data_size;

    auto header_type_l = compile_type(enum_type->data.enum_.enum_header_struct);

    // Emit enum name and variant name globals
    auto &enum_data = enum_type->data.enum_;
    auto &name_info = m_ctx->enum_name_table[&enum_data];
    auto enum_name = node->name;
    auto enum_name_str = (llvm::Constant *)compile_string_literal(enum_name);
    name_info.enum_name = new llvm::GlobalVariable(
        *m_ctx->llvm_module, enum_name_str->getType(), true,
        llvm::GlobalValue::InternalLinkage, enum_name_str,
        fmt::format("{}.enum_name", enum_name));

    for (auto variant : node->data.enum_decl.variants) {
        auto variant_display_name = variant->resolved_type->get_display_name();
        auto variant_name = variant->resolved_type->name.value_or("");
        auto variant_name_str = (llvm::Constant *)compile_string_literal(variant_name);
        auto disc_value = variant->data.enum_variant.resolved_value;
        name_info.variant_names[disc_value] = new llvm::GlobalVariable(
            *m_ctx->llvm_module, variant_name_str->getType(), true,
            llvm::GlobalValue::InternalLinkage, variant_name_str,
            fmt::format("{}.variant_name", variant_display_name));
        auto display_str = fmt::format("{}.{}", enum_name, variant_name);
        auto display_str_val = (llvm::Constant *)compile_string_literal(display_str);
        name_info.display_names[disc_value] = new llvm::GlobalVariable(
            *m_ctx->llvm_module, display_str_val->getType(), true,
            llvm::GlobalValue::InternalLinkage, display_str_val,
            fmt::format("{}.display_name", variant_display_name));

        auto disc_type_l = compile_type(enum_type->data.enum_.discriminator);
        auto variant_value = llvm::ConstantStruct::get(
            (llvm::StructType *)header_type_l,
            {
                llvm::ConstantInt::get((llvm::IntegerType *)disc_type_l, disc_value),
            });
        auto var = new llvm::GlobalVariable(*m_ctx->llvm_module, header_type_l, true,
                                            llvm::GlobalValue::InternalLinkage, variant_value,
                                            fmt::format("{}.constant", variant_display_name));
        m_ctx->enum_variant_table[variant->data.enum_variant.resolved_enum_variant] = var;
    }

    // compile base struct methods
    if (auto base_struct = enum_type->data.enum_.base_struct) {
        auto base_value_type = enum_type->data.enum_.base_value_type;
        auto type_env_entry = get_resolver()->get_generics()->struct_envs.get(enum_type->global_id);
        for (auto member : base_struct->data.struct_.members) {
            if (member->is_method()) {
                auto fn_node = member->node;
                auto fn = compile_fn_proto(fn_node->data.fn_def.fn_proto, fn_node);
                if (type_env_entry && type_env_entry->subtype) {
                    fn->container_subtype = &type_env_entry->subtype->data.subtype;
                    fn->container_type = type_env_entry->subtype;
                    fn->type_env = &type_env_entry->subs;
                } else {
                    fn->container_type = base_value_type;
                }
                m_ctx->pending_fns.add(fn);
            }
        }
    }

    // Compile vtables and reflection indices for enum value struct,
    // same as _compile_struct does for regular structs
    auto base_value_type = enum_type->data.enum_.base_value_type;
    auto resolved_struct = base_value_type->data.enum_value.resolved_struct;
    if (resolved_struct) {
        // Generate per-enum intrinsic functions BEFORE vtables so display() is in the vtable
        compile_enum_name_intrinsics(&enum_data, base_value_type, resolved_struct);

        auto &sdata = resolved_struct->data.struct_;
        if (sdata.interfaces.size()) {
            compile_struct_vtables(resolved_struct, nullptr);
        }
        for (auto member : sdata.members) {
            if (member->is_method() && !member->resolved_type->is_placeholder) {
                auto method_fn = get_fn(member->node, base_value_type);
                if (method_fn) {
                    member->vtable_index = m_ctx->reflection_vtable.size();
                    m_ctx->reflection_vtable.push_back(method_fn->llvm_fn);
                }
            }
        }
    }
}

void Compiler::compile_enum_name_intrinsics(ChiTypeEnum *enum_data, ChiType *base_value_type,
                                            ChiType *resolved_struct) {
    auto name_it = m_ctx->enum_name_table.get(enum_data);
    if (!name_it) return;
    auto &name_info = *name_it;

    auto &builder_ref = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto &llvm_module = *m_ctx->llvm_module;

    // Save current insert point — we'll restore it after generating intrinsic functions
    auto saved_block = builder_ref.GetInsertBlock();
    auto saved_point = builder_ref.GetInsertPoint();

    auto str_type_l = compile_type(get_resolver()->get_system_types()->string);
    auto enum_type_l = compile_type(base_value_type);
    auto disc_type_l = compile_type(enum_data->discriminator);

    // Function type: (sret ptr, this ptr) -> void
    auto fn_type_l = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_ctx),
        {llvm::PointerType::get(llvm_ctx, 0), llvm::PointerType::get(llvm_ctx, 0)}, false);

    auto type_display = get_resolver()->format_type_display(base_value_type);

    // Helper to register a generated function in the function table.
    // Only registers if the member has the expected intrinsic symbol (i.e., was not
    // overridden by user code in a custom struct block).
    auto register_fn = [&](const string &method_name, llvm::Function *llvm_fn,
                           IntrinsicSymbol expected_symbol) {
        for (auto member : resolved_struct->data.struct_.members) {
            if (member->is_method() && member->get_name() == method_name) {
                if (expected_symbol != IntrinsicSymbol::None &&
                    member->symbol != expected_symbol) {
                    break; // User overrode this method; don't replace
                }
                auto type_key = get_resolver()->format_type_qualified_name(
                    base_value_type, member->node->module->global_id());
                auto key = fmt::format("{}.{}.{}", member->node->module->global_id(),
                                       type_key, method_name);
                auto fn = m_ctx->functions.emplace(new Function(m_ctx, llvm_fn, member->node))->get();
                fn->qualified_name = key;
                m_ctx->function_table[key] = fn;
                break;
            }
        }
    };

    // Generate enum_name()
    {
        auto fn_name = fmt::format("{}.enum_name", type_display);
        auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage,
                                               fn_name, llvm_module);
        auto sret = llvm_fn->getArg(0);
        auto entry = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
        builder_ref.SetInsertPoint(entry);
        auto str_val = builder_ref.CreateLoad(str_type_l, name_info.enum_name);
        builder_ref.CreateStore(str_val, sret);
        builder_ref.CreateRetVoid();
        register_fn("enum_name", llvm_fn, IntrinsicSymbol::EnumName);
    }

    // Generate discriminator_name()
    {
        auto fn_name = fmt::format("{}.discriminator_name", type_display);
        auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage,
                                               fn_name, llvm_module);
        auto sret = llvm_fn->getArg(0);
        auto this_ptr = llvm_fn->getArg(1);
        auto entry = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
        builder_ref.SetInsertPoint(entry);
        auto disc_gep = builder_ref.CreateStructGEP(enum_type_l, this_ptr, 0);
        auto disc = builder_ref.CreateLoad(disc_type_l, disc_gep, "disc");

        auto default_b = llvm::BasicBlock::Create(llvm_ctx, "default", llvm_fn);
        auto sw = builder_ref.CreateSwitch(disc, default_b, name_info.variant_names.size());
        for (auto &[disc_val, name_global] : name_info.variant_names.data) {
            auto case_b = llvm::BasicBlock::Create(llvm_ctx, fmt::format("disc_{}", disc_val), llvm_fn);
            sw->addCase(llvm::ConstantInt::get((llvm::IntegerType *)disc_type_l, disc_val), case_b);
            builder_ref.SetInsertPoint(case_b);
            auto str_val = builder_ref.CreateLoad(str_type_l, name_global);
            builder_ref.CreateStore(str_val, sret);
            builder_ref.CreateRetVoid();
        }
        builder_ref.SetInsertPoint(default_b);
        auto empty = compile_string_literal("");
        builder_ref.CreateStore(empty, sret);
        builder_ref.CreateRetVoid();
        register_fn("discriminator_name", llvm_fn, IntrinsicSymbol::DiscriminatorName);
    }

    // Generate display() — returns "EnumName.VariantName" using pre-computed globals
    // Skip if the enum has a custom display() override
    bool has_custom_display = enum_data->base_struct != nullptr;
    if (has_custom_display) {
        // Check if the custom struct block actually implements Display
        has_custom_display = false;
        auto base_struct_type = get_resolver()->resolve_struct_type(enum_data->base_struct);
        if (base_struct_type) {
            for (auto iface : base_struct_type->interfaces) {
                if (iface->inteface_symbol == IntrinsicSymbol::Display) {
                    has_custom_display = true;
                    break;
                }
            }
        }
    }
    if (!has_custom_display) {
        auto fn_name = fmt::format("{}.display", type_display);
        auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage,
                                               fn_name, llvm_module);
        auto sret = llvm_fn->getArg(0);
        auto this_ptr = llvm_fn->getArg(1);
        auto entry = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
        builder_ref.SetInsertPoint(entry);
        auto disc_gep = builder_ref.CreateStructGEP(enum_type_l, this_ptr, 0);
        auto disc = builder_ref.CreateLoad(disc_type_l, disc_gep, "disc");

        auto default_b = llvm::BasicBlock::Create(llvm_ctx, "default", llvm_fn);
        auto sw = builder_ref.CreateSwitch(disc, default_b, name_info.display_names.size());
        for (auto &[disc_val, display_global] : name_info.display_names.data) {
            auto case_b = llvm::BasicBlock::Create(llvm_ctx, fmt::format("disp_{}", disc_val), llvm_fn);
            sw->addCase(llvm::ConstantInt::get((llvm::IntegerType *)disc_type_l, disc_val), case_b);
            builder_ref.SetInsertPoint(case_b);
            auto str_val = builder_ref.CreateLoad(str_type_l, display_global);
            builder_ref.CreateStore(str_val, sret);
            builder_ref.CreateRetVoid();
        }
        builder_ref.SetInsertPoint(default_b);
        auto empty = compile_string_literal("");
        builder_ref.CreateStore(empty, sret);
        builder_ref.CreateRetVoid();
        register_fn("display", llvm_fn, IntrinsicSymbol::None);
    }

    // Restore insert point
    if (saved_block) {
        builder_ref.SetInsertPoint(saved_block, saved_point);
    }
}

void Compiler::compile_concrete_enum(ChiTypeEnum *enum_data) {
    if (enum_data->compiled_data_size >= 0) {
        return;
    }

    int data_size = 0;
    for (auto variant : enum_data->variants) {
        auto enum_value_type = variant->resolved_type;
        assert(enum_value_type->kind == TypeKind::EnumValue);
        if (auto inner_struct = enum_value_type->data.enum_value.variant_struct) {
            data_size = std::max(data_size, (int)llvm_type_size(compile_type(inner_struct)));
        }
    }
    enum_data->compiled_data_size = data_size;

    auto header_type_l = compile_type(enum_data->enum_header_struct);

    // Emit enum name and variant name globals
    auto &name_info = m_ctx->enum_name_table[enum_data];
    auto enum_name = enum_data->base_value_type->get_display_name();
    // Strip ".BaseEnumValue" suffix if present
    auto suffix = string(".BaseEnumValue");
    if (enum_name.size() > suffix.size() &&
        enum_name.substr(enum_name.size() - suffix.size()) == suffix) {
        enum_name = enum_name.substr(0, enum_name.size() - suffix.size());
    }
    auto enum_name_str = (llvm::Constant *)compile_string_literal(enum_name);
    name_info.enum_name = new llvm::GlobalVariable(
        *m_ctx->llvm_module, enum_name_str->getType(), true,
        llvm::GlobalValue::InternalLinkage, enum_name_str,
        fmt::format("{}.enum_name", enum_name));

    for (auto variant : enum_data->variants) {
        auto variant_display_name = variant->resolved_type->get_display_name();
        auto variant_name_s = variant->name;
        auto variant_name_str = (llvm::Constant *)compile_string_literal(variant_name_s);
        auto disc_value = variant->node->data.enum_variant.resolved_value;
        name_info.variant_names[disc_value] = new llvm::GlobalVariable(
            *m_ctx->llvm_module, variant_name_str->getType(), true,
            llvm::GlobalValue::InternalLinkage, variant_name_str,
            fmt::format("{}.variant_name", variant_display_name));
        auto display_str = fmt::format("{}.{}", enum_name, variant_name_s);
        auto display_str_val = (llvm::Constant *)compile_string_literal(display_str);
        name_info.display_names[disc_value] = new llvm::GlobalVariable(
            *m_ctx->llvm_module, display_str_val->getType(), true,
            llvm::GlobalValue::InternalLinkage, display_str_val,
            fmt::format("{}.display_name", variant_display_name));

        auto disc_type_l = compile_type(enum_data->discriminator);
        auto variant_value = llvm::ConstantStruct::get(
            (llvm::StructType *)header_type_l,
            {
                llvm::ConstantInt::get((llvm::IntegerType *)disc_type_l, disc_value),
            });
        auto var = new llvm::GlobalVariable(*m_ctx->llvm_module, header_type_l, true,
                                            llvm::GlobalValue::InternalLinkage, variant_value,
                                            fmt::format("{}.constant", variant_display_name));
        m_ctx->enum_variant_table[variant] = var;
    }

    // Compile base struct methods (same as compile_enum)
    if (auto base_struct = enum_data->base_struct) {
        auto base_value_type = enum_data->base_value_type;
        auto enum_type_id = base_value_type && base_value_type->data.enum_value.enum_type
            ? base_value_type->data.enum_value.enum_type->global_id
            : "";
        auto type_env_entry = get_resolver()->get_generics()->struct_envs.get(enum_type_id);
        for (auto member : base_struct->data.struct_.members) {
            if (member->is_method()) {
                auto fn_node = member->node;
                auto fn = compile_fn_proto(fn_node->data.fn_def.fn_proto, fn_node);
                if (type_env_entry && type_env_entry->subtype) {
                    fn->container_subtype = &type_env_entry->subtype->data.subtype;
                    fn->container_type = type_env_entry->subtype;
                    fn->type_env = &type_env_entry->subs;
                } else {
                    fn->container_type = base_value_type;
                }
                m_ctx->pending_fns.add(fn);
            }
        }
    }

    // Generate per-enum intrinsic functions and vtables
    auto base_value_type = enum_data->base_value_type;
    if (base_value_type) {
        auto resolved_struct = base_value_type->data.enum_value.resolved_struct;
        if (resolved_struct) {
            // Generate intrinsics BEFORE vtables so display() is in the vtable
            compile_enum_name_intrinsics(enum_data, base_value_type, resolved_struct);

            auto &sdata = resolved_struct->data.struct_;
            if (sdata.interfaces.size()) {
                compile_struct_vtables(resolved_struct, nullptr);
            }
            for (auto member : sdata.members) {
                if (member->is_method() && !member->resolved_type->is_placeholder) {
                    auto method_fn = get_fn(member->node, base_value_type);
                    if (method_fn) {
                        member->vtable_index = m_ctx->reflection_vtable.size();
                        m_ctx->reflection_vtable.push_back(method_fn->llvm_fn);
                    }
                }
            }
        }
    }
}

llvm::DISubroutineType *Compiler::compile_di_fn_type(Function *fn) {
    llvm::SmallVector<llvm::Metadata *, 8> types;
    if (fn->fn_type->data.fn.container_ref) {
        types.push_back(compile_di_type(fn->fn_type->data.fn.container_ref));
    }
    for (auto param : fn->fn_type->data.fn.params) {
        types.push_back(compile_di_type(param));
    }
    return m_ctx->dbg_builder->createSubroutineType(
        m_ctx->dbg_builder->getOrCreateTypeArray(types));
}

llvm::DIType *Compiler::compile_di_type(ChiType *type) {
    type = eval_type(type);
    auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
    auto &llvm_module = *(m_ctx->llvm_module.get());
    auto &llvm_builder = *(m_ctx->llvm_builder.get());
    auto &llvm_db = *(m_ctx->dbg_builder.get());
    auto &llvm_cu = *(m_ctx->dbg_cu);

    switch (type->kind) {
    case TypeKind::Never:
    case TypeKind::Void: {
        return llvm_db.createBasicType("void", 0, llvm::dwarf::DW_ATE_address);
    }
    case TypeKind::Null:
    case TypeKind::Unit: {
        return llvm_db.createBasicType("Unit", 8, llvm::dwarf::DW_ATE_unsigned);
    }
    case TypeKind::Tuple: {
        auto name = get_resolver()->format_type_display(type);
        auto llvm_type = compile_type(type);
        auto size = m_ctx->llvm_module->getDataLayout().getTypeSizeInBits(llvm_type);
        return llvm_db.createBasicType(name, size, llvm::dwarf::DW_ATE_unsigned);
    }
    case TypeKind::Bool: {
        return llvm_db.createBasicType("bool", 8, llvm::dwarf::DW_ATE_boolean);
    }
    case TypeKind::Byte: {
        return llvm_db.createBasicType("byte", 8, llvm::dwarf::DW_ATE_unsigned_char);
    }
    case TypeKind::Rune: {
        return llvm_db.createBasicType("rune", 32, llvm::dwarf::DW_ATE_unsigned);
    }
    case TypeKind::Int: {
        return llvm_db.createBasicType("int", type->data.int_.bit_count,
                                       llvm::dwarf::DW_ATE_signed);
    }
    case TypeKind::Float: {
        return llvm_db.createBasicType("float", 32, llvm::dwarf::DW_ATE_float);
    }
    case TypeKind::String: {
        return llvm_db.createBasicType("string", 0, llvm::dwarf::DW_ATE_UTF);
    }
    case TypeKind::Fn: {
        auto &data = type->data.fn;
        llvm::SmallVector<llvm::Metadata *, 8> types;
        for (auto param : data.params) {
            types.push_back(compile_di_type(param));
        }
        return llvm_db.createSubroutineType(llvm_db.getOrCreateTypeArray(types));
    }
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef: {
        auto &data = type->data.pointer;
        auto elem_type = compile_di_type(data.elem);
        auto size = llvm_type_size(compile_type(data.elem));
        return llvm_db.createPointerType(elem_type, size, 0, llvm::dwarf::DW_ATE_address);
    }
    case TypeKind::Array: {
        auto &data = type->data.array;
        auto elem_type = compile_di_type(data.elem);
        auto size = llvm_type_size(compile_type(data.elem));
        return llvm_db.createArrayType(0, size, elem_type, {});
    }
    case TypeKind::Span: {
        auto &data = type->data.span;
        auto elem_type = compile_di_type(data.elem);
        auto size = llvm_type_size(compile_type(data.elem));
        return llvm_db.createArrayType(0, size, elem_type, {});
    }
    case TypeKind::Struct: {
        auto &data = type->data.struct_;
        if (!data.fields.size()) {
            return llvm_db.createBasicType("void", 0, llvm::dwarf::DW_ATE_address);
        }
        auto cu = &llvm_cu;
        auto module = data.node ? data.node->module : nullptr;
        if (module) {
            if (auto p = m_ctx->module_cu_table.get(module->global_id())) {
                cu = *p;
            }
        }
        auto name = m_ctx->resolver.format_type_display(type);
        auto file = llvm_db.createFile(cu->getFilename(), cu->getDirectory());
        auto line_no = 0;
        if (data.node) {
            line_no = data.node->token->pos.line_number();
        }
        auto scope = m_ctx->dbg_scopes.size() ? m_ctx->dbg_scopes.last() : nullptr;
        if (!scope) {
            auto dbg_builder = m_ctx->dbg_builder.get();
            auto unit = dbg_builder->createFile(cu->getFilename(), cu->getDirectory());
            scope = unit;
        }
        return llvm_db.createStructType(scope, name, file, line_no, 0, 0, llvm::DINode::FlagZero,
                                        nullptr, {});
    }
    case TypeKind::Subtype: {
        return compile_di_type(type->data.subtype.final_type);
    }
    case TypeKind::This: {
        auto e = type->eval();
        if (e != type)
            return compile_di_type(e);
        return llvm_db.createBasicType("this", 0, llvm::dwarf::DW_ATE_address);
    }
    default:
        auto size = llvm_type_size(compile_type(type));
        return llvm_db.createBasicType(m_ctx->resolver.format_type_display(type), size,
                                       llvm::dwarf::DW_ATE_unsigned);
    }
}

Function *Compiler::compile_fn_def(ast::Node *node, Function *fn) {
    auto is_generated = node->type == ast::NodeType::GeneratedFn;
    auto fn_def_node = is_generated ? node->data.generated_fn.original_fn : node;

    assert(fn_def_node->type == ast::NodeType::FnDef);
    auto &fn_def = fn_def_node->data.fn_def;
    // auto proto_node = is_generated ? node->data.generated_fn.fn_proto : fn_def.fn_proto;
    auto proto_node = fn_def.fn_proto;
    auto &fn_proto = proto_node->data.fn_proto;

    if (!fn) {
        fn = compile_fn_proto(proto_node, node);
    }
    if (fn->body_compiled) {
        return fn;
    }
    if (fn->is_compiling_body) {
        panic("function body compilation re-entered");
        return nullptr;
    }
    fn->is_compiling_body = true;

    // debug info
    auto name = get_resolver()->resolve_display_name(node);
    auto cu = get_module_cu(fn->node->module);
    assert(cu);
    auto dbg_builder = m_ctx->dbg_builder.get();
    auto file = dbg_builder->createFile(cu->getFilename(), cu->getDirectory());
    llvm::DIScope *dctx = file;
    auto line_no = fn_def.fn_proto->token->pos.line_number();
    auto sp = dbg_builder->createFunction(
        dctx, name, llvm::StringRef(), file, 0, compile_di_fn_type(fn), line_no,
        llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
    fn->llvm_fn->setSubprogram(sp);
    auto saved_dbg_scope_len = m_ctx->dbg_scopes.size();
    m_ctx->dbg_scopes.add(sp);
    emit_dbg_location(nullptr); // unset the location for the prologue emission

    // build function
    auto &builder = *m_ctx->llvm_builder.get();
    auto &llvm_ctx = *m_ctx->llvm_ctx.get();
    auto *entry_b = fn->new_label("_entry");
    fn->use_label(entry_b);

    llvm::Value *sret_arg = nullptr;

    for (const auto &param_info : fn->parameter_info) {
        auto llvm_param = fn->llvm_fn->getArg(param_info.llvm_index);

        if (param_info.kind == ParameterKind::SRet) {
            sret_arg = llvm_param;
        } else if (param_info.kind == ParameterKind::Bind) {
            fn->bind_ptr = llvm_param;
        } else if (param_info.kind == ParameterKind::Regular) {
            auto idx = param_info.user_param_index;
            if (idx >= fn_proto.params.size()) {
                printf("ERROR: idx %d >= proto.params.size() %lu\n", idx, fn_proto.params.size());
                continue;
            }

            auto param = fn_proto.params[idx];
            auto param_type = param_info.type;
            // Never heap-allocate parameters - use stack allocation regardless of escape status
            // Parameters are function arguments and should always be stack-local
            auto var = fn->entry_alloca(compile_type(param_type), param->name);

            emit_dbg_location(param);

            // Bitwise store only — caller is responsible for copy semantics.
            // For rvalue args (temps), this avoids a redundant copy + destroy.
            // For named var args, the caller does copy before passing.
            builder.CreateStore(llvm_param, var);
            add_var(param, var);

            // Allocate drop flag for maybe-moved parameters
            if (fn->get_def() && fn->get_def()->flow.is_maybe_sunk(param)) {
                alloc_drop_flag(fn, param, true);
            }

            // debug info
            auto param_line_no = param->token->pos.line_number();
            auto user_param_offset = fn->get_user_param_llvm_offset();
            auto dbg_var = dbg_builder->createParameterVariable(
                sp, llvm_param->getName(), param_info.llvm_index + user_param_offset + 1, file,
                param_line_no, compile_di_type(param_type));
            dbg_builder->insertDeclare(
                var, dbg_var, dbg_builder->createExpression(),
                llvm::DILocation::get(sp->getContext(), param_line_no, 0, sp),
                builder.GetInsertBlock());
        }
    }

    // function return
    auto fn_type = fn->fn_type;
    auto return_type = fn_type->data.fn.return_type;
    auto return_type_l = compile_type(return_type);
    if (return_type->kind != TypeKind::Void && return_type->kind != TypeKind::Never) {
        fn->return_value =
            sret_arg ? sret_arg : builder.CreateAlloca(return_type_l, nullptr, "_fn_ret");
    }

    // main function initialization
    auto is_entry = fn_def.decl_spec->has_flag(ast::DECL_IS_ENTRY);
    if (is_entry) {
        emit_dbg_location(fn_def.body);
        auto runtime_start = get_system_fn("cx_runtime_start");
        auto set_program_args = get_system_fn("cx_set_program_args");
        auto stack_marker =
            builder.CreateAlloca(llvm::IntegerType::getInt1Ty(llvm_ctx), nullptr, "_stack_marker");
        builder.CreateCall(runtime_start->llvm_fn, {stack_marker});
        if (fn->llvm_fn->arg_size() >= 2) {
            builder.CreateCall(set_program_args->llvm_fn,
                               {fn->llvm_fn->getArg(0), fn->llvm_fn->getArg(1)});
        }

        auto vtable = compile_reflection_vtable();
        if (vtable) {
            auto set_vtable = get_system_fn("cx_set_program_vtable");
            builder.CreateCall(set_vtable->llvm_fn, {vtable});
        }

        // Initialize module-level let variables
        for (auto &[var_node, global_var] : m_ctx->pending_global_inits) {
            auto &var_data = var_node->data.var_decl;
            if (var_data.expr) {
                auto var_type = get_chitype(var_node);
                compile_assignment_to_ptr(fn, var_data.expr, global_var, var_type);
            }
        }
    }

    // function body
    emit_dbg_location(fn_def.body);
    auto return_b = fn->new_label("_return");
    fn->return_label = return_b;

    if (fn_def.body) {
        // Check if this is an async function with awaits
        if (fn_def.is_async()) {
            compile_async_fn_body(fn);
        } else {
            compile_block(fn, node, fn_def.body, return_b);
        }
    }
    if (fn->get_def()->has_try_or_cleanup() || fn->async_reject_promise_ptr) {
        fn->llvm_fn->setPersonalityFn(get_system_fn("cx_personality")->llvm_fn);
    }

    // clean up & return (block-local vars are destroyed at block exit or inline at return sites)
    fn->use_label(return_b);
    emit_cleanup_owners(fn);
    if (is_entry) {
        auto runtime_stop = get_system_fn("cx_runtime_stop");
        builder.CreateCall(runtime_stop->llvm_fn, {});
        if (fn_def.is_async() && fn->return_value &&
            get_resolver()->type_needs_destruction(return_type)) {
            compile_destruction_for_type(fn, fn->return_value, return_type);
        }
        builder.CreateRet(
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*m_ctx->llvm_ctx), 0));
    } else if (return_type->kind == TypeKind::Void || return_type->kind == TypeKind::Never ||
               fn->use_sret()) {
        builder.CreateRetVoid();
    } else {
        auto value = builder.CreateLoad(return_type_l, fn->return_value);
        builder.CreateRet(value);
    }

    // cleanup landing pad (for handling exceptions)
    if (fn->cleanup_landing_label) {
        fn->use_label(fn->cleanup_landing_label);

        if (fn->async_reject_promise_ptr) {
            // Async function: catch typed errors and reject the promise
            auto landing = builder.CreateLandingPad(m_ctx->get_caught_result_type(), 1);
            landing->addClause(llvm::ConstantPointerNull::get(builder.getPtrTy()));
            emit_async_reject_landing_pad(fn, landing);
            // Store rejected promise into return_value and return normally
            auto promise_type_l = compile_type(fn->async_promise_type);
            auto rejected_promise =
                builder.CreateLoad(promise_type_l, fn->async_reject_promise_ptr);
            builder.CreateStore(rejected_promise, fn->return_value);
            builder.CreateBr(fn->return_label);
        } else {
            // Normal function: cleanup and re-throw
            auto landing = builder.CreateLandingPad(m_ctx->get_caught_result_type(), 0);
            landing->setCleanup(true);
            builder.CreateExtractValue(landing, {0});
            builder.CreateExtractValue(landing, {1});
            // Destroy function body block's vars (conservative: covers all function-level locals)
            if (fn->get_def()->body) {
                auto &body_block = fn->get_def()->body->data.block;
                compile_block_cleanup(fn, &body_block, nullptr, body_block.exit_flow);
            }
            emit_cleanup_owners(fn);
            fn->insn_noop();
            builder.CreateResume(landing);
        }
    }

    llvm::verifyFunction(*fn->llvm_fn);
    m_ctx->dbg_scopes.resize(saved_dbg_scope_len);
    fn->is_compiling_body = false;
    fn->body_compiled = true;
    return fn;
}

llvm::Value *Compiler::compile_constant_value(Function *fn, const ConstantValue &value,
                                              ChiType *type, llvm::Type *llvm_type) {
    auto t = llvm_type ? llvm_type : compile_type(type);
    if (type->kind == TypeKind::Null) {
        return llvm::ConstantPointerNull::get((llvm::PointerType *)t);
    }
    if (type->kind == TypeKind::Optional) {
        // null optional: zeroed struct {has_value=false, value=zeroed}
        return llvm::ConstantAggregateZero::get(t);
    }
    if (type->is_pointer_like()) {
        return llvm::ConstantPointerNull::get((llvm::PointerType *)t);
    }
    if (VARIANT_TRY(value, const_int_t, v)) {
        return llvm::ConstantInt::get(t, *v);
    } else if (VARIANT_TRY(value, const_float_t, v)) {
        return llvm::ConstantFP::get(t, *v);
    } else if (VARIANT_TRY(value, string, v)) {
        return compile_string_literal(*v);
    }
    return nullptr;
}

llvm::Value *Compiler::compile_string_literal(const string &str) {
    auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
    auto &llvm_module = *(m_ctx->llvm_module.get());
    auto &llvm_builder = *(m_ctx->llvm_builder.get());
    auto str_lit_type = compile_type(get_resolver()->get_system_types()->str_lit);
    auto str_value = llvm::ConstantDataArray::getString(llvm_ctx, str, false);
    auto char_array_type = llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx), str.size());
    auto str_global = new llvm::GlobalVariable(llvm_module, char_array_type, true,
                                               llvm::GlobalValue::PrivateLinkage, str_value, "str");
    auto str_len = llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), str.size());
    auto str_type_l = compile_type(get_resolver()->get_system_types()->string);
    auto one = llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), 1);
    // auto padding =
    //     llvm::ConstantArray::get(llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx), 3),
    //                              {
    //                                  llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_ctx), 0),
    //                                  llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_ctx), 0),
    //                                  llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_ctx), 0),
    //                              });
    auto str_struct = llvm::ConstantStruct::get((llvm::StructType *)str_type_l, {
                                                                                    str_global,
                                                                                    str_len,
                                                                                    one,
                                                                                });
    return str_struct;
}

llvm::Value *Compiler::compile_c_string_literal(const string &str) {
    auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
    auto &llvm_module = *(m_ctx->llvm_module.get());
    // Create null-terminated string constant
    auto str_value =
        llvm::ConstantDataArray::getString(llvm_ctx, str, true); // true = add null terminator
    auto char_array_type = llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx), str.size() + 1);
    auto str_global = new llvm::GlobalVariable(
        llvm_module, char_array_type, true, llvm::GlobalValue::PrivateLinkage, str_value, "cstr");
    // Return pointer to first element (char*)
    return llvm::ConstantExpr::getPointerCast(str_global, llvm::Type::getInt8PtrTy(llvm_ctx));
}

llvm::Value *Compiler::compile_assignment_to_type(Function *fn, ast::Node *expr,
                                                  ChiType *dest_type,
                                                  bool allow_saved_owning_conversion) {
    auto *effective_expr = unwrap_noop_cast(expr);
    if (effective_expr != expr) {
        return compile_assignment_to_type(fn, effective_expr, dest_type,
                                          allow_saved_owning_conversion);
    }
    auto src_type = get_chitype(effective_expr);

    // Check if src_type has placeholder type params (for generic struct field defaults)
    auto has_placeholder_params = [](ChiType *type) -> bool {
        if (type->is_placeholder)
            return true;
        if (type->kind == TypeKind::Struct && type->data.struct_.type_params.size() > 0) {
            return true;
        }
        return false;
    };

    // Handle ConstructExpr with placeholder type - use dest_type for compilation
    if (effective_expr->type == ast::NodeType::ConstructExpr && has_placeholder_params(src_type) &&
        dest_type && !has_placeholder_params(dest_type)) {
        auto &builder = *m_ctx->llvm_builder.get();
        auto ptr = fn->entry_alloca(compile_type(dest_type), "placeholder_tmp");
        compile_construction(fn, ptr, dest_type, effective_expr);
        return builder.CreateLoad(compile_type(dest_type), ptr);
    }

    if (allow_saved_owning_conversion && needs_implicit_owning_conversion(effective_expr)) {
        auto &builder = *m_ctx->llvm_builder.get();
        auto tmp = fn->entry_alloca(compile_type(dest_type),
                                    dest_type->kind == TypeKind::Any ? "implicit_any"
                                                                     : "implicit_wrap");
        bool converted =
            compile_implicit_owning_conversion_to_ptr(fn, effective_expr, tmp, dest_type);
        assert(converted);
        return builder.CreateLoad(compile_type(dest_type), tmp);
    }

    auto src_value = compile_expr(fn, effective_expr);
    if (src_type->kind == TypeKind::Undefined) {
        return nullptr;
    }
    if (src_type->kind == TypeKind::ZeroInit) {
        return llvm::Constant::getNullValue(compile_type(dest_type));
    }
    if (!dest_type || get_resolver()->is_same_type(src_type, dest_type)) {
        return src_value;
    }
    if (effective_expr->type == ast::NodeType::ConstructExpr &&
        get_resolver()->is_same_type(src_type, dest_type)) {
        return src_value;
    }
    auto value = src_value;
    if (dest_type) {
        emit_dbg_location(effective_expr);
        value = compile_conversion(fn, src_value, src_type, dest_type);
    }
    return value;
}

ast::Node *Compiler::create_cleanup_temp_var(Function *fn, ast::Node *expr, ChiType *type,
                                             ast::Block *cleanup_block, const string &name) {
    auto *temp = get_resolver()->get_dummy_var(name);
    temp->resolved_type = type;
    temp->token = expr && expr->token ? expr->token : (fn && fn->node ? fn->node->token : nullptr);
    temp->module = expr && expr->module ? expr->module : (fn && fn->node ? fn->node->module : nullptr);
    assert(temp->module && "cleanup temp var must have a module");
    if (cleanup_block && get_resolver()->should_destroy(temp, type)) {
        get_resolver()->add_cleanup_var(cleanup_block, temp);
    }
    return temp;
}

void Compiler::push_cleanup_block(Function *fn, ast::Block &block) {
    fn->push_scope();
    fn->push_active_block(&block);
}

void Compiler::pop_cleanup_block(Function *fn, ast::Block &block) {
    auto &builder = *m_ctx->llvm_builder.get();
    if (!builder.GetInsertBlock()->getTerminator()) {
        compile_block_cleanup(fn, &block, nullptr, block.exit_flow);
    }
    fn->pop_active_block();
    fn->pop_scope();
}

llvm::Value *Compiler::compile_arg_for_call(Function *fn, ast::Node *expr, ChiType *param_type,
                                            ast::Block *cleanup_block,
                                            std::vector<ast::Node *> *transferred_cleanup_vars) {
    auto &builder = *m_ctx->llvm_builder;
    auto src_type = get_chitype(expr);
    if (needs_implicit_owning_conversion(expr)) {
        auto *temp_var = create_cleanup_temp_var(
            fn, expr, param_type, cleanup_block,
            param_type->kind == TypeKind::Any ? "_arg_any" : "_arg_wrap");
        auto *temp_ptr = compile_alloc(fn, temp_var, false, param_type);
        add_var(temp_var, temp_ptr);
        compile_implicit_owning_conversion_to_ptr(fn, expr, temp_ptr, param_type);
        if (transferred_cleanup_vars) {
            transferred_cleanup_vars->push_back(temp_var);
        }
        return builder.CreateLoad(compile_type(param_type), temp_ptr);
    }

    // Caller-side copy: for non-moved args (named vars) passed to by-value
    // params, do copy here since the callee only does a bitwise store.
    // Moved args (rvalues) are already fresh values — ownership transfers.
    if (!expr->analysis.moved && param_type && !param_type->is_reference() &&
        get_resolver()->type_needs_destruction(param_type)) {
        auto *temp_var = create_cleanup_temp_var(fn, expr, param_type, cleanup_block, "_arg_copy");
        auto *temp_ptr = compile_alloc(fn, temp_var, false, param_type);
        add_var(temp_var, temp_ptr);
        emit_dbg_location(expr);
        auto src_ref = compile_expr_ref(fn, expr);
        compile_ref_value_to_ptr(fn, src_ref, src_type, temp_ptr, param_type);
        if (transferred_cleanup_vars) {
            transferred_cleanup_vars->push_back(temp_var);
        }
        return builder.CreateLoad(compile_type(param_type), temp_ptr);
    }

    // Moved arg whose source lives at an addressable location (e.g. an async
    // frame slot via m_async_await_refs). Load bits directly and, if the
    // source address belongs to an async frame slot, clear its alive flag so
    // the frame destructor doesn't double-destroy the value.
    if (expr->analysis.moved && param_type && !param_type->is_reference()) {
        auto src_ref = compile_expr_ref(fn, expr);
        if (src_ref.address) {
            auto arg_value = src_ref.value;
            if (!arg_value) {
                arg_value = builder.CreateLoad(compile_type(src_type), src_ref.address);
            }
            emit_dbg_location(expr);
            arg_value = compile_conversion(fn, arg_value, src_type, param_type, true);
            if (auto alive_ptr = async_frame_alive_ptr_for_addr(fn, src_ref.address)) {
                builder.CreateStore(llvm::ConstantInt::getFalse(*m_ctx->llvm_ctx), alive_ptr);
            }
            return arg_value;
        }
        if (src_ref.value) {
            emit_dbg_location(expr);
            return compile_conversion(fn, src_ref.value, src_type, param_type, src_ref.owns_value);
        }
    }

    return compile_assignment_to_type(fn, expr, param_type);
}

llvm::Value *Compiler::compile_extern_variadic_arg(Function *fn, ast::Node *expr) {
    auto &builder = *m_ctx->llvm_builder;
    auto src_type = get_chitype(expr);
    auto arg_value = compile_expr(fn, expr);
    if (!src_type) {
        return arg_value;
    }

    switch (src_type->kind) {
    case TypeKind::Float:
        if (src_type->data.float_.bit_count == 32) {
            return builder.CreateFPExt(arg_value, llvm::Type::getDoubleTy(*m_ctx->llvm_ctx));
        }
        return arg_value;
    case TypeKind::Bool:
        return builder.CreateZExt(arg_value, llvm::Type::getInt32Ty(*m_ctx->llvm_ctx));
    case TypeKind::Byte:
        return builder.CreateZExt(arg_value, llvm::Type::getInt32Ty(*m_ctx->llvm_ctx));
    case TypeKind::Int:
        if (src_type->data.int_.bit_count < 32) {
            if (src_type->data.int_.is_unsigned) {
                return builder.CreateZExt(arg_value, llvm::Type::getInt32Ty(*m_ctx->llvm_ctx));
            }
            return builder.CreateSExt(arg_value, llvm::Type::getInt32Ty(*m_ctx->llvm_ctx));
        }
        return arg_value;
    default:
        return arg_value;
    }
}

llvm::Value *Compiler::compile_direct_call_arg(Function *fn, ast::Node *expr, ChiType *param_type) {
    auto &builder = *m_ctx->llvm_builder;
    auto src_type = get_chitype(expr);

    if (needs_implicit_owning_conversion(expr)) {
        return compile_arg_for_call(fn, expr, param_type);
    }

    if (expr->analysis.moved && param_type && !param_type->is_reference()) {
        auto src_ref = compile_expr_ref(fn, expr);
        if (src_ref.address) {
            // Lvalue move: load bits directly from address. For sync paths the
            // caller skips block cleanup via compile_block_cleanup's skip_var
            // and async_frame_owned_vars filter. For async paths the value may
            // live in a frame slot whose alive flag must be cleared so the
            // frame destructor doesn't double-destroy it.
            auto arg_value = src_ref.value;
            if (!arg_value) {
                arg_value = builder.CreateLoad(compile_type(src_type), src_ref.address);
            }
            emit_dbg_location(expr);
            arg_value = compile_conversion(fn, arg_value, src_type, param_type, true);
            if (auto alive_ptr = async_frame_alive_ptr_for_addr(fn, src_ref.address)) {
                builder.CreateStore(llvm::ConstantInt::getFalse(*m_ctx->llvm_ctx), alive_ptr);
            }
            return arg_value;
        }
        if (src_ref.value) {
            // Rvalue move: expression already compiled, use value directly.
            emit_dbg_location(expr);
            return compile_conversion(fn, src_ref.value, src_type, param_type, src_ref.owns_value);
        }
    }

    return compile_arg_for_call(fn, expr, param_type);
}

llvm::Value *Compiler::build_span_value(ChiType *span_type, llvm::Value *data_ptr,
                                        llvm::Value *length) {
    auto &builder = *m_ctx->llvm_builder.get();
    auto *span_type_l = compile_type(span_type);
    llvm::Value *span_value = llvm::UndefValue::get(span_type_l);
    span_value = builder.CreateInsertValue(span_value, data_ptr, {0});
    span_value = builder.CreateInsertValue(span_value, length, {1});
    return span_value;
}

void Compiler::compile_any_box_ref_to_ptr(Function *fn, RefValue src, ChiType *src_type,
                                          llvm::Value *dest, ChiType *dest_type) {
    auto &builder = *m_ctx->llvm_builder.get();
    auto &llvm_ctx = *m_ctx->llvm_ctx.get();
    auto *any_type_l = (llvm::StructType *)compile_type(dest_type);

    auto *ti_gep = builder.CreateStructGEP(any_type_l, dest, 0);
    builder.CreateStore(compile_type_info(src_type), ti_gep);

    auto *inlined_gep = builder.CreateStructGEP(any_type_l, dest, 1);
    auto *src_type_l = compile_type(src_type);
    auto type_size = llvm_type_size(src_type_l);
    auto inlined = type_size <= sizeof(CxAnyStorage);
    builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_ctx), inlined),
                        inlined_gep);

    auto *data_gep = builder.CreateStructGEP(any_type_l, dest, 3);
    if (inlined) {
        compile_copy_with_ref(fn, src, data_gep, src_type, nullptr, false);
        return;
    }

    auto *malloc_fn = get_system_fn("cx_malloc");
    auto *size_l = llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), type_size);
    auto *heap_ptr = builder.CreateCall(malloc_fn->llvm_fn, {size_l, get_null_ptr()}, "any_alloc");
    compile_copy_with_ref(fn, src, heap_ptr, src_type, nullptr, false);
    builder.CreateStore(heap_ptr, data_gep);
}

void Compiler::compile_ref_value_to_ptr(Function *fn, RefValue src, ChiType *src_type,
                                        llvm::Value *dest, ChiType *dest_type) {
    auto &builder = *m_ctx->llvm_builder.get();

    if (get_resolver()->is_same_type(src_type, dest_type)) {
        compile_copy_with_ref(fn, src, dest, dest_type, nullptr, false);
        return;
    }

    if (get_resolver()->use_implicit_owning_coercion(src_type, dest_type)) {
        if (dest_type->kind == TypeKind::Optional) {
            auto *opt_type_l = compile_type(dest_type);
            auto *has_value_p = builder.CreateStructGEP(opt_type_l, dest, 0);
            builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt1Ty(*m_ctx->llvm_ctx), 1),
                                has_value_p);
            auto *value_p = builder.CreateStructGEP(opt_type_l, dest, 1);
            compile_ref_value_to_ptr(fn, src, src_type, value_p, dest_type->get_elem());
            return;
        }
        if (dest_type->kind == TypeKind::Any) {
            compile_any_box_ref_to_ptr(fn, src, src_type, dest, dest_type);
            return;
        }
    }

    auto *value = src.value;
    if (!value) {
        value = builder.CreateLoad(compile_type(src_type), src.address);
    }
    auto *converted = compile_conversion(fn, value, src_type, dest_type, src.owns_value);
    builder.CreateStore(converted, dest);
}

llvm::Value *Compiler::compile_variadic_span_arg(Function *fn, array<ast::Node *> args,
                                                 int va_start, ChiType *span_type,
                                                 ast::Node *dbg_node) {
    auto &builder = *m_ctx->llvm_builder.get();
    auto *resolver = get_resolver();
    auto *elem_type = span_type->get_elem();
    auto *i32_ty = llvm::Type::getInt32Ty(*m_ctx->llvm_ctx);

    if ((int)args.size() <= va_start) {
        return llvm::Constant::getNullValue(compile_type(span_type));
    }

    if ((int)args.size() == va_start + 1 && args[va_start]->type == ast::NodeType::PackExpansion) {
        auto &pack_data = args[va_start]->data.pack_expansion;
        if (pack_data.can_forward_directly) {
            return compile_assignment_to_type(fn, pack_data.expr, span_type);
        }
    }

    struct PackSource {
        ast::Node *arg = nullptr;
        ChiType *seq_type = nullptr;
        llvm::Value *data = nullptr;
        llvm::Value *length = nullptr;
    };

    array<PackSource> pack_sources = {};
    llvm::Value *total_count = llvm::ConstantInt::get(i32_ty, args.size() - va_start);
    bool has_pack = false;

    for (int i = va_start; i < args.size(); i++) {
        auto *arg = args[i];
        if (arg->type != ast::NodeType::PackExpansion) {
            continue;
        }

        has_pack = true;
        total_count = builder.CreateSub(total_count, llvm::ConstantInt::get(i32_ty, 1));

        auto *seq_expr = arg->data.pack_expansion.expr;
        auto *seq_type = resolver->to_value_type(get_chitype(seq_expr));

        auto seq_ref = compile_expr_ref(fn, seq_expr);
        auto [data_value, length_value] =
            compile_sequence_data_and_length(fn, seq_ref.address, seq_type);
        total_count = builder.CreateAdd(total_count, length_value);
        pack_sources.add({arg, seq_type, data_value, length_value});
    }

    llvm::Value *blob_data = nullptr;
    llvm::Value *length_value = nullptr;
    if (!has_pack) {
        auto count = (uint32_t)(args.size() - va_start);
        auto *length_const = llvm::ConstantInt::get(i32_ty, count);
        if (count == 0) {
            return llvm::Constant::getNullValue(compile_type(span_type));
        }

        auto *fixed_array_type = resolver->get_fixed_array_type(elem_type, count);
        auto *fixed_array_ptr =
            fn->entry_alloca(compile_type(fixed_array_type), "vararg_blob");
        emit_dbg_location(dbg_node);
        auto *arr_type_l = compile_type(fixed_array_type);
        auto *zero = llvm::ConstantInt::get(i32_ty, 0);
        bool elem_needs_destruct = resolver->type_needs_destruction(elem_type);
        llvm::Value *active_var = nullptr;
        if (elem_needs_destruct) {
            active_var = register_reusable_cleanup_slot(
                fn, fixed_array_ptr, fixed_array_type, "_vararg_fixed.active");
        }
        for (uint32_t i = 0; i < count; i++) {
            auto *idx = llvm::ConstantInt::get(i32_ty, i);
            auto *elem_ptr = builder.CreateGEP(arr_type_l, fixed_array_ptr, {zero, idx});
            compile_assignment_to_ptr(fn, args[va_start + (int)i], elem_ptr, elem_type, false);
        }
        blob_data = builder.CreateGEP(arr_type_l, fixed_array_ptr, {zero, zero});
        length_value = length_const;
        if (elem_needs_destruct) {
            builder.CreateStore(llvm::ConstantInt::getTrue(*m_ctx->llvm_ctx), active_var);
        }
    } else {
        emit_dbg_location(dbg_node);
        auto *array_type = resolver->get_array_type(elem_type);
        auto *array_struct_type = resolver->eval_struct_type(array_type);
        assert(array_struct_type);
        auto *array_ptr = fn->entry_alloca(compile_type(array_type), "vararg_array");
        emit_construct_init(fn, array_ptr, array_type);

        auto *elem_size =
            llvm::ConstantInt::get(i32_ty, llvm_type_size(compile_type(elem_type)).getFixedValue());
        auto reserve_fn = get_system_fn("cx_array_reserve");
        builder.CreateCall(reserve_fn->llvm_fn, {array_ptr, elem_size, total_count});

        int pack_index = 0;

        for (int i = va_start; i < args.size(); i++) {
            auto *arg = args[i];
            if (arg->type != ast::NodeType::PackExpansion) {
                auto add_fn = get_system_fn("cx_array_add");
                auto *slot_void =
                    builder.CreateCall(add_fn->llvm_fn, {array_ptr, elem_size}, "vararg_slot");
                auto *dest_elem_ptr =
                    builder.CreateBitCast(slot_void, compile_type(resolver->get_pointer_type(elem_type)));
                compile_assignment_to_ptr(fn, arg, dest_elem_ptr, elem_type, false);
                continue;
            }

            auto pack = pack_sources[pack_index++];
            auto *loop_index = fn->entry_alloca(i32_ty, "_pack_i");
            builder.CreateStore(llvm::ConstantInt::get(i32_ty, 0), loop_index);
            auto *bb_cond = fn->new_label("pack_expand_cond");
            auto *bb_body = fn->new_label("pack_expand_body");
            auto *bb_end = fn->new_label("pack_expand_end");
            builder.CreateBr(bb_cond);

            fn->use_label(bb_cond);
            auto *src_idx = builder.CreateLoad(i32_ty, loop_index);
            auto *cond = builder.CreateICmpULT(src_idx, pack.length);
            builder.CreateCondBr(cond, bb_body, bb_end);

            fn->use_label(bb_body);
            auto add_fn = get_system_fn("cx_array_add");
            auto *slot_void =
                builder.CreateCall(add_fn->llvm_fn, {array_ptr, elem_size}, "vararg_slot");
            auto *dst_elem_ptr =
                builder.CreateBitCast(slot_void, compile_type(resolver->get_pointer_type(elem_type)));
            auto *src_elem_ptr =
                builder.CreateGEP(compile_type(pack.seq_type->get_elem()), pack.data, {src_idx});
            compile_ref_value_to_ptr(fn, RefValue::from_address(src_elem_ptr), pack.seq_type->get_elem(),
                                     dst_elem_ptr, elem_type);
            builder.CreateStore(
                builder.CreateAdd(src_idx, llvm::ConstantInt::get(i32_ty, 1)), loop_index);
            builder.CreateBr(bb_cond);

            fn->use_label(bb_end);
        }

        auto *array_data_field = array_struct_type->data.struct_.find_member("data");
        auto *array_length_field = array_struct_type->data.struct_.find_member("length");
        assert(array_data_field && array_length_field);
        auto *data_ptr = compile_dot_access(fn, array_ptr, array_type, array_data_field);
        auto *length_ptr = compile_dot_access(fn, array_ptr, array_type, array_length_field);
        blob_data = builder.CreateLoad(compile_type(array_data_field->resolved_type), data_ptr);
        length_value = builder.CreateLoad(i32_ty, length_ptr);

        register_cleanup_owner(fn, array_ptr, array_type, "_vararg_array.active");
    }

    return build_span_value(span_type, blob_data, length_value);
}

std::pair<llvm::Value *, llvm::Value *> Compiler::compile_sequence_data_and_length(Function *fn,
                                                                                    llvm::Value *seq_ptr,
                                                                                    ChiType *seq_type) {
    auto &builder = *m_ctx->llvm_builder.get();
    assert(seq_type && (seq_type->kind == TypeKind::Span || seq_type->kind == TypeKind::Array));
    auto *seq_struct = get_resolver()->resolve_struct_type(seq_type);
    assert(seq_struct);
    auto *data_field = seq_struct->find_member("data");
    auto *length_field = seq_struct->find_member("length");
    assert(data_field && length_field);

    auto *data_ptr = compile_dot_access(fn, seq_ptr, seq_type, data_field);
    auto *length_ptr = compile_dot_access(fn, seq_ptr, seq_type, length_field);
    auto *data_value = builder.CreateLoad(compile_type(data_field->resolved_type), data_ptr);
    auto *length_value =
        builder.CreateLoad(llvm::Type::getInt32Ty(*m_ctx->llvm_ctx), length_ptr);
    return {data_value, length_value};
}

bool Compiler::needs_implicit_owning_conversion(ast::Node *expr) {
    return expr && expr->analysis.conversion_type == ast::ConversionType::OwningCoercion;
}

ast::ConversionType Compiler::get_saved_conversion_type(ast::Node *expr) {
    assert(expr && expr->type == ast::NodeType::CastExpr);
    assert(expr->analysis.conversion_type != ast::ConversionType::None);
    return expr->analysis.conversion_type;
}

ast::Node *Compiler::unwrap_noop_cast(ast::Node *expr) {
    if (!expr || expr->type != ast::NodeType::CastExpr) {
        return expr;
    }
    if (get_saved_conversion_type(expr) == ast::ConversionType::NoOp) {
        return expr->data.cast_expr.expr;
    }
    return expr;
}

ast::Node *Compiler::get_owning_conversion_source_expr(ast::Node *expr) {
    if (!expr || expr->type != ast::NodeType::CastExpr) {
        return expr;
    }
    if (get_saved_conversion_type(expr) == ast::ConversionType::OwningCoercion) {
        return expr->data.cast_expr.expr;
    }
    return expr;
}

bool Compiler::compile_implicit_owning_conversion_to_ptr(Function *fn, ast::Node *expr,
                                                         llvm::Value *dest, ChiType *dest_type,
                                                         bool destruct_old) {
    auto src_type = get_chitype(expr);
    if (!needs_implicit_owning_conversion(expr)) {
        return false;
    }
    if (dest_type->kind == TypeKind::Optional) {
        compile_optional_wrap_to_ptr(fn, expr, dest, dest_type, destruct_old);
        return true;
    }
    if (dest_type->kind == TypeKind::Any) {
        compile_any_box_to_ptr(fn, expr, dest, dest_type, destruct_old);
        return true;
    }
    return false;
}

void Compiler::compile_any_box_to_ptr(Function *fn, ast::Node *expr, llvm::Value *dest,
                                      ChiType *dest_type, bool destruct_old) {
    auto &builder = *m_ctx->llvm_builder.get();
    auto &llvm_ctx = *m_ctx->llvm_ctx.get();
    auto any_type_l = (llvm::StructType *)compile_type(dest_type);

    auto source_expr = get_owning_conversion_source_expr(expr);
    auto src_type = get_chitype(source_expr);

    if (destruct_old) {
        compile_destruction_for_type(fn, dest, dest_type);
    }

    auto ti_gep = builder.CreateStructGEP(any_type_l, dest, 0);
    builder.CreateStore(compile_type_info(src_type), ti_gep);

    auto inlined_gep = builder.CreateStructGEP(any_type_l, dest, 1);
    auto src_type_l = compile_type(src_type);
    auto type_size = llvm_type_size(src_type_l);
    auto inlined = type_size <= sizeof(CxAnyStorage);
    builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_ctx), inlined), inlined_gep);

    auto data_gep = builder.CreateStructGEP(any_type_l, dest, 3);
    if (inlined) {
        compile_assignment_to_ptr(fn, source_expr, data_gep, src_type, false, false);
        return;
    }

    auto malloc_fn = get_system_fn("cx_malloc");
    auto size_l = llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), type_size);
    auto heap_ptr = builder.CreateCall(malloc_fn->llvm_fn, {size_l, get_null_ptr()}, "any_alloc");
    compile_assignment_to_ptr(fn, source_expr, heap_ptr, src_type, false, false);
    builder.CreateStore(heap_ptr, data_gep);
}

void Compiler::compile_optional_wrap_to_ptr(Function *fn, ast::Node *expr, llvm::Value *dest,
                                            ChiType *dest_type, bool destruct_old) {
    auto &builder = *m_ctx->llvm_builder.get();
    auto opt_type_l = compile_type(dest_type);
    auto elem_type = eval_type(dest_type->get_elem());
    if (destruct_old) {
        compile_destruction_for_type(fn, dest, dest_type);
    }
    auto has_value_p = builder.CreateStructGEP(opt_type_l, dest, 0);
    builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt1Ty(*m_ctx->llvm_ctx), 1),
                        has_value_p);
    auto value_p = builder.CreateStructGEP(opt_type_l, dest, 1);
    auto source_expr = get_owning_conversion_source_expr(expr);
    compile_assignment_to_ptr(fn, source_expr, value_p, elem_type, false, false);
}

llvm::Value *Compiler::compile_aliasing_safe_assignment(Function *fn, ast::Node *rhs,
                                                        llvm::Value *dest_ptr,
                                                        ChiType *dest_type) {
    auto &builder = *m_ctx->llvm_builder;
    auto *ty_l = compile_type(dest_type);
    auto *tmp = fn->entry_alloca(ty_l, "_assign_tmp");
    compile_assignment_to_ptr(fn, rhs, tmp, dest_type, /*destruct_old=*/false);
    compile_destruction_for_type(fn, dest_ptr, dest_type);
    builder.CreateMemCpy(dest_ptr, llvm::MaybeAlign(), tmp, llvm::MaybeAlign(),
                         llvm_type_size(ty_l));
    return builder.CreateLoad(ty_l, dest_ptr);
}

void Compiler::compile_assignment_to_ptr(Function *fn, ast::Node *expr, llvm::Value *dest,
                                         ChiType *dest_type, bool destruct_old,
                                         bool allow_saved_owning_conversion) {
    auto *effective_expr = unwrap_noop_cast(expr);
    if (effective_expr != expr) {
        compile_assignment_to_ptr(fn, effective_expr, dest, dest_type, destruct_old,
                                  allow_saved_owning_conversion);
        return;
    }
    auto saved_outlet = effective_expr->resolved_outlet;
    effective_expr->resolved_outlet = nullptr;

    auto src_type = get_chitype(effective_expr);
    // Zeroinit: bypass field-wise copy (which would cx_string_copy a zeroed
    // string and leak a malloc(0)). Emit a direct bitwise zero-store instead.
    if (src_type && src_type->kind == TypeKind::ZeroInit && dest_type) {
        if (destruct_old) {
            compile_destruction_for_type(fn, dest, dest_type);
        }
        m_ctx->llvm_builder->CreateStore(
            llvm::Constant::getNullValue(compile_type(dest_type)), dest);
        effective_expr->resolved_outlet = saved_outlet;
        return;
    }
    if (allow_saved_owning_conversion &&
        compile_implicit_owning_conversion_to_ptr(fn, effective_expr, dest, dest_type,
                                                  destruct_old)) {
        effective_expr->resolved_outlet = saved_outlet;
        return;
    }
    // RVO: construct directly at destination when types match
    if (effective_expr->type == ast::NodeType::ConstructExpr &&
        get_resolver()->is_same_type(src_type, dest_type) &&
        !effective_expr->data.construct_expr.is_new) {
        if (destruct_old) {
            compile_destruction_for_type(fn, dest, dest_type);
        }
        compile_construction(fn, dest, dest_type, effective_expr);
        effective_expr->resolved_outlet = saved_outlet;
        return;
    }
    // RVO: forward dest as sret for function calls (non-lambda, non-optional-chain)
    if (effective_expr->type == ast::NodeType::FnCallExpr &&
        get_resolver()->is_same_type(src_type, dest_type)) {
        auto &fn_call_data = effective_expr->data.fn_call_expr;
        bool is_lambda = fn_call_data.fn_ref_expr->resolved_type->kind == TypeKind::FnLambda;
        bool is_optional_chain = fn_call_data.fn_ref_expr->type == ast::NodeType::DotExpr &&
                                 fn_call_data.fn_ref_expr->data.dot_expr.is_optional_chain;
        if (!is_lambda && !is_optional_chain) {
            compile_fn_call(fn, effective_expr, nullptr, dest);
            effective_expr->resolved_outlet = saved_outlet;
            return;
        }
    }
    if (!effective_expr->analysis.moved && dest_type &&
        get_resolver()->type_needs_destruction(dest_type) &&
        get_resolver()->is_same_type(src_type, dest_type)) {
        auto src_ref = compile_expr_ref(fn, effective_expr);
        compile_copy_with_ref(fn, src_ref, dest, dest_type, effective_expr, destruct_old);
        effective_expr->resolved_outlet = saved_outlet;
        return;
    }
    auto value = compile_assignment_to_type(fn, effective_expr, dest_type,
                                            allow_saved_owning_conversion);
    if (value) {
        compile_store_or_copy(fn, value, dest, dest_type, effective_expr, destruct_old);
    }
    effective_expr->resolved_outlet = saved_outlet;
}

llvm::Value *Compiler::compile_assignment_value(Function *fn, ast::Node *expr, ast::Node *dest) {
    return compile_assignment_to_type(fn, expr, get_chitype(dest));
}

llvm::TypeSize Compiler::llvm_type_size(llvm::Type *type) {
    if (type->isVoidTy()) {
        return llvm::TypeSize::Fixed(0);
    }
    return m_ctx->llvm_module->getDataLayout().getTypeAllocSize(type);
}

void Compiler::compile_struct_vtables(ChiType *type, ChiType *lookup_type /*= nullptr*/) {
    array<CompiledVtable> vtables = {};
    int32_t count = 0;
    std::vector<llvm::Constant *> methods;

    assert(type->kind == TypeKind::Struct);
    // For generic instantiations (subtypes), dtor/copier are keyed by the subtype in
    // destructor_table/copier_table. Use lookup_type if provided, otherwise fall back to type.
    auto dtor_lookup = lookup_type ? lookup_type : type;
    auto copier_lookup = lookup_type ? lookup_type : type;

    // Get destructor for this concrete type (may be null)
    auto dtor_it = m_ctx->destructor_table.get(dtor_lookup);
    llvm::Constant *dtor_ptr =
        dtor_it && *dtor_it ? (llvm::Constant *)(*dtor_it)->llvm_fn : get_null_ptr();

    // Get copier for this concrete type (may be null)
    auto copier_it = m_ctx->copier_table.get(copier_lookup);
    llvm::Constant *copier_ptr =
        copier_it && *copier_it ? (llvm::Constant *)(*copier_it)->llvm_fn : get_null_ptr();

    for (auto impl : type->data.struct_.interfaces) {
        auto vtable = vtables.add({});
        vtable->offset = count;
        vtable->impl = impl;

        // Vtable header: [typeinfo_ptr, destructor_fn_ptr, copier_fn_ptr, method0, method1, ...]
        methods.push_back((llvm::Constant *)compile_type_info(type));
        methods.push_back(dtor_ptr);
        methods.push_back(copier_ptr);
        count += 3;

        for (auto &method : impl->impl_members) {
            if (method->is_method()) {
                auto method_fn = get_fn(method->node, type);
                methods.push_back(method_fn->llvm_fn);
                count++;
            }
        }
    }

    auto vtable_type_l = llvm::ArrayType::get(get_llvm_ptr_type(), count);
    auto vtable_data_l = llvm::ConstantArray::get(vtable_type_l, methods);
    auto global = new llvm::GlobalVariable(*m_ctx->llvm_module, vtable_type_l, true,
                                           llvm::GlobalValue::PrivateLinkage, vtable_data_l,
                                           "vtables." + get_resolver()->format_type_display(type));

    for (auto &vtable : vtables) {
        if (vtable.offset == 0) {
            m_ctx->impl_table[vtable.impl] = global;
        } else {
            auto idx = llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(m_ctx->llvm_module->getContext()), vtable.offset);
            m_ctx->impl_table[vtable.impl] =
                llvm::ConstantExpr::getGetElementPtr(get_llvm_ptr_type(), global, idx);
        }
    }
}

static bool is_reflect_array_type(Resolver *resolver, ChiType *type) {
    if (!type) {
        return false;
    }
    if (type->kind == TypeKind::Array) {
        return true;
    }
    auto rt_array_type = resolver->get_context()->rt_array_type;
    if (type->kind == TypeKind::Subtype && rt_array_type && type->data.subtype.generic &&
        resolver->to_value_type(type->data.subtype.generic) == resolver->to_value_type(rt_array_type) &&
        type->data.subtype.args.size() > 0) {
        return true;
    }
    return false;
}

static ChiType *get_reflect_array_elem_type(Resolver *resolver, ChiType *type) {
    if (!type) {
        return nullptr;
    }
    if (type->kind == TypeKind::Array) {
        return type->get_elem();
    }
    auto rt_array_type = resolver->get_context()->rt_array_type;
    if (type->kind == TypeKind::Subtype && rt_array_type && type->data.subtype.generic &&
        resolver->to_value_type(type->data.subtype.generic) == resolver->to_value_type(rt_array_type) &&
        type->data.subtype.args.size() > 0) {
        return type->data.subtype.args[0];
    }
    return nullptr;
}

TypeInfoWorkItem Compiler::get_typeinfo_work_item(ChiType *type) {
    auto final_type = eval_type(type);
    if (is_reflect_array_type(get_resolver(), type)) {
        return {type, final_type};
    }
    return {final_type, final_type};
}

string Compiler::get_typeinfo_cache_key(const TypeInfoWorkItem &item) {
    auto reflect_id = item.reflect_type->global_id.empty()
                          ? get_resolver()->format_type_id(item.reflect_type)
                          : item.reflect_type->global_id;
    if (item.reflect_type != item.final_type) {
        return "reflect_typeid:" + reflect_id;
    }
    return reflect_id;
}

llvm::GlobalVariable *Compiler::ensure_type_info_global(ChiType *type, bool queue) {
    auto item = get_typeinfo_work_item(type);
    auto reflect_type = item.reflect_type;
    auto final_type = item.final_type;
    auto key = get_typeinfo_cache_key(item);
    if (auto info = m_ctx->typeinfo_table.get(key)) {
        if (queue) {
            if (!m_ctx->pending_typeinfo_items.has_key(key)) {
                m_ctx->pending_typeinfo_keys.add(key);
            }
            m_ctx->pending_typeinfo_items[key] = item;
        }
        return *info;
    }

    auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
    auto ptr_type_l = llvm::PointerType::get(llvm_ctx, 0);
    constexpr size_t tidata_word_count =
        (sizeof(TypeInfoData) + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    auto tidata_type_l =
        llvm::ArrayType::get(llvm::Type::getInt64Ty(llvm_ctx), (uint32_t)tidata_word_count);
    auto ti_type_l = llvm::StructType::get(
        llvm_ctx,
        {llvm::Type::getInt32Ty(llvm_ctx), llvm::Type::getInt32Ty(llvm_ctx), tidata_type_l,
         ptr_type_l, ptr_type_l, llvm::Type::getInt32Ty(llvm_ctx), ptr_type_l,
         llvm::Type::getInt32Ty(llvm_ctx), ptr_type_l, llvm::Type::getInt32Ty(llvm_ctx),
         llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx), sizeof(TypeInfo::name))},
        false);

    auto info_global = new llvm::GlobalVariable(
        *m_ctx->llvm_module, ti_type_l, true, llvm::GlobalValue::PrivateLinkage,
        llvm::Constant::getNullValue(ti_type_l),
        "typeinfo." + get_resolver()->format_type_display(reflect_type));
    m_ctx->typeinfo_table[key] = info_global;

    if (!m_ctx->pending_typeinfo_items.has_key(key)) {
        m_ctx->pending_typeinfo_keys.add(key);
    }
    m_ctx->pending_typeinfo_items[key] = item;

    return info_global;
}

llvm::Constant *Compiler::build_type_info_initializer(ChiType *type) {
    auto reflect_type = type;
    auto final_type = eval_type(type);
    auto reflect_kind = is_reflect_array_type(get_resolver(), reflect_type) ? TypeKind::Array : final_type->kind;
    auto type_l = compile_type(final_type);
    auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
    auto &llvm_module = *(m_ctx->llvm_module.get());
    constexpr size_t tidata_word_count =
        (sizeof(TypeInfoData) + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    auto tidata_type_l =
        llvm::ArrayType::get(llvm::Type::getInt64Ty(llvm_ctx), (uint32_t)tidata_word_count);
    auto ptr_type_l = llvm::PointerType::get(llvm_ctx, 0);
    auto i8_ty = llvm::Type::getInt8Ty(llvm_ctx);

    // Compile meta table
    auto meta_table_len = 0;
    TypeMetaEntry *meta_table_data = nullptr;
    auto field_table_len = 0;
    std::vector<llvm::Constant *> field_table_consts = {};

    if (final_type->kind == TypeKind::Struct || final_type->kind == TypeKind::Array ||
        final_type->kind == TypeKind::Span || final_type->kind == TypeKind::EnumValue ||
        final_type->kind == TypeKind::Tuple || final_type->kind == TypeKind::FixedArray ||
        reflect_kind == TypeKind::Array) {
        ChiTypeStruct *sty;
        if (final_type->kind == TypeKind::EnumValue) {
            // Use the parent enum's base value struct — vtable indices are assigned there,
            // and all variants share the same method layout
            auto parent_enum = final_type->data.enum_value.parent_enum();
            sty = get_resolver()->resolve_struct_type(parent_enum->base_value_type);
        } else if (final_type->kind == TypeKind::Tuple || final_type->kind == TypeKind::FixedArray) {
            sty = nullptr;
        } else {
            sty = get_resolver()->resolve_struct_type(final_type);
        }
        if (sty) {
            for (auto member : sty->members) {
                if (member->is_method() && member->vtable_index >= 0) {
                    auto new_len = meta_table_len + 1;
                    meta_table_data = reallocate_nonzero(meta_table_data, meta_table_len, new_len);
                    auto new_entry = &meta_table_data[meta_table_len];
                    new_entry->vtable_index = member->vtable_index;
                    new_entry->symbol = member->symbol;
                    auto member_name = member->get_name();
                    auto name_len = (uint32_t)member_name.size();
                    if (name_len > sizeof(new_entry->name)) {
                        name_len = sizeof(new_entry->name);
                    }
                    new_entry->name_len = name_len;
                    memset(new_entry->name, 0, sizeof(meta_table_data->name));
                    if (name_len > 0) {
                        memcpy(new_entry->name, member_name.data(), name_len);
                    }
                    meta_table_len = new_len;
                }
            }
        }

        if ((final_type->kind == TypeKind::Struct || reflect_kind == TypeKind::Array) &&
            sty && !ChiTypeStruct::is_interface(final_type)) {
            auto own_fields = sty->own_fields();
            if (own_fields.size() > 0) {
                auto layout = llvm_module.getDataLayout().getStructLayout((llvm::StructType *)type_l);
                for (auto field : own_fields) {
                    auto new_len = field_table_len + 1;
                    auto field_name = field->get_name();
                    auto name_len = (uint32_t)field_name.size();
                    if (name_len > sizeof(TypeFieldEntry::name)) {
                        name_len = sizeof(TypeFieldEntry::name);
                    }
                    std::vector<uint8_t> field_name_buf(sizeof(TypeFieldEntry::name), 0);
                    if (name_len > 0) {
                        memcpy(field_name_buf.data(), field_name.data(), name_len);
                    }
                    auto field_name_l = llvm::ConstantDataArray::get(
                        llvm_ctx, llvm::ArrayRef<uint8_t>(field_name_buf.data(), field_name_buf.size()));
                    auto field_entry_ty = llvm::StructType::get(
                        llvm_ctx,
                        {ptr_type_l, llvm::Type::getInt32Ty(llvm_ctx),
                         llvm::Type::getInt32Ty(llvm_ctx), llvm::Type::getInt32Ty(llvm_ctx),
                         llvm::ArrayType::get(i8_ty, field_name_buf.size())},
                        false);
                    auto field_type_info = llvm::ConstantExpr::getPointerCast(
                        ensure_type_info_global(field->resolved_type, false), ptr_type_l);
                    field_table_consts.push_back(llvm::ConstantStruct::get(
                        field_entry_ty,
                        {field_type_info,
                         llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx),
                                                (int32_t)layout->getElementOffset(field->field_index)),
                         llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx),
                                                (int32_t)field->get_visibility()),
                         llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), name_len),
                         field_name_l}));
                    field_table_len = new_len;
                }
            }
        } else if (final_type->kind == TypeKind::Tuple) {
            auto &elements = final_type->data.tuple.elements;
            if (elements.size() > 0) {
                auto layout = llvm_module.getDataLayout().getStructLayout((llvm::StructType *)type_l);
                for (int i = 0; i < elements.size(); i++) {
                    auto elem_type = elements[i];
                    auto field_name = std::to_string(i);
                    auto name_len = (uint32_t)field_name.size();
                    if (name_len > sizeof(TypeFieldEntry::name)) {
                        name_len = sizeof(TypeFieldEntry::name);
                    }
                    std::vector<uint8_t> field_name_buf(sizeof(TypeFieldEntry::name), 0);
                    if (name_len > 0) {
                        memcpy(field_name_buf.data(), field_name.data(), name_len);
                    }
                    auto field_name_l = llvm::ConstantDataArray::get(
                        llvm_ctx, llvm::ArrayRef<uint8_t>(field_name_buf.data(), field_name_buf.size()));
                    auto field_entry_ty = llvm::StructType::get(
                        llvm_ctx,
                        {ptr_type_l, llvm::Type::getInt32Ty(llvm_ctx),
                         llvm::Type::getInt32Ty(llvm_ctx), llvm::Type::getInt32Ty(llvm_ctx),
                         llvm::ArrayType::get(i8_ty, field_name_buf.size())},
                        false);
                    auto field_type_info = llvm::ConstantExpr::getPointerCast(
                        ensure_type_info_global(elem_type, false), ptr_type_l);
                    field_table_consts.push_back(llvm::ConstantStruct::get(
                        field_entry_ty,
                        {field_type_info,
                         llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx),
                                                (int32_t)layout->getElementOffset(i)),
                         llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx),
                                                (int32_t)Visibility::Public),
                         llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), name_len),
                         field_name_l}));
                    field_table_len++;
                }
            }
        } else if (final_type->kind == TypeKind::FixedArray) {
            auto elem_type = final_type->data.fixed_array.elem;
            auto elem_llvm_type = compile_type(elem_type);
            auto elem_size =
                (int32_t)llvm_module.getDataLayout().getTypeAllocSize(elem_llvm_type);
            for (uint32_t i = 0; i < final_type->data.fixed_array.size; i++) {
                auto field_name = std::to_string(i);
                auto name_len = (uint32_t)field_name.size();
                if (name_len > sizeof(TypeFieldEntry::name)) {
                    name_len = sizeof(TypeFieldEntry::name);
                }
                std::vector<uint8_t> field_name_buf(sizeof(TypeFieldEntry::name), 0);
                if (name_len > 0) {
                    memcpy(field_name_buf.data(), field_name.data(), name_len);
                }
                auto field_name_l = llvm::ConstantDataArray::get(
                    llvm_ctx, llvm::ArrayRef<uint8_t>(field_name_buf.data(), field_name_buf.size()));
                auto field_entry_ty = llvm::StructType::get(
                    llvm_ctx,
                    {ptr_type_l, llvm::Type::getInt32Ty(llvm_ctx),
                     llvm::Type::getInt32Ty(llvm_ctx), llvm::Type::getInt32Ty(llvm_ctx),
                     llvm::ArrayType::get(i8_ty, field_name_buf.size())},
                    false);
                auto field_type_info = llvm::ConstantExpr::getPointerCast(
                    ensure_type_info_global(elem_type, false), ptr_type_l);
                field_table_consts.push_back(llvm::ConstantStruct::get(
                    field_entry_ty,
                    {field_type_info,
                     llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx),
                                            (int32_t)(i * elem_size)),
                     llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx),
                                            (int32_t)Visibility::Public),
                     llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), name_len),
                     field_name_l}));
                field_table_len++;
            }
        }
    }

    auto meta_table_size = sizeof(TypeMetaEntry) * meta_table_len;
    llvm::Constant *meta_table_ptr_l = get_null_ptr();
    if (meta_table_size > 0) {
        auto ti_meta_table_l = llvm::ConstantDataArray::get(
            llvm_ctx, llvm::ArrayRef<uint8_t>((uint8_t *)meta_table_data, meta_table_size));
        auto ti_meta_table_type_l = llvm::ArrayType::get(i8_ty, meta_table_size);
        auto meta_global =
            new llvm::GlobalVariable(llvm_module, ti_meta_table_type_l, true,
                                     llvm::GlobalValue::PrivateLinkage, ti_meta_table_l,
                                     "typeinfo.meta." + get_resolver()->format_type_display(type));
        meta_table_ptr_l =
            llvm::ConstantExpr::getPointerCast(meta_global, ptr_type_l);
    }
    free(meta_table_data);

    llvm::Constant *field_table_ptr_l = get_null_ptr();
    if (field_table_len > 0) {
        auto field_entry_ty = field_table_consts[0]->getType();
        auto ti_field_table_type_l = llvm::ArrayType::get(field_entry_ty, field_table_len);
        auto ti_field_table_l = llvm::ConstantArray::get(
            ti_field_table_type_l, field_table_consts);
        auto field_global =
            new llvm::GlobalVariable(llvm_module, ti_field_table_type_l, true,
                                     llvm::GlobalValue::PrivateLinkage, ti_field_table_l,
                                     "typeinfo.fields." + get_resolver()->format_type_display(type));
        field_table_ptr_l =
            llvm::ConstantExpr::getPointerCast(field_global, ptr_type_l);
    }

    auto typesize = llvm_type_size(type_l);

    // For reference/pointer types, store elem TypeInfo* in the data field.
    // For other types, serialize the data as raw bytes.
    llvm::Constant *typedata_l;
    auto array_elem_type = get_reflect_array_elem_type(get_resolver(), reflect_type);
    if (reflect_kind == TypeKind::Array && array_elem_type) {
        std::vector<llvm::Constant *> word_consts = {
            llvm::ConstantExpr::getPtrToInt(ensure_type_info_global(final_type, false),
                                            llvm::Type::getInt64Ty(llvm_ctx)),
            llvm::ConstantExpr::getPtrToInt(ensure_type_info_global(array_elem_type, false),
                                            llvm::Type::getInt64Ty(llvm_ctx))};
        typedata_l = llvm::ConstantArray::get(tidata_type_l, word_consts);
    } else if ((final_type->kind == TypeKind::Reference || final_type->kind == TypeKind::MutRef ||
                final_type->kind == TypeKind::MoveRef || final_type->kind == TypeKind::Pointer ||
                final_type->kind == TypeKind::Optional) &&
               final_type->get_elem()) {
        uint64_t elem_offset = 0;
        if (final_type->kind == TypeKind::Optional) {
            auto layout = llvm_module.getDataLayout().getStructLayout((llvm::StructType *)type_l);
            elem_offset = layout->getElementOffset(1);
        }
        std::vector<llvm::Constant *> word_consts = {
            llvm::ConstantExpr::getPtrToInt(ensure_type_info_global(final_type->get_elem(), false),
                                            llvm::Type::getInt64Ty(llvm_ctx)),
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvm_ctx), elem_offset)};
        typedata_l = llvm::ConstantArray::get(tidata_type_l, word_consts);
    } else {
        auto typedata = (uint8_t *)&final_type->data;
        std::array<uint64_t, tidata_word_count> words = {};
        memcpy(words.data(), typedata, sizeof(TypeInfoData));
        std::vector<llvm::Constant *> word_consts;
        word_consts.reserve(words.size());
        for (auto word : words) {
            word_consts.push_back(llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvm_ctx), word));
        }
        typedata_l = llvm::ConstantArray::get(tidata_type_l, word_consts);
    }

    // Generate destructor/copier wrappers for any type-erasure
    auto dtor_fn = generate_any_destructor(final_type);
    llvm::Constant *dtor_ptr = dtor_fn ? (llvm::Constant *)dtor_fn->llvm_fn : get_null_ptr();
    auto copy_fn = generate_any_copier(final_type);
    llvm::Constant *copy_ptr = copy_fn ? (llvm::Constant *)copy_fn->llvm_fn : get_null_ptr();

    std::vector<uint8_t> type_name_buf(sizeof(TypeInfo::name), 0);
    auto type_name = get_resolver()->format_type_display(reflect_kind == TypeKind::Array ? reflect_type : final_type);
    auto type_name_len = (uint32_t)type_name.size();
    if (type_name_len > type_name_buf.size()) {
        type_name_len = (uint32_t)type_name_buf.size();
    }
    if (type_name_len > 0) {
        memcpy(type_name_buf.data(), type_name.data(), type_name_len);
    }
    auto type_name_l = llvm::ConstantDataArray::get(
        llvm_ctx, llvm::ArrayRef<uint8_t>(type_name_buf.data(), type_name_buf.size()));

    auto ti_type_l = llvm::StructType::get(
        llvm_ctx,
        {llvm::Type::getInt32Ty(llvm_ctx), llvm::Type::getInt32Ty(llvm_ctx), tidata_type_l,
         ptr_type_l, ptr_type_l, llvm::Type::getInt32Ty(llvm_ctx), ptr_type_l,
         llvm::Type::getInt32Ty(llvm_ctx), ptr_type_l, llvm::Type::getInt32Ty(llvm_ctx),
         llvm::ArrayType::get(i8_ty, type_name_buf.size())},
        false);

    auto info_l = llvm::ConstantStruct::get(
        ti_type_l,
        {
            /* kind */
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), (int32_t)reflect_kind),
            /* typesize */
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), (int32_t)typesize),
            /* data */
            typedata_l,
            /* destructor */
            dtor_ptr,
            /* copier */
            copy_ptr,
            /* meta_table_len */
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), meta_table_len),
            /* meta_table */
            meta_table_ptr_l,
            /* field_table_len */
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), field_table_len),
            /* field_table */
            field_table_ptr_l,
            /* name_len */
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), type_name_len),
            /* name */
            type_name_l,
        });
    return info_l;
}

void Compiler::finalize_pending_typeinfos() {
    while (m_ctx->pending_typeinfo_keys.size()) {
        auto keys = m_ctx->pending_typeinfo_keys;
        m_ctx->pending_typeinfo_keys.clear();
        for (auto &key : keys) {
            auto item_p = m_ctx->pending_typeinfo_items.get(key);
            auto global_p = m_ctx->typeinfo_table.get(key);
            if (!item_p || !global_p) {
                continue;
            }
            auto initializer = build_type_info_initializer(item_p->reflect_type);
            (*global_p)->setInitializer(initializer);
            m_ctx->pending_typeinfo_items.unset(key);
        }
    }
}

llvm::Value *Compiler::compile_type_info(ChiType *type) {
    return ensure_type_info_global(type, true);
}

// ============================================================================
// __CxLambda construction helpers
// ============================================================================

Compiler::CxLambdaInit Compiler::compile_cxlambda_init(Function *fn, llvm::Value *fn_ptr,
                                                       uint32_t capture_size) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto ptr_type = llvm::PointerType::get(llvm_ctx, 0);

    auto rt_lambda = get_resolver()->get_context()->rt_lambda_type;
    assert(rt_lambda && "__CxLambda type not found in runtime");
    auto struct_type_l = compile_type(rt_lambda);
    auto alloca_ptr = fn->entry_alloca(struct_type_l, "lambda");

    // Call generated constructor to initialize default field values
    auto generated_ctor = generate_constructor(rt_lambda, nullptr);
    if (generated_ctor) {
        builder.CreateCall(generated_ctor->llvm_fn, {alloca_ptr});
    }

    // Call __CxLambda.new(fn_ptr, size)
    std::optional<TypeId> variant_type_id = std::nullopt;
    auto new_member = rt_lambda->data.struct_.find_member("new");
    assert(new_member && "new() method not found in __CxLambda");
    auto new_method_node = get_variant_member_node(new_member, variant_type_id);
    auto new_method = get_fn(new_method_node);

    auto fn_ptr_cast = builder.CreateBitCast(fn_ptr, ptr_type);
    auto size_value = llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), capture_size);
    builder.CreateCall(new_method->llvm_fn, {alloca_ptr, fn_ptr_cast, size_value});

    return {alloca_ptr, struct_type_l};
}

Compiler::CxCaptureInfo Compiler::compile_cxcapture_create(uint32_t payload_size,
                                                           llvm::Value *type_info,
                                                           llvm::Value *dtor) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    auto capture_new_fn = get_system_fn("cx_capture_new");
    auto size_l = llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), payload_size);
    auto capture_ptr = builder.CreateCall(capture_new_fn->llvm_fn, {size_l, type_info, dtor});

    auto capture_get_data_fn = get_system_fn("cx_capture_get_data");
    auto payload_data_ptr = builder.CreateCall(capture_get_data_fn->llvm_fn, {capture_ptr});

    return {capture_ptr, payload_data_ptr};
}

void Compiler::compile_cxlambda_set_captures(llvm::Value *lambda_alloca, llvm::Value *capture_ptr) {
    auto &builder = *m_ctx->llvm_builder;

    auto rt_lambda = get_resolver()->get_context()->rt_lambda_type;
    assert(rt_lambda && "__CxLambda type not found in runtime");

    std::optional<TypeId> variant_type_id = std::nullopt;
    auto set_captures_ptr_member = rt_lambda->data.struct_.find_member("set_captures_ptr");
    assert(set_captures_ptr_member && "set_captures_ptr() method not found in __CxLambda");
    auto set_captures_ptr_node = get_variant_member_node(set_captures_ptr_member, variant_type_id);
    auto set_captures_ptr_method = get_fn(set_captures_ptr_node);
    builder.CreateCall(set_captures_ptr_method->llvm_fn, {lambda_alloca, capture_ptr});
}

llvm::Value *Compiler::compile_lambda_alloc(Function *fn, ChiType *lambda_type, llvm::Value *fn_ptr,
                                            array<ast::FnCapture> *captures) {
    auto &builder = *(m_ctx->llvm_builder.get());

    // Determine size and prepare fn_ptr
    llvm::Value *final_fn_ptr = nullptr;
    uint32_t bind_size = 0;

    if (captures && captures->size()) {
        auto bstruct = lambda_type->data.fn_lambda.bind_struct;
        assert(bstruct);
        auto bstruct_l = compile_type(bstruct);
        bind_size = llvm_type_size(bstruct_l);
        final_fn_ptr = fn_ptr;
    } else {
        final_fn_ptr = generate_lambda_proxy_function(fn, fn_ptr, lambda_type, nullptr);
    }

    // Initialize __CxLambda struct
    auto [var, struct_type_l] = compile_cxlambda_init(fn, final_fn_ptr, bind_size);

    // For lambdas with captures, set the data field
    if (captures && captures->size()) {
        auto bstruct = lambda_type->data.fn_lambda.bind_struct;
        auto bstruct_l = (llvm::StructType *)compile_type(bstruct);

        // Create bind_struct on stack to hold captures
        auto bind_var = builder.CreateAlloca(bstruct_l, nullptr, "bind_struct");

        // Store captures into bind_struct
        for (int i = 0; i < captures->size(); i++) {
            auto &cap = (*captures)[i];
            auto capture = cap.decl;
            auto capture_gep = builder.CreateStructGEP(bstruct_l, bind_var, i);
            auto capture_flag_gep = builder.CreateStructGEP(bstruct_l, bind_var, captures->size() + i);

            // Get source address of the captured variable
            llvm::Value *src_addr = nullptr;
            llvm::Value *src_move_flag = nullptr;
            auto &current_captures = fn->node->data.fn_def.captures;
            for (int j = 0; j < current_captures.size(); j++) {
                if (current_captures[j].decl == capture && fn->bind_ptr) {
                    auto current_fn_type = get_chitype(fn->node);
                    if (current_fn_type->kind == TypeKind::FnLambda) {
                        auto current_bstruct = current_fn_type->data.fn_lambda.bind_struct;
                        auto current_bstruct_l = (llvm::StructType *)compile_type(current_bstruct);
                        auto nested_gep =
                            builder.CreateStructGEP(current_bstruct_l, fn->bind_ptr, j);
                        if (current_captures[j].mode == ast::CaptureMode::ByValue) {
                            src_addr = nested_gep;
                        } else {
                            src_addr =
                                builder.CreateLoad(current_bstruct_l->elements()[j], nested_gep);
                            auto nested_flag_gep = builder.CreateStructGEP(
                                current_bstruct_l, fn->bind_ptr, current_captures.size() + j);
                            src_move_flag = builder.CreateLoad(
                                current_bstruct_l->elements()[current_captures.size() + j],
                                nested_flag_gep);
                        }
                        break;
                    }
                }
            }

            if (!src_addr) {
                if (m_ctx->var_table.get(capture)) {
                    src_addr = get_var(capture);
                }
            }

            if (cap.mode == ast::CaptureMode::ByValue) {
                // By-value: copy value directly into bind struct field
                assert(src_addr && "by-value capture source not found");
                auto value_type = capture->resolved_type;
                auto value_type_l = compile_type(value_type);
                auto val = builder.CreateLoad(value_type_l, src_addr);
                auto dbg_loc = builder.getCurrentDebugLocation();
                compile_copy(fn, val, capture_gep, value_type, nullptr);
                builder.SetCurrentDebugLocation(dbg_loc);
            } else {
                // By-reference: store pointer to original variable
                if (!src_addr) {
                    auto capture_type = compile_type(capture->resolved_type);
                    src_addr = llvm::Constant::getNullValue(capture_type);
                }
                builder.CreateStore(src_addr, capture_gep);
            }

            if (!src_move_flag && cap.mode == ast::CaptureMode::ByRef &&
                m_ctx->drop_flags.has_key(capture)) {
                src_move_flag = m_ctx->drop_flags[capture];
            }
            auto *flag_field_type = bstruct_l->getElementType(captures->size() + i);
            if (!src_move_flag) {
                src_move_flag = llvm::Constant::getNullValue(flag_field_type);
            }
            builder.CreateStore(src_move_flag, capture_flag_gep);
        }

        // Monomorphize bstruct before generating dtor — it may still hold generic placeholders.
        auto eval_bstruct = eval_type(bstruct);
        llvm::Value *dtor_ptr = llvm::ConstantPointerNull::get(builder.getInt8PtrTy());
        if (get_resolver()->type_needs_destruction(eval_bstruct)) {
            if (auto dtor = generate_destructor(eval_bstruct, nullptr)) {
                dtor_ptr = builder.CreateBitCast(dtor->llvm_fn, builder.getInt8PtrTy());
            }
        }

        // Allocate type-erased capture box and store bind struct into it
        auto captures_ti = compile_type_info(bstruct);
        auto captures_ti_ptr = builder.CreateBitCast(captures_ti, builder.getInt8PtrTy());

        auto [capture_ptr, payload_data_ptr] =
            compile_cxcapture_create(bind_size, captures_ti_ptr, dtor_ptr);

        auto bind_struct_value = builder.CreateLoad(bstruct_l, bind_var);
        auto payload_typed_ptr = builder.CreateBitCast(payload_data_ptr, bstruct_l->getPointerTo());
        builder.CreateStore(bind_struct_value, payload_typed_ptr);

        compile_cxlambda_set_captures(var, capture_ptr);
    }

    return builder.CreateLoad(struct_type_l, var);
}

// ============================================================================
// Async/Await Codegen
// ============================================================================

bool Compiler::contains_unresolved_await(ast::Node *node, const std::set<ast::Node *> &resolved) {
    if (!node) {
        return false;
    }
    if (node->type == ast::NodeType::AwaitExpr) {
        return !resolved.count(node);
    }
    return cx::visit_async_children(node, false, [&](ast::Node *child) {
        return contains_unresolved_await(child, resolved);
    });
}

AwaitSite Compiler::find_unresolved_await_site(ast::Node *node,
                                               const std::set<ast::Node *> &resolved) {
    if (!node) {
        return {};
    }
    if (node->type == ast::NodeType::AwaitExpr) {
        if (resolved.count(node)) {
            return {};
        }
        return {node, node};
    }
    if (node->type == ast::NodeType::TryExpr) {
        auto try_expr = node->data.try_expr.resolved_expr ? node->data.try_expr.resolved_expr : node->data.try_expr.expr;
        auto site = find_unresolved_await_site(try_expr, resolved);
        if (site.await_expr && site.resume_expr &&
            site.resume_expr->type != ast::NodeType::TryExpr) {
            site.resume_expr = node;
        }
        return site;
    }
    AwaitSite site = {};
    cx::visit_async_children(node, false, [&](ast::Node *child) {
        site = find_unresolved_await_site(child, resolved);
        return site.await_expr != nullptr;
    });
    return site;
}

ChiType *Compiler::get_async_resume_value_type(ast::Node *root, ast::Node *await_expr,
                                               ast::Node *resume_expr) {
    if (!await_expr) {
        return nullptr;
    }
    std::function<ChiType *(ast::Node *, bool)> find_type = [&](ast::Node *node, bool in_try) -> ChiType * {
        if (!node) {
            return nullptr;
        }
        if (node == await_expr) {
            if (in_try) {
                return get_resolver()->get_result_type(
                    get_chitype(await_expr),
                    get_resolver()->get_shared_type(get_resolver()->get_context()->rt_error_type));
            }
            return get_chitype(await_expr);
        }
        if (node->type == ast::NodeType::TryExpr) {
            auto try_expr = node->data.try_expr.resolved_expr ? node->data.try_expr.resolved_expr : node->data.try_expr.expr;
            return find_type(try_expr, true);
        }
        ChiType *found = nullptr;
        cx::visit_async_children(node, false, [&](ast::Node *child) {
            found = find_type(child, in_try);
            return found != nullptr;
        });
        return found;
    };
    return find_type(root ? root : await_expr, false);
}

void Compiler::collect_async_frame_nodes(ast::Node *node, std::vector<ast::Node *> &vars,
                                         std::vector<ast::Node *> &awaits,
                                         std::set<ast::Node *> &seen_vars,
                                         std::set<ast::Node *> &seen_awaits) {
    if (!node) {
        return;
    }
    if (node->type == ast::NodeType::AwaitExpr) {
        if (!seen_awaits.count(node)) {
            seen_awaits.insert(node);
            awaits.push_back(node);
        }
        collect_async_frame_nodes(node->data.await_expr.expr, vars, awaits, seen_vars, seen_awaits);
        return;
    }
    if (node->type == ast::NodeType::VarDecl || node->type == ast::NodeType::ParamDecl) {
        if (!seen_vars.count(node)) {
            seen_vars.insert(node);
            vars.push_back(node);
        }
    }
    if (node->type == ast::NodeType::ForStmt &&
        node->data.for_stmt.effective_kind() == ast::ForLoopKind::IntRange &&
        node->data.for_stmt.expr &&
        node->data.for_stmt.expr->type == ast::NodeType::RangeExpr) {
        auto &for_data = node->data.for_stmt;
        auto iter_node = node->data.for_stmt.bind ? node->data.for_stmt.bind : node;
        if (!seen_vars.count(iter_node)) {
            seen_vars.insert(iter_node);
            vars.push_back(iter_node);
        }
        if (for_data.index_bind && !seen_vars.count(for_data.index_bind)) {
            seen_vars.insert(for_data.index_bind);
            vars.push_back(for_data.index_bind);
        }
        auto end_expr = node->data.for_stmt.expr->data.range_expr.end;
        if (end_expr && !seen_vars.count(end_expr)) {
            seen_vars.insert(end_expr);
            vars.push_back(end_expr);
        }
    } else if (node->type == ast::NodeType::ForStmt) {
        auto &for_data = node->data.for_stmt;
        auto expr_type = for_data.expr ? get_chitype(for_data.expr) : nullptr;
        auto fixed_array_loop = expr_type && ((expr_type->kind == TypeKind::FixedArray) ||
                                              (expr_type->is_reference() &&
                                               expr_type->get_elem()->kind == TypeKind::FixedArray));
        if (for_data.bind && !seen_vars.count(for_data.bind)) {
            seen_vars.insert(for_data.bind);
            vars.push_back(for_data.bind);
        }
        if (for_data.index_bind && !seen_vars.count(for_data.index_bind)) {
            seen_vars.insert(for_data.index_bind);
            vars.push_back(for_data.index_bind);
        }
        auto for_kind = for_data.effective_kind();
        if ((for_kind == ast::ForLoopKind::Range || for_kind == ast::ForLoopKind::Iter ||
             fixed_array_loop) && !seen_vars.count(node)) {
            seen_vars.insert(node);
            vars.push_back(node);
        }
    }
    if (node->type == ast::NodeType::Block) {
        for (auto implicit : node->data.block.implicit_vars) {
            collect_async_frame_nodes(implicit, vars, awaits, seen_vars, seen_awaits);
        }
        for (auto temp : node->data.block.stmt_temp_vars) {
            collect_async_frame_nodes(temp, vars, awaits, seen_vars, seen_awaits);
        }
    }
    cx::visit_async_children(node, true, [&](ast::Node *child) {
        collect_async_frame_nodes(child, vars, awaits, seen_vars, seen_awaits);
        return false;
    });
}

ChiType *Compiler::get_async_frame_node_type(ast::Node *node) {
    if (!node) {
        return nullptr;
    }
    if (node->type != ast::NodeType::ForStmt) {
        return get_chitype(node);
    }

    auto &data = node->data.for_stmt;
    auto kind = data.effective_kind();
    if (kind == ast::ForLoopKind::IntRange && data.expr &&
        data.expr->type == ast::NodeType::RangeExpr) {
        return data.bind ? get_chitype(data.bind) : get_chitype(data.expr->data.range_expr.start);
    }

    auto expr_type = data.expr ? get_chitype(data.expr) : nullptr;
    auto fixed_array_loop = expr_type && ((expr_type->kind == TypeKind::FixedArray) ||
                                          (expr_type->is_reference() &&
                                           expr_type->get_elem()->kind == TypeKind::FixedArray));
    if (fixed_array_loop) {
        return get_system_types()->uint32;
    }

    if (kind == ast::ForLoopKind::Range && data.expr) {
        auto sty = get_resolver()->resolve_struct_type(expr_type);
        auto beginp = sty ? sty->member_table.get("begin") : nullptr;
        return beginp ? (*beginp)->resolved_type->data.fn.return_type : nullptr;
    }

    if (kind == ast::ForLoopKind::Iter && data.expr) {
        auto sty = get_resolver()->resolve_struct_type(expr_type);
        auto iter_fn = sty ? sty->member_table.get("to_iter_mut") : nullptr;
        return iter_fn ? (*iter_fn)->resolved_type->data.fn.return_type : nullptr;
    }

    return get_chitype(node);
}

llvm::Value *Compiler::get_async_frame_field_ptr(Function *fn, AsyncStateMachine &machine,
                                                 ast::Node *node) {
    auto it = machine.frame_var_index.find(node);
    if (it == machine.frame_var_index.end()) {
        panic("async frame missing node '{}' type {}", node ? node->name : "<null>",
              node ? (int)node->type : -1);
    }
    return m_ctx->llvm_builder->CreateStructGEP(machine.frame_struct_type, machine.frame_ptr,
                                                it->second);
}

llvm::Value *Compiler::get_async_frame_var_alive_ptr(Function *fn, AsyncStateMachine &machine,
                                                     ast::Node *node) {
    auto it = machine.frame_var_alive_index.find(node);
    if (it == machine.frame_var_alive_index.end()) {
        return nullptr;
    }
    return m_ctx->llvm_builder->CreateStructGEP(machine.frame_struct_type, machine.frame_ptr,
                                                it->second);
}

// If `ptr` is `getelementptr machine.frame_ptr, 0, N`, return N. Otherwise -1.
// Used to recognize addresses that point at top-level async frame slots.
static int async_frame_field_index_of(llvm::Value *ptr, AsyncStateMachine &machine) {
    if (!ptr) {
        return -1;
    }
    auto stripped = ptr->stripPointerCasts();
    auto gep = llvm::dyn_cast<llvm::GEPOperator>(stripped);
    if (!gep || gep->getPointerOperand() != machine.frame_ptr) {
        return -1;
    }
    if (gep->getNumIndices() != 2) {
        return -1;
    }
    auto idx_it = gep->idx_begin();
    auto zero_index = llvm::dyn_cast<llvm::ConstantInt>(idx_it->get());
    ++idx_it;
    auto field = llvm::dyn_cast<llvm::ConstantInt>(idx_it->get());
    if (!zero_index || !zero_index->isZero() || !field) {
        return -1;
    }
    return (int)field->getSExtValue();
}

static bool is_same_async_frame_field_ptr(llvm::Value *ptr, AsyncStateMachine &machine,
                                          int field_index) {
    return async_frame_field_index_of(ptr, machine) == field_index;
}

llvm::Value *Compiler::get_async_frame_await_ptr(Function *fn, AsyncStateMachine &machine,
                                                 ast::Node *await_expr) {
    auto it = machine.frame_await_index.find(await_expr);
    assert(it != machine.frame_await_index.end());
    return m_ctx->llvm_builder->CreateStructGEP(machine.frame_struct_type, machine.frame_ptr,
                                                it->second);
}

llvm::Value *Compiler::get_async_frame_await_alive_ptr(Function *fn, AsyncStateMachine &machine,
                                                       ast::Node *await_expr) {
    auto it = machine.frame_await_alive_index.find(await_expr);
    if (it == machine.frame_await_alive_index.end()) {
        return nullptr;
    }
    return m_ctx->llvm_builder->CreateStructGEP(machine.frame_struct_type, machine.frame_ptr,
                                                it->second);
}

llvm::Value *Compiler::async_frame_alive_ptr_for_addr(Function *fn, llvm::Value *addr) {
    if (!fn->async_machine) {
        return nullptr;
    }
    auto &machine = *fn->async_machine;
    if (!machine.frame_struct_type || !machine.frame_ptr) {
        return nullptr;
    }
    int field_index = async_frame_field_index_of(addr, machine);
    if (field_index < 0) {
        return nullptr;
    }
    auto it = machine.alive_index_by_field.find(field_index);
    if (it == machine.alive_index_by_field.end()) {
        return nullptr;
    }
    return m_ctx->llvm_builder->CreateStructGEP(machine.frame_struct_type, machine.frame_ptr,
                                                it->second);
}

void Compiler::initialize_async_frame(Function *fn, AsyncStateMachine &machine, ast::Node *body) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    std::set<ast::Node *> seen_vars;
    std::set<ast::Node *> seen_awaits;
    if (fn->node && fn->node->type == ast::NodeType::FnDef && fn->get_def()->fn_proto) {
        for (auto param : fn->get_def()->fn_proto->data.fn_proto.params) {
            if (!seen_vars.count(param)) {
                seen_vars.insert(param);
                machine.frame_vars.push_back(param);
            }
        }
    }
    collect_async_frame_nodes(body, machine.frame_vars, machine.frame_awaits, seen_vars, seen_awaits);

    std::vector<llvm::Type *> frame_types;
    frame_types.push_back(machine.promise_struct_type);
    frame_types.push_back(builder.getInt8PtrTy());
    machine.frame_state_index = (int)frame_types.size();
    frame_types.push_back(builder.getInt32Ty());
    if (fn->fn_type && fn->fn_type->data.fn.container_ref && fn->get_bind_param()) {
        machine.bind_type = fn->fn_type->data.fn.container_ref;
        machine.frame_bind_index = (int)frame_types.size();
        frame_types.push_back(compile_type(machine.bind_type));
    }
    for (auto var : machine.frame_vars) {
        int field_index = (int)frame_types.size();
        machine.frame_var_index[var] = field_index;
        frame_types.push_back(compile_type(get_async_frame_node_type(var)));
        if (get_resolver()->type_needs_destruction(get_async_frame_node_type(var))) {
            int alive_index = (int)frame_types.size();
            machine.frame_var_alive_index[var] = alive_index;
            machine.alive_index_by_field[field_index] = alive_index;
            frame_types.push_back(builder.getInt1Ty());
        }
    }
    for (auto await_expr : machine.frame_awaits) {
        auto await_type = get_async_resume_value_type(body, await_expr);
        machine.frame_await_types[await_expr] = await_type;
        int field_index = (int)frame_types.size();
        machine.frame_await_index[await_expr] = field_index;
        frame_types.push_back(compile_type(await_type));
        if (get_resolver()->type_needs_destruction(await_type)) {
            int alive_index = (int)frame_types.size();
            machine.frame_await_alive_index[await_expr] = alive_index;
            machine.alive_index_by_field[field_index] = alive_index;
            frame_types.push_back(builder.getInt1Ty());
        }
    }

    // Shared<Error> slot for async try-catch: rejection forwarder stores error here
    // Only allocate when the function actually contains a try block
    if (fn->get_def()->has_try) {
        auto shared_error_type = get_resolver()->get_shared_type(get_resolver()->get_context()->rt_error_type);
        machine.async_error_index = (int)frame_types.size();
        frame_types.push_back(compile_type(shared_error_type));
        machine.async_error_alive_index = (int)frame_types.size();
        frame_types.push_back(builder.getInt1Ty());
    }

    machine.frame_struct_type = llvm::StructType::get(llvm_ctx, frame_types);
    auto frame_size = m_ctx->llvm_module->getDataLayout().getTypeAllocSize(machine.frame_struct_type);
    auto null_ti = llvm::ConstantPointerNull::get(builder.getInt8PtrTy());
    llvm::Value *dtor_ptr = llvm::ConstantPointerNull::get(builder.getInt8PtrTy());
    if (async_frame_needs_destruction(machine)) {
        if (auto dtor = generate_async_frame_destructor(fn, machine)) {
            dtor_ptr = builder.CreateBitCast(dtor->llvm_fn, builder.getInt8PtrTy());
        }
    }
    auto [capture_ptr, payload_ptr] = compile_cxcapture_create((uint32_t)frame_size, null_ti, dtor_ptr);
    machine.frame_capture_ptr = capture_ptr;
    machine.frame_ptr =
        builder.CreateBitCast(payload_ptr, machine.frame_struct_type->getPointerTo());
    builder.CreateMemSet(payload_ptr,
                         llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(llvm_ctx), 0),
                         frame_size, {});

    auto result_ptr =
        builder.CreateStructGEP(machine.frame_struct_type, machine.frame_ptr, 0);
    auto capture_ptr_slot =
        builder.CreateStructGEP(machine.frame_struct_type, machine.frame_ptr, 1);
    auto state_ptr =
        builder.CreateStructGEP(machine.frame_struct_type, machine.frame_ptr, machine.frame_state_index);
    builder.CreateStore(capture_ptr, capture_ptr_slot);
    builder.CreateStore(llvm::ConstantInt::get(builder.getInt32Ty(), 0), state_ptr);
    if (machine.frame_bind_index >= 0) {
        auto bind_ptr =
            builder.CreateStructGEP(machine.frame_struct_type, machine.frame_ptr, machine.frame_bind_index);
        builder.CreateStore(fn->get_this_arg(), bind_ptr);
    }
    builder.CreateCall(get_fn(get_variant_member_node(
                           get_resolver()->resolve_struct_type(machine.promise_type)->find_member("new"),
                           resolve_variant_type_id(fn, machine.promise_type)))
                           ->llvm_fn,
                       {result_ptr});
    machine.result_promise_ptr = result_ptr;

    for (const auto &param_info : fn->parameter_info) {
        if (param_info.kind != ParameterKind::Regular)
            continue;
        auto param = fn->get_def()->fn_proto->data.fn_proto.params[param_info.user_param_index];
        auto llvm_param = fn->llvm_fn->getArg(param_info.llvm_index);
        auto field_ptr = get_async_frame_field_ptr(fn, machine, param);
        builder.CreateStore(llvm_param, field_ptr);
        if (auto alive_ptr = get_async_frame_var_alive_ptr(fn, machine, param)) {
            builder.CreateStore(llvm::ConstantInt::getTrue(*m_ctx->llvm_ctx), alive_ptr);
        }
        add_var(param, field_ptr);
    }
}

bool Compiler::async_frame_needs_destruction(AsyncStateMachine &machine) {
    if (get_resolver()->type_needs_destruction(machine.promise_type)) {
        return true;
    }
    for (auto var : machine.frame_vars) {
        if (get_resolver()->type_needs_destruction(get_async_frame_node_type(var))) {
            return true;
        }
    }
    for (auto await_expr : machine.frame_awaits) {
        auto it = machine.frame_await_types.find(await_expr);
        if (it != machine.frame_await_types.end() &&
            get_resolver()->type_needs_destruction(it->second)) {
            return true;
        }
    }
    return false;
}

void Compiler::destroy_async_frame_slot_if_alive(Function *fn, llvm::Value *slot,
                                                 llvm::Value *alive_ptr, ChiType *type) {
    if (!alive_ptr) {
        compile_destruction_for_type(fn, slot, type);
        return;
    }

    auto &builder = *m_ctx->llvm_builder;
    auto alive = builder.CreateLoad(builder.getInt1Ty(), alive_ptr);
    auto destroy_b = fn->new_label("async_slot_drop");
    auto done_b = fn->new_label("async_slot_done");
    builder.CreateCondBr(alive, destroy_b, done_b);
    fn->use_label(destroy_b);
    compile_destruction_for_type(fn, slot, type);
    builder.CreateStore(llvm::ConstantInt::getFalse(*m_ctx->llvm_ctx), alive_ptr);
    builder.CreateBr(done_b);
    fn->use_label(done_b);
}

Function *Compiler::generate_async_frame_destructor(Function *fn, AsyncStateMachine &machine) {
    if (!async_frame_needs_destruction(machine)) {
        return nullptr;
    }

    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto &llvm_module = *m_ctx->llvm_module;

    auto ptr_type = llvm::PointerType::get(llvm_ctx, 0);
    auto fn_type = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type}, false);
    auto fn_name = fmt::format("{}__async_frame_dtor", fn->qualified_name);
    auto dtor_llvm_fn =
        llvm::Function::Create(fn_type, llvm::Function::PrivateLinkage, fn_name, llvm_module);
    auto dtor_fn = m_ctx->functions.emplace(new Function(m_ctx, dtor_llvm_fn, fn->node))->get();
    dtor_fn->qualified_name = fn_name;

    auto saved_ip = builder.saveIP();
    auto entry_b = dtor_fn->new_label("_entry");
    dtor_fn->use_label(entry_b);

    auto data_arg = dtor_llvm_fn->getArg(0);
    auto frame_ptr = builder.CreateBitCast(data_arg, machine.frame_struct_type->getPointerTo());

    for (int i = (int)machine.frame_awaits.size() - 1; i >= 0; i--) {
        auto await_expr = machine.frame_awaits[i];
        auto type_it = machine.frame_await_types.find(await_expr);
        if (type_it == machine.frame_await_types.end() ||
            !get_resolver()->type_needs_destruction(type_it->second)) {
            continue;
        }
        auto slot = builder.CreateStructGEP(machine.frame_struct_type, frame_ptr,
                                            machine.frame_await_index[await_expr]);
        llvm::Value *alive_ptr = nullptr;
        auto alive_it = machine.frame_await_alive_index.find(await_expr);
        if (alive_it != machine.frame_await_alive_index.end()) {
            alive_ptr = builder.CreateStructGEP(machine.frame_struct_type, frame_ptr, alive_it->second);
        }
        destroy_async_frame_slot_if_alive(dtor_fn, slot, alive_ptr, type_it->second);
    }

    for (int i = (int)machine.frame_vars.size() - 1; i >= 0; i--) {
        auto var = machine.frame_vars[i];
        auto var_type = get_async_frame_node_type(var);
        if (!get_resolver()->type_needs_destruction(var_type)) {
            continue;
        }
        auto slot = builder.CreateStructGEP(machine.frame_struct_type, frame_ptr,
                                            machine.frame_var_index[var]);
        llvm::Value *alive_ptr = nullptr;
        auto alive_it = machine.frame_var_alive_index.find(var);
        if (alive_it != machine.frame_var_alive_index.end()) {
            alive_ptr = builder.CreateStructGEP(machine.frame_struct_type, frame_ptr, alive_it->second);
        }
        destroy_async_frame_slot_if_alive(dtor_fn, slot, alive_ptr, var_type);
    }

    // Destroy async error slot (Shared<Error>) if alive
    if (machine.async_error_index >= 0) {
        auto shared_error_type = get_resolver()->get_shared_type(get_resolver()->get_context()->rt_error_type);
        auto err_slot = builder.CreateStructGEP(machine.frame_struct_type, frame_ptr,
                                                machine.async_error_index);
        auto alive_ptr = builder.CreateStructGEP(machine.frame_struct_type, frame_ptr,
                                                  machine.async_error_alive_index);
        destroy_async_frame_slot_if_alive(dtor_fn, err_slot, alive_ptr, shared_error_type);
    }

    if (get_resolver()->type_needs_destruction(machine.promise_type)) {
        auto promise_ptr = builder.CreateStructGEP(machine.frame_struct_type, frame_ptr, 0);
        compile_destruction_for_type(dtor_fn, promise_ptr, machine.promise_type);
    }

    builder.CreateRetVoid();
    llvm::verifyFunction(*dtor_llvm_fn);
    builder.restoreIP(saved_ip);
    return dtor_fn;
}

void Compiler::initialize_async_dispatcher(Function *fn, AsyncStateMachine &machine) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto &llvm_module = *m_ctx->llvm_module;

    auto ptr_type = llvm::PointerType::get(llvm_ctx, 0);
    auto fn_type = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type}, false);
    auto dispatch_name = fmt::format("{}__async_dispatch", fn->qualified_name);
    machine.dispatcher_fn = llvm::Function::Create(
        fn_type, llvm::Function::PrivateLinkage, dispatch_name, llvm_module);

    auto saved_ip = builder.saveIP();
    auto entry_b = llvm::BasicBlock::Create(llvm_ctx, "_entry", machine.dispatcher_fn);
    auto default_b = llvm::BasicBlock::Create(llvm_ctx, "_default", machine.dispatcher_fn);
    builder.SetInsertPoint(entry_b);

    auto data_arg = machine.dispatcher_fn->getArg(0);
    auto frame_ptr = builder.CreateBitCast(data_arg, machine.frame_struct_type->getPointerTo());
    auto state_ptr = builder.CreateStructGEP(machine.frame_struct_type, frame_ptr,
                                             machine.frame_state_index);
    auto state = builder.CreateLoad(builder.getInt32Ty(), state_ptr);
    machine.dispatcher_switch = builder.CreateSwitch(state, default_b, 0);

    builder.SetInsertPoint(default_b);
    builder.CreateUnreachable();

    builder.restoreIP(saved_ip);
}

llvm::Value *Compiler::get_async_frame_state_ptr(Function *fn, AsyncStateMachine &machine) {
    return m_ctx->llvm_builder->CreateStructGEP(machine.frame_struct_type, machine.frame_ptr,
                                                machine.frame_state_index);
}

AsyncLambdaValue Compiler::build_async_frame_lambda(Function *fn, AsyncStateMachine &machine,
                                                    llvm::Function *target_fn) {
    auto &builder = *m_ctx->llvm_builder;
    auto capture_slot =
        builder.CreateStructGEP(machine.frame_struct_type, machine.frame_ptr, 1);
    auto capture_ptr = builder.CreateLoad(builder.getInt8PtrTy(), capture_slot);
    builder.CreateCall(get_system_fn("cx_capture_retain")->llvm_fn, {capture_ptr});
    auto frame_size = m_ctx->llvm_module->getDataLayout().getTypeAllocSize(machine.frame_struct_type);
    auto [lambda_alloca, lambda_struct_type_l] =
        compile_cxlambda_init(fn, target_fn, (uint32_t)frame_size);
    compile_cxlambda_set_captures(lambda_alloca, capture_ptr);
    return {lambda_alloca, builder.CreateLoad(lambda_struct_type_l, lambda_alloca)};
}

AsyncLambdaValue Compiler::build_async_resume_forwarder_lambda(Function *fn,
                                                               AsyncStateMachine &machine,
                                                               int state_id, ChiType *value_type,
                                                               ast::Node *await_expr) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto &llvm_module = *m_ctx->llvm_module;

    auto ptr_type = llvm::PointerType::get(llvm_ctx, 0);
    auto value_type_l = compile_type(value_type);
    auto fn_type = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type, value_type_l}, false);
    auto fwd_name =
        fmt::format("{}__async_resume_{}", fn->qualified_name, (uintptr_t)await_expr);
    auto fwd_llvm_fn =
        llvm::Function::Create(fn_type, llvm::Function::PrivateLinkage, fwd_name, llvm_module);
    auto fwd_fn = m_ctx->functions.emplace(new Function(m_ctx, fwd_llvm_fn, fn->node))->get();
    fwd_fn->qualified_name = fwd_name;

    auto saved_ip = builder.saveIP();
    auto entry_b = fwd_fn->new_label("_entry");
    fwd_fn->use_label(entry_b);

    auto data_arg = fwd_llvm_fn->getArg(0);
    auto value_arg = fwd_llvm_fn->getArg(1);
    auto previous_frame_ptr = machine.frame_ptr;
    machine.frame_ptr = builder.CreateBitCast(data_arg, machine.frame_struct_type->getPointerTo());
    write_async_frame_await_value(fwd_fn, machine, await_expr,
                                  RefValue::from_owned_value(value_arg), value_type, await_expr);

    auto state_ptr = get_async_frame_state_ptr(fwd_fn, machine);
    builder.CreateStore(llvm::ConstantInt::get(builder.getInt32Ty(), state_id), state_ptr);

    emit_dbg_location(fn->node);
    builder.CreateCall(machine.dispatcher_fn, {data_arg});
    builder.CreateRetVoid();

    llvm::verifyFunction(*fwd_llvm_fn);
    machine.frame_ptr = previous_frame_ptr;
    builder.restoreIP(saved_ip);
    return build_async_frame_lambda(fn, machine, fwd_llvm_fn);
}

AsyncLambdaValue Compiler::build_async_try_await_forwarder_lambda(Function *fn,
                                                                  AsyncStateMachine &machine,
                                                                  int state_id,
                                                                  ChiType *settled_type,
                                                                  ast::Node *await_expr,
                                                                  bool is_error) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto &llvm_module = *m_ctx->llvm_module;

    auto settled_enum = get_resolver()->resolve_subtype(settled_type, await_expr);
    assert(settled_enum && settled_enum->kind == TypeKind::Enum);

    ChiType *arg_type = is_error
                            ? get_resolver()->get_shared_type(get_resolver()->get_context()->rt_error_type)
                            : get_chitype(await_expr);
    auto ptr_type = llvm::PointerType::get(llvm_ctx, 0);
    auto arg_type_l = compile_type(arg_type);
    auto fn_type = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type, arg_type_l}, false);
    auto fwd_name = fmt::format("{}__try_await_{}_{}",
                                fn->qualified_name, is_error ? "err" : "ok",
                                (uintptr_t)await_expr);
    auto fwd_llvm_fn =
        llvm::Function::Create(fn_type, llvm::Function::PrivateLinkage, fwd_name, llvm_module);
    auto fwd_fn = m_ctx->functions.emplace(new Function(m_ctx, fwd_llvm_fn, fn->node))->get();
    fwd_fn->qualified_name = fwd_name;

    auto saved_ip = builder.saveIP();
    auto entry_b = fwd_fn->new_label("_entry");
    fwd_fn->use_label(entry_b);

    auto data_arg = fwd_llvm_fn->getArg(0);
    auto value_arg = fwd_llvm_fn->getArg(1);
    auto settled_type_l = compile_type(settled_type);
    auto settled_var = fwd_fn->entry_alloca(settled_type_l, "settled");
    auto settled_size = m_ctx->llvm_module->getDataLayout().getTypeAllocSize(settled_type_l);
    builder.CreateMemSet(settled_var,
                         llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(llvm_ctx), 0),
                         settled_size, {});

    auto variant_member = settled_enum->data.enum_.find_member(is_error ? "Err" : "Ok");
    assert(variant_member && variant_member->resolved_type &&
           variant_member->resolved_type->kind == TypeKind::EnumValue);
    auto enum_variant_p = m_ctx->enum_variant_table.get(variant_member);
    assert(enum_variant_p);
    auto enum_var = *enum_variant_p;
    auto copy_size = llvm_type_size(((llvm::GlobalVariable *)enum_var)->getValueType());
    builder.CreateMemCpy(settled_var, {}, enum_var, {}, copy_size);

    auto payload_fields = get_resolver()->get_enum_payload_fields(variant_member->resolved_type);
    if (payload_fields.size() > 0) {
        auto payload_ptr = compile_dot_access(fwd_fn, settled_var, variant_member->resolved_type,
                                              payload_fields[0]);
        compile_copy_with_ref(fwd_fn, RefValue::from_owned_value(value_arg), payload_ptr,
                              payload_fields[0]->resolved_type, await_expr, true);
    }

    auto previous_frame_ptr = machine.frame_ptr;
    machine.frame_ptr = builder.CreateBitCast(data_arg, machine.frame_struct_type->getPointerTo());
    write_async_frame_await_value(fwd_fn, machine, await_expr, RefValue::from_address(settled_var),
                                  settled_type, await_expr);
    if (get_resolver()->type_needs_destruction(settled_type)) {
        compile_destruction_for_type(fwd_fn, settled_var, settled_type);
    }
    auto state_ptr = get_async_frame_state_ptr(fwd_fn, machine);
    builder.CreateStore(llvm::ConstantInt::get(builder.getInt32Ty(), state_id), state_ptr);
    emit_dbg_location(fn->node);
    builder.CreateCall(machine.dispatcher_fn, {data_arg});
    builder.CreateRetVoid();

    llvm::verifyFunction(*fwd_llvm_fn);
    machine.frame_ptr = previous_frame_ptr;
    builder.restoreIP(saved_ip);
    return build_async_frame_lambda(fn, machine, fwd_llvm_fn);
}

void Compiler::collect_vars_used_in_node(ast::Node *node, std::set<ast::Node *> &vars) {
    if (!node)
        return;

    switch (node->type) {
    case ast::NodeType::Identifier: {
        auto decl = node->data.identifier.decl;
        if (decl && (decl->type == ast::NodeType::VarDecl ||
                     decl->type == ast::NodeType::ParamDecl)) {
            vars.insert(decl);
        }
        break;
    }
    case ast::NodeType::AwaitExpr:
        collect_vars_used_in_node(node->data.await_expr.expr, vars);
        break;
    default:
        cx::visit_async_children(node, true, [&](ast::Node *child) {
            collect_vars_used_in_node(child, vars);
            return false;
        });
        break;
    }
}

AsyncLambdaValue Compiler::build_async_rejection_forwarder_lambda(Function *fn,
                                                                  AsyncStateMachine &machine) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto &llvm_module = *m_ctx->llvm_module;
    auto ptr_type = llvm::PointerType::get(llvm_ctx, 0);

    auto promise_struct = get_resolver()->resolve_struct_type(machine.promise_type);
    auto variant_type_id = resolve_variant_type_id(fn, machine.promise_type);
    auto reject_shared_member = promise_struct->find_member("reject_shared");
    assert(reject_shared_member && "Promise.reject_shared() method not found");
    auto reject_shared_node = get_variant_member_node(reject_shared_member, variant_type_id);
    auto reject_shared_fn = get_fn(reject_shared_node);
    auto shared_error_type_l = reject_shared_fn->llvm_fn->getFunctionType()->getParamType(1);

    auto fwd_fn_type = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx),
                                               {ptr_type, shared_error_type_l}, false);
    auto fwd_name = fmt::format("{}__async_reject_fwd", fn->qualified_name);
    auto fwd_llvm_fn =
        llvm::Function::Create(fwd_fn_type, llvm::Function::PrivateLinkage, fwd_name, llvm_module);
    auto fwd_fn = m_ctx->functions.emplace(new Function(m_ctx, fwd_llvm_fn, fn->node))->get();
    fwd_fn->qualified_name = fwd_name;

    auto saved_ip = builder.saveIP();
    auto entry_b = fwd_fn->new_label("_entry");
    fwd_fn->use_label(entry_b);

    auto data_arg = fwd_llvm_fn->getArg(0);
    auto err_arg = fwd_llvm_fn->getArg(1);
    auto previous_frame_ptr = machine.frame_ptr;
    machine.frame_ptr = builder.CreateBitCast(data_arg, machine.frame_struct_type->getPointerTo());

    if (machine.active_try_catch_state_id >= 0 && machine.async_error_index >= 0) {
        // Inside a try-block: store the Shared<Error> in the frame error slot
        // and transition to the catch state instead of rejecting the promise
        auto err_slot = builder.CreateStructGEP(machine.frame_struct_type, machine.frame_ptr,
                                                machine.async_error_index);
        builder.CreateStore(err_arg, err_slot);
        // Mark the error slot as alive
        auto alive_slot = builder.CreateStructGEP(machine.frame_struct_type, machine.frame_ptr,
                                                   machine.async_error_alive_index);
        builder.CreateStore(llvm::ConstantInt::getTrue(llvm_ctx), alive_slot);
        // Transition to catch state
        auto state_ptr = get_async_frame_state_ptr(fwd_fn, machine);
        builder.CreateStore(
            llvm::ConstantInt::get(builder.getInt32Ty(), machine.active_try_catch_state_id),
            state_ptr);
        emit_dbg_location(fn->node);
        auto frame_data_ptr =
            builder.CreateBitCast(machine.frame_ptr, llvm::PointerType::get(llvm_ctx, 0));
        builder.CreateCall(machine.dispatcher_fn, {frame_data_ptr});
    } else {
        auto promise_ptr =
            builder.CreateStructGEP(machine.frame_struct_type, machine.frame_ptr, 0);
        emit_dbg_location(fn->node);
        builder.CreateCall(reject_shared_fn->llvm_fn, {promise_ptr, err_arg});
    }
    builder.CreateRetVoid();

    llvm::verifyFunction(*fwd_llvm_fn);
    machine.frame_ptr = previous_frame_ptr;
    builder.restoreIP(saved_ip);
    return build_async_frame_lambda(fn, machine, fwd_llvm_fn);
}

void Compiler::sync_async_frame_var(Function *fn, AsyncStateMachine &machine, ast::Node *var,
                                    map<ast::Node *, llvm::Value *> &local_vars) {
    if (!machine.frame_var_index.count(var)) {
        local_vars[var] = get_var(var);
        return;
    }

    auto &builder = *m_ctx->llvm_builder;
    auto frame_ptr = get_async_frame_field_ptr(fn, machine, var);
    auto alive_ptr = get_async_frame_var_alive_ptr(fn, machine, var);
    auto current_ptr = get_var(var);
    auto var_type = get_async_frame_node_type(var);
    auto resolved_var_type = var_type;
    while (resolved_var_type && resolved_var_type->kind == TypeKind::Subtype &&
           resolved_var_type->data.subtype.final_type) {
        resolved_var_type = resolved_var_type->data.subtype.final_type;
    }
    auto field_index = machine.frame_var_index[var];
    if (!is_same_async_frame_field_ptr(current_ptr, machine, field_index)) {
        if (get_resolver()->type_needs_destruction(var_type)) {
            destroy_async_frame_slot_if_alive(fn, frame_ptr, alive_ptr, var_type);
        }
        auto current_value = builder.CreateLoad(compile_type(var_type), current_ptr);
        compile_store_or_copy(fn, current_value, frame_ptr, var_type, var, false);
        if (alive_ptr) {
            builder.CreateStore(llvm::ConstantInt::getTrue(*m_ctx->llvm_ctx), alive_ptr);
        }
        if (get_resolver()->type_needs_destruction(var_type)) {
            if (resolved_var_type && resolved_var_type->kind == TypeKind::MoveRef) {
                builder.CreateStore(llvm::Constant::getNullValue(compile_type(var_type)),
                                    current_ptr);
            } else if (get_resolver()->is_non_copyable(var_type)) {
                auto size = llvm_type_size(compile_type(var_type));
                builder.CreateMemSet(
                    current_ptr,
                    llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(*m_ctx->llvm_ctx), 0),
                    size, {});
            } else {
                compile_destruction_for_type(fn, current_ptr, var_type);
            }
        }
    }
    add_var(var, frame_ptr);
    fn->async_frame_owned_vars.insert(var);
    local_vars[var] = frame_ptr;
}

void Compiler::flush_async_frame_vars(Function *fn, AsyncStateMachine &machine) {
    auto &builder = *m_ctx->llvm_builder;
    for (auto var : machine.frame_vars) {
        if (!m_ctx->var_table.has_key(var)) {
            continue;
        }
        auto current_ptr = get_var(var);
        auto frame_ptr = get_async_frame_field_ptr(fn, machine, var);
        auto alive_ptr = get_async_frame_var_alive_ptr(fn, machine, var);
        auto field_index = machine.frame_var_index[var];
        if (is_same_async_frame_field_ptr(current_ptr, machine, field_index)) {
            continue;
        }
        auto var_type = get_async_frame_node_type(var);
        auto resolved_var_type = var_type;
        while (resolved_var_type && resolved_var_type->kind == TypeKind::Subtype &&
               resolved_var_type->data.subtype.final_type) {
            resolved_var_type = resolved_var_type->data.subtype.final_type;
        }
        if (get_resolver()->type_needs_destruction(var_type)) {
            destroy_async_frame_slot_if_alive(fn, frame_ptr, alive_ptr, var_type);
        }
        auto current_value = builder.CreateLoad(compile_type(var_type), current_ptr);
        compile_store_or_copy(fn, current_value, frame_ptr, var_type, var, false);
        if (alive_ptr) {
            builder.CreateStore(llvm::ConstantInt::getTrue(*m_ctx->llvm_ctx), alive_ptr);
        }
        if (get_resolver()->type_needs_destruction(var_type)) {
            if (resolved_var_type && resolved_var_type->kind == TypeKind::MoveRef) {
                builder.CreateStore(llvm::Constant::getNullValue(compile_type(var_type)),
                                    current_ptr);
            } else if (get_resolver()->is_non_copyable(var_type)) {
                auto size = llvm_type_size(compile_type(var_type));
                builder.CreateMemSet(
                    current_ptr,
                    llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(*m_ctx->llvm_ctx), 0),
                    size, {});
            } else {
                compile_destruction_for_type(fn, current_ptr, var_type);
            }
        }
    }
}

void Compiler::write_async_frame_await_value(Function *fn, AsyncStateMachine &machine,
                                             ast::Node *await_expr, RefValue value,
                                             ChiType *type, ast::Node *expr) {
    auto &builder = *m_ctx->llvm_builder;
    auto slot = get_async_frame_await_ptr(fn, machine, await_expr);
    auto alive_ptr = get_async_frame_await_alive_ptr(fn, machine, await_expr);
    if (get_resolver()->type_needs_destruction(type)) {
        destroy_async_frame_slot_if_alive(fn, slot, alive_ptr, type);
    }
    compile_copy_with_ref(fn, value, slot, type, expr, false);
    if (alive_ptr) {
        builder.CreateStore(llvm::ConstantInt::getTrue(*m_ctx->llvm_ctx), alive_ptr);
    }
}

std::vector<AsyncLoopResumeContext> Compiler::snapshot_async_loop_stack(Function *fn) {
    std::vector<AsyncLoopResumeContext> out;
    for (auto &loop : fn->loop_labels) {
        if (loop.async_continue_state_id < 0 && !loop.async_break_block) {
            continue;
        }
        AsyncLoopResumeContext ctx = {};
        ctx.active_blocks_depth = loop.active_blocks_depth;
        ctx.continue_state_id = loop.async_continue_state_id;
        ctx.break_block = loop.async_break_block;
        ctx.break_stmt_index = loop.async_break_stmt_index;
        out.push_back(ctx);
    }
    return out;
}

void Compiler::restore_async_loop_stack(Function *fn,
                                        const std::vector<AsyncLoopResumeContext> &loop_stack) {
    for (auto &ctx : loop_stack) {
        auto loop = fn->push_loop();
        loop->active_blocks_depth = ctx.active_blocks_depth;
        loop->async_continue_state_id = ctx.continue_state_id;
        loop->async_break_block = ctx.break_block;
        loop->async_break_stmt_index = ctx.break_stmt_index;
    }
}

std::set<ast::Node *> Compiler::get_async_resolved_awaits(const AsyncResumePoint *resume_point,
                                                          ast::Node *resume_stmt) {
    std::set<ast::Node *> resolved;
    if (!resume_point || resume_point->resume_stmt != resume_stmt) {
        return resolved;
    }
    for (auto &binding : resume_point->resolved_awaits) {
        resolved.insert(binding.await_expr);
    }
    return resolved;
}

AsyncResumePoint Compiler::build_async_resume_point(ast::Node *block, int stmt_index,
                                                    ast::Node *resume_stmt,
                                                    const AsyncResumePoint *resume_point,
                                                    ast::Node *await_expr,
                                                    ast::Node *continue_block,
                                                    int continue_stmt_index,
                                                    int continue_state_id,
                                                    ast::Node *tail_return_expr_parent) {
    AsyncResumePoint next = {};
    next.block = block;
    next.stmt_index = stmt_index;
    next.resume_stmt = resume_stmt;
    bool has_explicit_continue = continue_block || continue_stmt_index >= 0 ||
                                 continue_state_id >= 0 || tail_return_expr_parent;
    if (resume_point) {
        next.resolved_awaits = resume_point->resolved_awaits;
        if (!has_explicit_continue) {
            next.continue_block = resume_point->continue_block;
            next.continue_stmt_index = resume_point->continue_stmt_index;
            next.continue_state_id = resume_point->continue_state_id;
            next.tail_return_expr_parent = resume_point->tail_return_expr_parent;
        }
    }
    if (has_explicit_continue || !resume_point) {
        next.continue_block = continue_block;
        next.continue_stmt_index = continue_stmt_index;
        next.continue_state_id = continue_state_id;
        next.tail_return_expr_parent = tail_return_expr_parent;
    }
    next.resolved_awaits.push_back({await_expr});
    return next;
}

void Compiler::emit_async_function_return(Function *fn, AsyncStateMachine &machine,
                                          llvm::Value *result_promise_ptr) {
    auto &builder = *m_ctx->llvm_builder;
    if (fn->return_label) {
        compile_copy_with_ref(fn, RefValue::from_address(result_promise_ptr), fn->return_value,
                              machine.promise_type, fn->node);
        builder.CreateBr(fn->return_label);
    } else {
        emit_cleanup_owners(fn);
        builder.CreateRetVoid();
    }
}

static ast::Node *get_async_switch_expr(ast::Node *stmt) {
    if (!stmt) {
        return nullptr;
    }
    if (stmt->type == ast::NodeType::SwitchExpr) {
        return stmt;
    }
    if (stmt->type == ast::NodeType::ReturnStmt && stmt->data.return_stmt.expr &&
        stmt->data.return_stmt.expr->type == ast::NodeType::SwitchExpr) {
        return stmt->data.return_stmt.expr;
    }
    return nullptr;
}

void Compiler::emit_async_state_transition(Function *fn, AsyncStateMachine &machine, int state_id) {
    auto &builder = *m_ctx->llvm_builder;
    auto state_ptr = get_async_frame_state_ptr(fn, machine);
    builder.CreateStore(builder.getInt32(state_id), state_ptr);
    auto frame_data_ptr =
        builder.CreateBitCast(machine.frame_ptr, llvm::PointerType::get(*m_ctx->llvm_ctx, 0));
    emit_dbg_location(fn->node);
    builder.CreateCall(machine.dispatcher_fn, {frame_data_ptr});
    emit_cleanup_owners(fn);
    builder.CreateRetVoid();
}

int Compiler::get_async_loop_head_state(Function *fn, AsyncStateMachine &machine,
                                        ast::Node *while_stmt, ast::Node *parent_block,
                                        int stmt_index, int continue_state_id) {
    auto existing = machine.loop_head_state_ids.find(while_stmt);
    if (existing != machine.loop_head_state_ids.end()) {
        return existing->second;
    }

    auto state_id = machine.next_state_id++;
    machine.loop_head_state_ids[while_stmt] = state_id;

    AsyncResumePoint head = {};
    head.block = parent_block;
    head.stmt_index = stmt_index;
    head.resume_stmt = nullptr;
    head.is_loop_head = true;
    head.continue_state_id = continue_state_id;
    register_async_resume_state(fn, machine, head, state_id);
    return state_id;
}

void Compiler::emit_async_suspend(Function *fn, AsyncStateMachine &machine, int state_id,
                                  ast::Node *await_expr, ChiType *settled_type,
                                  llvm::Value *result_promise_ptr) {
    auto &builder = *m_ctx->llvm_builder;
    flush_async_frame_vars(fn, machine);
    auto promise_expr = await_expr->data.await_expr.expr;
    auto promise_ref = compile_expr_ref(fn, promise_expr);
    auto promise_type = get_chitype(await_expr->data.await_expr.expr);
    auto promise_type_l = compile_type(promise_type);
    auto promise_ptr = fn->entry_alloca(promise_type_l, "awaited");
    if (promise_ref.address) {
        compile_copy_with_ref(fn, promise_ref, promise_ptr, promise_type, promise_expr);
        // compile_copy_with_ref destroys the source when owns_value is true. When
        // the source is a resolver-allocated outlet (owns_value false), block
        // cleanup would normally destroy it — but emit_async_function_return
        // branches to return_label bypassing cleanup, so destroy it inline.
        if (!promise_ref.owns_value && promise_expr->type == ast::NodeType::FnCallExpr) {
            compile_destruction_for_type(fn, promise_ref.address, promise_type);
        }
    } else {
        compile_copy(fn, promise_ref.value, promise_ptr, promise_type, promise_expr);
    }

    auto promise_struct = get_resolver()->resolve_struct_type(promise_type);
    std::optional<TypeId> variant_type_id = std::nullopt;
    if (promise_type->kind == TypeKind::Subtype && !promise_type->is_placeholder) {
        variant_type_id = promise_type->id;
    }
    auto then_member = promise_struct->find_member("on_resolve");
    auto then_method = get_fn(get_variant_member_node(then_member, variant_type_id));
    auto on_reject_member = promise_struct->find_member("on_reject");
    auto on_reject_method = get_fn(get_variant_member_node(on_reject_member, variant_type_id));

    if (settled_type && get_resolver()->is_result_type(settled_type)) {
        auto ok_lambda = build_async_try_await_forwarder_lambda(
            fn, machine, state_id, settled_type, await_expr, false);
        auto err_lambda = build_async_try_await_forwarder_lambda(
            fn, machine, state_id, settled_type, await_expr, true);
        builder.CreateCall(then_method->llvm_fn, {promise_ptr, ok_lambda.value});
        builder.CreateCall(on_reject_method->llvm_fn, {promise_ptr, err_lambda.value});
    } else {
        auto ok_lambda =
            build_async_resume_forwarder_lambda(fn, machine, state_id, settled_type, await_expr);
        builder.CreateCall(then_method->llvm_fn, {promise_ptr, ok_lambda.value});
        auto reject_lambda = build_async_rejection_forwarder_lambda(fn, machine);
        builder.CreateCall(on_reject_method->llvm_fn, {promise_ptr, reject_lambda.value});
    }

    compile_destruction_for_type(fn, promise_ptr, promise_type);

    emit_async_function_return(fn, machine, result_promise_ptr);
}

int Compiler::register_async_resume_state(Function *fn, AsyncStateMachine &machine,
                                          const AsyncResumePoint &resume_point,
                                          int forced_state_id) {
    auto &builder = *m_ctx->llvm_builder;
    auto ctx = begin_async_worker_state(fn, machine, "", forced_state_id);
    auto state_fn = ctx.state_fn;
    auto result_promise_ptr = ctx.result_promise_ptr;

    machine.states.push_back({ctx.state_id, resume_point, ctx.worker_llvm_fn});

    state_fn->default_method_struct = fn->default_method_struct;
    if (machine.frame_bind_index >= 0 && machine.bind_type) {
        auto bind_slot =
            builder.CreateStructGEP(machine.frame_struct_type, machine.frame_ptr, machine.frame_bind_index);
        state_fn->bind_ptr = builder.CreateLoad(compile_type(machine.bind_type), bind_slot);
    }

    restore_async_loop_stack(state_fn, resume_point.loop_stack);

    // If we're inside an async try-block, set up landing pad for the resumed state
    label_t *try_catch_landing_b = nullptr;
    int saved_try_catch_id = -1;
    if (machine.active_try_catch_state_id >= 0) {
        try_catch_landing_b = state_fn->new_label("_async_try_catch_landing");
        state_fn->try_block_landing = try_catch_landing_b;
        saved_try_catch_id = machine.active_try_catch_state_id;
    }

    // Build resumed_locals on top of vars already bound by begin_async_worker_state
    map<ast::Node *, llvm::Value *> resumed_locals;
    for (auto var : machine.frame_vars) {
        resumed_locals[var] = get_var(var);
        state_fn->async_frame_owned_vars.insert(var);
    }

    auto previous_await_refs = m_async_await_refs;
    for (auto &binding : resume_point.resolved_awaits) {
        auto captured_value_ptr = get_async_frame_await_ptr(state_fn, machine, binding.await_expr);
        m_async_await_refs[binding.await_expr] = RefValue::from_address(captured_value_ptr);
    }

    AsyncBlockContext resume_ctx = {state_fn, &machine, &resumed_locals, result_promise_ptr,
                                    &resume_point, resume_point.continue_state_id};
    compile_async_block_recursive(resume_ctx, resume_point.block, resume_point.stmt_index,
                                  resume_point.continue_block, resume_point.continue_stmt_index,
                                  resume_point.tail_return_expr_parent);

    if (!builder.GetInsertBlock()->getTerminator()) {
        if (resume_point.continue_state_id >= 0) {
            emit_async_state_transition(state_fn, machine, resume_point.continue_state_id);
        } else if (resume_point.continue_block) {
            auto cont_ctx = resume_ctx;
            cont_ctx.resume_point = nullptr;
            cont_ctx.continue_state_id = -1;
            compile_async_block_recursive(cont_ctx, resume_point.continue_block,
                                          resume_point.continue_stmt_index);
        }
    }

    // If inside a try-block, add the try-catch landing pad that transitions to catch state
    if (try_catch_landing_b && try_catch_landing_b->getParent()) {
        if (!ctx.worker_llvm_fn->hasPersonalityFn()) {
            ctx.worker_llvm_fn->setPersonalityFn(get_system_fn("cx_personality")->llvm_fn);
        }
        state_fn->use_label(try_catch_landing_b);
        auto landing = builder.CreateLandingPad(m_ctx->get_caught_result_type(), 1);
        landing->addClause(llvm::ConstantPointerNull::get(builder.getPtrTy()));
        emit_async_landing_pad(state_fn, landing);
        builder.CreateCall(get_system_fn("cx_clear_panic_location")->llvm_fn, {});
        emit_async_state_transition(state_fn, machine, saved_try_catch_id);
    }

    state_fn->try_block_landing = nullptr;
    m_async_await_refs = previous_await_refs;
    for (size_t i = 0; i < resume_point.loop_stack.size(); i++) {
        state_fn->pop_loop();
    }

    end_async_worker_state(machine, ctx);
    return ctx.state_id;
}

int Compiler::register_async_custom_state(Function *fn, AsyncStateMachine &machine,
                                          const string &name_suffix,
                                          const std::function<void(Function *)> &emit_body,
                                          int forced_state_id) {
    auto ctx = begin_async_worker_state(fn, machine, name_suffix, forced_state_id);
    emit_body(ctx.state_fn);
    end_async_worker_state(machine, ctx);
    return ctx.state_id;
}

void Compiler::compile_async_if_recursive(const AsyncBlockContext &ctx, ast::Node *if_stmt,
                                          ast::Node *parent_block, int next_stmt_index) {
    auto fn = ctx.fn;
    auto &machine = *ctx.machine;
    auto &local_vars = *ctx.local_vars;
    auto result_promise_ptr = ctx.result_promise_ptr;
    auto resume_point = ctx.resume_point;
    auto continue_state_id = ctx.continue_state_id;
    auto &builder = *m_ctx->llvm_builder;
    auto &data = if_stmt->data.if_expr;

    auto resolved = get_async_resolved_awaits(resume_point, if_stmt);

    if (contains_unresolved_await(data.condition, resolved)) {
        auto promise_site = find_unresolved_await_site(data.condition, resolved);
        auto promise_expr = promise_site.await_expr;
        auto settled_type =
            get_async_resume_value_type(if_stmt, promise_expr, promise_site.resume_expr);
        auto next = build_async_resume_point(parent_block, next_stmt_index - 1, if_stmt,
                                             resume_point, promise_expr, nullptr, -1, continue_state_id);
        next.loop_stack = snapshot_async_loop_stack(fn);
        auto state_id = register_async_resume_state(fn, machine, next);
        emit_async_suspend(fn, machine, state_id, promise_expr, settled_type, result_promise_ptr);
        return;
    }

    auto cond = compile_expr(fn, data.condition);
    auto then_b = fn->new_label("_async_if_then");
    auto else_b = fn->new_label("_async_if_else");
    auto done_b = fn->new_label("_async_if_done");
    builder.CreateCondBr(cond, then_b, else_b);

    fn->use_label(then_b);
    auto then_locals = local_vars;
    auto saved_then_var_table = m_ctx->var_table;
    auto then_ctx = ctx;
    then_ctx.local_vars = &then_locals;
    then_ctx.resume_point = nullptr;
    compile_async_block_recursive(then_ctx, data.then_block, 0,
                                  parent_block, next_stmt_index);
    if (!builder.GetInsertBlock()->getTerminator()) {
        compile_async_block_recursive(then_ctx, parent_block, next_stmt_index);
        if (!builder.GetInsertBlock()->getTerminator()) {
            builder.CreateBr(done_b);
        }
    }
    m_ctx->var_table = saved_then_var_table;

    fn->use_label(else_b);
    auto else_locals = local_vars;
    auto saved_else_var_table = m_ctx->var_table;
    auto else_ctx = ctx;
    else_ctx.local_vars = &else_locals;
    else_ctx.resume_point = nullptr;
    if (data.else_node && data.else_node->type == ast::NodeType::Block) {
        compile_async_block_recursive(else_ctx, data.else_node, 0,
                                      parent_block, next_stmt_index);
        if (!builder.GetInsertBlock()->getTerminator()) {
            compile_async_block_recursive(else_ctx, parent_block, next_stmt_index);
            if (!builder.GetInsertBlock()->getTerminator()) {
                builder.CreateBr(done_b);
            }
        }
    } else if (data.else_node && data.else_node->type == ast::NodeType::IfExpr) {
        compile_async_if_recursive(else_ctx, data.else_node, parent_block, next_stmt_index);
    } else if (!data.else_node) {
        compile_async_block_recursive(else_ctx, parent_block, next_stmt_index);
        if (!builder.GetInsertBlock()->getTerminator()) {
            builder.CreateBr(done_b);
        }
    }
    m_ctx->var_table = saved_else_var_table;

    fn->use_label(done_b);
}

void Compiler::compile_async_while_recursive(const AsyncBlockContext &ctx,
                                             ast::Node *while_stmt, ast::Node *parent_block,
                                             int stmt_index, int next_stmt_index) {
    auto fn = ctx.fn;
    auto &machine = *ctx.machine;
    auto &local_vars = *ctx.local_vars;
    auto result_promise_ptr = ctx.result_promise_ptr;
    auto resume_point = ctx.resume_point;
    auto continue_state_id = ctx.continue_state_id;
    auto &builder = *m_ctx->llvm_builder;
    auto &data = while_stmt->data.while_stmt;

    if (!get_resolver()->contains_await(while_stmt)) {
        compile_stmt(fn, while_stmt);
        if (!builder.GetInsertBlock()->getTerminator()) {
            auto cont = ctx;
            cont.resume_point = nullptr;
            compile_async_block_recursive(cont, parent_block, next_stmt_index);
        }
        return;
    }

    auto loop_head_state_id = get_async_loop_head_state(fn, machine, while_stmt, parent_block,
                                                        stmt_index, continue_state_id);
    auto resolved = get_async_resolved_awaits(resume_point, while_stmt);
    if (data.condition && contains_unresolved_await(data.condition, resolved)) {
        auto site = find_unresolved_await_site(data.condition, resolved);
        auto promise_expr = site.await_expr;
        auto settled_type =
            get_async_resume_value_type(while_stmt, promise_expr, site.resume_expr);
        auto next = build_async_resume_point(parent_block, stmt_index, while_stmt, resume_point,
                                             promise_expr, nullptr, -1, -1);
        next.loop_stack = snapshot_async_loop_stack(fn);
        auto state_id = register_async_resume_state(fn, machine, next);
        emit_async_suspend(fn, machine, state_id, promise_expr, settled_type, result_promise_ptr);
        return;
    }

    auto cond = data.condition ? compile_assignment_to_type(fn, data.condition, get_system_types()->bool_)
                               : builder.getTrue();
    auto body_b = fn->new_label("_async_while_body");
    auto end_b = fn->new_label("_async_while_end");
    auto done_b = fn->new_label("_async_while_done");
    builder.CreateCondBr(cond, body_b, end_b);

    fn->use_label(body_b);
    auto body_locals = local_vars;
    const AsyncResumePoint *body_resume =
        (resume_point && resume_point->block == data.body) ? resume_point : nullptr;
    auto loop = fn->push_loop();
    loop->async_continue_state_id = loop_head_state_id;
    loop->async_break_block = parent_block;
    loop->async_break_stmt_index = next_stmt_index;
    auto body_ctx = ctx;
    body_ctx.local_vars = &body_locals;
    body_ctx.resume_point = body_resume;
    body_ctx.continue_state_id = loop_head_state_id;
    compile_async_block_recursive(body_ctx, data.body,
                                  body_resume ? body_resume->stmt_index : 0);
    fn->pop_loop();
    if (!builder.GetInsertBlock()->getTerminator()) {
        emit_async_state_transition(fn, machine, loop_head_state_id);
    }

    fn->use_label(end_b);
    if (!builder.GetInsertBlock()->getTerminator()) {
        auto end_ctx = ctx;
        end_ctx.resume_point = nullptr;
        compile_async_block_recursive(end_ctx, parent_block, next_stmt_index);
        if (!builder.GetInsertBlock()->getTerminator()) {
            builder.CreateBr(done_b);
        }
    }

    fn->use_label(done_b);
}

void Compiler::compile_async_for_recursive(const AsyncBlockContext &ctx,
                                           ast::Node *for_stmt, ast::Node *parent_block,
                                           int stmt_index, int next_stmt_index) {
    auto fn = ctx.fn;
    auto &machine = *ctx.machine;
    auto &local_vars = *ctx.local_vars;
    auto result_promise_ptr = ctx.result_promise_ptr;
    auto resume_point = ctx.resume_point;
    auto continue_state_id = ctx.continue_state_id;
    auto &builder = *m_ctx->llvm_builder;
    auto &data = for_stmt->data.for_stmt;

    if (!get_resolver()->contains_await(for_stmt)) {
        compile_stmt(fn, for_stmt);
        if (!builder.GetInsertBlock()->getTerminator()) {
            auto cont = ctx;
            cont.resume_point = nullptr;
            compile_async_block_recursive(cont, parent_block, next_stmt_index);
        }
        return;
    }

    auto ensure_loop_var = [&](ast::Node *var) {
        if (!var) {
            return;
        }
        if (machine.frame_var_index.count(var)) {
            auto frame_ptr = get_async_frame_field_ptr(fn, machine, var);
            add_var(var, frame_ptr);
            local_vars[var] = frame_ptr;
            return;
        }
        if (!m_ctx->var_table.has_key(var)) {
            if (var->type == ast::NodeType::BindIdentifier) {
                auto storage =
                    fn->entry_alloca(compile_type(get_async_frame_node_type(var)), "_for_bind_var");
                add_var(var, storage);
            } else {
                compile_stmt(fn, var);
            }
        }
        local_vars[var] = get_var(var);
    };

    ensure_loop_var(data.bind);
    ensure_loop_var(data.index_bind);

    auto kind = data.effective_kind();
    bool is_int_range = kind == ast::ForLoopKind::IntRange && data.expr &&
                        data.expr->type == ast::NodeType::RangeExpr;
    auto expr_type = !is_int_range && data.expr ? get_chitype(data.expr) : nullptr;
    auto fixed_array_loop = expr_type && ((expr_type->kind == TypeKind::FixedArray) ||
                                          (expr_type->is_reference() &&
                                           expr_type->get_elem()->kind == TypeKind::FixedArray));
    bool is_index_range = fixed_array_loop || kind == ast::ForLoopKind::Range;
    bool is_iter_loop = kind == ast::ForLoopKind::Iter;

    if (kind == ast::ForLoopKind::Ternary) {
        // Ternary for loop: for init; cond; post { body }
        bool loop_resume = resume_point && resume_point->is_loop_head &&
                           resume_point->block == parent_block &&
                           resume_point->stmt_index == stmt_index && !resume_point->resume_stmt;
        if (!loop_resume) {
            if (data.init) {
                compile_stmt(fn, data.init);
            }
        }

        auto loop_head_state_id = get_async_loop_head_state(fn, machine, for_stmt, parent_block,
                                                             stmt_index, continue_state_id);
        auto resolved = get_async_resolved_awaits(resume_point, for_stmt);
        if (data.condition && contains_unresolved_await(data.condition, resolved)) {
            auto site = find_unresolved_await_site(data.condition, resolved);
            auto promise_expr = site.await_expr;
            auto settled_type =
                get_async_resume_value_type(for_stmt, promise_expr, site.resume_expr);
            auto next = build_async_resume_point(parent_block, stmt_index, for_stmt, resume_point,
                                                 promise_expr, nullptr, -1, -1);
            next.loop_stack = snapshot_async_loop_stack(fn);
            auto state_id = register_async_resume_state(fn, machine, next);
            emit_async_suspend(fn, machine, state_id, promise_expr, settled_type, result_promise_ptr);
            return;
        }

        auto post_state_id = register_async_custom_state(
            fn, machine, "for_post",
            [&](Function *state_fn) {
                auto &post_builder = *m_ctx->llvm_builder;
                state_fn->push_scope();
                if (data.post) {
                    compile_stmt(state_fn, data.post);
                }
                state_fn->pop_scope();
                emit_async_state_transition(state_fn, machine, loop_head_state_id);
            });

        auto cond = data.condition
                        ? compile_assignment_to_type(fn, data.condition, get_system_types()->bool_)
                        : builder.getTrue();
        auto body_b = fn->new_label("_async_for_body");
        auto end_b = fn->new_label("_async_for_end");
        auto done_b = fn->new_label("_async_for_done");
        builder.CreateCondBr(cond, body_b, end_b);

        fn->use_label(body_b);
        auto body_locals = local_vars;
        const AsyncResumePoint *body_resume =
            (resume_point && resume_point->block == data.body) ? resume_point : nullptr;
        auto loop = fn->push_loop();
        loop->async_continue_state_id = post_state_id;
        loop->async_break_block = parent_block;
        loop->async_break_stmt_index = next_stmt_index;
        auto body_ctx = ctx;
        body_ctx.local_vars = &body_locals;
        body_ctx.resume_point = body_resume;
        body_ctx.continue_state_id = post_state_id;
        compile_async_block_recursive(body_ctx, data.body,
                                      body_resume ? body_resume->stmt_index : 0);
        fn->pop_loop();
        if (!builder.GetInsertBlock()->getTerminator()) {
            emit_async_state_transition(fn, machine, post_state_id);
        }

        fn->use_label(end_b);
        if (!builder.GetInsertBlock()->getTerminator()) {
            auto end_ctx = ctx;
            end_ctx.resume_point = nullptr;
            end_ctx.continue_state_id = -1;
            compile_async_block_recursive(end_ctx, parent_block, next_stmt_index);
            if (!builder.GetInsertBlock()->getTerminator()) {
                builder.CreateBr(done_b);
            }
        }

        fn->use_label(done_b);
        return;
    }

    auto iter_node = (is_int_range && data.bind) ? data.bind : for_stmt;
    ast::Node *end_node = is_int_range ? data.expr->data.range_expr.end : nullptr;

    bool loop_resume = resume_point && resume_point->is_loop_head &&
                       resume_point->block == parent_block &&
                       resume_point->stmt_index == stmt_index && !resume_point->resume_stmt;
    if (!loop_resume) {
        auto iter_ptr = get_async_frame_field_ptr(fn, machine, iter_node);
        add_var(iter_node, iter_ptr);
        local_vars[iter_node] = iter_ptr;

        if (is_int_range) {
            auto &range = data.expr->data.range_expr;
            auto start_value =
                compile_assignment_to_type(fn, range.start, get_async_frame_node_type(iter_node));
            compile_store_or_copy(fn, start_value, get_var(iter_node),
                                  get_async_frame_node_type(iter_node), range.start);
            if (auto alive_ptr = get_async_frame_var_alive_ptr(fn, machine, iter_node)) {
                builder.CreateStore(llvm::ConstantInt::getTrue(*m_ctx->llvm_ctx), alive_ptr);
            }

            auto end_value = compile_assignment_to_type(fn, end_node, get_chitype(end_node));
            auto end_ptr = get_async_frame_field_ptr(fn, machine, end_node);
            compile_store_or_copy(fn, end_value, end_ptr, get_chitype(end_node), end_node);
            if (auto alive_ptr = get_async_frame_var_alive_ptr(fn, machine, end_node)) {
                builder.CreateStore(llvm::ConstantInt::getTrue(*m_ctx->llvm_ctx), alive_ptr);
            }
            add_var(end_node, end_ptr);
            local_vars[end_node] = end_ptr;
        } else if (fixed_array_loop) {
            builder.CreateStore(llvm::ConstantInt::get(compile_type(get_async_frame_node_type(iter_node)), 0),
                                iter_ptr);
        } else if (kind == ast::ForLoopKind::Range) {
            auto ptr = compile_dot_ptr(fn, data.expr);
            auto sty = get_resolver()->resolve_struct_type(get_chitype(data.expr));
            auto begin = *sty->member_table.get("begin");
            auto iter_begin =
                builder.CreateCall(get_fn(begin->node)->llvm_fn, {ptr}, "_iter_begin");
            compile_store_or_copy(fn, iter_begin, iter_ptr, get_async_frame_node_type(iter_node), data.expr);
            if (auto alive_ptr = get_async_frame_var_alive_ptr(fn, machine, iter_node)) {
                builder.CreateStore(llvm::ConstantInt::getTrue(*m_ctx->llvm_ctx), alive_ptr);
            }
        } else if (kind == ast::ForLoopKind::Iter) {
            auto container_ptr = compile_dot_ptr(fn, data.expr);
            auto sty = get_resolver()->resolve_struct_type(get_chitype(data.expr));
            auto iter_fn = *sty->member_table.get("to_iter_mut");
            auto iter_fn_type = iter_fn->resolved_type;
            auto iter_ret_type = iter_fn_type->data.fn.return_type;
            auto iter_llvm_fn = get_fn(iter_fn->node)->llvm_fn;
            if (iter_fn_type->data.fn.should_use_sret()) {
                builder.CreateCall(iter_llvm_fn, {iter_ptr, container_ptr});
            } else {
                auto iter_val = builder.CreateCall(iter_llvm_fn, {container_ptr}, "_iter_val");
                compile_store_or_copy(fn, iter_val, iter_ptr, iter_ret_type, data.expr);
            }
            if (auto alive_ptr = get_async_frame_var_alive_ptr(fn, machine, iter_node)) {
                builder.CreateStore(llvm::ConstantInt::getTrue(*m_ctx->llvm_ctx), alive_ptr);
            }
            if (data.index_bind) {
                builder.CreateStore(llvm::ConstantInt::get(
                                        llvm::Type::getInt32Ty(m_ctx->llvm_module->getContext()), 0),
                                    get_var(data.index_bind));
            }
        }
    } else {
        add_var(iter_node, get_async_frame_field_ptr(fn, machine, iter_node));
        local_vars[iter_node] = get_var(iter_node);
        if (end_node) {
            add_var(end_node, get_async_frame_field_ptr(fn, machine, end_node));
            local_vars[end_node] = get_var(end_node);
        }
    }

    auto loop_head_state_id = get_async_loop_head_state(fn, machine, for_stmt, parent_block,
                                                        stmt_index, continue_state_id);
    auto post_state_id = register_async_custom_state(
        fn, machine, "for_post",
        [&](Function *state_fn) {
            auto &post_builder = *m_ctx->llvm_builder;
            auto iter_ptr = get_var(iter_node);
            auto iter_type = get_async_frame_node_type(iter_node);
            auto iter_type_l = compile_type(iter_type);
            if (is_int_range || fixed_array_loop) {
                auto cur = post_builder.CreateLoad(iter_type_l, iter_ptr);
                auto next_value =
                    post_builder.CreateAdd(cur, llvm::ConstantInt::get(iter_type_l, 1));
                compile_store_or_copy(state_fn, next_value, iter_ptr, iter_type, iter_node);
            } else if (kind == ast::ForLoopKind::Range) {
                auto ptr = compile_dot_ptr(state_fn, data.expr);
                auto sty = get_resolver()->resolve_struct_type(get_chitype(data.expr));
                auto next = *sty->member_table.get("next");
                auto iter_value = post_builder.CreateLoad(iter_type_l, iter_ptr);
                auto iter_next =
                    post_builder.CreateCall(get_fn(next->node)->llvm_fn, {ptr, iter_value}, "_iter_next");
                compile_store_or_copy(state_fn, iter_next, iter_ptr, iter_type, data.expr);
            } else if (kind == ast::ForLoopKind::Iter && data.index_bind) {
                auto idx_type = llvm::Type::getInt32Ty(m_ctx->llvm_module->getContext());
                auto cur = post_builder.CreateLoad(idx_type, get_var(data.index_bind));
                post_builder.CreateStore(post_builder.CreateAdd(cur, llvm::ConstantInt::get(idx_type, 1)),
                                         get_var(data.index_bind));
            }
            emit_async_state_transition(state_fn, machine, loop_head_state_id);
        });

    auto resolved = get_async_resolved_awaits(resume_point, for_stmt);
    auto iter_type = get_async_frame_node_type(iter_node);
    auto iter_value = builder.CreateLoad(compile_type(iter_type), get_var(iter_node));
    llvm::Value *cond = nullptr;
    if (is_int_range) {
        auto end_value = builder.CreateLoad(compile_type(get_chitype(end_node)), get_var(end_node));
        cond = builder.CreateICmpSLT(iter_value, end_value);
    } else if (fixed_array_loop) {
        auto arr_type = expr_type->is_reference() ? expr_type->get_elem() : expr_type;
        auto size_val =
            llvm::ConstantInt::get(iter_value->getType(), arr_type->data.fixed_array.size);
        cond = builder.CreateICmpULT(iter_value, size_val);
    } else if (kind == ast::ForLoopKind::Range) {
        auto ptr = compile_dot_ptr(fn, data.expr);
        auto sty = get_resolver()->resolve_struct_type(get_chitype(data.expr));
        auto end = *sty->member_table.get("end");
        auto iter_end = builder.CreateCall(get_fn(end->node)->llvm_fn, {ptr}, "_iter_end");
        cond = builder.CreateICmpSLT(iter_value, iter_end);
    } else if (kind == ast::ForLoopKind::Iter) {
        auto sty = get_resolver()->resolve_struct_type(get_chitype(data.expr));
        auto next_fn = *get_resolver()->resolve_struct_type(get_async_frame_node_type(iter_node))
                            ->member_table.get("next");
        auto next_fn_type = next_fn->resolved_type;
        auto next_ret_type = next_fn_type->data.fn.return_type;
        auto next_ret_type_l = compile_type(next_ret_type);
        auto next_llvm_fn = get_fn(next_fn->node)->llvm_fn;
        auto opt_alloca = fn->entry_alloca(next_ret_type_l, "_opt_alloca");
        if (next_fn_type->data.fn.should_use_sret()) {
            builder.CreateCall(next_llvm_fn, {opt_alloca, get_var(iter_node)});
        } else {
            auto opt_result = builder.CreateCall(next_llvm_fn, {get_var(iter_node)}, "_opt_result");
            builder.CreateStore(opt_result, opt_alloca);
        }
        auto has_value_p = builder.CreateStructGEP(next_ret_type_l, opt_alloca, 0);
        cond = builder.CreateLoad(llvm::Type::getInt1Ty(m_ctx->llvm_module->getContext()), has_value_p);
        if (data.bind) {
            auto value_p = builder.CreateStructGEP(next_ret_type_l, opt_alloca, 1);
            auto value =
                builder.CreateLoad(compile_type(data.bind->resolved_type), value_p, "_iter_item");
            builder.CreateStore(value, get_var(data.bind));
        }
    }

    auto body_b = fn->new_label("_async_for_body");
    auto end_b = fn->new_label("_async_for_end");
    auto done_b = fn->new_label("_async_for_done");
    builder.CreateCondBr(cond, body_b, end_b);

    fn->use_label(body_b);
    auto body_locals = local_vars;
    if (fixed_array_loop || kind == ast::ForLoopKind::Range) {
        if (data.index_bind) {
            auto idx_type = llvm::Type::getInt32Ty(m_ctx->llvm_module->getContext());
            auto idx_val = iter_value->getType() == idx_type
                               ? iter_value
                               : builder.CreateIntCast(iter_value, idx_type, false);
            builder.CreateStore(idx_val, get_var(data.index_bind));
        }
        if (data.bind) {
            if (fixed_array_loop) {
                auto arr_type = expr_type->is_reference() ? expr_type->get_elem() : expr_type;
                auto arr_type_l = compile_type(arr_type);
                auto elem_type = arr_type->data.fixed_array.elem;
                auto elem_type_l = compile_type(elem_type);
                auto arr_ref = compile_expr_ref(fn, data.expr);
                auto arr_addr = expr_type->is_reference()
                                    ? builder.CreateLoad(compile_type(expr_type), arr_ref.address, "_fa_deref")
                                    : arr_ref.address;
                auto zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(m_ctx->llvm_module->getContext()), 0);
                auto idx = builder.CreateLoad(iter_value->getType(), get_var(iter_node));
                auto elem_ptr = builder.CreateGEP(arr_type_l, arr_addr, {zero, idx});
                if (data.bind_sigil != ast::SigilKind::None) {
                    builder.CreateStore(elem_ptr, get_var(data.bind));
                } else {
                    auto value = builder.CreateLoad(elem_type_l, elem_ptr, "_item_value");
                    compile_store_or_copy(fn, value, get_var(data.bind), get_chitype(data.bind), data.bind);
                }
            } else {
                auto ptr = compile_dot_ptr(fn, data.expr);
                auto sty = get_resolver()->resolve_struct_type(get_chitype(data.expr));
                auto index = *sty->member_table.get("index_mut");
                auto item_ref =
                    builder.CreateCall(get_fn(index->node)->llvm_fn, {ptr, iter_value}, "_iter_item");
                if (data.bind_sigil != ast::SigilKind::None) {
                    builder.CreateStore(item_ref, get_var(data.bind));
                } else {
                    auto value = builder.CreateLoad(compile_type(data.bind->resolved_type), item_ref,
                                                    "_item_value");
                    compile_store_or_copy(fn, value, get_var(data.bind),
                                          get_chitype(data.bind), data.bind);
                }
            }
        }
    }
    const AsyncResumePoint *body_resume =
        (resume_point && resume_point->block == data.body) ? resume_point : nullptr;
    auto loop = fn->push_loop();
    loop->async_continue_state_id = post_state_id;
    loop->async_break_block = parent_block;
    loop->async_break_stmt_index = next_stmt_index;
    auto body_ctx = ctx;
    body_ctx.local_vars = &body_locals;
    body_ctx.resume_point = body_resume;
    body_ctx.continue_state_id = post_state_id;
    compile_async_block_recursive(body_ctx, data.body,
                                  body_resume ? body_resume->stmt_index : 0);
    fn->pop_loop();
    if (!builder.GetInsertBlock()->getTerminator()) {
        emit_async_state_transition(fn, machine, post_state_id);
    }

    fn->use_label(end_b);
    if (!builder.GetInsertBlock()->getTerminator()) {
        auto end_ctx = ctx;
        end_ctx.resume_point = nullptr;
        end_ctx.continue_state_id = continue_state_id;
        compile_async_block_recursive(end_ctx, parent_block, next_stmt_index);
        if (!builder.GetInsertBlock()->getTerminator()) {
            builder.CreateBr(done_b);
        }
    }

    fn->use_label(done_b);
}

void Compiler::compile_async_switch_recursive(const AsyncBlockContext &ctx,
                                              ast::Node *stmt, ast::Node *switch_expr,
                                              ast::Node *parent_block, int stmt_index,
                                              int next_stmt_index) {
    auto fn = ctx.fn;
    auto &machine = *ctx.machine;
    auto &local_vars = *ctx.local_vars;
    auto result_promise_ptr = ctx.result_promise_ptr;
    auto resume_point = ctx.resume_point;
    auto continue_state_id = ctx.continue_state_id;
    auto &builder = *m_ctx->llvm_builder;
    auto &data = switch_expr->data.switch_expr;
    auto resolved = get_async_resolved_awaits(resume_point, stmt);
    auto tail_return = stmt->type == ast::NodeType::ReturnStmt &&
                       stmt->data.return_stmt.expr == switch_expr;

    if (data.expr && contains_unresolved_await(data.expr, resolved)) {
        auto site = find_unresolved_await_site(data.expr, resolved);
        auto promise_expr = site.await_expr;
        auto settled_type =
            get_async_resume_value_type(switch_expr, promise_expr, site.resume_expr);
        auto next = build_async_resume_point(parent_block, stmt_index, stmt, resume_point,
                                             promise_expr, nullptr, -1, continue_state_id,
                                             tail_return ? switch_expr : nullptr);
        next.loop_stack = snapshot_async_loop_stack(fn);
        auto state_id = register_async_resume_state(fn, machine, next);
        emit_async_suspend(fn, machine, state_id, promise_expr, settled_type, result_promise_ptr);
        return;
    }

    auto continue_case = [&](map<ast::Node *, llvm::Value *> &case_locals) {
        if (tail_return) {
            return;
        }
        auto case_ctx = ctx;
        case_ctx.local_vars = &case_locals;
        case_ctx.resume_point = nullptr;
        compile_async_block_recursive(case_ctx, parent_block, next_stmt_index);
    };

    auto compile_case_body = [&](ast::Node *scase) {
        auto case_locals = local_vars;
        auto saved_var_table = m_ctx->var_table;
        auto case_ctx = ctx;
        case_ctx.local_vars = &case_locals;
        case_ctx.resume_point = nullptr;
        case_ctx.continue_state_id = tail_return ? -1 : continue_state_id;
        compile_async_block_recursive(case_ctx, scase->data.case_expr.body, 0,
                                      tail_return ? nullptr : parent_block,
                                      tail_return ? -1 : next_stmt_index,
                                      tail_return ? switch_expr : nullptr);
        if (!builder.GetInsertBlock()->getTerminator()) {
            continue_case(case_locals);
        }
        m_ctx->var_table = saved_var_table;
    };

    if (data.is_type_switch) {
        auto iref_ref = compile_expr_ref(fn, data.expr);
        auto fp = builder.CreateLoad(compile_type(get_chitype(data.expr)), iref_ref.address,
                                     "fat_ptr");
        auto expr_type = get_chitype(data.expr);
        auto iface_elem = expr_type->get_elem();
        auto done_label = fn->new_label("_async_tswitch_done");
        auto else_label = fn->new_label("_async_tswitch_else");
        ast::Node *else_case = nullptr;

        for (auto scase : data.cases) {
            if (scase->data.case_expr.is_else) {
                else_case = scase;
                continue;
            }

            auto case_label = fn->new_label("_async_tswitch_case");
            auto next_label = fn->new_label("_async_tswitch_next");
            auto clause = scase->data.case_expr.clauses[0];
            auto clause_type = get_resolver()->node_get_type(clause);
            auto clause_elem =
                clause_type->is_pointer_like() ? clause_type->get_elem() : clause_type;
            auto cmp = compile_interface_type_match(fn, fp, iface_elem, clause_elem);
            builder.CreateCondBr(cmp, case_label, next_label);

            fn->use_label(case_label);
            compile_case_body(scase);
            if (!builder.GetInsertBlock()->getTerminator()) {
                builder.CreateBr(done_label);
            }

            fn->use_label(next_label);
        }

        builder.CreateBr(else_label);
        fn->use_label(else_label);
        if (else_case) {
            compile_case_body(else_case);
        } else if (!tail_return) {
            auto else_ctx = ctx;
            else_ctx.resume_point = nullptr;
            compile_async_block_recursive(else_ctx, parent_block, next_stmt_index);
        }
        if (!builder.GetInsertBlock()->getTerminator()) {
            builder.CreateBr(done_label);
        }

        fn->use_label(done_label);
        return;
    }

    auto expr_value = compile_comparator(fn, data.expr);
    auto comparator_type = expr_value->getType();
    auto default_label = fn->new_label("_async_switch_default");
    auto switch_b = builder.CreateSwitch(expr_value, default_label, data.cases.size());
    auto done_label = fn->new_label("_async_switch_done");

    array<label_t *> case_labels;
    for (auto scase : data.cases) {
        auto label = default_label;
        if (!scase->data.case_expr.is_else) {
            label = fn->new_label("_async_switch_case");
        }
        for (auto clause : scase->data.case_expr.clauses) {
            auto clause_value = get_resolver()->resolve_constant_value(clause);
            assert(clause_value);
            auto cond_value = (llvm::ConstantInt *)compile_constant_value(
                fn, *clause_value, get_chitype(clause), comparator_type);
            switch_b->addCase(cond_value, label);
        }
        case_labels.add(label);
    }

    bool has_else = false;
    for (int i = 0; i < data.cases.size(); i++) {
        fn->use_label(case_labels[i]);
        auto scase = data.cases[i];
        if (scase->data.case_expr.is_else) {
            has_else = true;
        }
        compile_case_body(scase);
        if (!builder.GetInsertBlock()->getTerminator()) {
            builder.CreateBr(done_label);
        }
    }

    if (!has_else) {
        fn->use_label(default_label);
        if (!tail_return) {
            auto default_ctx = ctx;
            default_ctx.resume_point = nullptr;
            compile_async_block_recursive(default_ctx, parent_block, next_stmt_index);
        }
        if (!builder.GetInsertBlock()->getTerminator()) {
            builder.CreateBr(done_label);
        }
    }

    fn->use_label(done_label);
}

void Compiler::compile_async_try_recursive(const AsyncBlockContext &ctx,
                                           ast::Node *try_stmt, ast::Node *parent_block,
                                           int next_stmt_index) {
    auto fn = ctx.fn;
    auto &machine = *ctx.machine;
    auto &local_vars = *ctx.local_vars;
    auto result_promise_ptr = ctx.result_promise_ptr;
    auto resume_point = ctx.resume_point;
    auto continue_state_id = ctx.continue_state_id;
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto &data = try_stmt->data.try_expr;
    auto resolved = get_async_resolved_awaits(resume_point, try_stmt);

    // Non-block try expression or no await: let compile_expr handle it synchronously
    if (data.expr->type != ast::NodeType::Block ||
        !contains_unresolved_await(try_stmt, resolved)) {
        compile_stmt(fn, try_stmt);
        if (!builder.GetInsertBlock()->getTerminator()) {
            auto cont = ctx;
            cont.resume_point = nullptr;
            compile_async_block_recursive(cont, parent_block, next_stmt_index);
        }
        return;
    }

    // --- Async try-block with await ---

    // 1. Register a catch state that will handle errors from any state within the try block.
    //    The catch state extracts the error from TLS and compiles the catch block.
    int catch_state_id = -1;
    if (data.catch_block) {
        auto try_stmt_ref = try_stmt;
        catch_state_id = register_async_custom_state(fn, machine, "try_catch", [&](Function *state_fn) {
            auto error_iface = get_resolver()->get_context()->rt_error_type;
            auto shared_error_type = get_resolver()->get_shared_type(error_iface);
            auto shared_error_type_l = compile_type(shared_error_type);

            auto err_alive_slot = builder.CreateStructGEP(machine.frame_struct_type, machine.frame_ptr,
                                                          machine.async_error_alive_index);
            auto err_alive = builder.CreateLoad(builder.getInt1Ty(), err_alive_slot, "_err_from_frame");

            auto from_tls_b = state_fn->new_label("_catch_from_tls");
            auto from_frame_b = state_fn->new_label("_catch_from_frame");
            builder.CreateCondBr(err_alive, from_frame_b, from_tls_b);

            // --- Path 1: Error from TLS (sync throw via landing pad) ---
            state_fn->use_label(from_tls_b);
            auto [tls_error_data, tls_error_vtable] = extract_tls_error(state_fn);
            auto tls_fat_iface_type_l = compile_type(
                get_resolver()->get_pointer_type(error_iface, TypeKind::MoveRef));
            llvm::Value *tls_fat_ptr = llvm::UndefValue::get(tls_fat_iface_type_l);
            tls_fat_ptr = builder.CreateInsertValue(tls_fat_ptr, tls_error_data, {0});
            tls_fat_ptr = builder.CreateInsertValue(tls_fat_ptr, tls_error_vtable, {1});
            auto tls_shared_error = compile_shared_new(state_fn, shared_error_type, tls_fat_ptr);
            auto merge_b = state_fn->new_label("_catch_merge");
            builder.CreateBr(merge_b);

            // --- Path 2: Error from frame slot (async rejection) ---
            state_fn->use_label(from_frame_b);
            auto frame_err_slot = builder.CreateStructGEP(machine.frame_struct_type, machine.frame_ptr,
                                                          machine.async_error_index);
            auto frame_shared_error = builder.CreateLoad(shared_error_type_l, frame_err_slot, "_frame_err");
            // Clear alive flag (we've consumed the error)
            builder.CreateStore(llvm::ConstantInt::getFalse(*m_ctx->llvm_ctx), err_alive_slot);
            builder.CreateBr(merge_b);

            // --- Merge ---
            state_fn->use_label(merge_b);
            auto shared_error = builder.CreatePHI(shared_error_type_l, 2, "_shared_error");
            llvm::cast<llvm::PHINode>(shared_error)->addIncoming(tls_shared_error, from_tls_b);
            llvm::cast<llvm::PHINode>(shared_error)->addIncoming(frame_shared_error, from_frame_b);
            // Shared.ref() expects a pointer (self) — spill the PHI value to a slot
            auto shared_error_ptr = state_fn->entry_alloca(shared_error_type_l, "_shared_error_slot");
            builder.CreateStore(shared_error, shared_error_ptr);

            // Track ownership of the shared_error so it is released on catch-state
            // exit paths that do not transfer the refcount to an err binding or
            // forward it to promise.reject_shared (typed-mismatch re-throw).
            auto *shared_error_active = register_cleanup_owner(
                state_fn, shared_error_ptr, shared_error_type, "_shared_error_slot.active");

            // Type check for typed catch
            if (data.catch_expr) {
                auto catch_type = get_resolver()->to_value_type(get_chitype(data.catch_expr));
                auto error_iface_ref = compile_shared_ref(state_fn, shared_error_ptr, shared_error_type);
                auto type_matches = compile_interface_type_match(
                    state_fn, error_iface_ref, error_iface, catch_type);

                auto match_b = state_fn->new_label("_catch_type_match");
                auto nomatch_b = state_fn->new_label("_catch_type_nomatch");
                builder.CreateCondBr(type_matches, match_b, nomatch_b);

                // Type doesn't match: reject promise (re-throw). reject_shared
                // takes the Shared<Error> by value bitwise, so clear the active
                // flag before the call to avoid a double release.
                state_fn->use_label(nomatch_b);
                builder.CreateStore(llvm::ConstantInt::getFalse(llvm_ctx), shared_error_active);
                emit_async_promise_reject_shared(state_fn, shared_error);
                builder.CreateRetVoid();

                state_fn->use_label(match_b);
            }

            // Set up error binding variable
            if (data.catch_err_var) {
                auto err_var = compile_alloc(state_fn, data.catch_err_var);
                add_var(data.catch_err_var, err_var);
                if (data.catch_expr) {
                    // Typed catch: extract data ptr from Shared<Error>. The
                    // wrapper's refcount still needs to be released on exit —
                    // leave shared_error_active set so cleanup_owner_vars fires.
                    auto ref = compile_shared_ref(state_fn, shared_error_ptr, shared_error_type);
                    auto raw_data = extract_interface_data_ptr(ref);
                    builder.CreateStore(raw_data, err_var);
                } else {
                    // Untyped catch: transfer the Shared<Error> into err_var
                    // bitwise. err_var's scope exit releases it, so clear
                    // shared_error_active to avoid a double release.
                    builder.CreateStore(shared_error, err_var);
                    builder.CreateStore(llvm::ConstantInt::getFalse(llvm_ctx), shared_error_active);
                }
                data.catch_block->data.block.implicit_vars.clear();
            }

            // Compile catch block (may contain awaits — handled by async lowering)
            auto saved_try_id = machine.active_try_catch_state_id;
            auto saved_try_stmt = machine.active_try_stmt;
            machine.active_try_catch_state_id = -1;
            machine.active_try_stmt = nullptr;

            map<ast::Node *, llvm::Value *> catch_locals;
            AsyncBlockContext catch_ctx = {state_fn, &machine, &catch_locals,
                                           state_fn->async_reject_promise_ptr, nullptr,
                                           continue_state_id};
            compile_async_block_recursive(catch_ctx, data.catch_block, 0,
                                          parent_block, next_stmt_index);

            // After catch, continue with parent's remaining statements
            if (!builder.GetInsertBlock()->getTerminator()) {
                compile_async_block_recursive(catch_ctx, parent_block, next_stmt_index);
            }

            machine.active_try_catch_state_id = saved_try_id;
            machine.active_try_stmt = saved_try_stmt;

            if (!builder.GetInsertBlock()->getTerminator()) {
                emit_cleanup_owners(state_fn);
                builder.CreateRetVoid();
            }
        });
    }

    // 2. Register a post-try continuation state that handles parent_block[next_stmt_index:]
    //    Uses register_async_resume_state (not custom state) to get full context:
    //    bind_ptr, default_method_struct, loop stack, try-block landing.
    AsyncResumePoint post_try_point = {};
    post_try_point.block = parent_block;
    post_try_point.stmt_index = next_stmt_index;
    post_try_point.continue_state_id = continue_state_id;
    post_try_point.loop_stack = snapshot_async_loop_stack(fn);
    int post_try_state_id = register_async_resume_state(fn, machine, post_try_point);

    int try_body_continue_id = post_try_state_id;

    // 3. Set try-catch context on the state machine (propagates to resumed states)
    auto saved_try_id = machine.active_try_catch_state_id;
    auto saved_try_stmt = machine.active_try_stmt;
    machine.active_try_catch_state_id = catch_state_id;
    machine.active_try_stmt = try_stmt;

    // 4. Set up landing pad for throws in the current worker function
    auto landing_b = fn->new_label("_async_try_landing");
    auto saved_landing = fn->try_block_landing;
    fn->try_block_landing = landing_b;

    // 5. Compile try block body — async lowering handles awaits, return, break, continue
    auto try_ctx = ctx;
    try_ctx.continue_state_id = try_body_continue_id;
    compile_async_block_recursive(try_ctx, data.expr, 0);

    // 6. Restore context
    fn->try_block_landing = saved_landing;
    machine.active_try_catch_state_id = saved_try_id;
    machine.active_try_stmt = saved_try_stmt;

    // 7. Normal completion: transition to Ok-wrapping state (result mode) or post-try state
    if (!builder.GetInsertBlock()->getTerminator()) {
        emit_async_state_transition(fn, machine, try_body_continue_id);
    }

    // 8. Landing pad for throws in the current worker function
    if (!landing_b->getParent()) {
        return; // No invokes used this landing pad
    }
    fn->llvm_fn->setPersonalityFn(get_system_fn("cx_personality")->llvm_fn);
    fn->use_label(landing_b);
    auto landing = builder.CreateLandingPad(m_ctx->get_caught_result_type(), 1);
    landing->addClause(llvm::ConstantPointerNull::get(builder.getPtrTy()));

    emit_async_landing_pad(fn, landing);
    if (data.catch_block) {
        builder.CreateCall(get_system_fn("cx_clear_panic_location")->llvm_fn, {});
        emit_async_state_transition(fn, machine, catch_state_id);
    } else {
        // No catch block (result mode): reject the promise
        auto [error_data, error_vtable] = extract_tls_error(fn);
        emit_async_promise_reject(fn, error_data, error_vtable);
        builder.CreateRetVoid();
    }
}

void Compiler::compile_async_block_recursive(const AsyncBlockContext &ctx, ast::Node *block,
                                             int stmt_index,
                                             ast::Node *continue_block,
                                             int continue_stmt_index,
                                             ast::Node *tail_return_expr_parent) {
    auto fn = ctx.fn;
    auto &machine = *ctx.machine;
    auto &local_vars = *ctx.local_vars;
    auto result_promise_ptr = ctx.result_promise_ptr;
    auto resume_point = ctx.resume_point;
    auto continue_state_id = ctx.continue_state_id;

    auto &builder = *m_ctx->llvm_builder;
    auto &data = block->data.block;

    auto scope = fn->push_scope();
    fn->push_active_block(&data);

    bool entering_block = !(resume_point && resume_point->block == block);
    if (entering_block && stmt_index == 0) {
        for (auto var : data.implicit_vars) {
            compile_stmt(fn, var);
            sync_async_frame_var(fn, machine, var, local_vars);
        }
        for (auto var : data.stmt_temp_vars) {
            compile_stmt(fn, var);
            // stmt_temp_vars have no initializer — they are filled at call sites.
            // Only redirect to frame slot; do NOT load/copy from the uninitialized local.
            if (machine.frame_var_index.count(var)) {
                auto frame_ptr = get_async_frame_field_ptr(fn, machine, var);
                add_var(var, frame_ptr);
                fn->async_frame_owned_vars.insert(var);
                local_vars[var] = frame_ptr;
            } else {
                local_vars[var] = get_var(var);
            }
        }
    }

    for (int i = stmt_index; i < data.statements.size(); i++) {
        fn->active_block_stmt_idx.back() = i;
        auto stmt = data.statements[i];
        const AsyncResumePoint *current_resume = nullptr;
        if (resume_point && resume_point->block == block && resume_point->stmt_index == i) {
            current_resume = resume_point;
            stmt = resume_point->resume_stmt ? resume_point->resume_stmt : stmt;
        }
        if (!current_resume && resume_point) {
            if (stmt->type == ast::NodeType::WhileStmt &&
                resume_point->block == stmt->data.while_stmt.body) {
                current_resume = resume_point;
            } else if (stmt->type == ast::NodeType::ForStmt &&
                       resume_point->block == stmt->data.for_stmt.body) {
                current_resume = resume_point;
            }
        }
        auto resolved = get_async_resolved_awaits(current_resume, stmt);

        if (stmt->type == ast::NodeType::IfExpr) {
            auto stmt_ctx = ctx;
            stmt_ctx.resume_point = current_resume;
            compile_async_if_recursive(stmt_ctx, stmt, block, i + 1);
            fn->pop_active_block();
            fn->pop_scope();
            return;
        }
        if (auto switch_expr = get_async_switch_expr(stmt)) {
            auto stmt_ctx = ctx;
            stmt_ctx.resume_point = current_resume;
            compile_async_switch_recursive(stmt_ctx, stmt, switch_expr, block, i, i + 1);
            fn->pop_active_block();
            fn->pop_scope();
            return;
        }
        if (stmt->type == ast::NodeType::WhileStmt) {
            auto stmt_ctx = ctx;
            stmt_ctx.resume_point = current_resume;
            compile_async_while_recursive(stmt_ctx, stmt, block, i, i + 1);
            fn->pop_active_block();
            fn->pop_scope();
            return;
        }
        if (stmt->type == ast::NodeType::ForStmt) {
            auto stmt_ctx = ctx;
            stmt_ctx.resume_point = current_resume;
            compile_async_for_recursive(stmt_ctx, stmt, block, i, i + 1);
            fn->pop_active_block();
            fn->pop_scope();
            return;
        }
        if (stmt->type == ast::NodeType::TryExpr) {
            auto stmt_ctx = ctx;
            stmt_ctx.resume_point = current_resume;
            compile_async_try_recursive(stmt_ctx, stmt, block, i + 1);
            fn->pop_active_block();
            fn->pop_scope();
            return;
        }
        if (stmt->type == ast::NodeType::BranchStmt) {
            auto token = stmt->token;
            auto loop = fn->get_loop();
            auto &break_flow = fn->active_blocks.back()->exit_flow;
            for (int j = fn->active_blocks.size() - 1; j >= (int)loop->active_blocks_depth; j--) {
                compile_block_cleanup(fn, fn->active_blocks[j], nullptr, break_flow);
            }
            if (token->type == TokenType::KW_BREAK) {
                auto break_ctx = ctx;
                break_ctx.resume_point = nullptr;
                break_ctx.continue_state_id = -1;
                compile_async_block_recursive(break_ctx, loop->async_break_block,
                                              loop->async_break_stmt_index);
            }
            if (token->type == TokenType::KW_CONTINUE) {
                emit_async_state_transition(fn, machine, loop->async_continue_state_id);
            }
            fn->pop_active_block();
            fn->pop_scope();
            return;
        }

        if (contains_unresolved_await(stmt, resolved)) {
            auto site = find_unresolved_await_site(stmt, resolved);
            auto promise_expr = site.await_expr;
            auto settled_type = get_async_resume_value_type(stmt, promise_expr, site.resume_expr);
            auto next = build_async_resume_point(block, i, stmt, current_resume, promise_expr,
                                                 continue_block,
                                                 continue_stmt_index, continue_state_id);
            next.loop_stack = snapshot_async_loop_stack(fn);
            auto state_id = register_async_resume_state(fn, machine, next);
            emit_async_suspend(fn, machine, state_id, promise_expr, settled_type,
                               result_promise_ptr);

            fn->pop_active_block();
            fn->pop_scope();
            return;
        }

        compile_stmt(fn, stmt);
        if (stmt->type == ast::NodeType::VarDecl) {
            sync_async_frame_var(fn, machine, stmt, local_vars);
        }
        if (builder.GetInsertBlock()->getTerminator()) {
            fn->pop_active_block();
            fn->pop_scope();
            return;
        }
    }

    if (data.return_expr) {
        fn->active_block_stmt_idx.back() = (int)data.statements.size();
    }
    const AsyncResumePoint *return_resume = nullptr;
    if (resume_point && resume_point->block == block &&
        resume_point->stmt_index == data.statements.size()) {
        return_resume = resume_point;
    }
    auto return_resolved = get_async_resolved_awaits(return_resume, data.return_expr);
    if (data.return_expr && contains_unresolved_await(data.return_expr, return_resolved)) {
        auto site = find_unresolved_await_site(data.return_expr, return_resolved);
        auto promise_expr = site.await_expr;
        auto settled_type = get_async_resume_value_type(data.return_expr, promise_expr, site.resume_expr);
        auto next = build_async_resume_point(block, data.statements.size(), data.return_expr,
                                             return_resume, promise_expr,
                                             continue_block, continue_stmt_index,
                                             continue_state_id, tail_return_expr_parent);
        next.loop_stack = snapshot_async_loop_stack(fn);
        auto state_id = register_async_resume_state(fn, machine, next);
        emit_async_suspend(fn, machine, state_id, promise_expr, settled_type, result_promise_ptr);

        fn->pop_active_block();
        fn->pop_scope();
        return;
    }

    if (data.return_expr && tail_return_expr_parent) {
        auto &llvm_builder = *m_ctx->llvm_builder.get();
        auto inner_type = get_chitype(tail_return_expr_parent);
        auto ret_value = compile_direct_call_arg(fn, data.return_expr, inner_type);

        auto promise_struct = get_resolver()->resolve_struct_type(machine.promise_type);
        std::optional<TypeId> variant_type_id = std::nullopt;
        if (machine.promise_type->kind == TypeKind::Subtype && !machine.promise_type->is_placeholder) {
            variant_type_id = machine.promise_type->id;
        }
        auto resolve_member = promise_struct->find_member("resolve");
        assert(resolve_member && "Promise.resolve() method not found");
        auto resolve_method_node = get_variant_member_node(resolve_member, variant_type_id);
        auto resolve_method = get_fn(resolve_method_node);
        llvm_builder.CreateCall(resolve_method->llvm_fn, {result_promise_ptr, ret_value});

        ast::Node *move_returned_var = nullptr;
        if (data.return_expr->analysis.moved &&
            data.return_expr->type == ast::NodeType::Identifier) {
            move_returned_var = data.return_expr->data.identifier.decl;
        }
        compile_block_cleanup(fn, &data, move_returned_var, data.exit_flow);
        fn->pop_active_block();
        fn->pop_scope();

        if (fn->return_label) {
            emit_async_function_return(fn, machine, result_promise_ptr);
        } else {
            emit_cleanup_owners(fn);
            llvm_builder.CreateRetVoid();
        }
        return;
    }

    if (!scope->branched && !builder.GetInsertBlock()->getTerminator()) {
        compile_block_cleanup(fn, &data, nullptr, data.exit_flow);
    }

    if (!data.return_expr && !continue_block && continue_state_id < 0 && !builder.GetInsertBlock()->getTerminator()) {
        auto inner_type = get_resolver()->get_promise_value_type(machine.promise_type);
        if (inner_type && inner_type->kind == TypeKind::Unit) {
            auto promise_struct = get_resolver()->resolve_struct_type(machine.promise_type);
            std::optional<TypeId> variant_type_id = std::nullopt;
            if (machine.promise_type->kind == TypeKind::Subtype &&
                !machine.promise_type->is_placeholder) {
                variant_type_id = machine.promise_type->id;
            }
            auto resolve_member = promise_struct->find_member("resolve");
            assert(resolve_member && "Promise.resolve() method not found");
            auto resolve_method_node = get_variant_member_node(resolve_member, variant_type_id);
            auto resolve_method = get_fn(resolve_method_node);
            builder.CreateCall(resolve_method->llvm_fn,
                               {result_promise_ptr,
                                llvm::Constant::getNullValue(compile_type(inner_type))});

            if (fn->return_label) {
                emit_async_function_return(fn, machine, result_promise_ptr);
            } else {
                emit_cleanup_owners(fn);
                builder.CreateRetVoid();
            }
            fn->pop_active_block();
            fn->pop_scope();
            return;
        }
    }

    fn->pop_active_block();
    fn->pop_scope();
}

void Compiler::compile_async_fn_body(Function *fn) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    auto fn_def = fn->get_def();
    auto body = fn_def->body;

    for (const auto &param_info : fn->parameter_info) {
        if (param_info.kind != ParameterKind::Regular)
            continue;
        auto param = fn_def->fn_proto->data.fn_proto.params[param_info.user_param_index];
        if (m_ctx->var_table.has_key(param))
            continue;
        auto llvm_param = fn->llvm_fn->getArg(param_info.llvm_index);
        auto param_type = param_info.type;
        auto var = fn->entry_alloca(compile_type(param_type), param->name);
        builder.CreateStore(llvm_param, var);
        add_var(param, var);
    }

    // Get return type (Promise<T>)
    auto return_type = fn->fn_type->data.fn.return_type;
    assert(get_resolver()->is_promise_type(return_type));

    // Promise<T> is now a Chi-native struct, compile it directly
    auto promise_struct_type_l = (llvm::StructType *)compile_type(return_type);

    // Fast path: no await sites, keep the existing direct lowering.
    if (!get_resolver()->contains_await(body)) {
        // No awaits - compile normally but with async reject enabled.
        // Pre-initialize the result promise so reject can use it if a throw happens.
        auto result_promise_ptr = fn->entry_alloca(promise_struct_type_l, "result_promise");
        auto promise_struct = get_resolver()->resolve_struct_type(return_type);
        auto new_member = promise_struct->find_member("new");
        std::optional<TypeId> variant_type_id = std::nullopt;
        if (return_type->kind == TypeKind::Subtype && !return_type->is_placeholder) {
            variant_type_id = return_type->id;
        }
        auto new_method_node = get_variant_member_node(new_member, variant_type_id);
        auto new_method = get_fn(new_method_node);
        builder.CreateCall(new_method->llvm_fn, {result_promise_ptr});

        fn->async_reject_promise_ptr = result_promise_ptr;
        fn->async_promise_type = return_type;
        compile_block(fn, fn->node, body, fn->return_label);
        return;
    }

    AsyncStateMachine machine = {};
    machine.parent_fn = fn;
    machine.promise_type = return_type;
    machine.promise_struct_type = promise_struct_type_l;
    auto saved_async_machine = fn->async_machine;
    fn->async_machine = &machine;
    initialize_async_frame(fn, machine, body);
    initialize_async_dispatcher(fn, machine);

    auto initial_state = AsyncResumePoint{body, 0, nullptr, {}};
    // Set tail_return_expr_parent so the body's trailing expression resolves the promise
    initial_state.tail_return_expr_parent = fn->node;
    register_async_resume_state(fn, machine, initial_state, 0);

    auto state_ptr = get_async_frame_state_ptr(fn, machine);
    builder.CreateStore(builder.getInt32(0), state_ptr);
    auto frame_data_ptr =
        builder.CreateBitCast(machine.frame_ptr, llvm::PointerType::get(*m_ctx->llvm_ctx, 0));
    emit_dbg_location(fn->node);
    builder.CreateCall(machine.dispatcher_fn, {frame_data_ptr});

    compile_copy_with_ref(fn, RefValue::from_address(machine.result_promise_ptr), fn->return_value,
                          return_type, fn->node);
    builder.CreateCall(get_system_fn("cx_capture_release")->llvm_fn, {machine.frame_capture_ptr});
    builder.CreateBr(fn->return_label);
    fn->async_machine = saved_async_machine;
}

// Emits the common landing pad logic: extract thrown_ptr, check is_typed,
// panic path (resume), and leaves insert point at the typed-error block. The
// typed-error path releases the C++ exception object via cx_dispose_exception
// since all typed async consumers (reject promise, catch state transition)
// consume the error from TLS and never resume unwinding.
void Compiler::emit_async_landing_pad(Function *fn, llvm::Value *landing) {
    auto &builder = *m_ctx->llvm_builder;

    auto thrown_ptr = builder.CreateExtractValue(landing, {0}, "thrown_ptr");
    auto is_typed = builder.CreateICmpNE(
        thrown_ptr, llvm::ConstantPointerNull::get(builder.getPtrTy()), "is_typed_error");

    auto typed_error_b = fn->new_label("_async_typed_error");
    auto panic_b = fn->new_label("_async_panic");
    builder.CreateCondBr(is_typed, typed_error_b, panic_b);

    // Panic: re-throw (unrecoverable)
    fn->use_label(panic_b);
    builder.CreateResume((llvm::LandingPadInst *)landing);

    fn->use_label(typed_error_b);
    builder.CreateCall(get_system_fn("cx_dispose_exception")->llvm_fn, {thrown_ptr});
}

void Compiler::emit_async_reject_landing_pad(Function *fn, llvm::Value *landing) {
    emit_async_landing_pad(fn, landing);
    auto [error_data, error_vtable] = extract_tls_error(fn);
    emit_async_promise_reject(fn, error_data, error_vtable);
}

void Compiler::emit_async_cleanup_landing_pad(Function *fn, llvm::Function *worker_llvm_fn) {
    if (!fn->cleanup_landing_label) return;
    auto &builder = *m_ctx->llvm_builder;
    worker_llvm_fn->setPersonalityFn(get_system_fn("cx_personality")->llvm_fn);
    fn->use_label(fn->cleanup_landing_label);
    auto landing = builder.CreateLandingPad(m_ctx->get_caught_result_type(), 1);
    landing->addClause(llvm::ConstantPointerNull::get(builder.getPtrTy()));
    emit_async_reject_landing_pad(fn, landing);
    emit_cleanup_owners(fn);
    builder.CreateRetVoid();
}

std::pair<llvm::Value *, llvm::Value *> Compiler::extract_tls_error(Function *fn) {
    auto &builder = *m_ctx->llvm_builder;
    auto error_data = builder.CreateCall(get_system_fn("cx_get_error_data")->llvm_fn, {}, "error_data");
    auto error_vtable =
        builder.CreateCall(get_system_fn("cx_get_error_vtable")->llvm_fn, {}, "error_vtable");
    builder.CreateCall(get_system_fn("cx_clear_panic_location")->llvm_fn, {});
    return {error_data, error_vtable};
}

llvm::Value *Compiler::init_async_worker_frame(Function *fn, AsyncStateMachine &machine,
                                                llvm::Function *worker_llvm_fn) {
    auto &builder = *m_ctx->llvm_builder;
    auto data_arg = worker_llvm_fn->getArg(0);
    machine.frame_ptr = builder.CreateBitCast(data_arg, machine.frame_struct_type->getPointerTo());
    auto result_promise_ptr =
        builder.CreateStructGEP(machine.frame_struct_type, machine.frame_ptr, 0);
    fn->async_reject_promise_ptr = result_promise_ptr;
    fn->async_promise_type = machine.promise_type;
    fn->async_machine = &machine;
    return result_promise_ptr;
}

AsyncWorkerContext Compiler::begin_async_worker_state(Function *fn, AsyncStateMachine &machine,
                                                      const string &name_suffix,
                                                      int forced_state_id) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto &llvm_module = *m_ctx->llvm_module;

    int state_id = forced_state_id >= 0 ? forced_state_id : machine.next_state_id++;
    if (forced_state_id >= 0 && machine.next_state_id <= forced_state_id) {
        machine.next_state_id = forced_state_id + 1;
    }

    auto ptr_type = llvm::PointerType::get(llvm_ctx, 0);
    auto worker_type = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type}, false);
    auto worker_name = name_suffix.empty()
                           ? fmt::format("{}__async_state_{}", fn->qualified_name, state_id)
                           : fmt::format("{}__async_state_{}_{}", fn->qualified_name, state_id,
                                         name_suffix);
    auto worker_llvm_fn = llvm::Function::Create(worker_type, llvm::Function::PrivateLinkage,
                                                  worker_name, llvm_module);
    auto state_fn = m_ctx->functions.emplace(new Function(m_ctx, worker_llvm_fn, fn->node))->get();
    state_fn->qualified_name = worker_name;

    auto saved_ip = builder.saveIP();

    auto dispatch_case_b =
        llvm::BasicBlock::Create(llvm_ctx, fmt::format("_state_{}", state_id), machine.dispatcher_fn);
    machine.dispatcher_switch->addCase(builder.getInt32(state_id), dispatch_case_b);
    builder.SetInsertPoint(dispatch_case_b);
    auto dispatch_data_arg = machine.dispatcher_fn->getArg(0);
    builder.CreateCall(worker_llvm_fn, {dispatch_data_arg});
    builder.CreateRetVoid();

    auto entry_b = state_fn->new_label("_entry");
    state_fn->use_label(entry_b);

    auto previous_frame_ptr = machine.frame_ptr;
    auto result_promise_ptr = init_async_worker_frame(state_fn, machine, worker_llvm_fn);

    map<ast::Node *, llvm::Value *> shadowed_vars;
    std::set<ast::Node *> had_shadowed_binding;
    for (auto var : machine.frame_vars) {
        if (m_ctx->var_table.has_key(var)) {
            shadowed_vars[var] = get_var(var);
            had_shadowed_binding.insert(var);
        }
        auto frame_ptr = get_async_frame_field_ptr(state_fn, machine, var);
        add_var(var, frame_ptr);
    }

    return {state_id,
            state_fn,
            worker_llvm_fn,
            result_promise_ptr,
            saved_ip,
            previous_frame_ptr,
            std::move(shadowed_vars),
            std::move(had_shadowed_binding)};
}

void Compiler::end_async_worker_state(AsyncStateMachine &machine, AsyncWorkerContext &ctx) {
    auto &builder = *m_ctx->llvm_builder;

    if (!builder.GetInsertBlock()->getTerminator()) {
        emit_cleanup_owners(ctx.state_fn);
        builder.CreateRetVoid();
    }

    emit_async_cleanup_landing_pad(ctx.state_fn, ctx.worker_llvm_fn);

    for (auto var : machine.frame_vars) {
        if (ctx.had_shadowed_binding.count(var)) {
            m_ctx->var_table[var] = ctx.shadowed_vars[var];
        } else {
            m_ctx->var_table.unset(var);
        }
    }

    llvm::verifyFunction(*ctx.worker_llvm_fn);
    machine.frame_ptr = ctx.previous_frame_ptr;
    builder.restoreIP(ctx.saved_ip);
}

void Compiler::emit_async_promise_reject(Function *fn, llvm::Value *data_ptr,
                                         llvm::Value *vtable_ptr) {
    auto &builder = *m_ctx->llvm_builder;

    // Build FatIFacePointer<Error> = { data_ptr, vtable_ptr }
    auto rt_error = get_resolver()->get_context()->rt_error_type;
    auto move_ref_error = get_resolver()->get_pointer_type(rt_error, TypeKind::MoveRef);
    auto fat_iface_type_l = compile_type(move_ref_error);
    llvm::Value *fat_ptr = llvm::UndefValue::get(fat_iface_type_l);
    fat_ptr = builder.CreateInsertValue(fat_ptr, data_ptr, {0});
    fat_ptr = builder.CreateInsertValue(fat_ptr, vtable_ptr, {1});

    // Resolve and call promise.reject(err)
    auto promise_struct = get_resolver()->resolve_struct_type(fn->async_promise_type);
    std::optional<TypeId> variant_type_id = std::nullopt;
    if (fn->async_promise_type->kind == TypeKind::Subtype &&
        !fn->async_promise_type->is_placeholder) {
        variant_type_id = fn->async_promise_type->id;
    }
    auto reject_member = promise_struct->find_member("reject");
    assert(reject_member && "Promise.reject() method not found");
    auto reject_method_node = get_variant_member_node(reject_member, variant_type_id);
    auto reject_method = get_fn(reject_method_node);
    emit_dbg_location(fn->node);
    builder.CreateCall(reject_method->llvm_fn, {fn->async_reject_promise_ptr, fat_ptr});
}

void Compiler::emit_async_promise_reject_shared(Function *fn, llvm::Value *shared_error) {
    auto &builder = *m_ctx->llvm_builder;

    auto promise_struct = get_resolver()->resolve_struct_type(fn->async_promise_type);
    std::optional<TypeId> variant_type_id = std::nullopt;
    if (fn->async_promise_type->kind == TypeKind::Subtype &&
        !fn->async_promise_type->is_placeholder) {
        variant_type_id = fn->async_promise_type->id;
    }
    auto reject_member = promise_struct->find_member("reject_shared");
    assert(reject_member && "Promise.reject_shared() method not found");
    auto reject_method_node = get_variant_member_node(reject_member, variant_type_id);
    auto reject_method = get_fn(reject_method_node);
    emit_dbg_location(fn->node);
    builder.CreateCall(reject_method->llvm_fn, {fn->async_reject_promise_ptr, shared_error});
}

llvm::Value *Compiler::compile_shared_new(Function *fn, ChiType *shared_type,
                                          llvm::Value *owned_value) {
    auto &builder = *m_ctx->llvm_builder;
    auto shared_type_l = compile_type(shared_type);
    auto shared_var = fn->entry_alloca(shared_type_l, "_shared_new");

    auto shared_struct = get_resolver()->resolve_struct_type(shared_type);
    assert(shared_struct);
    auto variant_type_id = resolve_variant_type_id(fn, shared_type);
    auto new_member = shared_struct->find_member("new");
    assert(new_member && "Shared.new() method not found");
    auto new_node = get_variant_member_node(new_member, variant_type_id);
    auto new_fn = get_fn(new_node);
    builder.CreateCall(new_fn->llvm_fn, {shared_var, owned_value});
    return builder.CreateLoad(shared_type_l, shared_var, "_shared_value");
}

llvm::Value *Compiler::compile_shared_ref(Function *fn, llvm::Value *shared_ptr,
                                          ChiType *shared_type) {
    auto shared_struct = get_resolver()->resolve_struct_type(shared_type);
    assert(shared_struct);
    auto ref_member = shared_struct->find_member("ref");
    assert(ref_member && "Shared.ref() method not found");
    auto variant_type_id = resolve_variant_type_id(fn, shared_type);
    auto ref_node = get_variant_member_node(ref_member, variant_type_id);
    auto ref_fn = get_fn(ref_node);
    return m_ctx->llvm_builder->CreateCall(ref_fn->llvm_fn, {shared_ptr});
}

llvm::Value *Compiler::extract_interface_data_ptr(llvm::Value *fat_ptr) {
    return m_ctx->llvm_builder->CreateExtractValue(fat_ptr, {0}, "iface_data_ptr");
}

llvm::Value *Compiler::extract_interface_vtable_ptr(llvm::Value *fat_ptr) {
    return m_ctx->llvm_builder->CreateExtractValue(fat_ptr, {1}, "iface_vtable_ptr");
}

llvm::Value *Compiler::load_runtime_type_info_from_vtable(llvm::Value *vtable_ptr) {
    auto ptr_type = get_llvm_ptr_type();
    return m_ctx->llvm_builder->CreateLoad(ptr_type, vtable_ptr, "runtime_ti");
}

llvm::Value *Compiler::load_runtime_type_info_from_fat_ptr(llvm::Value *fat_ptr) {
    auto vtable_ptr = extract_interface_vtable_ptr(fat_ptr);
    return load_runtime_type_info_from_vtable(vtable_ptr);
}

llvm::Value *Compiler::compile_interface_type_match(Function *fn, llvm::Value *fat_ptr,
                                                    ChiType *iface_type,
                                                    ChiType *concrete_type) {
    auto &builder = *m_ctx->llvm_builder;
    auto ptr_type = get_llvm_ptr_type();
    auto runtime_ti = load_runtime_type_info_from_fat_ptr(fat_ptr);

    auto impl = concrete_type->data.struct_.interface_table[iface_type];
    assert(impl);
    auto vtable_global = m_ctx->impl_table[impl];
    assert(vtable_global);
    auto case_ti = builder.CreateLoad(ptr_type, vtable_global, "case_ti");
    return builder.CreateICmpEQ(runtime_ti, case_ti, "type_matches");
}

// Allocates a stack drop flag for `owned_ptr`, initialized false at function
// entry and set true at the current insert point, then registers the pair in
// `fn->cleanup_owner_vars` so `emit_cleanup_owners` destroys the value on any
// state-worker exit. Caller stores `false` into the returned flag when the
// refcount/ownership is transferred elsewhere (avoiding double destruction).
llvm::Value *Compiler::register_cleanup_owner(Function *fn, llvm::Value *owned_ptr,
                                              ChiType *concrete_type,
                                              const std::string &active_name) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto *active_var = fn->entry_alloca(llvm::Type::getInt1Ty(llvm_ctx), active_name);
    {
        llvm::IRBuilder<> tmp(llvm_ctx);
        tmp.SetInsertPoint(llvm::cast<llvm::Instruction>(active_var)->getNextNode());
        tmp.CreateStore(llvm::ConstantInt::getFalse(llvm_ctx), active_var);
    }
    builder.CreateStore(llvm::ConstantInt::getTrue(llvm_ctx), active_var);
    fn->cleanup_owner_vars.push_back({owned_ptr, concrete_type, active_var, false});
    return active_var;
}

// For a stack slot that may be dynamically re-entered (e.g., a vararg blob
// inside a loop), this allocates a flag init-false at entry, emits a
// conditional destroy-and-clear at the current insert point, then registers
// the slot for exit cleanup. Caller must store `true` into the returned flag
// after populating the slot.
llvm::Value *Compiler::register_reusable_cleanup_slot(Function *fn, llvm::Value *slot_ptr,
                                                     ChiType *concrete_type,
                                                     const std::string &active_name) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto *i1_ty = llvm::Type::getInt1Ty(llvm_ctx);
    auto *active_var = fn->entry_alloca(i1_ty, active_name);
    {
        llvm::IRBuilder<> tmp(llvm_ctx);
        tmp.SetInsertPoint(llvm::cast<llvm::Instruction>(active_var)->getNextNode());
        tmp.CreateStore(llvm::ConstantInt::getFalse(llvm_ctx), active_var);
    }
    auto *alive = builder.CreateLoad(i1_ty, active_var);
    auto *do_destroy = fn->new_label("_slot_reuse_destroy");
    auto *after_destroy = fn->new_label("_slot_reuse_done");
    builder.CreateCondBr(alive, do_destroy, after_destroy);
    fn->use_label(do_destroy);
    compile_destruction_for_type(fn, slot_ptr, concrete_type);
    builder.CreateStore(llvm::ConstantInt::getFalse(llvm_ctx), active_var);
    builder.CreateBr(after_destroy);
    fn->use_label(after_destroy);
    fn->cleanup_owner_vars.push_back({slot_ptr, concrete_type, active_var, false});
    return active_var;
}

void Compiler::emit_cleanup_owners(Function *fn) {
    auto &builder = *m_ctx->llvm_builder;
    for (auto &owner : fn->cleanup_owner_vars) {
        llvm::Value *should_cleanup = nullptr;
        llvm::Value *owned_ptr = nullptr;
        if (owner.active_var) {
            should_cleanup =
                builder.CreateLoad(llvm::Type::getInt1Ty(*m_ctx->llvm_ctx), owner.active_var);
        } else {
            assert(owner.free_heap);
            owned_ptr = builder.CreateLoad(builder.getPtrTy(), owner.ptr_var);
            should_cleanup = builder.CreateICmpNE(
                owned_ptr, llvm::ConstantPointerNull::get(builder.getPtrTy()));
        }

        auto do_cleanup_b = fn->new_label("_owner_cleanup");
        auto skip_cleanup_b = fn->new_label("_owner_cleanup_done");
        builder.CreateCondBr(should_cleanup, do_cleanup_b, skip_cleanup_b);

        fn->use_label(do_cleanup_b);
        if (owner.free_heap) {
            if (owner.concrete_type) {
                compile_destruction_for_type(fn, owned_ptr, owner.concrete_type);
            }
            builder.CreateCall(get_system_fn("cx_free")->llvm_fn, {owned_ptr});
        } else if (owner.concrete_type) {
            compile_destruction_for_type(fn, owner.ptr_var, owner.concrete_type);
        }
        builder.CreateBr(skip_cleanup_b);

        fn->use_label(skip_cleanup_b);
    }
}

llvm::BasicBlock *Compiler::emit_invoke_unwind_landing(Function *fn) {
    auto &builder = *m_ctx->llvm_builder;
    auto saved_bb = builder.GetInsertBlock();

    if (!fn->llvm_fn->hasPersonalityFn()) {
        fn->llvm_fn->setPersonalityFn(get_system_fn("cx_personality")->llvm_fn);
    }

    auto landing_b = fn->new_label("_inv_landing");
    fn->use_label(landing_b);

    if (fn->async_reject_promise_ptr) {
        auto landing = builder.CreateLandingPad(m_ctx->get_caught_result_type(), 1);
        landing->addClause(llvm::ConstantPointerNull::get(builder.getPtrTy()));
        emit_async_reject_landing_pad(fn, landing);
        emit_cleanup_owners(fn);
        builder.CreateRetVoid();
    } else {
        auto landing = builder.CreateLandingPad(m_ctx->get_caught_result_type(), 0);
        landing->setCleanup(true);
        builder.CreateExtractValue(landing, {0});
        builder.CreateExtractValue(landing, {1});
        if (!fn->active_blocks.empty()) {
            auto &unwind_flow = fn->active_blocks.back()->exit_flow;
            for (int i = (int)fn->active_blocks.size() - 1; i >= 0; i--) {
                compile_block_cleanup(fn, fn->active_blocks[i], nullptr, unwind_flow);
            }
        } else if (fn->get_def() && fn->get_def()->body) {
            auto &body_block = fn->get_def()->body->data.block;
            compile_block_cleanup(fn, &body_block, nullptr, body_block.exit_flow);
        }
        emit_cleanup_owners(fn);
        fn->insn_noop();
        builder.CreateResume(landing);
    }

    if (saved_bb) {
        builder.SetInsertPoint(saved_bb);
    }
    return landing_b;
}

llvm::Value *Compiler::compile_fn_call_with_invoke(Function *fn, ast::Node *call_expr,
                                                   llvm::Value *dest) {
    auto &builder = *m_ctx->llvm_builder.get();
    InvokeInfo invoke;
    if (fn->try_block_landing) {
        // Inside a try-block: route exceptions to the try-block landing pad so
        // they are caught rather than cleaned-up-and-re-thrown (which would exit
        // the function before the catch block runs).
        invoke.landing = fn->try_block_landing;
    } else {
        invoke.landing = emit_invoke_unwind_landing(fn);
    }
    // Don't create normal label eagerly — the invoke sites create it on-demand.
    // This avoids orphaned blocks when compile_fn_call bypasses invoke
    // (e.g., builtin trait calls like int.hash() that can't throw).
    invoke.normal = nullptr;
    auto ret = compile_fn_call(fn, call_expr, &invoke, dest);
    if (invoke.used) {
        fn->has_cleanup_invoke = true;
    }
    if (dest) {
        if (invoke.sret) {
            // Struct return: already written to dest via sret
        } else if (ret) {
            builder.CreateStore(ret, dest);
        }
        return nullptr;
    }
    if (invoke.sret) {
        return builder.CreateLoad(invoke.sret_type, invoke.sret);
    }
    return ret;
}

llvm::Value *Compiler::compile_number_conversion(Function *fn, llvm::Value *value,
                                                 ChiType *from_type, ChiType *to_type) {
    auto from_int = from_type->is_int_like() || from_type->is_pointer_like();
    auto to_int = to_type->is_int_like() || to_type->is_pointer_like();

    if (from_int && to_type->kind == TypeKind::Float) {
        auto &builder = *m_ctx->llvm_builder;
        bool is_unsigned;
        if (from_type->kind == TypeKind::Bool) {
            is_unsigned = true;
        } else if (from_type->kind == TypeKind::Byte || from_type->kind == TypeKind::Rune) {
            is_unsigned = true;
        } else {
            is_unsigned = from_type->data.int_.is_unsigned;
        }
        auto from_type_l = compile_type(from_type);
        auto to_type_l = compile_type(to_type);
        if (is_unsigned) {
            return builder.CreateUIToFP(value, to_type_l);
        } else {
            return builder.CreateSIToFP(value, to_type_l);
        }
    } else if (from_type->kind == TypeKind::Float && to_int) {
        auto &builder = *m_ctx->llvm_builder;
        bool is_unsigned;
        if (to_type->kind == TypeKind::Bool || to_type->kind == TypeKind::Byte ||
            to_type->kind == TypeKind::Rune) {
            is_unsigned = true;
        } else {
            is_unsigned = to_type->data.int_.is_unsigned;
        }
        auto from_type_l = compile_type(from_type);
        auto to_type_l = compile_type(to_type);
        if (is_unsigned) {
            return builder.CreateFPToUI(value, to_type_l);
        } else {
            return builder.CreateFPToSI(value, to_type_l);
        }
    } else if (from_int && to_int) {
        auto &builder = *m_ctx->llvm_builder;
        auto from_type_l = compile_type(from_type);
        auto to_type_l = compile_type(to_type);
        bool is_signed;
        if (from_type->kind == TypeKind::Bool || from_type->kind == TypeKind::Byte ||
            from_type->kind == TypeKind::Rune) {
            is_signed = false;
        } else {
            is_signed = !from_type->data.int_.is_unsigned;
        }
        return builder.CreateIntCast(value, to_type_l, is_signed);

    } else if (from_type->kind == TypeKind::Float && to_type->kind == TypeKind::Float) {
        auto &builder = *m_ctx->llvm_builder;
        auto from_type_l = compile_type(from_type);
        auto from_size = llvm_type_size(from_type_l);
        auto to_type_l = compile_type(to_type);
        auto to_size = llvm_type_size(to_type_l);
        if (from_size < to_size) {
            return builder.CreateFPExt(value, to_type_l);
        } else if (from_size > to_size) {
            return builder.CreateFPTrunc(value, to_type_l);
        } else {
            return value;
        }
    }

    panic("number conversion not implemented: {} -> {}",
          get_resolver()->format_type_display(from_type),
          get_resolver()->format_type_display(to_type));
    return nullptr;
}

llvm::Value *Compiler::compile_conversion(Function *fn, llvm::Value *value, ChiType *from_type,
                                          ChiType *to_type, bool owns_value) {
    // never is the bottom type — unreachable code, return undef
    if (from_type->kind == TypeKind::Never) {
        return llvm::UndefValue::get(compile_type(to_type));
    }

    switch (to_type->kind) {
    case TypeKind::Any: {
        if (from_type->kind == TypeKind::Any) {
            return value;
        }
        if (get_resolver()->type_needs_destruction(from_type)) {
            panic("naive value conversion is forbidden for non-trivial type {} in function {}",
                  get_resolver()->format_type_display(from_type),
                  fn && fn->node ? fn->node->name : "<null>");
        }
        auto &llvm_builder = *(m_ctx->llvm_builder.get());
        auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
        auto ti_p = compile_type_info(from_type);
        auto from_type_l = compile_type(from_type);
        auto any_type_l = (llvm::StructType *)compile_type(to_type);
        auto any_var = (llvm::Value *)llvm_builder.CreateAlloca(any_type_l, nullptr, "to_any");
        auto ti_gep = llvm_builder.CreateStructGEP(any_type_l, any_var, 0);
        llvm_builder.CreateStore(ti_p, ti_gep);

        auto inlined_gep = llvm_builder.CreateStructGEP(any_type_l, any_var, 1);
        auto type_size = llvm_type_size(from_type_l);

        // Copy and inline the data if possible, otherwise allocate it
        auto inlined = type_size <= sizeof(CxAnyStorage);
        llvm_builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_ctx), inlined),
                                 inlined_gep);
        auto data_gep = llvm_builder.CreateStructGEP(any_type_l, any_var, 3);
        if (inlined) {
            compile_copy(fn, value, data_gep, from_type, nullptr);
        } else {
            auto copy_p = llvm_builder.CreateAlloca(from_type_l, nullptr, "any_data_copy");
            compile_copy(fn, value, copy_p, from_type, nullptr);
            llvm_builder.CreateStore(copy_p, data_gep);
        }
        return llvm_builder.CreateLoad(any_type_l, any_var);
    }
    case TypeKind::Bool: {
        auto &builder = *m_ctx->llvm_builder;
        switch (from_type->kind) {
        case TypeKind::Optional: {
            auto has_value = builder.CreateExtractValue(value, {0}, "has_value");
            return has_value;
        }
        case TypeKind::Pointer:
        case TypeKind::Reference:
        case TypeKind::MutRef:
        case TypeKind::MoveRef: {
            auto elem = from_type->get_elem();
            if (elem && ChiTypeStruct::is_interface(elem)) {
                // Fat pointer struct {data_ptr, vtable_ptr} — check data_ptr (field 0) for null
                auto data_ptr = builder.CreateExtractValue(value, {0}, "data_ptr");
                return builder.CreateICmp(llvm::CmpInst::Predicate::ICMP_NE, data_ptr,
                                          get_null_ptr());
            }
            return builder.CreateICmp(
                llvm::CmpInst::Predicate::ICMP_NE, value,
                llvm::ConstantPointerNull::get((llvm::PointerType *)compile_type(from_type)));
        }
        default: {
            // For numbers, compare to zero.
            auto *zero = llvm::ConstantInt::get(value->getType(), 0);
            return builder.CreateICmpNE(value, zero);
        }
        }
        break;
    }
    case TypeKind::Int: {
        if (from_type->kind == TypeKind::Float) {
            return compile_number_conversion(fn, value, from_type, to_type);
        }
        if (from_type->kind == TypeKind::Pointer) {
            return m_ctx->llvm_builder->CreatePtrToInt(value, compile_type(to_type));
        }
        // Plain enum -> int: extract discriminator and cast to target int type
        if (from_type->kind == TypeKind::EnumValue) {
            auto &builder = *m_ctx->llvm_builder;
            auto enum_ = from_type->data.enum_value.parent_enum();
            auto disc_type_l = compile_type(enum_->discriminator);
            auto disc_val = builder.CreateExtractValue(value, {0});
            return builder.CreateIntCast(disc_val, compile_type(to_type),
                                         !enum_->discriminator->data.int_.is_unsigned);
        }
        return compile_number_conversion(fn, value, from_type, to_type);
    }
    case TypeKind::Byte:
    case TypeKind::Rune: {
        return compile_number_conversion(fn, value, from_type, to_type);
    }
    case TypeKind::Float: {
        return compile_number_conversion(fn, value, from_type, to_type);
    }
    case TypeKind::FnLambda: {
        if (from_type->kind == TypeKind::Fn) {
            return compile_lambda_alloc(fn, to_type, value, nullptr);
        }
        // For FnLambda -> FnLambda with void->Unit return conversion,
        // wrap the lambda with a proxy that returns Unit
        if (from_type->kind == TypeKind::FnLambda) {
            auto from_fn = from_type->data.fn_lambda.fn;
            auto to_fn = to_type->data.fn_lambda.fn;
            if (from_fn->data.fn.return_type->kind == TypeKind::Void &&
                to_fn->data.fn.return_type->kind == TypeKind::Unit) {
                return compile_void_to_unit_lambda_wrapper(fn, value, from_type, to_type,
                                                           owns_value);
            }
        }
        return value;
    }
    case TypeKind::Unit: {
        // void -> Unit: produce zero Unit value
        if (from_type->kind == TypeKind::Void) {
            return llvm::Constant::getNullValue(compile_type(to_type));
        }
        return value;
    }
    case TypeKind::Optional: {
        if (from_type->kind == TypeKind::Null) {
            // null -> null optional
            return llvm::ConstantAggregateZero::get(compile_type(to_type));
        }
        if (get_resolver()->is_same_type(from_type, to_type)) {
            return value;
        }
        // Implicit T -> ?T wrap: construct optional with has_value=true
        auto &builder = *m_ctx->llvm_builder;
        auto opt_type_l = compile_type(to_type);
        auto opt_ptr = builder.CreateAlloca(opt_type_l, nullptr, "implicit_opt");
        // Set has_value = true
        auto has_value_p = builder.CreateStructGEP(opt_type_l, opt_ptr, 0);
        builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt1Ty(*m_ctx->llvm_ctx), 1),
                            has_value_p);
        // Store the value (convert if needed)
        auto elem_type = eval_type(to_type->get_elem());
        auto value_p = builder.CreateStructGEP(opt_type_l, opt_ptr, 1);
        auto inner_value = compile_conversion(fn, value, from_type, elem_type);
        emit_dbg_location(fn->node);
        compile_copy(fn, inner_value, value_p, elem_type, nullptr);
        return builder.CreateLoad(opt_type_l, opt_ptr);
    }
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef: {
        if (from_type->kind == TypeKind::Null) {
            auto t = compile_type(to_type);
            if (t->isPointerTy()) {
                return llvm::ConstantPointerNull::get((llvm::PointerType *)t);
            }
            // Fat pointer (interface ref): zeroed struct {null, null}
            return llvm::ConstantAggregateZero::get(t);
        }
        auto to_elem = to_type->get_elem();
        if (to_elem && ChiTypeStruct::is_interface(to_elem)) {
            auto from_elem = from_type->get_elem();
            // If source is already an interface reference, no conversion needed
            if (from_elem && ChiTypeStruct::is_interface(from_elem)) {
                return value;
            }
            // Build fat pointer {data_ptr, vtable_ptr}
            auto &builder = *m_ctx->llvm_builder;
            auto iface_type_l = compile_type(to_type);
            auto vp = builder.CreateAlloca(iface_type_l);
            auto data_p = builder.CreateStructGEP(iface_type_l, vp, 0);
            builder.CreateStore(value, data_p);
            auto vtable_p = builder.CreateStructGEP(iface_type_l, vp, 1);
            // Unwrap generic instantiations (Subtype) to their concrete struct type,
            // which has the interface_table populated by can_assign/struct_satisfies_interface.
            if (from_elem && from_elem->kind == TypeKind::Subtype) {
                assert(from_elem->data.subtype.final_type && "unresolved subtype in codegen");
                from_elem = from_elem->data.subtype.final_type;
            }
            if (from_elem && from_elem->kind == TypeKind::Struct &&
                !ChiTypeStruct::is_interface(from_elem)) {
                // &Concrete → &Interface: look up vtable from impl table
                auto impl = from_elem->data.struct_.interface_table[to_elem];
                assert(impl);
                auto vtable = m_ctx->impl_table[impl];
                assert(vtable);
                builder.CreateStore(vtable, vtable_p);
            } else {
                // *void → *Interface: no vtable yet, null
                builder.CreateStore(get_null_ptr(), vtable_p);
            }
            return builder.CreateLoad(iface_type_l, vp);
        }
        // Int to pointer
        if (from_type->is_int()) {
            return m_ctx->llvm_builder->CreateIntToPtr(value, compile_type(to_type));
        }
        return value;
    }
    case TypeKind::Struct: {
    }
    case TypeKind::EnumValue: {
        // int -> plain enum: cast to discriminator type, wrap in enum struct
        if (from_type->kind == TypeKind::Int) {
            auto &builder = *m_ctx->llvm_builder;
            auto enum_ = to_type->data.enum_value.parent_enum();
            auto disc_type = compile_type(enum_->discriminator);
            auto disc_val = builder.CreateIntCast(value, disc_type,
                                                  !from_type->data.int_.is_unsigned);
            auto enum_type_l = compile_type(to_type);
            auto alloca = builder.CreateAlloca(enum_type_l);
            auto gep = builder.CreateStructGEP(enum_type_l, alloca, 0);
            builder.CreateStore(disc_val, gep);
            return builder.CreateLoad(enum_type_l, alloca);
        }
        return value;
    }

    default:
        // by default, do nothing
        return value;
    }
}

static llvm::CmpInst::Predicate get_cmpop(TokenType op, ChiType *type) {
    if (type->kind == TypeKind::Float) {
        switch (op) {
        case TokenType::LT:
            return llvm::CmpInst::Predicate::FCMP_OLT;
        case TokenType::GT:
            return llvm::CmpInst::Predicate::FCMP_OGT;
        case TokenType::LE:
            return llvm::CmpInst::Predicate::FCMP_OLE;
        case TokenType::GE:
            return llvm::CmpInst::Predicate::FCMP_OGE;
        case TokenType::EQ:
            return llvm::CmpInst::Predicate::FCMP_OEQ;
        case TokenType::NE:
            return llvm::CmpInst::Predicate::FCMP_UNE;
        default:
            panic("not implemented: {}", PRINT_ENUM(op));
        }
    }
    auto is_unsigned = (type->kind == TypeKind::Int && type->data.int_.is_unsigned) ||
                       type->kind == TypeKind::Pointer || type->kind == TypeKind::Byte ||
                       type->kind == TypeKind::Rune;
    switch (op) {
    case TokenType::LT:
        return is_unsigned ? llvm::CmpInst::Predicate::ICMP_ULT
                           : llvm::CmpInst::Predicate::ICMP_SLT;
    case TokenType::GT:
        return is_unsigned ? llvm::CmpInst::Predicate::ICMP_UGT
                           : llvm::CmpInst::Predicate::ICMP_SGT;
    case TokenType::LE:
        return is_unsigned ? llvm::CmpInst::Predicate::ICMP_ULE
                           : llvm::CmpInst::Predicate::ICMP_SLE;
    case TokenType::GE:
        return is_unsigned ? llvm::CmpInst::Predicate::ICMP_UGE
                           : llvm::CmpInst::Predicate::ICMP_SGE;
    case TokenType::EQ:
        return llvm::CmpInst::Predicate::ICMP_EQ;
    case TokenType::NE:
        return llvm::CmpInst::Predicate::ICMP_NE;
    default:
        panic("not implemented: {}", PRINT_ENUM(op));
    };
    return llvm::CmpInst::Predicate::BAD_ICMP_PREDICATE;
}

static llvm::BinaryOperator::BinaryOps get_binop(TokenType op, ChiType *type) {
    if (type->kind == TypeKind::Float) {
        switch (op) {
        case TokenType::ADD:
            return llvm::BinaryOperator::BinaryOps::FAdd;
        case TokenType::SUB:
            return llvm::BinaryOperator::BinaryOps::FSub;
        case TokenType::MUL:
            return llvm::BinaryOperator::BinaryOps::FMul;
        case TokenType::DIV:
            return llvm::BinaryOperator::BinaryOps::FDiv;
        case TokenType::MOD:
            return llvm::BinaryOperator::BinaryOps::FRem;
        default:
            panic("not implemented: {}", PRINT_ENUM(op));
        }
        return llvm::BinaryOperator::BinaryOps::FAdd;
    }
    switch (op) {
    case TokenType::ADD:
        return llvm::BinaryOperator::BinaryOps::Add;
    case TokenType::SUB:
        return llvm::BinaryOperator::BinaryOps::Sub;
    case TokenType::MUL:
        return llvm::BinaryOperator::BinaryOps::Mul;
    case TokenType::DIV:
        return llvm::BinaryOperator::BinaryOps::SDiv;
    case TokenType::MOD:
        return llvm::BinaryOperator::BinaryOps::SRem;
    case TokenType::AND:
        return llvm::BinaryOperator::BinaryOps::And;
    case TokenType::OR:
        return llvm::BinaryOperator::BinaryOps::Or;
    case TokenType::XOR:
        return llvm::BinaryOperator::BinaryOps::Xor;
    case TokenType::LSHIFT:
        return llvm::BinaryOperator::BinaryOps::Shl;
    case TokenType::RSHIFT:
        return llvm::BinaryOperator::BinaryOps::AShr;
    case TokenType::LAND:
        return llvm::BinaryOperator::BinaryOps::And;
    case TokenType::LOR:
        return llvm::BinaryOperator::BinaryOps::Or;
    default:
        panic("not implemented: {}", PRINT_ENUM(op));
    }
    return llvm::BinaryOperator::BinaryOps::Add;
}

llvm::Value *Compiler::compile_expr(Function *fn, ast::Node *expr) {
    if (expr->resolved_node) {
        return compile_expr(fn, expr->resolved_node);
    }
    switch (expr->type) {
    case ast::NodeType::FnCallExpr: {
        // Write result directly into resolved_outlet alloca, then load from it
        if (expr->resolved_outlet) {
            auto &builder = *m_ctx->llvm_builder.get();
            auto dest = get_var(expr->resolved_outlet);
            auto type_l = compile_type(get_chitype(expr));
            if (fn->get_def()->has_cleanup || fn->async_reject_promise_ptr) {
                compile_fn_call_with_invoke(fn, expr, dest);
            } else {
                compile_fn_call(fn, expr, nullptr, dest);
            }
            mark_outlet_alive(fn, expr->resolved_outlet);
            return builder.CreateLoad(type_l, dest);
        }
        auto &call_data = expr->data.fn_call_expr;

        // Handle optional chaining method calls: obj?.method()
        if (call_data.fn_ref_expr->type == ast::NodeType::DotExpr &&
            call_data.fn_ref_expr->data.dot_expr.is_optional_chain) {
            auto &dot_data = call_data.fn_ref_expr->data.dot_expr;
            auto &builder = *m_ctx->llvm_builder.get();
            auto result_type = get_chitype(expr);
            auto result_type_l = compile_type(result_type);
            auto call_return_type = call_data.generated_fn
                                        ? get_chitype(call_data.generated_fn)->data.fn.return_type
                                        : call_data.fn_ref_expr->resolved_type->data.fn.return_type;
            llvm::Value *var = expr->resolved_outlet
                                   ? compile_expr_ref(fn, expr->resolved_outlet).address
                                   : compile_alloc(fn, expr, false);
            return compile_optional_branch(
                fn, dot_data.expr, result_type_l, "optcall",
                [&](llvm::Value *) -> llvm::Value * {
                    if (call_return_type == result_type) {
                        compile_fn_call(fn, expr, nullptr, var);
                    } else {
                        auto has_value_p = builder.CreateStructGEP(result_type_l, var, 0);
                        builder.CreateStore(
                            llvm::ConstantInt::get(llvm::Type::getInt1Ty(*m_ctx->llvm_ctx), 1),
                            has_value_p);
                        auto value_p = builder.CreateStructGEP(result_type_l, var, 1);
                        compile_fn_call(fn, expr, nullptr, value_p);
                    }
                    return nullptr;
                },
                [&]() -> llvm::Value * {
                    builder.CreateStore(llvm::ConstantAggregateZero::get(result_type_l), var);
                    return nullptr;
                },
                var);
        }

        if (fn->get_def()->has_cleanup || fn->async_reject_promise_ptr) {
            return compile_fn_call_with_invoke(fn, expr);
        }
        return compile_fn_call(fn, expr);
    }
    case ast::NodeType::Identifier: {
        auto &data = expr->data.identifier;
        auto &builder = *m_ctx->llvm_builder.get();
        auto ref = compile_expr_ref(fn, expr);
        auto type_l = compile_type(get_chitype(expr));
        return ref.address ? builder.CreateLoad(type_l, ref.address) : ref.value;
    }
    case ast::NodeType::LiteralExpr: {
        // Handle C string literals explicitly
        if (expr->token && expr->token->type == TokenType::C_STRING) {
            return compile_c_string_literal(expr->token->str);
        }
        auto value = get_resolver()->resolve_constant_value(expr);
        assert(value.has_value());
        return compile_constant_value(fn, *value, get_chitype(expr));
    }
    case ast::NodeType::UnaryOpExpr: {
        auto &data = expr->data.unary_op_expr;
        auto &builder = *m_ctx->llvm_builder.get();
        switch (data.op_type) {
        case TokenType::MUL: {
            if (data.resolved_call) {
                auto ref_ptr = compile_fn_call(fn, data.resolved_call);
                auto elem = expr->resolved_type;
                if (ChiTypeStruct::is_interface(elem))
                    return ref_ptr;
                return builder.CreateLoad(compile_type(elem), ref_ptr);
            }
            auto ptr = compile_expr(fn, data.op1);
            auto elem_type = get_chitype(data.op1)->get_elem();
            // Interface is abstract — deref returns the fat pointer value itself
            if (elem_type && ChiTypeStruct::is_interface(elem_type)) {
                return ptr;
            }
            auto elem_type_l = compile_type(elem_type);
            auto value = builder.CreateLoad(elem_type_l, ptr);
            return value;
        }
        case TokenType::LNOT: {
            if (data.is_suffix) {
                if (data.op1->resolved_type->kind == TypeKind::Optional) {
                    auto ref = compile_expr_ref(fn, data.op1);
                    auto opt_type_l = compile_type(data.op1->resolved_type);
                    auto has_value_p = builder.CreateStructGEP(opt_type_l, ref.address, 0);
                    auto has_value =
                        builder.CreateLoad(compile_type(get_system_types()->bool_), has_value_p);
                    emit_dbg_location(expr);
                    auto msg = compile_string_literal("unwrapping null optional");
                    emit_runtime_assert(fn, has_value, msg, expr);
                    auto value_p = builder.CreateStructGEP(opt_type_l, ref.address, 1);
                    return builder.CreateLoad(compile_type(expr->resolved_type), value_p);
                }
                if (data.op1->type == ast::NodeType::Identifier &&
                    data.op1->data.identifier.decl &&
                    data.op1->data.identifier.decl->type == ast::NodeType::VarDecl &&
                    data.op1->data.identifier.decl->data.var_decl.narrowed_from) {
                    return compile_expr(fn, data.op1);
                }
                if (data.op1->type == ast::NodeType::DotExpr &&
                    data.op1->data.dot_expr.narrowed_var) {
                    return compile_expr(fn, data.op1);
                }
                if (data.resolved_call) {
                    auto ref_ptr = compile_fn_call(fn, data.resolved_call);
                    auto elem = expr->resolved_type;
                    if (ChiTypeStruct::is_interface(elem))
                        return ref_ptr;
                    return builder.CreateLoad(compile_type(elem), ref_ptr);
                }
                panic("unreachable: suffix ! on non-optional type");
            } else {
                auto value = compile_assignment_to_type(fn, data.op1, get_system_types()->bool_);
                return builder.CreateXor(
                    value, llvm::ConstantInt::getTrue(compile_type(get_system_types()->bool_)));
            }
        }
        case TokenType::AND:
        case TokenType::MUTREF:
        case TokenType::MOVEREF: {
            auto ref = compile_expr_ref(fn, data.op1);
            if (!ref.address) {
                auto *ty = compile_type(get_chitype(data.op1));
                auto *tmp = builder.CreateAlloca(ty, nullptr, "_ref_tmp");
                builder.CreateStore(ref.value, tmp);
                ref.address = tmp;
            }
            auto result = ref.address;
            // For &move: null out the source after taking the pointer
            if (data.op_type == TokenType::MOVEREF) {
                auto ptr_val = builder.CreateLoad(compile_type(get_chitype(data.op1)), ref.address);
                builder.CreateStore(llvm::ConstantPointerNull::get(
                                        (llvm::PointerType *)compile_type(get_chitype(data.op1))),
                                    ref.address);
                return ptr_val;
            }
            return result;
        }
        case TokenType::KW_MOVE: {
            // move x — bitwise load, transfer ownership (skip copy)
            auto ref = compile_expr_ref(fn, data.op1);
            assert(ref.address);
            auto type = get_chitype(data.op1);
            auto type_l = compile_type(type);
            auto value = builder.CreateLoad(type_l, ref.address);
            // Clear drop flag for maybe-moved variables
            auto *src_decl = data.op1->type == ast::NodeType::Identifier
                                 ? data.op1->data.identifier.decl
                                 : nullptr;
            if (src_decl && data.op1->type == ast::NodeType::Identifier) {
                if (auto *flag_ptr = load_capture_move_flag_ptr(fn, data.op1)) {
                    auto *is_null = builder.CreateIsNull(flag_ptr);
                    auto *set_flag_block =
                        llvm::BasicBlock::Create(*m_ctx->llvm_ctx, "move.capture.flag", fn->llvm_fn);
                    auto *cont_block =
                        llvm::BasicBlock::Create(*m_ctx->llvm_ctx, "move.capture.cont", fn->llvm_fn);
                    builder.CreateCondBr(is_null, cont_block, set_flag_block);
                    builder.SetInsertPoint(set_flag_block);
                    builder.CreateStore(llvm::ConstantInt::getFalse(*m_ctx->llvm_ctx), flag_ptr);
                    builder.CreateBr(cont_block);
                    builder.SetInsertPoint(cont_block);
                } else if (m_ctx->drop_flags.has_key(src_decl)) {
                    builder.CreateStore(llvm::ConstantInt::getFalse(*m_ctx->llvm_ctx),
                                        m_ctx->drop_flags[src_decl]);
                }
            }
            return value;
        }
        case TokenType::INC:
        case TokenType::DEC: {
            auto op_type = get_chitype(data.op1);
            auto ref = compile_expr_ref(fn, data.op1);
            auto value = builder.CreateLoad(compile_type(op_type), ref.address);
            if (op_type->kind == TypeKind::Pointer) {
                auto elem_type_l = compile_type(op_type->get_elem());
                auto offset = data.op_type == TokenType::INC
                                  ? llvm::ConstantInt::get(builder.getInt64Ty(), 1)
                                  : llvm::ConstantInt::get(builder.getInt64Ty(), -1ULL);
                auto after = builder.CreateGEP(elem_type_l, value, {offset});
                builder.CreateStore(after, ref.address);
                return data.is_suffix ? value : after;
            }
            auto one = llvm::ConstantInt::get(value->getType(), 1);
            auto after = data.op_type == TokenType::INC ? builder.CreateAdd(value, one)
                                                        : builder.CreateSub(value, one);
            builder.CreateStore(after, ref.address);
            return data.is_suffix ? value : after;
        }
        case TokenType::ADD:
        case TokenType::SUB: {
            if (data.resolved_call) {
                data.resolved_call->resolved_outlet = expr->resolved_outlet;
                return compile_expr(fn, data.resolved_call);
            }
            auto value = compile_expr(fn, data.op1);
            auto type = get_chitype(data.op1);
            if (type->kind == TypeKind::Float) {
                auto zero = llvm::ConstantFP::get(compile_type(type), 0.0);
                return data.op_type == TokenType::ADD ? value : builder.CreateFSub(zero, value);
            } else {
                auto zero = llvm::ConstantInt::get(compile_type(type), 0);
                return data.op_type == TokenType::ADD ? value : builder.CreateSub(zero, value);
            }
        }
        case TokenType::NOT: {
            if (data.resolved_call) {
                data.resolved_call->resolved_outlet = expr->resolved_outlet;
                return compile_expr(fn, data.resolved_call);
            }
            auto value = compile_expr(fn, data.op1);
            return builder.CreateNot(value);
        }
        default:
            panic("not implemented: {}", PRINT_ENUM(data.op_type));
        }
    }
    case ast::NodeType::BinOpExpr: {
        auto &data = expr->data.bin_op_expr;
        auto &builder = *m_ctx->llvm_builder.get();
        switch (data.op_type) {
        case TokenType::LT:
        case TokenType::GT:
        case TokenType::LE:
        case TokenType::GE:
        case TokenType::EQ:
        case TokenType::NE: {
            // Struct comparison via operator method
            if (data.resolved_call) {
                auto result = compile_fn_call(fn, data.resolved_call);
                switch (data.op_type) {
                case TokenType::EQ:
                    return result; // Eq::eq → bool
                case TokenType::NE:
                    return builder.CreateNot(result); // !Eq::eq
                default: {
                    // Ord::cmp returns int; compare to 0
                    auto zero = llvm::ConstantInt::get(result->getType(), 0);
                    auto cmpop = get_cmpop(data.op_type, get_system_types()->int_);
                    return builder.CreateCmp(cmpop, result, zero);
                }
                }
            }
            // Optional null check: ?T == null / null == ?T
            {
                auto t1 = get_chitype(data.op1);
                auto t2 = get_chitype(data.op2);
                bool lhs_null = t1->kind == TypeKind::Null;
                bool rhs_null = t2->kind == TypeKind::Null;
                if ((lhs_null || rhs_null) &&
                    (data.op_type == TokenType::EQ || data.op_type == TokenType::NE)) {
                    auto opt_expr = lhs_null ? data.op2 : data.op1;
                    if (get_chitype(opt_expr)->kind == TypeKind::Optional) {
                        auto opt_val = compile_expr(fn, opt_expr);
                        auto has_value = builder.CreateExtractValue(opt_val, {0}, "has_value");
                        if (data.op_type == TokenType::EQ) {
                            return builder.CreateNot(has_value);
                        }
                        return has_value;
                    }
                }
            }
            auto lhs = compile_comparator(fn, data.op1);
            auto rhs = compile_comparator(fn, data.op2);
            auto cmpop = get_cmpop(data.op_type, get_chitype(data.op1));
            return builder.CreateCmp(cmpop, lhs, rhs);
        }
        case TokenType::LAND:
        case TokenType::LOR: {
            auto lhs = compile_assignment_to_type(fn, data.op1, get_system_types()->bool_);
            auto lhs_block = builder.GetInsertBlock();
            auto rhs_b = fn->new_label(data.op_type == TokenType::LAND ? "_and_rhs" : "_or_rhs");
            auto end_b = fn->new_label(data.op_type == TokenType::LAND ? "_and_end" : "_or_end");

            if (data.op_type == TokenType::LAND) {
                builder.CreateCondBr(lhs, rhs_b, end_b);
            } else {
                builder.CreateCondBr(lhs, end_b, rhs_b);
            }

            fn->use_label(rhs_b);
            for (auto var : data.rhs_narrow_vars) {
                compile_stmt(fn, var);
            }
            auto rhs = compile_assignment_to_type(fn, data.op2, get_system_types()->bool_);
            auto rhs_block = builder.GetInsertBlock();
            builder.CreateBr(end_b);

            fn->use_label(end_b);
            auto phi = builder.CreatePHI(llvm::Type::getInt1Ty(*m_ctx->llvm_ctx), 2);
            auto short_value = llvm::ConstantInt::getBool(
                *m_ctx->llvm_ctx, data.op_type == TokenType::LOR);
            phi->addIncoming(short_value, lhs_block);
            phi->addIncoming(rhs, rhs_block);
            return phi;
        }
        case TokenType::QUES: {
            auto result_type = get_chitype(expr);
            auto result_type_l = compile_type(result_type);
            auto lhs_type = get_chitype(data.op1);
            // Rewrap only when LHS and result are the same optional type (?T ?? ?T -> ?T).
            // For ??T ?? ?T -> ?T, the unwrapped LHS is already ?T, no rewrap needed.
            bool rewrap = result_type->kind == TypeKind::Optional &&
                          get_resolver()->is_same_type(lhs_type, result_type);
            if (get_resolver()->type_needs_destruction(result_type)) {
                llvm::Value *var = nullptr;
                if (expr->resolved_outlet) {
                    var = compile_expr_ref(fn, expr->resolved_outlet).address;
                } else {
                    var = compile_alloc(fn, expr, false);
                }
                auto result = compile_optional_branch(
                    fn, data.op1, result_type_l, "coalesce",
                    [&](llvm::Value *unwrapped_ptr) -> llvm::Value * {
                        if (rewrap) {
                            auto has_p = builder.CreateStructGEP(result_type_l, var, 0);
                            builder.CreateStore(llvm::ConstantInt::getTrue(*m_ctx->llvm_ctx), has_p);
                            auto val_p = builder.CreateStructGEP(result_type_l, var, 1);
                            compile_copy_with_ref(fn, RefValue::from_address(unwrapped_ptr), val_p,
                                                  result_type->get_elem(), data.op1, false);
                        } else {
                            compile_copy_with_ref(fn, RefValue::from_address(unwrapped_ptr), var,
                                                  result_type, data.op1, false);
                        }
                        return nullptr;
                    },
                    [&]() -> llvm::Value * {
                        compile_assignment_to_ptr(fn, data.op2, var, result_type);
                        return nullptr;
                    },
                    var);
                if (expr->resolved_outlet) {
                    mark_outlet_alive(fn, expr->resolved_outlet);
                }
                return result;
            }
            if (rewrap) {
                auto elem_type = result_type->get_elem();
                auto elem_type_l = compile_type(elem_type);
                return compile_optional_branch(
                    fn, data.op1, result_type_l, "coalesce",
                    [&](llvm::Value *unwrapped_ptr) -> llvm::Value * {
                        auto val = builder.CreateLoad(elem_type_l, unwrapped_ptr);
                        auto opt_ptr = builder.CreateAlloca(result_type_l, nullptr, "rewrap");
                        auto has_p = builder.CreateStructGEP(result_type_l, opt_ptr, 0);
                        builder.CreateStore(llvm::ConstantInt::getTrue(*m_ctx->llvm_ctx), has_p);
                        auto val_p = builder.CreateStructGEP(result_type_l, opt_ptr, 1);
                        builder.CreateStore(val, val_p);
                        return builder.CreateLoad(result_type_l, opt_ptr);
                    },
                    [&]() { return compile_assignment_to_type(fn, data.op2, result_type); });
            }
            return compile_optional_branch(
                fn, data.op1, result_type_l, "coalesce",
                [&](llvm::Value *unwrapped_ptr) {
                    return builder.CreateLoad(result_type_l, unwrapped_ptr);
                },
                [&]() { return compile_assignment_to_type(fn, data.op2, result_type); });
        }
        case TokenType::ASS: {
            auto *lhs = data.resolved_op1 ? data.resolved_op1 : data.op1;
            auto ref = compile_expr_ref(fn, lhs);
            assert(ref.address);
            auto dest_type = get_chitype(lhs);
            auto dest_ptr = ref.address;

            // Route owning assignment through a disjoint temp so RHS and
            // dest cannot alias (fixes self-assignment UAF).
            if (!data.is_initializing &&
                get_resolver()->type_needs_destruction(dest_type)) {
                return compile_aliasing_safe_assignment(fn, data.op2, dest_ptr, dest_type);
            }
            auto src_type = get_chitype(data.op2);
            bool destruct_old = !data.is_initializing;
            if (compile_implicit_owning_conversion_to_ptr(
                    fn, data.op2, dest_ptr, dest_type, destruct_old)) {
                return builder.CreateLoad(compile_type(dest_type), dest_ptr);
            }
            // Fallback for cloned AST nodes (e.g. subtype variants):
            // check if this is a field being initialized inside a constructor
            if (destruct_old && fn->node) {
                auto var = lhs->get_decl();
                if (var && var->type == ast::NodeType::VarDecl && var->data.var_decl.is_field &&
                    fn->node->type == ast::NodeType::FnDef &&
                    fn->node->data.fn_def.fn_kind == ast::FnKind::Constructor) {
                    auto init = var->data.var_decl.initialized_at;
                    // Field whose initialized_at still points to the parser default
                    // (the VarDecl itself) or to the original VarDecl before cloning
                    if (!init || init->type == ast::NodeType::VarDecl) {
                        destruct_old = false;
                    }
                }
            }
            // Check if RHS constructs in-place at dest (resolved_outlet set)
            bool in_place = data.op2->analysis.moved &&
                            data.op2->type == ast::NodeType::ConstructExpr &&
                            data.op2->resolved_outlet;
            // If in-place, destruct old BEFORE RHS overwrites dest
            if (in_place && destruct_old) {
                compile_destruction_for_type(fn, dest_ptr, dest_type);
            }

            // Get RHS as ref to preserve source address for efficient copy
            // (avoids load + temp alloca when source already has an address, e.g. ptr!)
            auto src_ref = compile_expr_ref(fn, data.op2);

            // Only load the value when needed (type conversion)
            if (dest_type && !get_resolver()->is_same_type(src_type, dest_type)) {
                if (!src_ref.value && src_ref.address) {
                    src_ref.value = builder.CreateLoad(compile_type(src_type), src_ref.address);
                }
                emit_dbg_location(data.op2);
                src_ref = {nullptr, compile_conversion(fn, src_ref.value, src_type, dest_type)};
            }

            if (in_place) {
                return src_ref.value; // Already written in-place
            }

            auto val = src_ref.value;
            if (!val && src_ref.address)
                val = builder.CreateLoad(compile_type(dest_type), src_ref.address);
            compile_store_or_copy(fn, val, dest_ptr, dest_type, data.op2, destruct_old);
            return val;
        }
        case TokenType::ADD_ASS:
        case TokenType::SUB_ASS:
        case TokenType::MUL_ASS:
        case TokenType::DIV_ASS:
        case TokenType::MOD_ASS:
        case TokenType::AND_ASS:
        case TokenType::OR_ASS:
        case TokenType::XOR_ASS:
        case TokenType::LSHIFT_ASS:
        case TokenType::RSHIFT_ASS: {
            auto lhs_type = get_chitype(data.op1);
            auto ref = compile_expr_ref(fn, data.op1);
            assert(ref.address);

            // Operator method (e.g. string += uses Add, Vec2 -= uses Sub)
            if (data.resolved_call) {
                auto result = compile_fn_call(fn, data.resolved_call);
                // Destruct old value, then move the temp result in (no copy needed)
                compile_destruction_for_type(fn, ref.address, lhs_type);
                builder.CreateStore(result, ref.address);
                return result;
            }

            // Primitive types: load, compute, store
            auto base_op = get_assignment_op(data.op_type);
            auto lhs_val = builder.CreateLoad(compile_type(lhs_type), ref.address);
            auto rhs_val = compile_expr(fn, data.op2);
            auto rhs_type = get_chitype(data.op2);
            if (rhs_type != lhs_type) {
                rhs_val = compile_conversion(fn, rhs_val, rhs_type, lhs_type);
            }
            auto llvm_op = get_binop(base_op, lhs_type);
            auto result = builder.CreateBinOp(llvm_op, lhs_val, rhs_val);
            builder.CreateStore(result, ref.address);
            return result;
        }
        default: {
            auto target_type = get_chitype(expr);
            auto lhs_type = get_chitype(data.op1);
            auto rhs_type = get_chitype(data.op2);

            // Pointer arithmetic
            if (lhs_type->kind == TypeKind::Pointer || rhs_type->kind == TypeKind::Pointer) {
                auto lhs = compile_expr(fn, data.op1);
                auto rhs = compile_expr(fn, data.op2);

                if (lhs_type->kind == TypeKind::Pointer && rhs_type->is_int()) {
                    // ptr + n / ptr - n
                    auto elem_type_l = compile_type(lhs_type->get_elem());
                    auto index = rhs;
                    if (data.op_type == TokenType::SUB) {
                        index = builder.CreateNeg(index);
                    }
                    return builder.CreateGEP(elem_type_l, lhs, {index});
                }
                if (lhs_type->is_int() && rhs_type->kind == TypeKind::Pointer) {
                    // n + ptr
                    auto elem_type_l = compile_type(rhs_type->get_elem());
                    return builder.CreateGEP(elem_type_l, rhs, {lhs});
                }
                if (lhs_type->kind == TypeKind::Pointer && rhs_type->kind == TypeKind::Pointer) {
                    // ptr - ptr → ptrdiff
                    auto elem_type_l = compile_type(lhs_type->get_elem());
                    auto lhs_int = builder.CreatePtrToInt(lhs, builder.getInt64Ty());
                    auto rhs_int = builder.CreatePtrToInt(rhs, builder.getInt64Ty());
                    auto diff_bytes = builder.CreateSub(lhs_int, rhs_int);
                    auto elem_size =
                        llvm::ConstantInt::get(builder.getInt64Ty(), llvm_type_size(elem_type_l));
                    return builder.CreateSDiv(diff_bytes, elem_size);
                }
            }

            // Use the operator method call resolved during the resolver pass.
            // Only use resolved_call when the concrete type is a struct — for primitive
            // types (int, float, etc.), fall through to built-in arithmetic. This handles
            // composite interface bounds (e.g. Numeric) where the resolver sets
            // resolved_call but the concrete instantiation is a primitive.
            if (data.resolved_call) {
                auto struct_type = get_resolver()->eval_struct_type(target_type);
                if (struct_type) {
                    data.resolved_call->resolved_outlet = expr->resolved_outlet;
                    return compile_expr(fn, data.resolved_call);
                }
            }

            // Fall back to built-in arithmetic operations for primitive types
            auto lhs = compile_expr(fn, data.op1);
            auto rhs = compile_expr(fn, data.op2);

            // Cast operands to target type if needed
            if (lhs_type != target_type) {
                lhs = compile_conversion(fn, lhs, lhs_type, target_type);
            }
            if (rhs_type != target_type) {
                rhs = compile_conversion(fn, rhs, rhs_type, target_type);
            }

            auto llvm_op = get_binop(data.op_type, target_type);
            return builder.CreateBinOp(llvm_op, lhs, rhs);
        }
        };
    }
    case ast::NodeType::TryExpr: {
        auto &data = expr->data.try_expr;
        auto &builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();
        auto effective_expr = data.resolved_expr ? data.resolved_expr : data.expr;
        auto try_expr_type = get_chitype(effective_expr);

        auto try_site = get_resolver()->find_await_site(effective_expr);
        auto try_it = try_site.await_expr ? m_async_await_refs.find(try_site.await_expr)
                                          : m_async_await_refs.end();
        if (try_it != m_async_await_refs.end() && get_resolver()->contains_await(effective_expr)) {
            auto site = try_site;
            assert(site.await_expr && "async try must contain a resumed await site");

            auto awaited_type = get_chitype(site.await_expr);
            auto shared_error_type =
                get_resolver()->get_shared_type(get_resolver()->get_context()->rt_error_type);
            auto settled_type = get_resolver()->get_result_type(awaited_type, shared_error_type);
            auto settled_enum = get_resolver()->resolve_subtype(settled_type, expr);
            assert(settled_enum && settled_enum->kind == TypeKind::Enum);

            auto settled_type_l = compile_type(settled_type);
            auto settled_var = fn->entry_alloca(settled_type_l, "try_await_settled");
            compile_copy_with_ref(fn, try_it->second, settled_var, settled_type, site.await_expr, false);

            auto err_member = settled_enum->data.enum_.find_member("Err");
            auto ok_member = settled_enum->data.enum_.find_member("Ok");
            assert(err_member && ok_member);

            auto err_variant_type = err_member->resolved_type;
            auto err_fields = get_resolver()->get_enum_payload_fields(err_variant_type);
            assert(err_fields.size() > 0);
            auto shared_error_ptr =
                compile_dot_access(fn, settled_var, err_variant_type, err_fields[0]);
            auto shared_error_value =
                builder.CreateLoad(compile_type(err_fields[0]->resolved_type), shared_error_ptr);

            auto err_value = llvm::ConstantInt::get(
                (llvm::IntegerType *)compile_type(settled_enum->data.enum_.discriminator),
                err_member->node->data.enum_variant.resolved_value);
            auto disc_ptr = builder.CreateStructGEP(compile_type(settled_enum->data.enum_.base_value_type),
                                                    settled_var, 0);
            auto disc = builder.CreateLoad(compile_type(settled_enum->data.enum_.discriminator),
                                           disc_ptr, "try_await_disc");

            auto is_err = builder.CreateICmpEQ(disc, err_value, "try_await_is_err");
            auto err_b = fn->new_label("_try_await_err");
            auto ok_b = fn->new_label("_try_await_ok");
            auto continue_b = fn->new_label("_try_await_continue");

            auto result_type = get_chitype(expr);
            auto result_type_l = compile_type(result_type);
            auto result_var = fn->entry_alloca(result_type_l, "try_await_result");

            ChiType *result_enum = nullptr;
            if (!data.catch_block) {
                result_enum = result_type;
                if (result_enum->kind == TypeKind::Subtype) {
                    result_enum = get_resolver()->resolve_subtype(result_enum, expr);
                }
                if (result_enum && result_enum->kind == TypeKind::EnumValue) {
                    result_enum = result_enum->data.enum_value.enum_type;
                }
            }
            auto init_result_variant = [&](const string &variant_name, auto store_payload) {
                assert(result_enum && result_enum->kind == TypeKind::Enum);
                auto variant_member = result_enum->data.enum_.find_member(variant_name);
                assert(variant_member);
                auto variant_type = variant_member->resolved_type;
                assert(variant_type && variant_type->kind == TypeKind::EnumValue);

                auto enum_variant_p = m_ctx->enum_variant_table.get(variant_member);
                assert(enum_variant_p);
                auto enum_var = *enum_variant_p;
                auto copy_size = llvm_type_size(((llvm::GlobalVariable *)enum_var)->getValueType());
                auto zero = llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(llvm_ctx), 0);

                builder.CreateMemSet(result_var, zero, llvm_type_size(result_type_l).getFixedValue(),
                                     llvm::MaybeAlign());
                builder.CreateMemCpy(result_var, {}, enum_var, {}, copy_size);

                auto payload_fields = get_resolver()->get_enum_payload_fields(variant_type);
                if (payload_fields.size() > 0) {
                    auto payload_ptr =
                        compile_dot_access(fn, result_var, variant_type, payload_fields[0]);
                    store_payload(payload_fields[0]->resolved_type, payload_ptr);
                }
            };

            auto reject_async_error = [&]() {
                emit_async_promise_reject_shared(fn, shared_error_value);
                builder.CreateRetVoid();
            };

            builder.CreateCondBr(is_err, err_b, ok_b);

            fn->use_label(err_b);
            if (data.catch_expr) {
                auto catch_type = get_resolver()->to_value_type(get_chitype(data.catch_expr));
                auto error_iface = compile_shared_ref(fn, shared_error_ptr, shared_error_type);
                auto type_matches = compile_interface_type_match(
                    fn, error_iface, get_resolver()->get_context()->rt_error_type, catch_type);
                auto match_b = fn->new_label("_try_await_type_match");
                auto nomatch_b = fn->new_label("_try_await_type_nomatch");
                builder.CreateCondBr(type_matches, match_b, nomatch_b);

                fn->use_label(nomatch_b);
                reject_async_error();

                fn->use_label(match_b);
            }

            if (!data.catch_block) {
                init_result_variant("Err", [&](ChiType *payload_type, llvm::Value *payload_ptr) {
                    builder.CreateStore(shared_error_value, payload_ptr);
                });
                builder.CreateBr(continue_b);
            } else {
                llvm::Value *shared_owner_var = nullptr;
                llvm::Value *shared_owner_active = nullptr;
                if (data.catch_err_var) {
                    auto shared_type_l = compile_type(shared_error_type);
                    shared_owner_var = fn->entry_alloca(shared_type_l, "try_await_err_owner");
                    builder.CreateStore(shared_error_value, shared_owner_var);
                    shared_owner_active = register_cleanup_owner(
                        fn, shared_owner_var, shared_error_type, "try_await_err_owner.active");
                } else {
                    compile_destruction_for_type(fn, shared_error_ptr, shared_error_type);
                }

                if (data.catch_err_var) {
                    auto err_var = compile_alloc(fn, data.catch_err_var);
                    add_var(data.catch_err_var, err_var);
                    auto owner_iface = compile_shared_ref(fn, shared_owner_var, shared_error_type);
                    auto concrete_data_ptr = extract_interface_data_ptr(owner_iface);
                    builder.CreateStore(concrete_data_ptr, err_var);
                    data.catch_block->data.block.implicit_vars.clear();
                }

                auto catch_cleanup_b = fn->new_label("_try_await_catch_cleanup");
                compile_block(fn, expr, data.catch_block, catch_cleanup_b, result_var);

                fn->use_label(catch_cleanup_b);
                if (shared_owner_var) {
                    auto cleanup_b = fn->new_label("_try_await_owner_cleanup");
                    auto cleanup_done_b = fn->new_label("_try_await_owner_cleanup_done");
                    auto owner_active =
                        builder.CreateLoad(llvm::Type::getInt1Ty(llvm_ctx), shared_owner_active);
                    builder.CreateCondBr(owner_active, cleanup_b, cleanup_done_b);

                    fn->use_label(cleanup_b);
                    compile_destruction_for_type(fn, shared_owner_var, shared_error_type);
                    builder.CreateStore(llvm::ConstantInt::getFalse(llvm_ctx), shared_owner_active);
                    builder.CreateBr(cleanup_done_b);

                    fn->use_label(cleanup_done_b);
                }
                if (!fn->get_scope()->branched && !builder.GetInsertBlock()->getTerminator()) {
                    builder.CreateBr(continue_b);
                }
            }

            fn->use_label(ok_b);
            auto ok_variant_type = ok_member->resolved_type;
            auto ok_fields = get_resolver()->get_enum_payload_fields(ok_variant_type);
            llvm::Value *payload_value = nullptr;
            if (ok_fields.size() > 0) {
                auto payload_ptr = compile_dot_access(fn, settled_var, ok_variant_type, ok_fields[0]);
                payload_value =
                    builder.CreateLoad(compile_type(ok_fields[0]->resolved_type), payload_ptr);
            }
            auto prev_async_refs = m_async_await_refs;
            if (ok_fields.size() > 0) {
                auto payload_ptr = compile_dot_access(fn, settled_var, ok_variant_type, ok_fields[0]);
                m_async_await_refs[site.await_expr] = RefValue::from_address(payload_ptr);
            } else {
                m_async_await_refs[site.await_expr] = RefValue{};
            }
            auto ok_value = compile_expr(fn, effective_expr);
            m_async_await_refs = prev_async_refs;

            if (!data.catch_block) {
                init_result_variant("Ok", [&](ChiType *payload_type, llvm::Value *payload_ptr) {
                    if (ok_value) {
                        compile_store_or_copy(fn, ok_value, payload_ptr, payload_type, effective_expr);
                    }
                });
            } else {
                if (ok_value) {
                    compile_store_or_copy(fn, ok_value, result_var, result_type, effective_expr);
                }
            }
            if (ok_fields.size() > 0 && !site.await_expr->analysis.moved) {
                auto payload_ptr = compile_dot_access(fn, settled_var, ok_variant_type, ok_fields[0]);
                compile_destruction_for_type(fn, payload_ptr, ok_fields[0]->resolved_type);
            }
            builder.CreateBr(continue_b);

            fn->use_label(continue_b);
            return builder.CreateLoad(result_type_l, result_var, "try_await_result");
        }

        auto result_type = get_chitype(expr);
        bool is_void =
            result_type->kind == TypeKind::Void || result_type->kind == TypeKind::Never;
        llvm::Type *result_type_l = nullptr;
        llvm::Value *result_var = nullptr;

        // Discarded `try f();` statement: resolver registers a cleanup temp so
        // the Result (and its Shared<Error>) is destroyed at block exit. Drop
        // flag guards resume/panic branches that exit without populating it.
        bool use_outlet = expr->resolved_outlet != nullptr;
        if (!is_void) {
            result_type_l = compile_type(result_type);
            if (use_outlet) {
                result_var = compile_expr_ref(fn, expr->resolved_outlet).address;
                if (!m_ctx->drop_flags.has_key(expr->resolved_outlet)) {
                    alloc_drop_flag(fn, expr->resolved_outlet, false);
                }
            } else {
                result_var = fn->entry_alloca(result_type_l, "try_result");
            }
        }

        ChiType *result_enum = nullptr;
        if (!is_void && !data.catch_block) {
            result_enum = result_type;
            if (result_enum->kind == TypeKind::Subtype) {
                result_enum = get_resolver()->resolve_subtype(result_enum, expr);
            }
        }
        if (result_enum && result_enum->kind == TypeKind::EnumValue) {
            result_enum = result_enum->data.enum_value.enum_type;
        }
        auto init_result_variant = [&](const string &variant_name, auto store_payload) {
            assert(result_enum && result_enum->kind == TypeKind::Enum);
            auto variant_member = result_enum->data.enum_.find_member(variant_name);
            assert(variant_member);
            auto variant_type = variant_member->resolved_type;
            assert(variant_type && variant_type->kind == TypeKind::EnumValue);

            auto enum_variant_p = m_ctx->enum_variant_table.get(variant_member);
            assert(enum_variant_p);
            auto enum_var = *enum_variant_p;
            auto copy_size = llvm_type_size(((llvm::GlobalVariable *)enum_var)->getValueType());
            auto zero = llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(llvm_ctx), 0);

            builder.CreateMemSet(result_var, zero, llvm_type_size(result_type_l).getFixedValue(),
                                 llvm::MaybeAlign());
            builder.CreateMemCpy(result_var, {}, enum_var, {}, copy_size);

            auto payload_fields = get_resolver()->get_enum_payload_fields(variant_type);
            if (payload_fields.size() > 0) {
                auto payload_ptr =
                    compile_dot_access(fn, result_var, variant_type, payload_fields[0]);
                store_payload(payload_fields[0]->resolved_type, payload_ptr);
            }
        };

        auto continue_b = fn->new_label("_try_continue");
        auto normal_b = fn->new_label("_try_normal");
        auto landing_b = fn->new_label("_try_landing");

        InvokeInfo invoke = {};
        invoke.normal = normal_b;
        invoke.landing = landing_b;

        llvm::Value *value = nullptr;
        // For block try in result mode, we need a separate temp to capture the block's
        // return value (T), since result_var holds Result<T,E> not T.
        llvm::Value *block_val_var = nullptr;
        ChiType *block_val_type = nullptr;
        if (data.expr->type == ast::NodeType::Block) {
            // try { block } — compile the block with invoke context set
            auto saved_landing = fn->try_block_landing;
            fn->try_block_landing = landing_b;
            if (!data.catch_block && !is_void) {
                // Result mode: store block value into a separate temp (T*), not result_var (Result*)
                block_val_type = get_chitype(data.expr);
                if (block_val_type && block_val_type->kind != TypeKind::Void) {
                    block_val_var = fn->entry_alloca(compile_type(block_val_type), "block_val");
                }
                compile_block(fn, data.expr, data.expr, normal_b, block_val_var);
            } else {
                // Catch-block mode: result_var is T*, pass directly
                compile_block(fn, expr, data.expr, normal_b, result_var);
            }
            fn->try_block_landing = saved_landing;
        } else {
            value = compile_fn_call(fn, data.expr, &invoke);
        }

        // === LANDING PAD (error path) ===
        fn->use_label(invoke.landing);
        auto caught_type_l = m_ctx->get_caught_result_type();
        auto landing = builder.CreateLandingPad(caught_type_l, 1);
        landing->addClause(llvm::ConstantPointerNull::get(
            llvm::PointerType::get(llvm::Type::getInt8Ty(llvm_ctx), 0)));

        // Extract thrown pointer — null means panic (unrecoverable), non-null means throw
        auto thrown_ptr = builder.CreateExtractValue(landing, {0}, "thrown_ptr");
        auto is_typed = builder.CreateICmpNE(
            thrown_ptr, llvm::ConstantPointerNull::get(builder.getPtrTy()), "is_typed_error");

        auto typed_error_b = fn->new_label("_try_typed_error");
        auto panic_b = fn->new_label("_try_panic");
        builder.CreateCondBr(is_typed, typed_error_b, panic_b);

        // -- Panic path: re-throw (unrecoverable) --
        fn->use_label(panic_b);
        builder.CreateResume(landing);

        // -- Typed error path --
        fn->use_label(typed_error_b);

        // Get error data from thread-local storage
        auto get_error_data_fn = get_system_fn("cx_get_error_data");
        auto get_error_vtable_fn = get_system_fn("cx_get_error_vtable");
        auto get_error_type_id_fn = get_system_fn("cx_get_error_type_id");
        auto clear_loc_fn = get_system_fn("cx_clear_panic_location");
        auto dispose_exception_fn = get_system_fn("cx_dispose_exception");

        auto error_data = builder.CreateCall(get_error_data_fn->llvm_fn, {}, "error_data");
        auto error_vtable = builder.CreateCall(get_error_vtable_fn->llvm_fn, {}, "error_vtable");
        auto make_shared_error_value = [&]() {
            auto rt_error = get_resolver()->get_context()->rt_error_type;
            auto move_ref_error = get_resolver()->get_pointer_type(rt_error, TypeKind::MoveRef);
            auto fat_iface_type_l = compile_type(move_ref_error);
            llvm::Value *fat_ptr = llvm::UndefValue::get(fat_iface_type_l);
            fat_ptr = builder.CreateInsertValue(fat_ptr, error_data, {0});
            fat_ptr = builder.CreateInsertValue(fat_ptr, error_vtable, {1});
            auto shared_error_type = get_resolver()->get_shared_type(rt_error);
            return compile_shared_new(fn, shared_error_type, fat_ptr);
        };

        if (data.catch_block) {
            // === CATCH BLOCK MODE: try f() catch (...) { block } → yields T ===

            // Save error_data pointer for cleanup (initialized to null, set on catch path)
            // This alloca is in the entry block, so it persists for function-level cleanup
            auto error_data_var = fn->entry_alloca(builder.getPtrTy(), "error_owner");
            {
                // Initialize in entry block right after alloca, not at current insertion point
                llvm::IRBuilder<> tmp(*m_ctx->llvm_ctx);
                tmp.SetInsertPoint(error_data_var->getNextNode());
                tmp.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()), error_data_var);
            }

            // Type check if specific catch
            ChiType *catch_type = nullptr;
            if (data.catch_expr) {
                catch_type = get_resolver()->to_value_type(get_chitype(data.catch_expr));
                auto caught_type_id =
                    builder.CreateCall(get_error_type_id_fn->llvm_fn, {}, "caught_type_id");
                auto expected_id =
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), catch_type->id);
                auto type_matches =
                    builder.CreateICmpEQ(caught_type_id, expected_id, "type_matches");

                auto match_b = fn->new_label("_try_type_match");
                auto nomatch_b = fn->new_label("_try_type_nomatch");
                builder.CreateCondBr(type_matches, match_b, nomatch_b);

                // Type doesn't match: re-throw
                fn->use_label(nomatch_b);
                builder.CreateResume(landing);

                fn->use_label(match_b);
            }

            // Take ownership of the error data pointer
            builder.CreateStore(error_data, error_data_var);
            builder.CreateCall(clear_loc_fn->llvm_fn, {});
            // Release the C++ exception object now that TLS holds everything
            // we need. Safe here because this path never resumes unwinding.
            builder.CreateCall(dispose_exception_fn->llvm_fn, {thrown_ptr});

            // Set up error binding variable
            if (data.catch_err_var) {
                auto err_var = compile_alloc(fn, data.catch_err_var);
                add_var(data.catch_err_var, err_var);

                if (data.catch_expr) {
                    // Typed catch: err is &ConcreteError — store data pointer directly
                    builder.CreateStore(error_data, err_var);
                } else {
                    // Catch-all: err is Error interface — build { data, vtable }
                    auto err_type = get_chitype(data.catch_err_var);
                    auto err_type_l = compile_type(err_type);
                    auto data_p = builder.CreateStructGEP(err_type_l, err_var, 0);
                    builder.CreateStore(error_data, data_p);
                    auto vtable_p = builder.CreateStructGEP(err_type_l, err_var, 1);
                    builder.CreateStore(error_vtable, vtable_p);
                }

                // Clear implicit_vars so compile_block doesn't re-alloca the err var
                data.catch_block->data.block.implicit_vars.clear();
            }

            // Register error_data_var for function-level cleanup BEFORE compiling the
            // catch block, so a `throw` or `return` inside the body runs through
            // emit_cleanup_owners and frees the caught error.
            fn->cleanup_owner_vars.push_back({error_data_var, catch_type, nullptr, true});

            // Compile catch block with a cleanup label instead of continue_b
            auto catch_cleanup_b = fn->new_label("_catch_cleanup");
            compile_block(fn, expr, data.catch_block, catch_cleanup_b, result_var);

            // === CATCH CLEANUP: destroy error object after catch block ===
            fn->use_label(catch_cleanup_b);
            {
                auto owned_ptr =
                    builder.CreateLoad(builder.getPtrTy(), error_data_var, "err_owned");
                auto is_nonnull = builder.CreateICmpNE(
                    owned_ptr, llvm::ConstantPointerNull::get(builder.getPtrTy()));
                auto do_free_b = fn->new_label("_catch_free");
                auto skip_free_b = fn->new_label("_catch_free_done");
                builder.CreateCondBr(is_nonnull, do_free_b, skip_free_b);

                fn->use_label(do_free_b);
                // Destroy the error struct's fields (string fields etc.)
                if (catch_type) {
                    compile_destruction_for_type(fn, owned_ptr, catch_type);
                }
                // Free the heap allocation
                auto free_fn = get_system_fn("cx_free");
                builder.CreateCall(free_fn->llvm_fn, {owned_ptr});
                // Clear ownership so function-level cleanup doesn't double-free
                builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()),
                                    error_data_var);
                builder.CreateBr(skip_free_b);

                fn->use_label(skip_free_b);
            }
            builder.CreateBr(continue_b);
        } else {
            // === RESULT MODE: try f() → Result<T, Shared<Error>> ===

            if (data.catch_expr) {
                auto catch_type = get_resolver()->to_value_type(get_chitype(data.catch_expr));
                auto caught_type_id =
                    builder.CreateCall(get_error_type_id_fn->llvm_fn, {}, "caught_type_id");
                auto expected_id =
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), catch_type->id);
                auto type_matches =
                    builder.CreateICmpEQ(caught_type_id, expected_id, "type_matches");

                auto match_b = fn->new_label("_try_type_match");
                auto nomatch_b = fn->new_label("_try_type_nomatch");
                builder.CreateCondBr(type_matches, match_b, nomatch_b);

                // Type doesn't match: re-throw for non-block try; wrap in Err for block try.
                // For block try, re-throwing would cause _cleanup_landing to catch the resume
                // and loop, so we wrap unmatched errors in Shared<Error> instead.
                fn->use_label(nomatch_b);
                if (data.expr->type == ast::NodeType::Block) {
                    init_result_variant("Err", [&](ChiType *, llvm::Value *payload_ptr) {
                        auto shared_error = make_shared_error_value();
                        builder.CreateStore(shared_error, payload_ptr);
                    });
                    builder.CreateCall(clear_loc_fn->llvm_fn, {});
                    builder.CreateCall(dispose_exception_fn->llvm_fn, {thrown_ptr});
                    builder.CreateBr(continue_b);
                } else {
                    builder.CreateResume(landing);
                }

                // Type matches: populate Result.Err(Shared<Error>)
                fn->use_label(match_b);
                init_result_variant("Err", [&](ChiType *, llvm::Value *payload_ptr) {
                    auto shared_error = make_shared_error_value();
                    builder.CreateStore(shared_error, payload_ptr);
                });
                builder.CreateCall(clear_loc_fn->llvm_fn, {});
            } else {
                // Catch-all: populate Result.Err(Shared<Error>)
                init_result_variant("Err", [&](ChiType *, llvm::Value *payload_ptr) {
                    auto shared_error = make_shared_error_value();
                    builder.CreateStore(shared_error, payload_ptr);
                });
                builder.CreateCall(clear_loc_fn->llvm_fn, {});
            }
            builder.CreateCall(dispose_exception_fn->llvm_fn, {thrown_ptr});
            builder.CreateBr(continue_b);
        }

        // === NORMAL PATH (success) ===
        fn->use_label(invoke.normal);
        if (data.catch_block) {
            // Catch block mode: store value directly (result_var is T)
            if (result_var) {
                if (invoke.sret) {
                    auto sret_loaded = builder.CreateLoad(invoke.sret_type, invoke.sret);
                    builder.CreateStore(sret_loaded, result_var);
                } else if (value && !value->getType()->isVoidTy()) {
                    builder.CreateStore(value, result_var);
                }
            }
        } else {
            // Result mode: populate Result.Ok(T)
            init_result_variant("Ok", [&](ChiType *, llvm::Value *payload_ptr) {
                if (invoke.sret) {
                    auto sret_loaded = builder.CreateLoad(invoke.sret_type, invoke.sret);
                    builder.CreateStore(sret_loaded, payload_ptr);
                } else if (value && !value->getType()->isVoidTy()) {
                    builder.CreateStore(value, payload_ptr);
                } else if (block_val_var) {
                    // Block try result mode: load block's return value from temp
                    auto loaded = builder.CreateLoad(compile_type(block_val_type), block_val_var);
                    builder.CreateStore(loaded, payload_ptr);
                }
            });
        }
        builder.CreateBr(continue_b);

        // === CONTINUE ===
        fn->use_label(continue_b);
        if (is_void) {
            return nullptr;
        }
        if (use_outlet) {
            set_drop_flag_alive(expr->resolved_outlet, true);
            mark_outlet_alive(fn, expr->resolved_outlet);
        }
        return builder.CreateLoad(result_type_l, result_var, "try_result");
    }
    case ast::NodeType::AwaitExpr: {
        // In async continuation context, the resolved value is passed directly
        // via value_arg — return it without compiling the promise expression.
        auto it = m_async_await_refs.find(expr);
        if (it != m_async_await_refs.end()) {
            if (it->second.value) {
                return it->second.value;
            }
            assert(it->second.address && "await ref must have value or address");
            auto &builder = *m_ctx->llvm_builder.get();
            return builder.CreateLoad(compile_type(get_chitype(expr)), it->second.address);
        }

        // Synchronous await fallback for already-resolved promises.
        // Calls Promise.value() to get ?T, then unwraps.
        auto &data = expr->data.await_expr;
        auto &builder = *m_ctx->llvm_builder.get();

        auto promise_type = get_chitype(data.expr);
        auto promise_type_l = compile_type(promise_type);

        // Compile the promise and store it (value() takes &self)
        auto promise_val = compile_expr(fn, data.expr);
        auto promise_ptr = builder.CreateAlloca(promise_type_l, nullptr, "await_promise");
        builder.CreateStore(promise_val, promise_ptr);

        // Call Promise<T>.value() -> ?T
        auto promise_struct = get_resolver()->resolve_struct_type(promise_type);
        auto value_member = promise_struct->find_member("value");
        assert(value_member && "Promise.value() method not found");
        std::optional<TypeId> variant_type_id = std::nullopt;
        if (promise_type->kind == TypeKind::Subtype && !promise_type->is_placeholder) {
            variant_type_id = promise_type->id;
        }
        auto value_method_node = get_variant_member_node(value_member, variant_type_id);
        auto value_method = get_fn(value_method_node);
        auto optional_val = builder.CreateCall(value_method->llvm_fn, {promise_ptr});

        // Unwrap ?T -> T (optional layout: { has_value, T })
        auto result_type = get_chitype(expr);
        auto result_type_l = compile_type(result_type);
        auto optional_type_l = optional_val->getType();
        auto optional_ptr = builder.CreateAlloca(optional_type_l, nullptr, "await_opt");
        builder.CreateStore(optional_val, optional_ptr);
        auto value_ptr = builder.CreateStructGEP(optional_type_l, optional_ptr, 1, "await_value");
        return builder.CreateLoad(result_type_l, value_ptr, "await_result");
    }
    case ast::NodeType::TypeInfoExpr: {
        auto &data = expr->data.type_info_expr;
        auto &builder = *m_ctx->llvm_builder.get();
        auto expr_type = get_chitype(data.expr);
        auto result_type = get_chitype(expr);
        auto raw_type_info = compile_type_info(expr_type);

        auto dest = compile_alloc(fn, expr, false);
        auto generated_ctor = generate_constructor(result_type, nullptr);
        if (generated_ctor) {
            builder.CreateCall(generated_ctor->llvm_fn, {dest});
        }

        auto ctor_fn = get_fn(data.resolved_ctor);
        assert(ctor_fn && "reflect.Type.new must be compiled");
        auto ctor_type_l = (llvm::FunctionType *)compile_type(get_chitype(data.resolved_ctor));
        builder.CreateCall(ctor_type_l, ctor_fn->llvm_fn, {dest, raw_type_info});
        return builder.CreateLoad(compile_type(result_type), dest, "type_expr");
    }
    case ast::NodeType::DotExpr: {
        auto &dot_data = expr->data.dot_expr;

        // FixedArray .length -> compile-time constant
        if (get_chitype(dot_data.expr)->kind == TypeKind::FixedArray) {
            return llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(*m_ctx->llvm_ctx),
                                          dot_data.resolved_value);
        }

        // Tuple field access: expr.0, expr.1
        if (dot_data.resolved_dot_kind == DotKind::TupleField) {
            auto tuple_val = compile_expr(fn, dot_data.expr);
            return m_ctx->llvm_builder->CreateExtractValue(tuple_val, {(unsigned)dot_data.resolved_value});
        }

        if (dot_data.is_optional_chain) {
            auto &builder = *m_ctx->llvm_builder.get();
            auto opt_type = get_chitype(dot_data.expr);
            auto result_type = get_chitype(expr);
            auto result_type_l = compile_type(result_type);
            if (get_resolver()->type_needs_destruction(result_type)) {
                llvm::Value *var = nullptr;
                if (expr->resolved_outlet) {
                    var = compile_expr_ref(fn, expr->resolved_outlet).address;
                } else {
                    var = compile_alloc(fn, expr, false);
                }
                auto result = compile_optional_branch(
                    fn, dot_data.expr, result_type_l, "optchain",
                    [&](llvm::Value *unwrapped_ptr) -> llvm::Value * {
                        auto unwrapped_type = opt_type->get_elem();
                        llvm::Value *struct_ptr;
                        ChiType *struct_type;
                        if (unwrapped_type->is_pointer_like()) {
                            struct_type = unwrapped_type->get_elem();
                            struct_ptr =
                                builder.CreateLoad(compile_type(unwrapped_type), unwrapped_ptr);
                        } else {
                            struct_type = unwrapped_type;
                            struct_ptr = unwrapped_ptr;
                        }
                        auto gep = compile_dot_access(fn, struct_ptr, struct_type,
                                                      dot_data.resolved_struct_member);
                        auto field_type = dot_data.resolved_struct_member->resolved_type;
                        compile_copy_with_ref(fn, RefValue::from_address(gep), var, result_type,
                                              dot_data.expr, false);
                        return nullptr;
                    },
                    [&]() -> llvm::Value * {
                        return llvm::ConstantAggregateZero::get(result_type_l);
                    },
                    var);
                if (expr->resolved_outlet) {
                    mark_outlet_alive(fn, expr->resolved_outlet);
                }
                return result;
            }
            return compile_optional_branch(
                fn, dot_data.expr, result_type_l, "optchain",
                [&](llvm::Value *unwrapped_ptr) {
                    auto unwrapped_type = opt_type->get_elem();
                    llvm::Value *struct_ptr;
                    ChiType *struct_type;
                    if (unwrapped_type->is_pointer_like()) {
                        struct_type = unwrapped_type->get_elem();
                        struct_ptr =
                            builder.CreateLoad(compile_type(unwrapped_type), unwrapped_ptr);
                    } else {
                        struct_type = unwrapped_type;
                        struct_ptr = unwrapped_ptr;
                    }
                    auto gep = compile_dot_access(fn, struct_ptr, struct_type,
                                                  dot_data.resolved_struct_member);
                    auto field_type = dot_data.resolved_struct_member->resolved_type;
                    auto field_val = builder.CreateLoad(compile_type(field_type), gep);
                    return compile_conversion(fn, field_val, field_type, result_type);
                },
                [&]() -> llvm::Value * { return llvm::ConstantAggregateZero::get(result_type_l); });
        }

        auto member = dot_data.resolved_struct_member;
        auto ref = compile_expr_ref(fn, expr);
        // For constants/values, return the value directly
        if (ref.value && !ref.address) {
            return ref.value;
        }
        assert(ref.address);
        auto &builder = *m_ctx->llvm_builder.get();
        auto type_l = compile_type(get_chitype(expr));
        return builder.CreateLoad(type_l, ref.address);
    }
    case ast::NodeType::FnDef: {
        // compile lambda function expression
        auto &data = expr->data.fn_def;
        assert(data.fn_kind == ast::FnKind::Lambda);

        // Generate a descriptive lambda name with parent function context
        string lambda_name;
        if (fn && !fn->qualified_name.empty()) {
            lambda_name = fn->qualified_name + "__lambda_" + std::to_string(expr->id);
        } else {
            lambda_name = "__lambda_" + std::to_string(expr->id);
        }
        auto lambda_fn = compile_fn_proto(data.fn_proto, expr, lambda_name);
        // Propagate parent's type_env to nested lambdas so that
        // placeholder types inside the lambda can be substituted correctly
        if (fn && fn->type_env && !lambda_fn->type_env) {
            lambda_fn->type_env = fn->type_env;
        }
        m_ctx->pending_fns.add(lambda_fn);
        return compile_lambda_alloc(fn, get_chitype(expr), lambda_fn->llvm_fn, &data.captures);
    }
    case ast::NodeType::ConstructExpr: {
        auto &builder = *m_ctx->llvm_builder.get();
        auto &data = expr->data.construct_expr;
        // Calculate element type first - for 'new' expressions, we need to allocate
        // the element type, not the pointer type
        auto type = data.is_new ? get_chitype(expr)->get_elem() : get_chitype(expr);
        auto ptr = expr->resolved_outlet ? compile_expr_ref(fn, expr->resolved_outlet).address
                                         : compile_alloc(fn, expr, data.is_new, type);
        compile_construction(fn, ptr, type, expr);
        if (expr->resolved_outlet && !data.is_new) {
            mark_outlet_alive(fn, expr->resolved_outlet);
        }
        return data.is_new ? ptr : builder.CreateLoad(compile_type(type), ptr);
    }
    case ast::NodeType::PrefixExpr: {
        auto &data = expr->data.prefix_expr;
        auto &llvm_ctx = *m_ctx->llvm_ctx;
        auto &builder = *m_ctx->llvm_builder;

        switch (data.prefix->type) {
        case TokenType::KW_SIZEOF: {
            auto sizeof_type = get_chitype(data.expr);
            if (sizeof_type && ChiTypeStruct::is_interface(sizeof_type)) {
                // Interface is abstract — compile the expression to get the fat pointer,
                // then extract runtime typesize from vtable
                auto fat_ptr = compile_expr(fn, data.expr);
                auto vtable_ptr = builder.CreateExtractValue(fat_ptr, {1}, "vtable_ptr");
                return load_typesize_from_vtable(vtable_ptr);
            }
            return llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(llvm_ctx),
                                          llvm_type_size(compile_type_of(data.expr)));
        }
        case TokenType::KW_DELETE: {
            emit_dbg_location(expr);
            auto expr_type = get_chitype(data.expr);
            if (expr_type->is_pointer_like()) {
                // Pointer delete: destroy + free
                auto ref = compile_expr_ref(fn, data.expr);
                auto &builder = *m_ctx->llvm_builder;
                auto ptr = ref.address ? builder.CreateLoad(compile_type(expr_type), ref.address)
                                       : compile_expr(fn, data.expr);
                auto elem_type = expr_type->get_elem();
                compile_heap_free(fn, ptr, elem_type);
                if (ref.address) {
                    builder.CreateStore(llvm::Constant::getNullValue(compile_type(expr_type)),
                                        ref.address);
                }
            } else {
                // Value delete: destroy in-place, no free
                auto ref = compile_expr_ref(fn, data.expr);
                assert(ref.address);
                compile_destruction_for_type(fn, ref.address, expr_type);
            }
            return nullptr;
        }
        default:
            panic("not implemented: {}", PRINT_ENUM(data.prefix->type));
        }
        return nullptr;
    }
    case ast::NodeType::CastExpr: {
        auto &data = expr->data.cast_expr;
        auto from_type = get_chitype(data.expr);
        auto to_type = get_chitype(expr);
        auto conversion_type = get_saved_conversion_type(expr);
        if (conversion_type == ast::ConversionType::NoOp) {
            return compile_expr(fn, data.expr);
        }
        if (conversion_type == ast::ConversionType::OwningCoercion) {
            auto &builder = *m_ctx->llvm_builder.get();
            auto tmp = fn->entry_alloca(compile_type(to_type),
                                        to_type->kind == TypeKind::Any ? "cast_any" : "cast_wrap");
            bool converted = compile_implicit_owning_conversion_to_ptr(fn, expr, tmp, to_type);
            assert(converted);
            return builder.CreateLoad(compile_type(to_type), tmp);
        }
        auto from_value = compile_expr(fn, data.expr);
        return compile_conversion(fn, from_value, from_type, to_type);
    }
    case ast::NodeType::IndexExpr: {
        auto &builder = *m_ctx->llvm_builder;
        auto ref = compile_expr_ref(fn, expr);
        return builder.CreateLoad(compile_type_of(expr), ref.address);
    }
    case ast::NodeType::SliceExpr: {
        auto &data = expr->data.slice_expr;

        // Get reference to the container
        auto ref = compile_expr_ref(fn, data.expr);
        if (!ref.address) {
            auto &builder_ = *m_ctx->llvm_builder;
            auto tmp = compile_alloc(fn, data.expr);
            builder_.CreateStore(ref.value, tmp);
            ref = RefValue::from_address(tmp);
        }

        // Resolve the slice method
        auto method = data.resolved_method;
        auto variant_type_id = resolve_variant_type_id(fn, data.expr->resolved_type);
        auto method_node = get_variant_member_node(method, variant_type_id);
        auto slice_fn = get_fn(method_node);
        auto fn_type = method->resolved_type;

        // Get ?uint32 type from the method's parameter types (params[0]=self, [1]=start, [2]=end)
        auto opt_param_type = fn_type->data.fn.params[1];
        auto opt_type_l = compile_type(opt_param_type);

        // Build start: ?uint32
        llvm::Value *start_opt;
        if (data.start) {
            auto start_val = compile_expr(fn, data.start);
            start_opt = compile_conversion(fn, start_val, get_chitype(data.start), opt_param_type);
        } else {
            start_opt = llvm::ConstantAggregateZero::get(opt_type_l);
        }

        // Build end: ?uint32
        llvm::Value *end_opt;
        if (data.end) {
            auto end_val = compile_expr(fn, data.end);
            end_opt = compile_conversion(fn, end_val, get_chitype(data.end), opt_param_type);
        } else {
            end_opt = llvm::ConstantAggregateZero::get(opt_type_l);
        }

        // Call the slice method
        std::vector<llvm::Value *> args = {ref.address, start_opt, end_opt};
        auto sret_type = fn_type->data.fn.should_use_sret()
                             ? compile_type(fn_type->data.fn.return_type)
                             : nullptr;
        auto result = create_fn_call_invoke(slice_fn->llvm_fn, args, sret_type, nullptr, nullptr);
        emit_dbg_location(expr);
        return result;
    }
    case ast::NodeType::ParenExpr: {
        auto child = expr->data.child_expr;
        if (expr->resolved_outlet && !child->resolved_outlet)
            child->resolved_outlet = expr->resolved_outlet;
        return compile_expr(fn, child);
    }
    case ast::NodeType::UnitExpr: {
        return llvm::Constant::getNullValue(compile_type(get_system_types()->unit));
    }
    case ast::NodeType::TupleExpr: {
        auto &data = expr->data.tuple_expr;
        auto tuple_type = compile_type(expr->resolved_type);
        llvm::Value *tuple = llvm::UndefValue::get(tuple_type);
        for (int i = 0; i < data.items.size(); i++) {
            auto elem = compile_expr(fn, data.items[i]);
            tuple = m_ctx->llvm_builder->CreateInsertValue(tuple, elem, {(unsigned)i});
        }
        if (expr->resolved_outlet) {
            auto outlet_addr = compile_expr_ref(fn, expr->resolved_outlet).address;
            m_ctx->llvm_builder->CreateStore(tuple, outlet_addr);
            mark_outlet_alive(fn, expr->resolved_outlet);
        }
        return tuple;
    }
    case ast::NodeType::IfExpr: {
        auto &data = expr->data.if_expr;
        auto ret_type = expr->resolved_type ? get_chitype(expr) : nullptr;
        auto &builder = *m_ctx->llvm_builder;

        llvm::Value *var = nullptr;
        if (ret_type && ret_type->kind != TypeKind::Void &&
            ret_type->kind != TypeKind::Never) {
            if (expr->resolved_outlet) {
                var = compile_expr_ref(fn, expr->resolved_outlet).address;
            } else {
                var = compile_alloc(fn, expr, false, ret_type);
            }
        }

        llvm::Value *cond = nullptr;
        if (data.binding_clause) {
            auto expr_value = compile_comparator(fn, data.condition);
            auto comparator_type = expr_value->getType();
            auto clause_value = get_resolver()->resolve_constant_value(data.binding_clause);
            assert(clause_value);
            auto cond_value = compile_constant_value(fn, *clause_value,
                                                     get_chitype(data.binding_clause),
                                                     comparator_type);
            cond = builder.CreateICmpEQ(expr_value, cond_value);
        } else if (data.binding_decl) {
            auto opt_value = compile_expr(fn, data.condition);
            auto opt_type = get_chitype(data.condition);
            assert(opt_type && opt_type->kind == TypeKind::Optional);
            cond = builder.CreateExtractValue(opt_value, {0}, "if_let_has_value");
        } else {
            cond = compile_assignment_to_type(fn, data.condition, get_system_types()->bool_);
        }
        auto then_b = fn->new_label("_if_then");
        auto end_b = fn->new_label("_if_end");
        label_t *else_b = nullptr;
        if (data.else_node) {
            else_b = fn->new_label("_if_else");
            builder.CreateCondBr(cond, then_b, else_b);
        } else {
            builder.CreateCondBr(cond, then_b, end_b);
        }

        fn->use_label(then_b);
        compile_block(fn, expr, data.then_block, end_b, var);

        if (data.else_node) {
            fn->use_label(else_b);
            if (data.else_node->type == ast::NodeType::Block) {
                compile_block(fn, expr, data.else_node, end_b, var);
            } else {
                // else-if chain: compile recursively as expression
                if (var) {
                    auto else_val = compile_assignment_to_type(fn, data.else_node, ret_type);
                    if (else_val)
                        builder.CreateStore(else_val, var);
                    builder.CreateBr(end_b);
                } else {
                    compile_expr(fn, data.else_node);
                    builder.CreateBr(end_b);
                }
            }
        }

        fn->use_label(end_b);
        for (auto nvar : data.post_narrow_vars) {
            compile_stmt(fn, nvar);
        }

        if (!var)
            return nullptr;
        if (expr->resolved_outlet) {
            mark_outlet_alive(fn, expr->resolved_outlet);
        }
        return builder.CreateLoad(compile_type(ret_type), var);
    }
    case ast::NodeType::SwitchExpr: {
        auto &data = expr->data.switch_expr;
        auto ret_type = get_chitype(expr);
        auto &builder = *m_ctx->llvm_builder;
        llvm::Value *var = nullptr;
        if (ret_type->kind != TypeKind::Void && ret_type->kind != TypeKind::Never) {
            if (expr->resolved_outlet) {
                var = compile_expr_ref(fn, expr->resolved_outlet).address;
            } else {
                var = compile_alloc(fn, expr, false, ret_type);
            }
        }

        if (data.is_type_switch) {
            // Type switch: compare typeinfo pointers from fat pointer vtable
            auto iref_ref = compile_expr_ref(fn, data.expr);
            auto fp = builder.CreateLoad(compile_type(get_chitype(data.expr)), iref_ref.address,
                                         "fat_ptr");

            // Get the interface type from the switch expression
            auto expr_type = get_chitype(data.expr);
            auto iface_elem = expr_type->get_elem();

            auto done_label = fn->new_label("_tswitch_done");
            auto else_label = fn->new_label("_tswitch_else");

            // Build if-else chain for each non-else case
            ast::Node *else_case = nullptr;
            for (auto scase : data.cases) {
                if (scase->data.case_expr.is_else) {
                    else_case = scase;
                    continue;
                }

                auto case_label = fn->new_label("_tswitch_case");
                auto next_label = fn->new_label("_tswitch_next");

                // Get typeinfo from the concrete type's vtable for this interface
                auto clause = scase->data.case_expr.clauses[0];
                auto clause_type = get_resolver()->node_get_type(clause);
                auto clause_elem =
                    clause_type->is_pointer_like() ? clause_type->get_elem() : clause_type;
                auto cmp = compile_interface_type_match(fn, fp, iface_elem, clause_elem);
                builder.CreateCondBr(cmp, case_label, next_label);

                fn->use_label(case_label);
                compile_block(fn, expr, scase->data.case_expr.body, done_label, var);

                fn->use_label(next_label);
            }

            // Fall through to else
            builder.CreateBr(else_label);
            fn->use_label(else_label);
            if (else_case) {
                compile_block(fn, expr, else_case->data.case_expr.body, done_label, var);
            } else {
                builder.CreateBr(done_label);
            }

            fn->use_label(done_label);
            if (!var)
                return nullptr;
            if (expr->resolved_outlet) {
                mark_outlet_alive(fn, expr->resolved_outlet);
            }
            return builder.CreateLoad(compile_type(ret_type), var);
        }

        // Normal switch: integer comparator with LLVM switch instruction
        auto expr_value = compile_comparator(fn, data.expr);
        auto comparator_type = expr_value->getType();
        auto default_label = fn->new_label("_switch_default");

        auto switch_b = builder.CreateSwitch(expr_value, default_label, data.cases.size());

        auto done_label = fn->new_label("_switch_done");

        array<label_t *> case_labels;
        for (auto scase : data.cases) {
            auto label = default_label;
            if (!scase->data.case_expr.is_else) {
                label = fn->new_label("_switch_case");
            }
            for (auto clause : scase->data.case_expr.clauses) {
                auto clause_value = get_resolver()->resolve_constant_value(clause);
                assert(clause_value);
                auto cond_value = (llvm::ConstantInt *)compile_constant_value(
                    fn, *clause_value, get_chitype(clause), comparator_type);
                switch_b->addCase(cond_value, label);
            }
            case_labels.add(label);
        }

        bool has_else = false;
        for (int i = 0; i < data.cases.size(); i++) {
            fn->use_label(case_labels[i]);
            auto scase = data.cases[i];
            if (scase->data.case_expr.is_else) has_else = true;
            compile_block(fn, expr, scase->data.case_expr.body, done_label, var);
        }

        // No else: default label needs a terminator
        if (!has_else) {
            fn->use_label(default_label);
            builder.CreateBr(done_label);
        }

        fn->use_label(done_label);
        if (!var)
            return nullptr;
        if (expr->resolved_outlet) {
            mark_outlet_alive(fn, expr->resolved_outlet);
        }
        return builder.CreateLoad(compile_type(ret_type), var);
    }
    default:
        panic("not implemented: {}", PRINT_ENUM(expr->type));
    }
    return nullptr;
}

llvm::Value *Compiler::compile_comparator(Function *fn, ast::Node *expr, ChiType *type) {
    auto &builder = *m_ctx->llvm_builder;
    if (!type) {
        type = get_chitype(expr);
    }

    auto enum_type = get_resolver()->get_enum_type(type);
    if (enum_type && enum_type->kind == TypeKind::Enum) {
        auto ref = compile_expr_ref(fn, expr);
        auto address = ref.address ? ref.address : ref.value;
        auto value_type = enum_type->data.enum_.base_value_type;
        auto gep = builder.CreateStructGEP(compile_type(value_type), address, 0);
        return builder.CreateLoad(compile_type(enum_type->data.enum_.discriminator), gep,
                                  "_load_enum_value");
    }

    switch (type->kind) {
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef:
    case TypeKind::Pointer: {
        return compile_comparator(fn, expr, type->get_elem());
    }
    default: {
        return compile_expr(fn, expr);
        break;
    }
    }
}

void Compiler::compile_copy(Function *fn, llvm::Value *value, llvm::Value *dest, ChiType *type,
                            ast::Node *expr) {
    return compile_copy_with_ref(fn, RefValue::from_value(value), dest, type, expr);
}

void Compiler::compile_store_or_copy(Function *fn, llvm::Value *value, llvm::Value *dest,
                                     ChiType *type, ast::Node *expr, bool destruct_old) {
    if (!value)
        return;
    if (expr->analysis.moved) {
        if (expr->type == ast::NodeType::AwaitExpr) {
            auto it = m_async_await_refs.find(expr);
            if (it != m_async_await_refs.end() && it->second.address) {
                if (destruct_old)
                    compile_destruction_for_type(fn, dest, type);
                auto &builder = *m_ctx->llvm_builder;
                auto moved_value = it->second.value;
                if (!moved_value) {
                    moved_value = builder.CreateLoad(compile_type(type), it->second.address);
                }
                builder.CreateStore(moved_value, dest);
                if (auto alive_ptr = async_frame_alive_ptr_for_addr(fn, it->second.address)) {
                    builder.CreateStore(llvm::ConstantInt::getFalse(*m_ctx->llvm_ctx), alive_ptr);
                }
                return;
            }
        }
        // Moved temporary — destruct old if needed, then bitwise store
        if (destruct_old)
            compile_destruction_for_type(fn, dest, type);
        m_ctx->llvm_builder->CreateStore(value, dest);
    } else {
        // Named value — deep copy via copy
        compile_copy_with_ref(fn, RefValue::from_value(value), dest, type, expr, destruct_old);
    }
}

void Compiler::compile_copy_with_ref(Function *fn, RefValue src, llvm::Value *dest, ChiType *type,
                                     ast::Node *expr, bool destruct_old) {
    auto &builder = *m_ctx->llvm_builder;
    assert(src.value || src.address);
    auto dbg_node = expr ? expr : fn->node;
    llvm::Value *owned_copy_src = nullptr;

    // Lazy load: derive value from address when needed
    auto ensure_value = [&]() {
        if (!src.value && src.address) {
            src.value = builder.CreateLoad(compile_type(type), src.address);
        }
        return src.value;
    };

    auto get_copy_source_address = [&](const char *tmp_name) -> llvm::Value * {
        if (src.address) {
            return src.address;
        }
        auto *from_address = builder.CreateAlloca(compile_type(type), nullptr, tmp_name);
        builder.CreateStore(ensure_value(), from_address);
        owned_copy_src = from_address;
        return from_address;
    };

    auto destroy_copy_source_temp = [&]() {
        if (!src.owns_value || !get_resolver()->type_needs_destruction(type)) {
            return;
        }
        auto *addr = owned_copy_src ? owned_copy_src : src.address;
        if (addr) {
            compile_destruction_for_type(fn, addr, type);
        }
    };

    auto resolved_type = type;
    while (resolved_type && resolved_type->kind == TypeKind::Subtype) {
        auto final_type = resolved_type->data.subtype.final_type;
        if (!final_type) {
            break;
        }
        resolved_type = final_type;
    }

    // Optional: copy has_value flag + deep-copy inner value
    if (resolved_type && resolved_type->kind == TypeKind::Optional) {
        auto elem_type = resolved_type->get_elem();
        if (elem_type && get_resolver()->type_needs_destruction(elem_type)) {
            if (destruct_old) {
                compile_destruction_for_type(fn, dest, type);
            }
            auto opt_type_l = compile_type(type);
            auto from_address = get_copy_source_address("_opt_copy_src");
            // Copy has_value (field 0)
            auto src_hv = builder.CreateStructGEP(opt_type_l, from_address, 0);
            auto dst_hv = builder.CreateStructGEP(opt_type_l, dest, 0);
            auto hv = builder.CreateLoad(llvm::Type::getInt1Ty(*m_ctx->llvm_ctx), src_hv);
            builder.CreateStore(hv, dst_hv);
            // Deep-copy inner value (field 1) only if has_value
            auto bb_copy = fn->new_label("opt_copy_value");
            auto bb_done = fn->new_label("opt_copy_done");
            auto saved_loc = builder.getCurrentDebugLocation();
            builder.CreateCondBr(hv, bb_copy, bb_done);
            fn->use_label(bb_copy);
            builder.SetCurrentDebugLocation(saved_loc);
            auto src_val = builder.CreateStructGEP(opt_type_l, from_address, 1);
            auto dst_val = builder.CreateStructGEP(opt_type_l, dest, 1);
            compile_copy_with_ref(fn, RefValue::from_address(src_val), dst_val, elem_type, expr,
                                  false);
            builder.CreateBr(bb_done);
            fn->use_label(bb_done);
            builder.SetCurrentDebugLocation(saved_loc);
            destroy_copy_source_temp();
            return;
        }
        // For Optional with trivially-copyable inner type, fall through to default
    }

    // Enum-backed values: delegate to generated copier (switch on discriminator, deep-copy variant fields)
    auto enum_copy_type = resolved_type;
    if (enum_copy_type && enum_copy_type->kind == TypeKind::Enum) {
        enum_copy_type = enum_copy_type->data.enum_.base_value_type;
    }
    if (enum_copy_type && enum_copy_type->kind == TypeKind::EnumValue) {
        auto copier = generate_copier_enum(type);
        if (copier) {
            if (destruct_old) {
                compile_destruction_for_type(fn, dest, type);
            }
            auto from_address = get_copy_source_address("_enum_copy_src");
            builder.CreateCall(copier->llvm_fn, {dest, from_address});
            destroy_copy_source_temp();
            return;
        }
        // No copier needed — fall through to default bitwise store
    }

    switch (type->kind) {
    case TypeKind::String: {
        auto from_address = get_copy_source_address("_op_str_copy");
        auto copy_fn = get_system_fn("cx_string_copy");
        auto call = builder.CreateCall(copy_fn->llvm_fn, {dest, from_address});
        call->setDebugLoc(m_ctx->llvm_builder->getCurrentDebugLocation());
        emit_dbg_location(dbg_node);
        destroy_copy_source_temp();
        return;
    }
    case TypeKind::FnLambda: {
        // For lambda types, we need to manually copy fields and increment refcount
        // because __CxLambda<T1> and __CxLambda<T2> can't use copy across types
        auto &builder = *m_ctx->llvm_builder;
        auto llvm_type = compile_type(type);

        // Extract fn_ptr (field 0)
        auto fn_ptr = builder.CreateExtractValue(ensure_value(), {0}, "fn_ptr");

        // Extract size (field 1)
        auto size_val = builder.CreateExtractValue(ensure_value(), {1}, "size");

        auto captures_ptr = builder.CreateExtractValue(ensure_value(), {2}, "captures");
        llvm::Value *typeinfo_val = nullptr;
        if (llvm_type->getStructNumElements() > 3) {
            typeinfo_val = builder.CreateExtractValue(ensure_value(), {3}, "captures_ti");
        } else {
            typeinfo_val = llvm::ConstantPointerNull::get(builder.getInt8PtrTy());
        }

        // Retain type-erased captures (null-safe). Skip the retain when we
        // own the source value — ownership transfers into dest with the
        // existing refcount.
        if (!src.owns_value) {
            auto retain_fn = get_system_fn("cx_capture_retain");
            builder.CreateCall(retain_fn->llvm_fn, {captures_ptr});
        }

        // Build the destination lambda struct
        llvm::Value *dest_val = llvm::UndefValue::get(llvm_type);
        dest_val = builder.CreateInsertValue(dest_val, fn_ptr, {0});
        dest_val = builder.CreateInsertValue(dest_val, size_val, {1});
        dest_val = builder.CreateInsertValue(dest_val, captures_ptr, {2});
        if (llvm_type->getStructNumElements() > 3) {
            dest_val = builder.CreateInsertValue(dest_val, typeinfo_val, {3});
        }

        // Store the result
        builder.CreateStore(dest_val, dest);
        return;
    }
    case TypeKind::Subtype:
    case TypeKind::Array:
    case TypeKind::Span:
    case TypeKind::Struct: {
        // Interface copy via vtable dispatch
        // dest and src.address are fat pointer struct VALUES {data_ptr, vtable_ptr}
        if (ChiTypeStruct::is_interface(type)) {
            // dest is the ADDRESS of the fat pointer {data_ptr, vtable_ptr}
            // Load the fat pointer to extract data and vtable pointers
            auto iface_type_l =
                llvm::StructType::get(*m_ctx->llvm_ctx, {get_llvm_ptr_type(), get_llvm_ptr_type()});
            auto dest_fp = builder.CreateLoad(iface_type_l, dest, "dest_fp");
            auto dest_data_ptr = builder.CreateExtractValue(dest_fp, {0}, "dest_data");
            auto dest_vtable = builder.CreateExtractValue(dest_fp, {1}, "dest_vtable");

            auto src_addr = src.address ? src.address : nullptr;
            llvm::Value *src_data_ptr, *src_vtable;
            if (src_addr) {
                auto src_fp = builder.CreateLoad(iface_type_l, src_addr, "src_fp");
                src_data_ptr = builder.CreateExtractValue(src_fp, {0}, "src_data");
                src_vtable = builder.CreateExtractValue(src_fp, {1}, "src_vtable");
            } else {
                src_data_ptr = builder.CreateExtractValue(src.value, {0}, "src_data");
                src_vtable = builder.CreateExtractValue(src.value, {1}, "src_vtable");
            }

            // Destruct old value at dest via vtable destructor (skip if vtable is null)
            if (destruct_old) {
                auto vtable_is_null = builder.CreateICmpEQ(dest_vtable, get_null_ptr());
                auto bb_has_vtable = fn->new_label("iface_has_vtable");
                auto bb_skip_dtor = fn->new_label("iface_skip_dtor");
                builder.CreateCondBr(vtable_is_null, bb_skip_dtor, bb_has_vtable);

                fn->use_label(bb_has_vtable);
                call_vtable_destructor(fn, dest_vtable, dest_data_ptr);
                builder.CreateBr(bb_skip_dtor);

                fn->use_label(bb_skip_dtor);
            }

            // Copy the underlying concrete value via vtable copier
            call_vtable_copier(fn, src_vtable, dest_data_ptr, src_data_ptr);

            // Update vtable in destination fat pointer to match source
            auto vtable_gep = builder.CreateStructGEP(iface_type_l, dest, 1);
            builder.CreateStore(src_vtable, vtable_gep);
            return;
        }

        auto sty = get_resolver()->resolve_struct_type(eval_type(type));
        if (!sty)
            break; // Not a struct type, fall through to default copy
        auto &builder = *m_ctx->llvm_builder;
        auto copy_fn_p = sty->member_intrinsics.get(IntrinsicSymbol::Copy);
        if (copy_fn_p) {
            auto copy_fn = *copy_fn_p;
            auto from_address = get_copy_source_address("_op_copy");
            if (destruct_old) {
                compile_destruction_for_type(fn, dest, type);
            }
            // User-defined Copy sees a freshly-defaulted `this` — run the
            // generated field-default ctor so defaults (including non-zero
            // ones) are applied correctly.
            emit_construct_init(fn, dest, type);
            // Use variant lookup to get the specialized copy method
            auto eval_t = eval_type(type);
            std::optional<TypeId> variant_type_id = std::nullopt;
            if (eval_t && eval_t->kind == TypeKind::Subtype && !eval_t->is_placeholder) {
                variant_type_id = eval_t->id;
            }
            auto copy_fn_node = get_variant_member_node(copy_fn, variant_type_id);
            auto loc = m_ctx->llvm_builder->getCurrentDebugLocation();
            auto call = builder.CreateCall(get_fn(copy_fn_node)->llvm_fn, {
                                                                              dest,
                                                                              from_address,
                                                                          });
            call->setDebugLoc(loc);
            emit_dbg_location(dbg_node);
            destroy_copy_source_temp();
            return;
        }

        // For structs without Copy, check if any field needs destruction
        // (transitively — handles nested structs with Copy/destructor fields).
        // If so, copy field-by-field to ensure proper deep copy semantics.
        if (type->kind == TypeKind::Struct) {
            auto own = type->data.struct_.own_fields();
            bool needs_field_copy = false;

            for (auto field : own) {
                if (get_resolver()->type_needs_destruction(field->resolved_type)) {
                    needs_field_copy = true;
                    break;
                }
            }

            if (needs_field_copy) {
                if (destruct_old) {
                    compile_destruction_for_type(fn, dest, type);
                }
                auto from_address = get_copy_source_address("_struct_copy_src");

                auto llvm_type = compile_type(type);
                for (auto field : own) {
                    auto field_src_gep =
                        builder.CreateStructGEP(llvm_type, from_address, field->field_index);
                    auto field_dest_gep =
                        builder.CreateStructGEP(llvm_type, dest, field->field_index);
                    compile_copy_with_ref(fn, RefValue::from_address(field_src_gep), field_dest_gep,
                                          field->resolved_type, expr, false);
                }
                destroy_copy_source_temp();
                return;
            }
        }
        break;
    }
    case TypeKind::FixedArray: {
        auto copier = generate_copier_fixed_array(type);
        if (copier) {
            if (destruct_old) {
                compile_destruction_for_type(fn, dest, type);
            }
            auto from_address = get_copy_source_address("_fa_copy_src");
            builder.CreateCall(copier->llvm_fn, {dest, from_address});
            destroy_copy_source_temp();
            return;
        }
        // Trivially-copyable elements: fall through to default store
        break;
    }
    case TypeKind::Any: {
        if (destruct_old) {
            compile_destruction_for_type(fn, dest, type);
        }

        auto &llvm_ctx = *m_ctx->llvm_ctx;
        auto any_type_l = compile_type(type);
        auto ptr_type = get_llvm_ptr_type();
        auto i32_ty = llvm::Type::getInt32Ty(llvm_ctx);
        auto i8_ty = llvm::Type::getInt8Ty(llvm_ctx);

        // Ensure we have an address to work from
        auto src_addr = src.address;
        if (!src_addr) {
            src_addr = builder.CreateAlloca(any_type_l, nullptr, "_any_copy_src");
            builder.CreateStore(ensure_value(), src_addr);
        }

        // Copy TypeInfo pointer (field 0)
        auto src_ti_gep = builder.CreateStructGEP(any_type_l, src_addr, 0);
        auto ti_ptr = builder.CreateLoad(ptr_type, src_ti_gep, "src_any_ti");
        auto dst_ti_gep = builder.CreateStructGEP(any_type_l, dest, 0);
        builder.CreateStore(ti_ptr, dst_ti_gep);

        // Copy inlined flag (field 1)
        auto src_inlined_gep = builder.CreateStructGEP(any_type_l, src_addr, 1);
        auto inlined = builder.CreateLoad(i8_ty, src_inlined_gep, "any_inlined");
        auto dst_inlined_gep = builder.CreateStructGEP(any_type_l, dest, 1);
        builder.CreateStore(inlined, dst_inlined_gep);

        auto is_inlined = builder.CreateICmpNE(inlined, llvm::ConstantInt::get(i8_ty, 0));

        // Load copier and size from TypeInfo
        auto ti_header_l = get_typeinfo_llvm_type();
        auto size_gep = builder.CreateStructGEP(ti_header_l, ti_ptr, 1);
        auto typesize = builder.CreateLoad(i32_ty, size_gep, "any_typesize");
        auto copier_gep = builder.CreateStructGEP(ti_header_l, ti_ptr, 4);
        auto copier_ptr = builder.CreateLoad(ptr_type, copier_gep, "any_copier");

        auto src_data_gep = builder.CreateStructGEP(any_type_l, src_addr, 3);
        auto dst_data_gep = builder.CreateStructGEP(any_type_l, dest, 3);

        auto bb_inlined = fn->new_label("any_cp_inlined");
        auto bb_heap = fn->new_label("any_cp_heap");
        auto bb_done = fn->new_label("any_cp_done");
        builder.CreateCondBr(is_inlined, bb_inlined, bb_heap);

        // Inlined path: copy data in-place
        fn->use_label(bb_inlined);
        {
            auto copier_is_null = builder.CreateICmpEQ(copier_ptr, get_null_ptr());
            auto bb_memcpy = fn->new_label("any_cp_inl_memcpy");
            auto bb_copier = fn->new_label("any_cp_inl_copier");
            builder.CreateCondBr(copier_is_null, bb_memcpy, bb_copier);

            fn->use_label(bb_memcpy);
            builder.CreateMemCpy(dst_data_gep, {}, src_data_gep, {}, typesize);
            builder.CreateBr(bb_done);

            fn->use_label(bb_copier);
            auto copier_fn_type = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx),
                                                          {ptr_type, ptr_type}, false);
            builder.CreateCall(copier_fn_type, copier_ptr, {dst_data_gep, src_data_gep});
            builder.CreateBr(bb_done);
        }

        // Heap path: malloc new buffer, copy into it, store pointer in dest.data
        fn->use_label(bb_heap);
        {
            auto src_heap_ptr = builder.CreateLoad(ptr_type, src_data_gep, "src_heap_ptr");
            auto malloc_fn = get_system_fn("cx_malloc");
            auto new_buf =
                builder.CreateCall(malloc_fn->llvm_fn, {typesize, get_null_ptr()}, "new_heap_buf");

            auto copier_is_null = builder.CreateICmpEQ(copier_ptr, get_null_ptr());
            auto bb_h_memcpy = fn->new_label("any_cp_heap_memcpy");
            auto bb_h_copier = fn->new_label("any_cp_heap_copier");
            auto bb_h_done = fn->new_label("any_cp_heap_done");
            builder.CreateCondBr(copier_is_null, bb_h_memcpy, bb_h_copier);

            fn->use_label(bb_h_memcpy);
            builder.CreateMemCpy(new_buf, {}, src_heap_ptr, {}, typesize);
            builder.CreateBr(bb_h_done);

            fn->use_label(bb_h_copier);
            auto copier_fn_type = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx),
                                                          {ptr_type, ptr_type}, false);
            builder.CreateCall(copier_fn_type, copier_ptr, {new_buf, src_heap_ptr});
            builder.CreateBr(bb_h_done);

            fn->use_label(bb_h_done);
            builder.CreateStore(new_buf, dst_data_gep);
            builder.CreateBr(bb_done);
        }

        fn->use_label(bb_done);
        return;
    }
    default:
        break;
    }

    if (destruct_old) {
        compile_destruction_for_type(fn, dest, type);
    }
    auto size = llvm_type_size(compile_type(type));
    if (size > 0) {
        builder.CreateStore(ensure_value(), dest);
    }
}

void Compiler::compile_construction(Function *fn, llvm::Value *dest, ChiType *type,
                                    ast::Node *expr) {
    auto &builder = *m_ctx->llvm_builder.get();
    switch (type->kind) {
    case TypeKind::EnumValue: {
        auto &type_data = type->data.enum_value;
        assert(type->data.enum_value.member);
        auto enum_variant = type_data.member;
        auto enum_variant_p = m_ctx->enum_variant_table.get(enum_variant);
        assert(enum_variant_p);
        auto enum_var = *enum_variant_p;
        auto copy_size = llvm_type_size(((llvm::GlobalVariable *)enum_var)->getValueType());
        auto full_size = llvm_type_size(compile_type(type));
        auto zero = llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(*m_ctx->llvm_ctx), 0);
        builder.CreateMemSet(dest, zero, full_size, {});
        builder.CreateMemCpy(dest, {}, enum_var, {}, copy_size);

        std::set<int> initialized_fields;
        auto payload_fields = get_resolver()->get_enum_payload_fields(type);
        for (uint32_t i = 0; i < expr->data.construct_expr.items.size() && i < payload_fields.size(); i++) {
            initialized_fields.insert(payload_fields[i]->field_index);
        }
        for (auto field_init : expr->data.construct_expr.field_inits) {
            auto &field_init_data = field_init->data.field_init_expr;
            if (field_init_data.resolved_field) {
                initialized_fields.insert(field_init_data.resolved_field->field_index);
            }
        }

        if (auto resolved_struct = type_data.resolved_struct) {
            for (auto field : resolved_struct->data.struct_.fields) {
                auto promoted_field = get_resolver()->get_struct_member(type, field->get_name());
                if (!promoted_field || initialized_fields.count(promoted_field->field_index)) {
                    continue;
                }
                emit_default_field_initializer(fn, dest, type, promoted_field);
            }
        }

        for (uint32_t i = 0; i < expr->data.construct_expr.items.size() && i < payload_fields.size(); i++) {
            auto field = payload_fields[i];
            auto item = expr->data.construct_expr.items[i];
            auto gep = compile_dot_access(fn, dest, type, field);
            compile_assignment_to_ptr(fn, item, gep, field->resolved_type);
            emit_dbg_location(expr);
        }

        for (auto field_init : expr->data.construct_expr.field_inits) {
            auto &data = field_init->data.field_init_expr;
            auto *field = get_resolver()->get_struct_member(type, data.resolved_field->get_name());
            assert(field);
            auto gep = compile_dot_access(fn, dest, type, field);
            data.compiled_field_address = gep;
            compile_assignment_to_ptr(fn, data.value, gep, field->resolved_type);
            emit_dbg_location(expr);
        }
        break;
    }
    case TypeKind::Array:
    case TypeKind::Span: {
        auto array_struct_type = get_resolver()->eval_struct_type(type);
        return compile_construction(fn, dest, array_struct_type, expr);
    }
    case TypeKind::FixedArray: {
        auto &items = expr->data.construct_expr.items;
        auto elem_type = type->data.fixed_array.elem;
        auto arr_type_l = compile_type(type);
        auto fa_size = type->data.fixed_array.size;
        if (items.size() < fa_size) {
            auto byte_size = m_ctx->llvm_module->getDataLayout().getTypeAllocSize(arr_type_l);
            builder.CreateMemSet(
                dest,
                llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(*m_ctx->llvm_ctx), 0),
                byte_size, {});
        }
        auto i32_ty = llvm::IntegerType::getInt32Ty(*m_ctx->llvm_ctx);
        auto zero = llvm::ConstantInt::get(i32_ty, 0);
        for (uint32_t i = 0; i < items.size(); i++) {
            auto idx = llvm::ConstantInt::get(i32_ty, i);
            auto elem_ptr = builder.CreateGEP(arr_type_l, dest, {zero, idx});
            compile_assignment_to_ptr(fn, items[i], elem_ptr, elem_type, false);
        }
        break;
    }
    case TypeKind::Subtype: {
        // Resolve generic struct instantiation to its concrete struct type
        auto struct_type = get_resolver()->eval_struct_type(type);
        return compile_construction(fn, dest, struct_type, expr);
    }
    case TypeKind::Struct: {
        emit_construct_init(fn, dest, type,
                            type->data.struct_.node ? type->data.struct_.node->module : nullptr);

        auto *list_init_member = type->data.struct_.member_intrinsics.get(IntrinsicSymbol::ListInit)
                                     ? *type->data.struct_.member_intrinsics.get(
                                           IntrinsicSymbol::ListInit)
                                     : nullptr;
        auto constructor = ChiTypeStruct::get_constructor(type);
        bool use_list_init = expr->data.construct_expr.use_list_init;
        if (constructor) {
            auto variant_type_id = resolve_variant_type_id(m_fn, expr->resolved_type);
            auto constructor_node = get_variant_member_node(constructor, variant_type_id);

            auto managed_module = get_codegen_context_module(fn);
            auto use_managed_ctor =
                managed_module && has_lang_flag(managed_module->get_lang_flags(), LANG_FLAG_MANAGED);
            auto base_constructor_fn = get_fn(constructor_node);
            auto constructor_fn =
                use_managed_ctor ? get_managed_fn(base_constructor_fn, managed_module)
                                 : base_constructor_fn;
            auto constructor_type_l = (llvm::FunctionType *)compile_type(constructor_fn->fn_type);
            auto args = std::vector<llvm::Value *>{dest};
            ast::Block arg_cleanup_block = {};
            std::vector<ast::Node *> transferred_cleanup_vars = {};
            push_cleanup_block(fn, arg_cleanup_block);
            NodeList ctor_items = {};
            if (!use_list_init) {
                ctor_items = expr->data.construct_expr.items;
            }
            auto remaining_args =
                compile_fn_args(fn, constructor_fn, ctor_items, expr, &arg_cleanup_block,
                                &transferred_cleanup_vars);
            args.insert(args.end(), remaining_args.begin(), remaining_args.end());

            // Compile default args for missing params (e.g. = {} on generic field)
            if (constructor_node->type == ast::NodeType::FnDef) {
                auto &proto = constructor_node->data.fn_def.fn_proto->data.fn_proto;
                for (size_t i = ctor_items.size(); i < proto.params.size(); i++) {
                    auto default_val = proto.params[i]->data.param_decl.effective_default_value();
                    if (!default_val)
                        break;
                    auto param_type = constructor_fn->fn_type->data.fn.get_param_at(i);
                    args.push_back(compile_arg_for_call(fn, default_val, param_type,
                                                        &arg_cleanup_block,
                                                        &transferred_cleanup_vars));
                }
            }
            emit_dbg_location(expr);
            builder.CreateCall(constructor_type_l, constructor_fn->llvm_fn, args);
            for (auto *var : transferred_cleanup_vars) {
                arg_cleanup_block.exit_flow.add_sink_edge(var, expr);
            }
            pop_cleanup_block(fn, arg_cleanup_block);
        }

        if (use_list_init) {
            auto variant_type_id = resolve_variant_type_id(m_fn, expr->resolved_type);
            assert(list_init_member && "ListInit member missing");
            auto list_init_node = get_variant_member_node(list_init_member, variant_type_id);
            auto list_init_type = get_chitype(list_init_node);
            auto list_init_id = get_resolver()->resolve_global_id(list_init_node);
            auto list_init_entry = m_ctx->function_table.get(list_init_id);
            assert(list_init_entry && "list init method not compiled");
            auto list_init_fn = *list_init_entry;
            auto list_init_type_l = (llvm::FunctionType *)compile_type(list_init_type);
            auto args = std::vector<llvm::Value *>{dest};
            ast::Block arg_cleanup_block = {};
            std::vector<ast::Node *> transferred_cleanup_vars = {};
            push_cleanup_block(fn, arg_cleanup_block);
            auto remaining_args = compile_fn_args(fn, list_init_fn, expr->data.construct_expr.items,
                                                  expr, &arg_cleanup_block,
                                                  &transferred_cleanup_vars);
            args.insert(args.end(), remaining_args.begin(), remaining_args.end());
            builder.CreateCall(list_init_type_l, list_init_fn->llvm_fn, args);
            for (auto *var : transferred_cleanup_vars) {
                arg_cleanup_block.exit_flow.add_sink_edge(var, expr);
            }
            pop_cleanup_block(fn, arg_cleanup_block);
            emit_dbg_location(expr);
            return;
        }

        if (expr->data.construct_expr.use_kv_init) {
            auto variant_type_id = resolve_variant_type_id(m_fn, expr->resolved_type);
            auto *kv_init_member =
                *type->data.struct_.member_intrinsics.get(IntrinsicSymbol::KvInit);
            assert(kv_init_member && "KvInit member missing");
            auto kv_init_node = get_variant_member_node(kv_init_member, variant_type_id);
            auto kv_init_type = get_chitype(kv_init_node);
            auto kv_init_id = get_resolver()->resolve_global_id(kv_init_node);
            auto kv_init_entry = m_ctx->function_table.get(kv_init_id);
            assert(kv_init_entry && "kv_init method not compiled");
            auto kv_init_fn = *kv_init_entry;
            auto kv_init_type_l = (llvm::FunctionType *)compile_type(kv_init_type);

            for (auto fi : expr->data.construct_expr.field_inits) {
                auto &fi_data = fi->data.field_init_expr;
                ast::Block arg_cleanup_block = {};
                std::vector<ast::Node *> transferred_cleanup_vars = {};
                push_cleanup_block(fn, arg_cleanup_block);

                NodeList kv_args = {};
                kv_args.add(fi_data.key_expr);
                kv_args.add(fi_data.value);
                auto remaining_args = compile_fn_args(fn, kv_init_fn, kv_args, fi,
                                                      &arg_cleanup_block,
                                                      &transferred_cleanup_vars);
                auto args = std::vector<llvm::Value *>{dest};
                args.insert(args.end(), remaining_args.begin(), remaining_args.end());
                builder.CreateCall(kv_init_type_l, kv_init_fn->llvm_fn, args);

                for (auto *var : transferred_cleanup_vars) {
                    arg_cleanup_block.exit_flow.add_sink_edge(var, fi);
                }
                pop_cleanup_block(fn, arg_cleanup_block);
            }
            emit_dbg_location(expr);
            return;
        }

        if (expr->data.construct_expr.spread_expr) {
            // Build set of target field names that have explicit overrides
            std::set<int> overridden;
            for (auto fi : expr->data.construct_expr.field_inits) {
                if (fi->data.field_init_expr.resolved_field)
                    overridden.insert(fi->data.field_init_expr.resolved_field->field_index);
            }

            auto spread_expr = expr->data.construct_expr.spread_expr;
            auto spread_ref = compile_expr_ref(fn, spread_expr);
            auto spread_type = get_chitype(spread_expr);
            auto spread_type_l = compile_type(spread_type);
            auto dest_type_l = compile_type(type);

            // Iterate source fields, find matching target field by name
            for (auto src_field : spread_type->data.struct_.own_fields()) {
                auto tgt_field = get_resolver()->get_struct_member(type, src_field->get_name());
                if (!tgt_field)
                    continue; // validated by resolver
                if (overridden.count(tgt_field->field_index))
                    continue;

                auto src_gep = builder.CreateStructGEP(spread_type_l, spread_ref.address,
                                                       src_field->field_index);
                auto dst_gep = builder.CreateStructGEP(dest_type_l, dest, tgt_field->field_index);
                auto field_type_l = compile_type(src_field->resolved_type);
                auto val = builder.CreateLoad(field_type_l, src_gep);
                builder.CreateStore(val, dst_gep);
                if (get_resolver()->type_needs_destruction(src_field->resolved_type))
                    compile_copy(fn, val, dst_gep, src_field->resolved_type, spread_expr);
            }
        }

        for (auto field_init : expr->data.construct_expr.field_inits) {
            auto &data = field_init->data.field_init_expr;
            auto *field = get_resolver()->get_struct_member(type, data.resolved_field->get_name());
            assert(field);
            auto field_gep = builder.CreateStructGEP(compile_type(type), dest, field->field_index);
            data.compiled_field_address = field_gep;
            bool destroys_old = field->node && field->node->data.var_decl.expr;
            compile_assignment_to_ptr(fn, data.value, field_gep, field->resolved_type,
                                      destroys_old);
        }
        break;
    }
    case TypeKind::String: {
        auto value = compile_string_literal("");
        builder.CreateStore(value, dest);
        break;
    }
    case TypeKind::Int: {
        builder.CreateStore(llvm::ConstantInt::get(compile_type(type), 0), dest);
        break;
    }
    case TypeKind::Optional: {
        assert(expr->type == ast::NodeType::ConstructExpr &&
               expr->data.construct_expr.items.size() == 1);
        auto value = expr->data.construct_expr.items[0];
        auto opt_type = get_chitype(expr);
        auto elem_type = eval_type(opt_type->get_elem());
        auto has_value_p = builder.CreateStructGEP(compile_type(opt_type), dest, 0);
        builder.CreateStore(
            llvm::ConstantInt::get(llvm::IntegerType::getInt1Ty(*m_ctx->llvm_ctx), 1), has_value_p);
        auto value_p = builder.CreateStructGEP(compile_type(opt_type), dest, 1);
        compile_assignment_to_ptr(fn, value, value_p, elem_type);
        break;
    }
    default: {
        auto size = llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(*m_ctx->llvm_ctx),
                                           llvm_type_size(compile_type(type)));
        builder.CreateMemSet(
            dest, llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(*m_ctx->llvm_ctx), 0), size,
            {});
        break;
    }
    }
}

void Compiler::compile_destructure(Function *fn, ast::DestructureDecl &data,
                                  llvm::Value *source_ptr, ChiType *source_type,
                                  llvm::Value *borrow_source_ptr) {
    auto &builder = *m_ctx->llvm_builder.get();
    if (!borrow_source_ptr)
        borrow_source_ptr = source_ptr;
    if (data.is_tuple) {
        auto tuple_ptr = source_ptr;
        auto tuple_type = source_type;
        if (data.resolved_as_tuple) {
            tuple_type = data.as_tuple_result_type;
            auto method = data.resolved_as_tuple;
            auto variant_type_id = resolve_variant_type_id(fn, source_type);
            auto method_node = get_variant_member_node(method, variant_type_id);
            auto as_tuple_fn = get_fn(method_node);
            auto fn_type = method->resolved_type;
            auto tuple_type_l = compile_type(tuple_type);
            tuple_ptr = builder.CreateAlloca(tuple_type_l, nullptr, "as_tuple");
            auto sret_type = fn_type->data.fn.should_use_sret() ? tuple_type_l : nullptr;
            std::vector<llvm::Value *> args = {source_ptr};
            auto result =
                create_fn_call_invoke(as_tuple_fn->llvm_fn, args, sret_type, nullptr, tuple_ptr);
            if (result) {
                builder.CreateStore(result, tuple_ptr);
            }
            borrow_source_ptr = tuple_ptr;
        }
        compile_tuple_destructure(fn, data, tuple_ptr, tuple_type, borrow_source_ptr);
    } else if (data.is_array) {
        compile_array_destructure(fn, data, source_ptr, source_type, borrow_source_ptr);
    } else {
        compile_destructure_fields(fn, data.fields, source_ptr, source_type, borrow_source_ptr);
    }
}

void Compiler::compile_destructure_fields(Function *fn, array<ast::Node *> &fields,
                                          llvm::Value *source_ptr, ChiType *source_type,
                                          llvm::Value *borrow_source_ptr) {
    auto &builder = *m_ctx->llvm_builder;
    auto struct_type_l = compile_type(source_type);
    size_t var_idx = 0;

    for (auto field_node : fields) {
        auto &field_data = field_node->data.destructure_field;
        auto member = field_data.resolved_field;
        auto field_ptr = compile_dot_access(fn, source_ptr, source_type, member);
        auto borrow_field_ptr = borrow_source_ptr
                                    ? compile_dot_access(fn, borrow_source_ptr, source_type, member)
                                    : field_ptr;

        if (field_data.nested) {
            compile_destructure(fn, field_data.nested->data.destructure_decl, field_ptr,
                               member->resolved_type, borrow_field_ptr);
        } else {
            // Allocate binding variable
            auto &gen_vars = field_node->parent->data.destructure_decl.generated_vars;
            assert(var_idx < gen_vars.size());
            auto var_node = gen_vars[var_idx++];
            auto var_ptr = compile_alloc(fn, var_node);
            add_var(var_node, var_ptr);

            if (field_data.sigil == ast::SigilKind::Reference ||
                field_data.sigil == ast::SigilKind::MutRef) {
                // Reference binding: store field address directly
                builder.CreateStore(borrow_field_ptr, var_ptr);
            } else {
                // Copy binding: load field value and copy
                auto var_type = get_chitype(var_node);
                auto var_type_l = compile_type(var_type);
                auto field_value = builder.CreateLoad(var_type_l, field_ptr);
                compile_store_or_copy(fn, field_value, var_ptr, var_type, field_node);
            }
        }
    }
}

void Compiler::compile_array_destructure(Function *fn, ast::DestructureDecl &data,
                                         llvm::Value *source_ptr, ChiType *source_type,
                                         llvm::Value *borrow_source_ptr) {
    auto &builder = *m_ctx->llvm_builder;
    if (!borrow_source_ptr)
        borrow_source_ptr = source_ptr;

    // Same pattern as IndexExpr codegen (Struct case, lines 3835-3843)
    auto method = data.resolved_index_method;
    auto variant_type_id = resolve_variant_type_id(fn, source_type);
    auto method_node = get_variant_member_node(method, variant_type_id);
    auto index_fn = get_fn(method_node);

    Function *slice_fn = nullptr;
    ChiStructMember *slice_method = data.resolved_slice_method;
    ChiType *slice_fn_type = nullptr;
    if (slice_method) {
        auto slice_method_node = get_variant_member_node(slice_method, variant_type_id);
        slice_fn = get_fn(slice_method_node);
        slice_fn_type = slice_method->resolved_type;
    }

    // Index parameter type (first param of index_mut)
    auto index_type = method->resolved_type->data.fn.get_param_at(0);
    auto index_type_l = compile_type(index_type);

    // Element type from return type (index_mut returns &T)
    auto elem_type = method->resolved_type->data.fn.return_type->get_elem();
    auto elem_type_l = compile_type(elem_type);

    for (size_t i = 0; i < data.fields.size(); i++) {
        auto field_node = data.fields[i];

        // Allocate binding variable
        assert(i < data.generated_vars.size());
        auto var_node = data.generated_vars[i];
        auto var_ptr = compile_alloc(fn, var_node);
        add_var(var_node, var_ptr);

        auto &field_data = field_node->data.destructure_field;
        if (field_data.is_rest) {
            assert(slice_fn && slice_fn_type);
            auto opt_param_type = slice_fn_type->data.fn.params[1];
            auto opt_type_l = compile_type(opt_param_type);
            auto start_val = llvm::ConstantInt::get(index_type_l, i);
            auto start_opt = compile_conversion(fn, start_val, index_type, opt_param_type);
            auto end_opt = llvm::ConstantAggregateZero::get(opt_type_l);
            auto var_type = get_chitype(var_node);
            auto var_type_l = compile_type(var_type);
            std::vector<llvm::Value *> args = {source_ptr, start_opt, end_opt};
            auto sret_type =
                slice_fn_type->data.fn.should_use_sret() ? var_type_l : nullptr;
            create_fn_call_invoke(slice_fn->llvm_fn, args, sret_type, nullptr, var_ptr);
            continue;
        }

        auto index_val = llvm::ConstantInt::get(index_type_l, i);
        auto elem_ptr = builder.CreateCall(index_fn->llvm_fn, {source_ptr, index_val});
        auto borrow_elem_ptr = builder.CreateCall(index_fn->llvm_fn, {borrow_source_ptr, index_val});

        if (field_data.sigil == ast::SigilKind::Reference ||
            field_data.sigil == ast::SigilKind::MutRef) {
            builder.CreateStore(borrow_elem_ptr, var_ptr);
        } else {
            auto elem_value = builder.CreateLoad(elem_type_l, elem_ptr);
            compile_store_or_copy(fn, elem_value, var_ptr, elem_type, field_node);
        }
    }
}

void Compiler::compile_tuple_destructure(Function *fn, ast::DestructureDecl &data,
                                         llvm::Value *source_ptr, ChiType *source_type,
                                         llvm::Value *borrow_source_ptr) {
    auto &builder = *m_ctx->llvm_builder;
    if (!borrow_source_ptr)
        borrow_source_ptr = source_ptr;
    auto source_type_l = compile_type(source_type);
    TypeList tuple_like_elems;
    array<ChiStructMember *> tuple_like_fields;
    bool tuple_like_struct = false;
    if (source_type->kind != TypeKind::Tuple) {
        auto stype = get_resolver()->resolve_struct_type(source_type);
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
        }
    }
    TypeList &elems = source_type->kind == TypeKind::Tuple ? source_type->data.tuple.elements
                                                           : tuple_like_elems;

    for (size_t i = 0; i < data.fields.size(); i++) {
        auto field_node = data.fields[i];
        auto &field_data = field_node->data.destructure_field;

        assert(i < data.generated_vars.size());
        auto var_node = data.generated_vars[i];
        auto var_ptr = compile_alloc(fn, var_node);
        add_var(var_node, var_ptr);

        if (field_data.is_rest) {
            auto rest_type = get_chitype(var_node);
            auto rest_type_l = compile_type(rest_type);
            int rest_count = elems.size() - (int)i;
            for (int j = 0; j < rest_count; j++) {
                llvm::Value *src_ptr;
                auto elem_type = elems[i + j];
                if (tuple_like_struct) {
                    src_ptr = compile_dot_access(fn, source_ptr, source_type,
                                                 tuple_like_fields[i + j]);
                } else {
                    src_ptr = builder.CreateStructGEP(source_type_l, source_ptr, (unsigned)(i + j));
                }
                auto dst_ptr = builder.CreateStructGEP(rest_type_l, var_ptr, (unsigned)j);
                auto elem_type_l = compile_type(elem_type);
                auto val = builder.CreateLoad(elem_type_l, src_ptr);
                builder.CreateStore(val, dst_ptr);
            }
        } else {
            auto elem_type = elems[i];
            auto elem_type_l = compile_type(elem_type);
            llvm::Value *elem_ptr;
            llvm::Value *borrow_elem_ptr;
            if (tuple_like_struct) {
                elem_ptr = compile_dot_access(fn, source_ptr, source_type, tuple_like_fields[i]);
                borrow_elem_ptr = compile_dot_access(fn, borrow_source_ptr, source_type,
                                                     tuple_like_fields[i]);
            } else {
                elem_ptr = builder.CreateStructGEP(source_type_l, source_ptr, (unsigned)i);
                borrow_elem_ptr =
                    builder.CreateStructGEP(source_type_l, borrow_source_ptr, (unsigned)i);
            }

            if (field_data.sigil == ast::SigilKind::Reference ||
                field_data.sigil == ast::SigilKind::MutRef) {
                builder.CreateStore(borrow_elem_ptr, var_ptr);
            } else {
                auto elem_value = builder.CreateLoad(elem_type_l, elem_ptr);
                compile_store_or_copy(fn, elem_value, var_ptr, elem_type, field_node);
            }
        }
    }
}

llvm::Value *Compiler::compile_dot_ptr(Function *fn, ast::Node *expr) {
    auto ctn_type = get_chitype(expr);
    // Optional chaining: unwrap ?T to get pointer to T value
    if (ctn_type->kind == TypeKind::Optional) {
        auto ref = compile_expr_ref(fn, expr);
        auto opt_type_l = compile_type(ctn_type);
        auto &builder = *m_ctx->llvm_builder;
        return builder.CreateStructGEP(opt_type_l, ref.address, 1);
    }
    if (ctn_type->is_pointer_like()) {
        // Interface references are fat pointers — need address for CreateStructGEP
        auto elem = ctn_type->get_elem();
        if (elem && ChiTypeStruct::is_interface(elem)) {
            auto ref = compile_expr_ref(fn, expr);
            return ref.address ? ref.address : ref.value;
        }
        return compile_expr(fn, expr);
    }
    auto ref = compile_expr_ref(fn, expr);
    if (ref.address)
        return ref.address;

    auto &builder = *m_ctx->llvm_builder;
    auto tmp = fn->entry_alloca(ref.value->getType(), "dot_tmp");
    builder.CreateStore(ref.value, tmp);
    return tmp;
}

llvm::Value *Compiler::compile_optional_branch(
    Function *fn, ast::Node *opt_expr, llvm::Type *result_type_l, const char *label,
    std::function<llvm::Value *(llvm::Value *unwrapped_ptr)> on_has_value,
    std::function<llvm::Value *()> on_null, llvm::Value *result_slot) {
    auto &builder = *m_ctx->llvm_builder.get();
    auto base_ref = compile_expr_ref(fn, opt_expr);
    auto opt_type_l = compile_type(get_chitype(opt_expr));

    auto has_value_p = builder.CreateStructGEP(opt_type_l, base_ref.address, 0);
    auto has_value = builder.CreateLoad(llvm::Type::getInt1Ty(*m_ctx->llvm_ctx), has_value_p);
    auto unwrapped_ptr = builder.CreateStructGEP(opt_type_l, base_ref.address, 1);

    auto has_val_bb = fn->new_label(string("_") + label + "_has");
    auto null_bb = fn->new_label(string("_") + label + "_null");
    auto merge_bb = fn->new_label(string("_") + label + "_merge");
    builder.CreateCondBr(has_value, has_val_bb, null_bb);

    fn->use_label(has_val_bb);
    auto has_val_result = on_has_value(unwrapped_ptr);
    auto has_val_end = builder.GetInsertBlock();
    builder.CreateBr(merge_bb);

    fn->use_label(null_bb);
    auto null_result = on_null();
    auto null_end = builder.GetInsertBlock();
    builder.CreateBr(merge_bb);

    fn->use_label(merge_bb);
    if (result_slot) {
        return builder.CreateLoad(result_type_l, result_slot);
    }
    auto phi = builder.CreatePHI(result_type_l, 2, string("_") + label);
    phi->addIncoming(has_val_result, has_val_end);
    phi->addIncoming(null_result, null_end);
    return phi;
}

llvm::Value *Compiler::compile_dot_access(Function *fn, llvm::Value *ptr, ChiType *type,
                                          ChiStructMember *member) {
    auto &builder = *m_ctx->llvm_builder;
    if (member->parent_member) {
        ptr = compile_dot_access(fn, ptr, type, member->parent_member);
        type = member->parent_member->resolved_type;
    }
    auto struct_type_l = compile_type(type);
    auto gep = builder.CreateStructGEP(struct_type_l, ptr, member->field_index);
    return gep;
}

RefValue Compiler::compile_expr_ref(Function *fn, ast::Node *expr) {
    if (expr->resolved_node) {
        return compile_expr_ref(fn, expr->resolved_node);
    }
    switch (expr->type) {
    case ast::NodeType::FnDef: {
        auto lambda_val = compile_expr(fn, expr);
        return RefValue::from_owned_value(lambda_val);
    }
    case ast::NodeType::VarDecl: {
        auto &var_data = expr->data.var_decl;
        if (var_data.kind == ast::VarKind::Constant && var_data.expr) {
            // Inline compile-time constant values
            return compile_expr_ref(fn, var_data.expr);
        }
        return RefValue::from_address(get_var(expr));
    }
    case ast::NodeType::Identifier:
        return compile_iden_ref(fn, expr);
    case ast::NodeType::FieldInitExpr: {
        auto &data = expr->data.field_init_expr;
        auto field_address = (llvm::Value *)data.compiled_field_address;
        assert(data.compiled_field_address);
        return RefValue::from_address(field_address);
    }
    case ast::NodeType::DotExpr: {
        auto &builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();

        auto &data = expr->data.dot_expr;

        auto *base_expr = data.effective_expr();

        // Narrowing redirect: use the narrowed var's GEP alias
        if (data.narrowed_var) {
            return RefValue::from_address(get_var(data.narrowed_var));
        }

        auto type = get_chitype(base_expr);

        if (data.resolved_dot_kind == DotKind::TupleField) {
            auto ref = compile_expr_ref(fn, base_expr);
            auto tuple_type = compile_type(type);
            auto gep = m_ctx->llvm_builder->CreateStructGEP(tuple_type, ref.address, (unsigned)data.resolved_value);
            return RefValue::from_address(gep);
        }

        llvm::Value *ptr = nullptr;
        if (data.resolved_dot_kind == DotKind::EnumVariant) {
            auto evalue_type = get_chitype(expr);
            assert(evalue_type->kind == TypeKind::EnumValue);
            auto evalue_type_l = compile_type(evalue_type);
            auto resolved_member = evalue_type->data.enum_value.member;
            if (resolved_member) {
                auto member_var = m_ctx->enum_variant_table.get(resolved_member);
                assert(member_var);
                return RefValue::from_address(*member_var);
            } else {
                panic("not implemented");
                return RefValue();
            }
        } else if (type->is_pointer_like()) {
            type = type->get_elem();
            ptr = compile_expr(fn, base_expr);
        } else {
            // Check if this is module member access (e.g., sdl.SDL_Init)
            if (base_expr->type == ast::NodeType::Identifier &&
                base_expr->data.identifier.decl &&
                base_expr->data.identifier.decl->type == ast::NodeType::ImportDecl) {
                // Module member access - use resolved_decl if available
                if (data.resolved_decl) {
                    // Resolver already found the declaration, compile it directly
                    return compile_expr_ref(fn, data.resolved_decl);
                }
                // Fallback: look up the member from the module directly
                auto import_decl = base_expr->data.identifier.decl;
                auto imported_module = import_decl->data.import_decl.resolved_module;
                if (!imported_module || !imported_module->scope) {
                    panic("ImportDecl for '{}' has no resolved module", base_expr->name);
                }
                auto member_name = data.field->str;
                auto member_node = imported_module->scope->find_one(member_name);
                if (!member_node) {
                    panic("Module '{}' has no member '{}'", base_expr->name, member_name);
                }
                return compile_expr_ref(fn, member_node);
            }

            auto ref = compile_expr_ref(fn, base_expr);
            if (type->kind == TypeKind::Fn) {
                ptr = ref.value;
            } else if (ref.address) {
                ptr = ref.address;
            } else if (ref.value) {
                auto base_ptr = fn->entry_alloca(compile_type(type), "dot_base");
                builder.CreateStore(ref.value, base_ptr);
                ptr = base_ptr;
            } else {
                ptr = nullptr;
            }
        }

        assert(ptr);
        auto gep = compile_dot_access(fn, ptr, type, data.resolved_struct_member);
        return RefValue::from_address(gep);
    }
    case ast::NodeType::UnaryOpExpr: {
        auto &data = expr->data.unary_op_expr;
        auto &builder = *m_ctx->llvm_builder;
        switch (data.op_type) {
        case TokenType::MUL: {
            if (data.resolved_call) {
                auto ref_ptr = compile_fn_call(fn, data.resolved_call);
                return RefValue::from_address(ref_ptr);
            }
            // For pointer-to-interface deref, return the fat pointer variable's address
            // so interface copy can update both data and vtable
            auto op1_type = get_chitype(data.op1);
            if (op1_type && op1_type->is_pointer_like() && op1_type->get_elem() &&
                ChiTypeStruct::is_interface(op1_type->get_elem())) {
                auto ref = compile_expr_ref(fn, data.op1);
                return RefValue::from_address(ref.address);
            }
            return RefValue::from_address(compile_expr(fn, data.op1));
        }
        case TokenType::LNOT: {
            if (data.is_suffix) {
                if (data.op1->resolved_type->kind == TypeKind::Optional) {
                    auto ref = compile_expr_ref(fn, data.op1);
                    auto opt_type_l = compile_type(data.op1->resolved_type);
                    auto has_value_p = builder.CreateStructGEP(opt_type_l, ref.address, 0);
                    auto has_value = builder.CreateLoad(
                        llvm::Type::getInt1Ty(*m_ctx->llvm_ctx), has_value_p);
                    emit_dbg_location(expr);
                    auto msg = compile_string_literal("unwrapping null optional");
                    emit_runtime_assert(fn, has_value, msg, expr);
                    auto value_p = builder.CreateStructGEP(opt_type_l, ref.address, 1);
                    return RefValue::from_address(value_p);
                }
                if (data.op1->type == ast::NodeType::Identifier &&
                    data.op1->data.identifier.decl &&
                    data.op1->data.identifier.decl->type == ast::NodeType::VarDecl &&
                    data.op1->data.identifier.decl->data.var_decl.narrowed_from) {
                    return compile_expr_ref(fn, data.op1);
                }
                if (data.op1->type == ast::NodeType::DotExpr &&
                    data.op1->data.dot_expr.narrowed_var) {
                    return compile_expr_ref(fn, data.op1);
                }
                if (data.resolved_call) {
                    auto ref_ptr = compile_fn_call(fn, data.resolved_call);
                    return RefValue::from_address(ref_ptr);
                }
            }
            panic("unreachable");
            break;
        }
        case TokenType::AND:
        case TokenType::MUTREF:
        case TokenType::MOVEREF:
        case TokenType::KW_MOVE:
        case TokenType::SUB:
            // These produce rvalues - return value only
            return RefValue::from_owned_value(compile_expr(fn, expr));
        default:
            panic("compile_expr_ref UnaryOpExpr not implemented: {}", PRINT_ENUM(data.op_type));
        }
    }
    case ast::NodeType::IndexExpr: {
        auto &builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();
        auto &data = expr->data.index_expr;
        auto type = get_chitype(data.expr);
        auto base_type = type;
        if (base_type && base_type->is_reference()) {
            base_type = base_type->get_elem();
        }
        auto subscript = compile_expr(fn, data.subscript);
        switch (base_type->kind) {
        case TypeKind::Pointer: {
            auto ptr = compile_expr(fn, data.expr);
            auto zero = llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(llvm_ctx), 0);
            return RefValue::from_address(
                builder.CreateGEP(compile_type(base_type->get_elem()), ptr, {subscript}));
        }
        case TypeKind::FixedArray: {
            auto ptr = compile_dot_ptr(fn, data.expr);
            auto arr_type_l = compile_type(base_type);
            auto fa_size = base_type->data.fixed_array.size;
            // Bounds check
            auto size_val =
                llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(llvm_ctx), fa_size);
            auto cond = builder.CreateICmpULT(subscript, size_val);
            auto msg =
                compile_string_literal(fmt::format("index out of bounds (size {})", fa_size));
            emit_runtime_assert(fn, cond, msg, expr);
            emit_dbg_location(expr);
            auto zero = llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(llvm_ctx), 0);
            return RefValue::from_address(
                builder.CreateGEP(arr_type_l, ptr, {zero, subscript}));
        }
        case TypeKind::Struct:
        case TypeKind::Subtype:
        case TypeKind::Array:
        case TypeKind::Span: {
            auto method = data.resolved_method;
            auto variant_type_id = resolve_variant_type_id(fn, data.expr->resolved_type);
            auto method_node = get_variant_member_node(method, variant_type_id);
            auto index_fn = get_fn(method_node);
            auto ctn_ptr = compile_dot_ptr(fn, data.expr);
            auto call = builder.CreateCall(index_fn->llvm_fn, {ctn_ptr, subscript});
            emit_dbg_location(expr);
            return RefValue::from_address(call);
        }
        default:
            panic("not implemented: {}", PRINT_ENUM(base_type->kind));
        }
    }
    case ast::NodeType::FnCallExpr: {
        llvm::Value *dest = nullptr;
        bool owns_temp = false;
        if (expr->resolved_outlet) {
            dest = get_var(expr->resolved_outlet);
        } else {
            dest = compile_alloc(fn, expr);
            owns_temp = true;
        }
        compile_fn_call(fn, expr, nullptr, dest);
        emit_dbg_location(expr);
        if (expr->resolved_outlet) {
            mark_outlet_alive(fn, expr->resolved_outlet);
        }
        return RefValue{dest, nullptr, owns_temp};
    }
    // Rvalue expressions - return value only (no meaningful address)
    case ast::NodeType::ParenExpr:
        return compile_expr_ref(fn, expr->data.child_expr);
    case ast::NodeType::IfExpr:
    case ast::NodeType::SwitchExpr:
    case ast::NodeType::TryExpr:
    case ast::NodeType::TypeInfoExpr:
    case ast::NodeType::BinOpExpr:
    case ast::NodeType::ConstructExpr:
    case ast::NodeType::LiteralExpr:
    case ast::NodeType::UnitExpr:
    case ast::NodeType::TupleExpr:
    case ast::NodeType::AwaitExpr: {
        auto it = m_async_await_refs.find(expr);
        if (it != m_async_await_refs.end()) {
            return it->second;
        }
        auto value = compile_expr(fn, expr);
        if (expr->resolved_outlet &&
            m_ctx->var_table.has_key(expr->resolved_outlet)) {
            return RefValue{get_var(expr->resolved_outlet), value, false};
        }
        return RefValue::from_owned_value(value);
    }
    case ast::NodeType::CastExpr: {
        auto &data = expr->data.cast_expr;
        if (get_chitype(data.expr) == get_chitype(expr)) {
            return compile_expr_ref(fn, data.expr);
        }
        auto it = m_async_await_refs.find(expr);
        if (it != m_async_await_refs.end()) {
            return it->second;
        }
        auto value = compile_expr(fn, expr);
        if (expr->resolved_outlet &&
            m_ctx->var_table.has_key(expr->resolved_outlet)) {
            return RefValue{get_var(expr->resolved_outlet), value, false};
        }
        return RefValue::from_owned_value(value);
    }
    default:
        panic("compile_expr_ref not implemented: {}", PRINT_ENUM(expr->type));
    }
    return {};
}

llvm::Value *Compiler::load_capture_move_flag_ptr(Function *fn, ast::Node *iden) {
    if (!fn || !fn->bind_ptr || !iden || !iden->analysis.is_capture() ||
        iden->analysis.capture_path.size() == 0) {
        return nullptr;
    }
    auto &immediate_capture = iden->analysis.capture_path[0];
    auto capture_idx = immediate_capture.capture_index;
    if (capture_idx < 0) {
        return nullptr;
    }

    auto fn_type = get_chitype(fn->node);
    if (!fn_type || fn_type->kind != TypeKind::FnLambda) {
        return nullptr;
    }
    auto &captures = fn->node->data.fn_def.captures;
    if (capture_idx >= captures.size() || captures[capture_idx].mode != ast::CaptureMode::ByRef) {
        return nullptr;
    }

    auto bstruct = fn_type->data.fn_lambda.bind_struct;
    auto bstruct_l = (llvm::StructType *)compile_type(bstruct);
    auto &builder = *m_ctx->llvm_builder.get();
    auto flag_gep = builder.CreateStructGEP(bstruct_l, fn->bind_ptr, captures.size() + capture_idx);
    return builder.CreateLoad(bstruct_l->elements()[captures.size() + capture_idx], flag_gep,
                              "capture_move_flag");
}

RefValue Compiler::compile_iden_ref(Function *fn, ast::Node *iden) {
    auto &builder = *m_ctx->llvm_builder.get();
    assert(iden->type == ast::NodeType::Identifier);
    auto &data = iden->data.identifier;

    if (data.kind == ast::IdentifierKind::This) {
        // Check for narrowed 'this' (e.g., enum variant narrowing in switch)
        if (data.decl && data.decl->data.var_decl.narrowed_from) {
            return RefValue::from_address(get_var(data.decl));
        }
        return RefValue::from_address(fn->get_this_arg());
    }
    // Unwrap ImportSymbol to reach the actual declaration
    auto *resolved_decl = data.decl;
    if (resolved_decl->type == ast::NodeType::ImportSymbol &&
        resolved_decl->data.import_symbol.resolved_decl) {
        resolved_decl = resolved_decl->data.import_symbol.resolved_decl;
    }

    if (resolved_decl->type == ast::NodeType::VarDecl) {
        auto &var = resolved_decl->data.var_decl;
        if (var.kind == ast::VarKind::Constant && !resolved_decl->parent_fn) {
            if (var.resolved_value.has_value()) {
                return RefValue::from_value(
                    compile_constant_value(fn, *var.resolved_value, get_chitype(resolved_decl)));
            } else if (var.expr) {
                // Inline the expression if resolved_value wasn't set
                return compile_expr_ref(fn, var.expr);
            }
        }
        // Module-level let: look up the global variable directly
        if (!resolved_decl->parent_fn) {
            return RefValue::from_address(get_var(resolved_decl));
        }
        goto normal;
    }
    if (data.decl->type == ast::NodeType::FnDef) {
        auto fn_obj = get_fn(data.decl);
        auto iden_type = get_chitype(iden);

        // If the identifier type is a lambda, we need to create a lambda struct
        if (iden_type->kind == TypeKind::FnLambda) {
            auto lambda_value = compile_lambda_alloc(fn, iden_type, fn_obj->llvm_fn, nullptr);
            return RefValue::from_owned_value(lambda_value);
        }

        // Otherwise, return the raw function pointer
        auto type_l = compile_type(iden_type);
        return RefValue::from_value(fn_obj->llvm_fn);
    }
    if (data.decl->type == ast::NodeType::ImportDecl) {
        // Module identifiers (import aliases) should not be compiled directly
        // They should only appear as part of DotExpr for module member access
        panic("Cannot compile module identifier '{}' directly - module member access should be "
              "resolved",
              iden->name);
    }

normal:
    // handle captured variables
    if (iden->analysis.is_capture()) {
        assert(fn->bind_ptr);
        assert(iden->analysis.capture_path.size() > 0);

        // The capture_path[0] represents the current function's capture
        // We just need to access the immediate capture from current function's bind_ptr
        auto &immediate_capture = iden->analysis.capture_path[0];
        auto capture_idx = immediate_capture.capture_index;

        // Get the binding structure for the current function
        auto fn_type = get_chitype(fn->node);
        assert(fn_type->kind == TypeKind::FnLambda);
        auto bstruct = fn_type->data.fn_lambda.bind_struct;
        auto bstruct_l = (llvm::StructType *)compile_type(bstruct);

        auto gep = builder.CreateStructGEP(bstruct_l, fn->bind_ptr, capture_idx);

        // Check the actual capture mode from the function's captures list
        auto &captures = fn->node->data.fn_def.captures;
        bool is_by_ref = capture_idx < captures.size() &&
                         captures[capture_idx].mode == ast::CaptureMode::ByRef;
        if (!is_by_ref) {
            // By-value capture: the field IS the value
            return RefValue::from_address(gep);
        }

        // By-reference capture: the field is a pointer, load it
        auto ptr_type = bstruct_l->elements()[capture_idx];
        auto captured_var_ptr = builder.CreateLoad(ptr_type, gep);
        return RefValue::from_address(captured_var_ptr);
    }
    return RefValue::from_address(get_var(data.decl));
}

std::vector<llvm::Value *> Compiler::compile_fn_args(
    Function *fn, Function *callee, array<ast::Node *> args, ast::Node *fn_call,
    ast::Block *cleanup_block, std::vector<ast::Node *> *transferred_cleanup_vars) {
    std::vector<llvm::Value *> call_args = {};
    auto &fn_spec = callee->fn_type->data.fn;
    auto va_start = fn_spec.get_va_start();

    bool is_variadic = callee->fn_type->data.fn.is_variadic;
    bool is_extern = callee->fn_type->data.fn.is_extern;

    for (int i = 0; i < args.size(); i++) {
        if (is_variadic && !is_extern && i >= va_start) {
            continue;
        }
        if (is_variadic && is_extern && i >= va_start) {
            call_args.push_back(compile_extern_variadic_arg(fn, args[i]));
            continue;
        }
        auto arg_node = args[i];
        auto param_type = fn_spec.get_param_at(i);

        // For C variadic args (param_type is nullptr), compile the expression directly
        if (param_type) {
            call_args.push_back(compile_arg_for_call(fn, arg_node, param_type, cleanup_block,
                                                     transferred_cleanup_vars));
        } else {
            call_args.push_back(compile_expr(fn, arg_node));
        }
    }

    if (is_variadic && !is_extern) {
        call_args.push_back(
            compile_variadic_span_arg(fn, args, va_start, fn_spec.get_variadic_span_param(), fn_call));
    }

    return call_args;
}

llvm::Value *Compiler::compile_builtin_trait_call(Function *fn, ast::Node *expr,
                                                   ChiType *concrete_type,
                                                   const std::string &method_name,
                                                   ast::FnCallExpr &fn_call_data) {
    auto &builder = *m_ctx->llvm_builder.get();
    auto &llvm_ctx = *m_ctx->llvm_ctx.get();
    auto receiver_expr = fn_call_data.fn_ref_expr->data.dot_expr.effective_expr();
    auto receiver = compile_expr(fn, receiver_expr);

    if (method_name == "hash") {
        // Declare cx_meiyan if not already declared
        auto i64_ty = llvm::Type::getInt64Ty(llvm_ctx);
        auto i32_ty = llvm::Type::getInt32Ty(llvm_ctx);
        auto ptr_ty = builder.getPtrTy();
        auto meiyan_fn_ty = llvm::FunctionType::get(i64_ty, {ptr_ty, i32_ty}, false);
        auto meiyan_fn = m_ctx->llvm_module->getOrInsertFunction("cx_meiyan", meiyan_fn_ty);

        // Int/float/bool/char/rune: alloca, store, hash the raw bytes
        auto value_type_l = compile_type(concrete_type);
        auto tmp = fn->entry_alloca(value_type_l, "hash_tmp");
        builder.CreateStore(receiver, tmp);
        auto size = llvm::ConstantInt::get(
            i32_ty, m_ctx->llvm_module->getDataLayout().getTypeAllocSize(value_type_l));
        return builder.CreateCall(meiyan_fn, {tmp, size}, "hash");
    } else if (method_name == "eq") {
        assert(fn_call_data.args.size() == 1 && "eq() expects one argument");
        auto other = compile_expr(fn, fn_call_data.args[0]);
        // Unwrap reference: if the arg is &T (from operator method wrapping), load to get T
        if (get_chitype(fn_call_data.args[0])->is_reference()) {
            other = builder.CreateLoad(compile_type(concrete_type), other, "deref_arg");
        }

        if (concrete_type->kind == TypeKind::Float) {
            return builder.CreateFCmpOEQ(receiver, other, "eq");
        } else {
            // Int-like, bool, char, rune: icmp eq
            return builder.CreateICmpEQ(receiver, other, "eq");
        }
    } else if (method_name == "cmp") {
        // Ord::cmp — returns int (lhs - rhs for numeric types)
        assert(fn_call_data.args.size() == 1 && "cmp() expects one argument");
        auto other = compile_expr(fn, fn_call_data.args[0]);
        // Unwrap reference: if the arg is &T (from operator method wrapping), load to get T
        if (get_chitype(fn_call_data.args[0])->is_reference()) {
            other = builder.CreateLoad(compile_type(concrete_type), other, "deref_arg");
        }

        if (concrete_type->kind == TypeKind::Float) {
            // Float comparison: convert to int result (-1, 0, 1)
            auto lt = builder.CreateFCmpOLT(receiver, other, "lt");
            auto gt = builder.CreateFCmpOGT(receiver, other, "gt");
            auto i64_ty = llvm::Type::getInt64Ty(llvm_ctx);
            auto neg1 = llvm::ConstantInt::getSigned(i64_ty, -1);
            auto zero = llvm::ConstantInt::getSigned(i64_ty, 0);
            auto one = llvm::ConstantInt::getSigned(i64_ty, 1);
            auto gt_val = builder.CreateSelect(gt, one, zero);
            return builder.CreateSelect(lt, neg1, gt_val, "cmp");
        } else {
            // Int-like: just subtract
            return builder.CreateSub(receiver, other, "cmp");
        }
    }

    panic("compile_builtin_trait_call: unhandled method '{}' on type {}", method_name,
          PRINT_ENUM(concrete_type->kind));
    return nullptr;
}

llvm::Value *Compiler::compile_intrinsic(Function *fn, ast::Node *expr, InvokeInfo *invoke,
                                         bool *handled) {
    auto &data = expr->data.fn_call_expr;
    auto &builder = *m_ctx->llvm_builder.get();
    auto callee_decl = data.fn_ref_expr->get_decl();
    if (handled) {
        *handled = false;
    }
    if (!callee_decl)
        return nullptr;

    auto get_required_const_bool_arg = [&](ast::Node *arg) {
        auto value = get_resolver()->resolve_constant_value(arg);
        assert(value.has_value() && holds_alternative<const_int_t>(*value) &&
               "intrinsic bool argument must be a compile-time constant");
        return get<const_int_t>(*value) != 0;
    };

    auto atomic_order = llvm::AtomicOrdering::SequentiallyConsistent;
    auto ptr_type = get_llvm_ptr_type();
    switch (callee_decl->symbol) {
    case IntrinsicSymbol::ReflectDynElem: {
        if (handled) {
            *handled = true;
        }

        auto value_ptr = compile_expr(fn, data.args[1]);
        auto value_type = get_dyn_elem_value_type(data.args[1]);
        value_type = value_type ? eval_type(value_type) : nullptr;
        if (!value_type) {
            return get_null_ptr();
        }

        if (value_type->kind == TypeKind::Any) {
            auto any_type_l = (llvm::StructType *)compile_type(value_type);
            auto type_gep = builder.CreateStructGEP(any_type_l, value_ptr, 0);
            return builder.CreateLoad(ptr_type, type_gep, "dyn_elem_any_ti");
        }

        if (value_type->is_pointer_like()) {
            auto elem_type = eval_type(value_type->get_elem());
            if (elem_type && ChiTypeStruct::is_interface(elem_type)) {
                auto fat_ptr_type_l = compile_type(value_type);
                auto fat_ptr = builder.CreateLoad(fat_ptr_type_l, value_ptr, "dyn_elem_fat_ptr");
                auto vtable_ptr = extract_interface_vtable_ptr(fat_ptr);
                auto vtable_is_null = builder.CreateICmpEQ(vtable_ptr, get_null_ptr());

                auto bb_runtime = fn->new_label("dyn_elem_runtime");
                auto bb_done = fn->new_label("dyn_elem_done");
                auto cur_bb = builder.GetInsertBlock();
                builder.CreateCondBr(vtable_is_null, bb_done, bb_runtime);

                fn->use_label(bb_runtime);
                auto runtime_ti = load_runtime_type_info_from_vtable(vtable_ptr);
                builder.CreateBr(bb_done);
                auto runtime_bb = builder.GetInsertBlock();

                fn->use_label(bb_done);
                auto result = builder.CreatePHI(ptr_type, 2, "dyn_elem_ti");
                result->addIncoming(get_null_ptr(), cur_bb);
                result->addIncoming(runtime_ti, runtime_bb);
                return result;
            }

            if (elem_type) {
                return compile_type_info(elem_type);
            }
        }

        return get_null_ptr();
    }
    case IntrinsicSymbol::MemCopy: {
        if (handled) {
            *handled = true;
        }
        auto dest_ptr = compile_expr(fn, data.args[0]);
        auto src_ptr = compile_expr(fn, data.args[1]);
        auto elem_type = get_chitype(data.args[0])->get_elem();
        assert(elem_type && "first arg of __copy must be a pointer type");
        bool destruct_old = get_required_const_bool_arg(data.args[2]);
        if (ChiTypeStruct::is_interface(elem_type)) {
            auto dest_data = builder.CreateExtractValue(dest_ptr, {0}, "dest_data");
            auto src_data = builder.CreateExtractValue(src_ptr, {0}, "src_data");
            auto src_vtable = builder.CreateExtractValue(src_ptr, {1}, "src_vtable");
            call_vtable_copier(fn, src_vtable, dest_data, src_data);
        } else {
            compile_copy_with_ref(fn, RefValue::from_address(src_ptr), dest_ptr, elem_type,
                                  nullptr, destruct_old);
        }
        return nullptr;
    }
    case IntrinsicSymbol::AnnotateCopy:
        if (handled) {
            *handled = true;
        }
        return nullptr;
    case IntrinsicSymbol::MemDestroy: {
        if (handled) {
            *handled = true;
        }
        auto dest_ptr = compile_expr(fn, data.args[0]);
        auto elem_type = find_nonvoid_pointee_type(data.args[0]);
        assert(elem_type && "first arg of destroy must be a typed pointer");
        compile_destruction_for_type(fn, dest_ptr, elem_type);
        return nullptr;
    }
    case IntrinsicSymbol::MemMove: {
        if (handled) {
            *handled = true;
        }
        auto dest_ptr = compile_expr(fn, data.args[0]);
        auto src_ptr = compile_expr(fn, data.args[1]);
        auto size = compile_expr(fn, data.args[2]);
        builder.CreateMemCpy(dest_ptr, llvm::MaybeAlign(), src_ptr, llvm::MaybeAlign(), size);
        return nullptr;
    }
    case IntrinsicSymbol::AtomicLoad: {
        if (handled) {
            *handled = true;
        }
        auto ptr = compile_expr(fn, data.args[0]);
        auto out_ptr = compile_expr(fn, data.args[1]);
        auto elem_type = find_nonvoid_pointee_type(data.args[1]);
        assert(elem_type && "could not recover atomic load element type");
        auto elem_type_l = compile_type(elem_type);
        auto align = m_ctx->llvm_module->getDataLayout().getABITypeAlign(elem_type_l);
        auto *load = builder.CreateAlignedLoad(elem_type_l, ptr, align);
        load->setAtomic(atomic_order);
        builder.CreateStore(load, out_ptr);
        return nullptr;
    }
    case IntrinsicSymbol::AtomicStore: {
        if (handled) {
            *handled = true;
        }
        auto ptr = compile_expr(fn, data.args[0]);
        auto value_ptr = compile_expr(fn, data.args[1]);
        auto elem_type = find_nonvoid_pointee_type(data.args[1]);
        assert(elem_type && "could not recover atomic store element type");
        auto elem_type_l = compile_type(elem_type);
        auto align = m_ctx->llvm_module->getDataLayout().getABITypeAlign(elem_type_l);
        auto value = builder.CreateLoad(elem_type_l, value_ptr);
        auto *store = builder.CreateAlignedStore(value, ptr, align);
        store->setAtomic(atomic_order);
        return nullptr;
    }
    case IntrinsicSymbol::AtomicCompareExchange: {
        if (handled) {
            *handled = true;
        }
        auto ptr = compile_expr(fn, data.args[0]);
        auto expected_ptr = compile_expr(fn, data.args[1]);
        auto desired_ptr = compile_expr(fn, data.args[2]);
        auto out_old_ptr = compile_expr(fn, data.args[3]);
        auto out_ok_ptr = compile_expr(fn, data.args[4]);
        auto elem_type = find_nonvoid_pointee_type(data.args[1]);
        assert(elem_type && "could not recover atomic compare_exchange element type");
        auto elem_type_l = compile_type(elem_type);
        auto align = m_ctx->llvm_module->getDataLayout().getABITypeAlign(elem_type_l);
        auto expected = builder.CreateLoad(elem_type_l, expected_ptr);
        auto desired = builder.CreateLoad(elem_type_l, desired_ptr);
        auto *cmpxchg =
            builder.CreateAtomicCmpXchg(ptr, expected, desired, align, atomic_order, atomic_order);
        cmpxchg->setWeak(false);
        auto old_value = builder.CreateExtractValue(cmpxchg, {0});
        auto ok_value = builder.CreateExtractValue(cmpxchg, {1});
        builder.CreateStore(old_value, out_old_ptr);
        builder.CreateStore(ok_value, out_ok_ptr);
        return nullptr;
    }
    case IntrinsicSymbol::AtomicFetchAdd: {
        if (handled) {
            *handled = true;
        }
        auto ptr = compile_expr(fn, data.args[0]);
        auto value_ptr = compile_expr(fn, data.args[1]);
        auto out_old_ptr = compile_expr(fn, data.args[2]);
        auto elem_type = find_nonvoid_pointee_type(data.args[1]);
        assert(elem_type && "could not recover atomic fetch_add element type");
        auto elem_type_l = compile_type(elem_type);
        auto align = m_ctx->llvm_module->getDataLayout().getABITypeAlign(elem_type_l);
        auto value = builder.CreateLoad(elem_type_l, value_ptr);
        auto *old_value = builder.CreateAtomicRMW(llvm::AtomicRMWInst::Add, ptr, value, align,
                                                  atomic_order);
        builder.CreateStore(old_value, out_old_ptr);
        return nullptr;
    }
    case IntrinsicSymbol::AtomicFetchSub: {
        if (handled) {
            *handled = true;
        }
        auto ptr = compile_expr(fn, data.args[0]);
        auto value_ptr = compile_expr(fn, data.args[1]);
        auto out_old_ptr = compile_expr(fn, data.args[2]);
        auto elem_type = find_nonvoid_pointee_type(data.args[1]);
        assert(elem_type && "could not recover atomic fetch_sub element type");
        auto elem_type_l = compile_type(elem_type);
        auto align = m_ctx->llvm_module->getDataLayout().getABITypeAlign(elem_type_l);
        auto value = builder.CreateLoad(elem_type_l, value_ptr);
        auto *old_value = builder.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, ptr, value, align,
                                                  atomic_order);
        builder.CreateStore(old_value, out_old_ptr);
        return nullptr;
    }
    default:
        break;
    }

    return nullptr;
}

llvm::Value *Compiler::compile_fn_call(Function *fn, ast::Node *expr, InvokeInfo *invoke,
                                       llvm::Value *sret_dest) {
    // If inside a try-block, create invoke with the block's landing pad
    InvokeInfo try_block_invoke;
    if (!invoke && fn->try_block_landing) {
        try_block_invoke.landing = fn->try_block_landing;
        invoke = &try_block_invoke;
    }

    auto &data = expr->data.fn_call_expr;
    auto &builder = *m_ctx->llvm_builder.get();

    bool intrinsic_handled = false;
    auto intrinsic_ret = compile_intrinsic(fn, expr, invoke, &intrinsic_handled);
    if (intrinsic_handled) {
        if (intrinsic_ret && sret_dest) {
            builder.CreateStore(intrinsic_ret, sret_dest);
            return nullptr;
        }
        return intrinsic_ret;
    }

    if (data.fn_ref_expr->resolved_type->kind == TypeKind::FnLambda) {
        auto ref = compile_expr_ref(fn, data.fn_ref_expr);
        auto lambda_type = get_chitype(data.fn_ref_expr);
        auto &fn_spec = lambda_type->data.fn_lambda.bound_fn->data.fn;
        auto bound_fn_type_l =
            (llvm::FunctionType *)compile_type(lambda_type->data.fn_lambda.bound_fn);
        std::vector<llvm::Value *> args = {};
        ast::Block arg_cleanup_block = {};
        std::vector<ast::Node *> transferred_cleanup_vars = {};
        push_cleanup_block(fn, arg_cleanup_block);

        // __CxLambda is no longer generic, so just use it directly
        auto struct_type = lambda_type->data.fn_lambda.internal;
        std::optional<TypeId> variant_type_id = std::nullopt;

        // Call as_ptr() method to get the function pointer
        auto as_ptr_member = struct_type->data.struct_.find_member("as_ptr");
        assert(as_ptr_member && "as_ptr() method not found in __CxLambda");
        auto as_ptr_method_node = get_variant_member_node(as_ptr_member, variant_type_id);
        auto as_ptr_method = get_fn(as_ptr_method_node);
        auto fn_ptr = builder.CreateCall(as_ptr_method->llvm_fn, {ref.address});

        // Call data_ptr() method to get the actual pointer to captures
        auto data_ptr_member = struct_type->data.struct_.find_member("data_ptr");
        assert(data_ptr_member && "data_ptr() method not found in __CxLambda");
        auto data_ptr_method_node = get_variant_member_node(data_ptr_member, variant_type_id);
        auto data_ptr_method = get_fn(data_ptr_method_node);
        auto data_ptr = builder.CreateCall(data_ptr_method->llvm_fn, {ref.address});

        // Check if the return type needs sret (struct return)
        auto return_type = fn_spec.return_type;
        bool use_sret = fn_spec.should_use_sret();
        llvm::Value *sret_var = nullptr;

        if (use_sret) {
            // For struct returns, allocate space and pass as first argument
            auto return_type_l = compile_type(return_type);
            sret_var = sret_dest ? sret_dest : fn->entry_alloca(return_type_l, "lambda_sret");
            args.push_back(sret_var);
        }

        // Always pass binding struct pointer as argument for all lambdas
        args.push_back(data_ptr);

        for (int i = 0; i < data.args.size(); i++) {
            auto arg = data.args[i];
            // User arguments always start from parameter index 1 (after binding struct)
            int param_index = i + 1;
            auto param_type = fn_spec.get_param_at(param_index);
            args.push_back(compile_arg_for_call(fn, arg, param_type, &arg_cleanup_block,
                                                &transferred_cleanup_vars));
        }

        llvm::FunctionCallee callee(bound_fn_type_l, fn_ptr);
        llvm::Value *ret = nullptr;
        if (invoke) {
            if (!invoke->normal) {
                invoke->normal = llvm::BasicBlock::Create(*m_ctx->llvm_ctx, "_invoke_next",
                                                          fn->llvm_fn);
            }
            ret = builder.CreateInvoke(callee, invoke->normal, invoke->landing, args);
            invoke->used = true;
            if (use_sret) {
                // For invoke with sret, the load happens later in the normal block
                invoke->sret = sret_var;
                invoke->sret_type = compile_type(return_type);
            }
            fn->use_label(invoke->normal);
            for (auto *var : transferred_cleanup_vars) {
                arg_cleanup_block.exit_flow.add_sink_edge(var, data.fn_ref_expr);
            }
            pop_cleanup_block(fn, arg_cleanup_block);
            // For scalar sret_dest, store result now (mirrors non-invoke path)
            if (!use_sret && sret_dest) {
                builder.CreateStore(ret, sret_dest);
                return nullptr;
            }
            // Invoke is a terminator - return value will be loaded by caller if needed
            return ret;
        } else {
            ret = builder.CreateCall(callee, args);
            for (auto *var : transferred_cleanup_vars) {
                arg_cleanup_block.exit_flow.add_sink_edge(var, data.fn_ref_expr);
            }
            pop_cleanup_block(fn, arg_cleanup_block);
            // For sret, load and return the struct value
            if (use_sret) {
                if (sret_dest) {
                    // sret_dest was used directly as sret_var, result is already there
                    return nullptr;
                }
                return builder.CreateLoad(compile_type(return_type), sret_var);
            }
            if (sret_dest) {
                builder.CreateStore(ret, sret_dest);
                return nullptr;
            }
            return ret;
        }
    }

    // For method calls via DotExpr, use the actual receiver type's ID for variant lookup
    std::optional<TypeId> container_type_id = std::nullopt;
    if (data.fn_ref_expr->type == ast::NodeType::DotExpr) {
        auto &dot_data = data.fn_ref_expr->data.dot_expr;
        if (dot_data.should_resolve_variant) {
            container_type_id = resolve_variant_type_id(fn, dot_data.expr->resolved_type);
        }
    }
    if (!container_type_id.has_value() && fn->container_type) {
        container_type_id = fn->container_type->id;
    }
    auto fn_decl = data.fn_ref_expr->get_decl(container_type_id);
    assert(fn_decl->type == ast::NodeType::FnDef);
    auto fn_type = get_chitype(fn_decl);
    ChiType *ctn_type = nullptr;

    if (expr->type == ast::NodeType::FnCallExpr) {
        auto &fn_call_data = expr->data.fn_call_expr;

        // For generic function calls, use the specialized function type
        if (fn_call_data.generated_fn) {
            fn_type = get_chitype(fn_call_data.generated_fn);
        }

        // Replace methods of type trait placeholder with concrete type
        if (fn_call_data.fn_ref_expr->type == ast::NodeType::DotExpr) {
            auto &dot_data = fn_call_data.fn_ref_expr->data.dot_expr;
            if (dot_data.resolved_dot_kind == DotKind::TypeTrait) {
                auto name = dot_data.field->get_name();
                auto concrete_type = get_chitype(dot_data.expr);
                auto concrete_struct = get_resolver()->resolve_struct_type(concrete_type);

                if (!concrete_struct) {
                    // Built-in type: generate intrinsic code for trait methods
                    auto result = compile_builtin_trait_call(fn, expr, concrete_type, name,
                                                             fn_call_data);
                    if (sret_dest && result) {
                        builder.CreateStore(result, sret_dest);
                        return nullptr;
                    }
                    return result;
                }

                auto concrete_member = concrete_struct->find_member(name);
                if (!concrete_member) {
                    // Built-in type mapped to a runtime struct (e.g. string → __CxString)
                    // that doesn't have the trait method — use intrinsic fallback
                    auto result = compile_builtin_trait_call(fn, expr, concrete_type, name,
                                                             fn_call_data);
                    if (sret_dest && result) {
                        builder.CreateStore(result, sret_dest);
                        return nullptr;
                    }
                    return result;
                }
                fn_type = concrete_member->resolved_type;
                ctn_type = concrete_type;
                fn_decl = concrete_member->node;
            }
        }
    }

    auto &fn_spec = fn_type->data.fn;
    auto is_variadic = fn_spec.is_variadic;
    auto is_extern = fn_spec.is_extern;
    auto va_start = fn_spec.get_va_start();

    std::vector<llvm::Value *> args;
    ast::Block arg_cleanup_block = {};
    std::vector<ast::Node *> transferred_cleanup_vars = {};
    push_cleanup_block(fn, arg_cleanup_block);

    llvm::FunctionCallee callee;
    llvm::Value *ctn_ptr = nullptr;
    if (fn_spec.container_ref && !fn_decl->declspec().is_static()) {
        auto dot_expr = data.fn_ref_expr->data.dot_expr;
        if (!ctn_type) {
            ctn_type = get_chitype(dot_expr.effective_expr());
        }
        // Unwrap Optional for ?. method calls
        if (ctn_type->kind == TypeKind::Optional) {
            ctn_type = ctn_type->get_elem();
        }
        auto ctn_type_l = compile_type(ctn_type);
        auto ptr = compile_dot_ptr(fn, dot_expr.effective_expr());

        // Check if receiver is an interface reference (fat pointer dispatch)
        bool receiver_is_interface = false;
        if (ctn_type->is_pointer_like()) {
            auto elem = ctn_type->get_elem();
            receiver_is_interface = elem && ChiTypeStruct::is_interface(elem);
        }

        bool redirected = false;
        if (fn->default_method_struct) {
            // Inside a default method body: redirect interface method calls on
            // 'this' to the concrete struct's implementation (direct call).
            // Only redirect when the receiver is actually 'this' — other locals
            // (e.g. Buffer) must dispatch to their own methods, not the concrete struct's.
            bool receiver_is_this =
                dot_expr.effective_expr()->type == ast::NodeType::Identifier &&
                dot_expr.effective_expr()->data.identifier.kind == ast::IdentifierKind::This;
            if (receiver_is_this) {
                auto concrete_struct =
                    get_resolver()->resolve_struct_type(fn->default_method_struct);
                auto concrete_member =
                    concrete_struct->find_member(dot_expr.field->get_name());
                if (concrete_member) {
                    fn_decl = concrete_member->node;
                    fn_type = concrete_member->resolved_type;
                    ctn_ptr = ptr; // thin pointer to concrete struct
                    redirected = true;
                }
            }
        }
        if (!redirected && receiver_is_interface) {
            // handle interface — ctn_type is &Interface, fat pointer {data_ptr, vtable_ptr}
            auto data_gep = builder.CreateStructGEP(ctn_type_l, ptr, 0);
            auto vtable_gep = builder.CreateStructGEP(ctn_type_l, ptr, 1);
            auto data_ptr = builder.CreateLoad(ctn_type_l->getStructElementType(0), data_gep);
            auto vtable_ptr = builder.CreateLoad(ctn_type_l->getStructElementType(1), vtable_gep);
            ctn_ptr = data_ptr;
            // +3 offset for typeinfo + destructor + copier in vtable header
            auto index = llvm::ConstantInt::get(
                *m_ctx->llvm_ctx,
                llvm::APInt(32, dot_expr.resolved_struct_member->method_index + 3));
            auto fn_gep = builder.CreateGEP(
                llvm::PointerType::get(compile_type(get_system_types()->void_ptr), 0), vtable_ptr,
                {index});
            auto callee_ptr =
                builder.CreateLoad(compile_type(get_system_types()->void_ptr), fn_gep);
            // Build function type with thin pointer for 'this' (concrete method signature)
            auto orig_fn_type = (llvm::FunctionType *)compile_type_of(fn_decl);
            std::vector<llvm::Type *> param_types;
            for (unsigned i = 0; i < orig_fn_type->getNumParams(); i++) {
                auto param = orig_fn_type->getParamType(i);
                // For embedded interface methods, the fn_decl's 'this' param may be
                // a fat pointer for the original interface (e.g. FatIFacePointer<Greetable>)
                // rather than the composite (FatIFacePointer<Polite>). Match any fat
                // interface pointer type.
                bool is_fat_iface = (param == ctn_type_l);
                if (!is_fat_iface) {
                    if (auto *st = llvm::dyn_cast<llvm::StructType>(param))
                        is_fat_iface = st->hasName() && st->getName().starts_with("FatIFacePointer<");
                }
                if (is_fat_iface) {
                    param_types.push_back(get_llvm_ptr_type()); // thin pointer for 'this'
                } else {
                    param_types.push_back(param);
                }
            }
            auto dispatch_fn_type = llvm::FunctionType::get(orig_fn_type->getReturnType(),
                                                            param_types, orig_fn_type->isVarArg());
            callee = {dispatch_fn_type, callee_ptr};
        } else {
            ctn_ptr = ptr;
        }
        args.push_back(ctn_ptr);
    }
    if (!callee.getCallee()) {
        Function *callee_fn = nullptr;

        // Check if this is a call to a specialized generic function
        if (data.generated_fn) {
            callee_fn = get_fn(data.generated_fn);
        } else {
            callee_fn = get_fn(fn_decl, ctn_type);
        }

        callee = callee_fn->llvm_fn;
    }

    for (int i = 0; i < data.args.size(); i++) {
        if (is_variadic && !is_extern && i >= va_start) {
            continue;
        }
        if (is_variadic && is_extern && i >= va_start) {
            args.push_back(compile_extern_variadic_arg(fn, data.args[i]));
            continue;
        }
        auto arg = data.args[i];
        auto param_type = fn_spec.get_param_at(i);

        // For C variadic args (param_type is nullptr), compile the expression directly
        if (!param_type) {
            args.push_back(compile_expr(fn, arg));
            continue;
        }

        args.push_back(compile_arg_for_call(fn, arg, param_type, &arg_cleanup_block,
                                            &transferred_cleanup_vars));
    }
    // Compile default values for missing arguments
    if (fn_decl->type == ast::NodeType::FnDef) {
        auto &proto = fn_decl->data.fn_def.fn_proto->data.fn_proto;
        for (size_t i = data.args.size(); i < proto.params.size(); i++) {
            auto default_val = proto.params[i]->data.param_decl.effective_default_value();
            if (!default_val)
                break;
            auto param_type = fn_spec.get_param_at(i);
            args.push_back(compile_arg_for_call(fn, default_val, param_type, &arg_cleanup_block,
                                                &transferred_cleanup_vars));
        }
    }

    if (is_variadic && !is_extern) {
        args.push_back(
            compile_variadic_span_arg(fn, data.args, va_start, fn_spec.get_variadic_span_param(), expr));
    }

    emit_dbg_location(expr);
    auto return_type = fn_type->data.fn.return_type;
    auto sret_type = fn_type->data.fn.should_use_sret() ? compile_type(return_type) : nullptr;
    auto result = create_fn_call_invoke(callee, args, sret_type, invoke, sret_dest);
    if (invoke && invoke->used) {
        fn->use_label(invoke->normal);
        // create_fn_call_invoke returns the invoke instr for scalar+sret_dest so
        // we can store it after switching to the normal block (invoke is a terminator)
        if (result && sret_dest && !sret_type) {
            builder.CreateStore(result, sret_dest);
            result = nullptr;
        }
    }
    for (auto *var : transferred_cleanup_vars) {
        arg_cleanup_block.exit_flow.add_sink_edge(var, expr);
    }
    pop_cleanup_block(fn, arg_cleanup_block);

    return result;
}

llvm::Value *Compiler::create_fn_call_invoke(llvm::FunctionCallee callee,
                                             std::vector<llvm::Value *> args, llvm::Type *sret_type,
                                             InvokeInfo *invoke, llvm::Value *sret_dest) {
    auto &builder = *m_ctx->llvm_builder.get();
    auto &llvm_ctx = *m_ctx->llvm_ctx.get();
    llvm::Value *sret_var = nullptr;
    if (sret_type) {
        // Use provided destination directly, or create temporary
        sret_var = sret_dest ? sret_dest : builder.CreateAlloca(sret_type, nullptr, "sret");
        args.insert(args.begin(), sret_var);
    }

    llvm::Value *ret = nullptr;
    if (invoke) {
        if (!invoke->normal) {
            invoke->normal = llvm::BasicBlock::Create(llvm_ctx, "_invoke_next",
                                                      builder.GetInsertBlock()->getParent());
        }
        ret = builder.CreateInvoke(callee, invoke->normal, invoke->landing, args);
        invoke->used = true;
        if (sret_type) {
            invoke->sret = sret_var;
            invoke->sret_type = sret_type;
            return ret;
        }
    } else {
        ret = builder.CreateCall(callee, args);
    }

    // If sret with destination, function wrote directly to dest - return nullptr
    if (sret_dest && sret_type) {
        return nullptr;
    }
    // For non-sret with destination, store the return value
    if (sret_dest && !sret_type) {
        if (!invoke) {
            builder.CreateStore(ret, sret_dest);
            return nullptr;
        }
        // With invoke: can't store here (invoke is a terminator).
        // Return the value so the caller can store after switching to normal label.
        return ret;
    }
    return sret_type ? builder.CreateLoad(sret_type, sret_var) : ret;
}

std::optional<TypeId> Compiler::resolve_variant_type_id(Function *fn, ChiType *type) {
    if (!type)
        return std::nullopt;

    // Unwrap pointer-like and optional types
    while (type && (type->kind == TypeKind::Pointer || type->kind == TypeKind::Reference ||
                    type->kind == TypeKind::MutRef ||
                    type->kind == TypeKind::MoveRef ||
                    type->kind == TypeKind::Optional)) {
        type = type->get_elem();
    }

    // Substitute placeholders using current function's context
    if (type && type->is_placeholder && fn && fn->container_subtype) {
        if (fn->fn_type && fn->fn_type->data.fn.container_ref) {
            auto container_ref = fn->fn_type->data.fn.container_ref;
            auto container_struct = get_resolver()->resolve_struct_type(container_ref);
            if (container_struct) {
                type = get_resolver()->type_placeholders_sub_selective(type, fn->container_subtype,
                                                                       container_struct->node);
            }
        }
    }

    // Also handle specialized_subtype for generic function parameters (e.g., Promise<T> in
    // promise<T>)
    if (type && type->is_placeholder && fn && fn->specialized_subtype &&
        fn->specialized_subtype->kind == TypeKind::Subtype) {
        auto &subtype_data = fn->specialized_subtype->data.subtype;
        if (subtype_data.root_node) {
            type = get_resolver()->type_placeholders_sub_selective(type, &subtype_data,
                                                                   subtype_data.root_node);
        }
    }

    // Only return ID if it's a fully resolved Subtype
    if (type && type->kind == TypeKind::Subtype && !type->is_placeholder) {
        // Ensure variants are registered before returning the id for lookup.
        if (!type->data.subtype.final_type) {
            get_resolver()->resolve_subtype(type);
        }
        return type->id;
    }
    return std::nullopt;
}

ast::Node *Compiler::get_variant_member_node(ChiStructMember *member,
                                             std::optional<TypeId> variant_type_id) {
    if (!member)
        return nullptr;

    ast::Node *node = member->node;
    if (variant_type_id.has_value()) {
        auto base_member = member->root_variant ? member->root_variant : member;
        auto variant_member = base_member->variants.get(*variant_type_id);
        if (variant_member) {
            node = (*variant_member)->node;
        }
    }
    return node;
}

void Compiler::generate_embed_proxy(Function *proxy_fn, Function *orig_fn,
                                    ChiStructMember *member, ChiType *struct_type) {
    auto &builder = *m_ctx->llvm_builder.get();
    auto saved_insert_point = builder.GetInsertBlock();

    auto entry_bb = llvm::BasicBlock::Create(*m_ctx->llvm_ctx, "entry", proxy_fn->llvm_fn);
    builder.SetInsertPoint(entry_bb);

    // GEP 'this' to the embedded field
    auto embedded_ptr = compile_dot_access(proxy_fn, proxy_fn->bind_ptr, struct_type, member->parent_member);

    // Forward all args, replacing 'this' with the embedded field pointer
    std::vector<llvm::Value *> args;
    for (unsigned i = 0; i < proxy_fn->llvm_fn->arg_size(); i++) {
        auto arg = proxy_fn->llvm_fn->getArg(i);
        if (arg == proxy_fn->bind_ptr) {
            args.push_back(embedded_ptr);
        } else {
            args.push_back(arg);
        }
    }

    auto call = builder.CreateCall(orig_fn->llvm_fn, args);
    if (proxy_fn->llvm_fn->getReturnType()->isVoidTy()) {
        builder.CreateRetVoid();
    } else {
        builder.CreateRet(call);
    }

    if (saved_insert_point) {
        builder.SetInsertPoint(saved_insert_point);
    }
}

llvm::Value *Compiler::generate_lambda_proxy_function(Function *fn, llvm::Value *original_fn_ptr,
                                                      ChiType *lambda_type, NodeList *captures) {
    auto &builder = *m_ctx->llvm_builder.get();

    // Save current insert point
    auto saved_insert_point = builder.GetInsertBlock();

    // Get the original function type
    auto original_fn_type = lambda_type->data.fn_lambda.fn;
    auto &original_fn_spec = original_fn_type->data.fn;

    // Create function type for the proxy: (bind_struct*, user_args...) -> return_type
    auto bound_fn_type = lambda_type->data.fn_lambda.bound_fn;
    auto proxy_fn_type = (llvm::FunctionType *)compile_type(bound_fn_type);

    // Create the proxy function
    auto proxy_name = fmt::format("__lambda_proxy_{}", lambda_type->id);
    auto proxy_llvm_fn = llvm::Function::Create(proxy_fn_type, llvm::Function::InternalLinkage,
                                                proxy_name, m_ctx->llvm_module.get());

    // Create function body
    auto entry_bb = llvm::BasicBlock::Create(*m_ctx->llvm_ctx, "entry", proxy_llvm_fn);
    builder.SetInsertPoint(entry_bb);

    // Prepare arguments for the original function call
    // We need to determine if the original function takes a _binds parameter:
    // - Lambda functions (including no-capture): YES, they take _binds as first parameter
    // - Regular functions converted to lambda: NO, they don't take _binds
    //
    // Check by comparing parameter counts:
    // - bound_fn_type has (_binds, user_args...)
    // - original_fn_type has (user_args...)
    // - If the original LLVM function has more params than original_fn_type, it's a lambda with
    // _binds

    std::vector<llvm::Value *> original_args;
    auto proxy_arg_count = proxy_llvm_fn->arg_size();

    // Get the LLVM function from the original_fn_ptr
    auto original_llvm_fn = llvm::dyn_cast<llvm::Function>(original_fn_ptr);
    auto original_fn_type_l = (llvm::FunctionType *)compile_type(original_fn_type);

    // Determine if we should pass the _binds parameter
    // If the original function has more parameters than the original type spec, it has _binds
    bool original_has_binds =
        original_llvm_fn && (original_llvm_fn->arg_size() > original_fn_type_l->getNumParams());

    // Check if the function uses sret (struct return)
    bool use_sret = bound_fn_type->data.fn.should_use_sret();

    // Start index: skip sret arg (if present), then skip _binds if original doesn't take it
    unsigned start_idx = use_sret ? 1 : 0; // Skip sret if present
    if (!original_has_binds) {
        start_idx++; // Skip _binds
    }

    for (unsigned i = start_idx; i < proxy_arg_count; ++i) {
        auto arg = proxy_llvm_fn->getArg(i);
        original_args.push_back(arg);
    }

    // Call the original function with the appropriate signature
    llvm::FunctionType *call_fn_type_l;
    if (original_has_binds) {
        // Lambda function - use bound_fn_type which includes _binds
        call_fn_type_l = (llvm::FunctionType *)compile_type(bound_fn_type);
    } else {
        // Regular function - use original_fn_type without _binds
        call_fn_type_l = original_fn_type_l;
    }

    // For sret, insert the sret pointer at the beginning of args
    if (use_sret) {
        auto sret_arg = proxy_llvm_fn->getArg(0); // First arg is the sret pointer
        original_args.insert(original_args.begin(), sret_arg);
    }

    llvm::FunctionCallee original_callee(call_fn_type_l, original_fn_ptr);
    auto result = builder.CreateCall(original_callee, original_args);

    // Return the result (for sret, return void since result is written to sret pointer)
    if (original_fn_spec.return_type->kind == TypeKind::Void || use_sret) {
        builder.CreateRetVoid();
    } else {
        builder.CreateRet(result);
    }

    // Restore insert point
    if (saved_insert_point) {
        builder.SetInsertPoint(saved_insert_point);
    }

    return proxy_llvm_fn;
}

llvm::Value *Compiler::compile_void_to_unit_lambda_wrapper(Function *fn, llvm::Value *lambda_value,
                                                           ChiType *from_type, ChiType *to_type,
                                                           bool owns_value) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto ptr_type = llvm::PointerType::get(llvm_ctx, 0);
    auto unit_type_l = compile_type(get_resolver()->get_system_types()->unit);
    auto rt_lambda = get_resolver()->get_context()->rt_lambda_type;
    auto lambda_type_l = compile_type(rt_lambda);

    // --- Generate wrapper function: Unit(ptr data, user_args...) ---
    // data points to the captured original __CxLambda
    auto saved_bb = builder.GetInsertBlock();

    auto &from_fn_spec = from_type->data.fn_lambda.fn->data.fn;
    std::vector<llvm::Type *> wrapper_params = {ptr_type};
    for (size_t i = 0; i < from_fn_spec.params.size(); i++)
        wrapper_params.push_back(compile_type(from_fn_spec.params[i]));

    static int counter = 0;
    auto wrapper_fn = llvm::Function::Create(
        llvm::FunctionType::get(unit_type_l, wrapper_params, false),
        llvm::Function::InternalLinkage,
        fmt::format("__void_to_unit_wrapper_{}", counter++), m_ctx->llvm_module.get());

    builder.SetInsertPoint(llvm::BasicBlock::Create(llvm_ctx, "entry", wrapper_fn));

    // data arg points to captured __CxLambda: {ptr fn_ptr, i32 length, ptr captures}
    auto data_arg = wrapper_fn->getArg(0);
    auto inner_fn = builder.CreateLoad(ptr_type, builder.CreateStructGEP(lambda_type_l, data_arg, 0));
    auto inner_captures = builder.CreateLoad(ptr_type, builder.CreateStructGEP(lambda_type_l, data_arg, 2));
    auto inner_data = builder.CreateCall(get_system_fn("cx_capture_get_data")->llvm_fn, {inner_captures});

    std::vector<llvm::Value *> fwd_args = {inner_data};
    for (unsigned i = 1; i < wrapper_fn->arg_size(); i++)
        fwd_args.push_back(wrapper_fn->getArg(i));

    auto orig_bound_type = (llvm::FunctionType *)compile_type(from_type->data.fn_lambda.bound_fn);
    builder.CreateCall(llvm::FunctionCallee(orig_bound_type, inner_fn), fwd_args);
    builder.CreateRet(llvm::Constant::getNullValue(unit_type_l));

    if (saved_bb) builder.SetInsertPoint(saved_bb);

    // --- Build new __CxLambda capturing the original lambda ---
    auto lambda_size = llvm_type_size(lambda_type_l);
    auto [new_lambda, new_lambda_type] = compile_cxlambda_init(fn, wrapper_fn, lambda_size);

    // Destructor to release the captured lambda's inner captures
    auto dtor = generate_destructor(rt_lambda);
    auto dtor_ptr = dtor ? builder.CreateBitCast(dtor->llvm_fn, builder.getInt8PtrTy())
                         : llvm::ConstantPointerNull::get(builder.getInt8PtrTy());
    auto null_ptr = llvm::ConstantPointerNull::get(builder.getInt8PtrTy());
    auto [cap_ptr, payload_ptr] = compile_cxcapture_create(lambda_size, null_ptr, dtor_ptr);

    // Store the original lambda into the payload. If we own the source value
    // ownership transfers into the payload; otherwise retain the inner
    // captures so the payload holds its own reference.
    auto typed_payload = builder.CreateBitCast(payload_ptr, lambda_type_l->getPointerTo());
    builder.CreateStore(lambda_value, typed_payload);

    if (!owns_value) {
        auto inner_cap_ptr = builder.CreateLoad(
            ptr_type, builder.CreateStructGEP(lambda_type_l, typed_payload, 2));
        auto has_captures = builder.CreateICmpNE(
            inner_cap_ptr, llvm::ConstantPointerNull::get(llvm::PointerType::get(llvm_ctx, 0)));
        auto retain_bb = llvm::BasicBlock::Create(llvm_ctx, "retain", fn->llvm_fn);
        auto cont_bb = llvm::BasicBlock::Create(llvm_ctx, "cont", fn->llvm_fn);
        builder.CreateCondBr(has_captures, retain_bb, cont_bb);

        builder.SetInsertPoint(retain_bb);
        builder.CreateCall(get_system_fn("cx_capture_retain")->llvm_fn, {inner_cap_ptr});
        builder.CreateBr(cont_bb);

        builder.SetInsertPoint(cont_bb);
    }
    compile_cxlambda_set_captures(new_lambda, cap_ptr);

    return builder.CreateLoad(new_lambda_type, new_lambda);
}

llvm::Value *Compiler::compile_alloc(Function *fn, ast::Node *decl, bool is_new, ChiType *type) {
    auto &llvm_builder = *m_ctx->llvm_builder.get();
    auto &llvm_ctx = *m_ctx->llvm_ctx.get();
    auto &llvm_module = *m_ctx->llvm_module.get();
    auto var_type_l = type ? compile_type(type) : compile_type_of(decl);

    // Debug: Show what type compile_alloc is using
    auto chi_type = get_chitype(decl);
    assert(!chi_type->is_placeholder && "compile_alloc called on placeholder type");

    Function *alloc_fn = nullptr;
    if (is_fn_managed(fn)) {
        if (is_new || decl->is_heap_allocated()) {
            alloc_fn = get_system_fn("cx_gc_alloc");
        }
    } else if (is_new) {
        alloc_fn = get_system_fn("cx_malloc");
    }

    if (alloc_fn) {
        auto ptr_type_l = llvm::PointerType::get(llvm::Type::getInt8Ty(llvm_ctx), 0);
        auto size = llvm_type_size(var_type_l);

        // In managed mode with cx_gc_alloc, provide destructor if type needs destruction
        llvm::Value *dtor_ptr = llvm::ConstantPointerNull::get(
            llvm::PointerType::get(llvm::Type::getInt8Ty(llvm_ctx), 0));

        if (is_fn_managed(fn) && alloc_fn->qualified_name == "cx_gc_alloc") {
            auto alloc_type = type ? type : chi_type;
            if (get_resolver()->type_needs_destruction(alloc_type)) {
                auto dtor = generate_destructor(alloc_type, nullptr);
                if (dtor) {
                    dtor_ptr = dtor->llvm_fn;
                }
            }
        }

        std::vector<llvm::Value *> args = {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), size),
            dtor_ptr,
        };
        auto result = llvm_builder.CreateCall(alloc_fn->llvm_fn, args);
        return result;
    }
    return fn->entry_alloca(var_type_l, decl->name);
}

void Compiler::compile_stmt(Function *fn, ast::Node *stmt) {
    emit_dbg_location(stmt);
    auto scope = fn->get_scope();
    if (scope->branched) {
        return;
    }
    if (!stmt->analysis.is_enabled) {
        return;
    }

    switch (stmt->type) {
    case ast::NodeType::EmptyStmt:
        break;
    case ast::NodeType::DestructureDecl: {
        auto &data = stmt->data.destructure_decl;
        auto &builder = *m_ctx->llvm_builder.get();
        auto *expr_node = data.effective_expr();

        // Evaluate RHS and store in temp
        auto source_type = get_chitype(expr_node);
        auto temp_ptr = compile_alloc(fn, data.temp_var);
        add_var(data.temp_var, temp_ptr);
        llvm::Value *borrow_source_ptr = nullptr;
        if (get_resolver()->find_root_decl(expr_node)) {
            auto source_ref = compile_expr_ref(fn, expr_node);
            if (source_ref.address) {
                borrow_source_ptr = source_ref.address;
                compile_copy_with_ref(fn, source_ref, temp_ptr, source_type, expr_node);
            }
        }
        if (!borrow_source_ptr) {
            auto rhs_value = compile_assignment_to_type(fn, expr_node, source_type);
            if (rhs_value) {
                compile_store_or_copy(fn, rhs_value, temp_ptr, source_type, expr_node);
            }
            borrow_source_ptr = temp_ptr;
        }

        // Extract elements
        compile_destructure(fn, data, temp_ptr, source_type, borrow_source_ptr);
        break;
    }
    case ast::NodeType::VarDecl: {
        auto &data = stmt->data.var_decl;
        auto &llvm_builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();
        auto &llvm_module = *m_ctx->llvm_module.get();

        // Narrowed variable: alias into original value
        if (data.narrowed_from) {
            auto ref = compile_expr_ref(fn, data.narrowed_from);
            auto addr = ref.address ? ref.address : ref.value;
            if (addr) {
                auto narrowed_type = get_chitype(stmt);
                if (narrowed_type->kind == TypeKind::EnumValue) {
                    // Enum variant narrowing: same address, more specific type
                    add_var(stmt, addr);
                } else {
                    // Check for interface → concrete narrowing (type switch)
                    auto from_type = get_chitype(data.narrowed_from);
                    auto from_elem = from_type->is_pointer_like() ? from_type->get_elem() : nullptr;
                    if (from_elem && ChiTypeStruct::is_interface(from_elem)) {
                        // Extract data_ptr (field 0) from the fat pointer
                        auto ptr_type = get_llvm_ptr_type();
                        auto iface_type_l =
                            llvm::StructType::get(*m_ctx->llvm_ctx, {ptr_type, ptr_type});
                        auto fp = llvm_builder.CreateLoad(iface_type_l, addr, "narrow_fp");
                        auto data_ptr = llvm_builder.CreateExtractValue(fp, {0}, "data_ptr");
                        // Store in alloca so from_address() works correctly
                        auto alloca = llvm_builder.CreateAlloca(ptr_type, nullptr, "narrow_ref");
                        llvm_builder.CreateStore(data_ptr, alloca);
                        add_var(stmt, alloca);
                    } else {
                        // Optional/Result narrowing: GEP to value field
                        auto original_type_l = compile_type(from_type);
                        auto value_ptr = llvm_builder.CreateStructGEP(original_type_l, addr, 1);
                        add_var(stmt, value_ptr);
                    }
                }
                break;
            }
        }

        auto var = compile_alloc(fn, stmt);
        add_var(stmt, var);
        auto var_type = get_chitype(stmt);

        // Allocate drop flag for maybe-moved variables
        if (fn->get_def() && fn->get_def()->flow.is_maybe_sunk(stmt)) {
            alloc_drop_flag(fn, stmt, false);
            set_drop_flag_alive(stmt, true);
        } else if (data.expr && fn->get_def() && fn->get_def()->has_cleanup &&
                   get_resolver()->type_needs_destruction(var_type)) {
            // Init expression may throw (function has cleanup landing). Allocate a
            // drop flag initialized to dead; flip to alive after init completes so
            // an unwind during init doesn't destroy the uninitialized slot (e.g.
            // sret destination of a throwing callee).
            alloc_drop_flag(fn, stmt, false);
        } else if (data.is_generated && !data.expr && fn->get_def() &&
                   fn->get_def()->has_cleanup &&
                   get_resolver()->type_needs_destruction(var_type)) {
            // Outlet temp (stmt_temp_var) filled later at its consuming call site.
            // Same unwind hazard as above — guard with a drop flag set alive only
            // after the populating call completes (see mark_outlet_alive).
            alloc_drop_flag(fn, stmt, false);
        }

        if (data.expr) {
            if (data.expr->type == ast::NodeType::FnCallExpr) {
                auto &fn_call_data = data.expr->data.fn_call_expr;
                auto expr_type = get_chitype(data.expr);
                // Only use direct sret for regular function calls, not lambdas or optional chains
                bool is_lambda =
                    fn_call_data.fn_ref_expr->resolved_type->kind == TypeKind::FnLambda;
                bool is_optional_chain = fn_call_data.fn_ref_expr->type == ast::NodeType::DotExpr &&
                                         fn_call_data.fn_ref_expr->data.dot_expr.is_optional_chain;
                if (!is_lambda && !is_optional_chain && expr_type == var_type) {
                    // Pass var directly as sret destination - avoids intermediate copy
                    if (fn->get_def()->has_cleanup || fn->async_reject_promise_ptr) {
                        compile_fn_call_with_invoke(fn, data.expr, var);
                    } else {
                        compile_fn_call(fn, data.expr, nullptr, var);
                    }
                } else {
                    compile_assignment_to_ptr(fn, data.expr, var, var_type);
                }
            } else {
                compile_assignment_to_ptr(fn, data.expr, var, var_type);
            }
        }
        // If we allocated a drop flag purely to guard against unwind during init,
        // mark the slot alive now that init completed successfully.
        if (data.expr && m_ctx->drop_flags.has_key(stmt) &&
            !(fn->get_def() && fn->get_def()->flow.is_maybe_sunk(stmt))) {
            set_drop_flag_alive(stmt, true);
        }
        break;
    }
    case ast::NodeType::ReturnStmt: {
        auto &data = stmt->data.return_stmt;
        auto &llvm_builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();
        auto scope = fn->get_scope();

        {
            bool continuation_async_return =
                fn->async_reject_promise_ptr && !fn->return_label && fn->async_promise_type;

            if (continuation_async_return) {
                auto return_type = fn->async_promise_type;
                auto promise_struct = get_resolver()->resolve_struct_type(return_type);
                std::optional<TypeId> variant_type_id = std::nullopt;
                if (return_type->kind == TypeKind::Subtype && !return_type->is_placeholder) {
                    variant_type_id = return_type->id;
                }

                auto inner_type = get_resolver()->get_promise_value_type(return_type);
                llvm::Value *ret_value;
                if (data.expr) {
                    ret_value = compile_direct_call_arg(fn, data.expr, inner_type);
                } else {
                    ret_value = llvm::Constant::getNullValue(compile_type(inner_type));
                }

                auto resolve_member = promise_struct->find_member("resolve");
                assert(resolve_member && "Promise.resolve() method not found");
                auto resolve_method_node = get_variant_member_node(resolve_member, variant_type_id);
                auto resolve_method = get_fn(resolve_method_node);
                llvm_builder.CreateCall(resolve_method->llvm_fn, {fn->async_reject_promise_ptr, ret_value});
            } else {
                assert(fn->return_label);

                // Check if this is an async function returning T (wrapped to Promise<T>)
                bool is_async = fn->node && fn->node->type == ast::NodeType::FnDef &&
                                fn->node->data.fn_def.is_async();
                auto return_type = fn->fn_type->data.fn.return_type;

                if (is_async && get_resolver()->is_promise_type(return_type)) {
                    assert(fn->async_reject_promise_ptr &&
                           "async fast path must pre-initialize result promise");
                    auto promise_struct = get_resolver()->resolve_struct_type(return_type);

                    std::optional<TypeId> variant_type_id = std::nullopt;
                    if (return_type->kind == TypeKind::Subtype && !return_type->is_placeholder) {
                        variant_type_id = return_type->id;
                    }

                    // Compile return value, or synthesize Unit{} for bare `return;`
                    auto inner_type = get_resolver()->get_promise_value_type(return_type);
                    llvm::Value *ret_value;
                    if (data.expr) {
                        ret_value = compile_direct_call_arg(fn, data.expr, inner_type);
                    } else {
                        ret_value = llvm::Constant::getNullValue(compile_type(inner_type));
                    }

                    // Resolve the pre-initialized result promise, then bitwise-move it to sret.
                    auto resolve_member = promise_struct->find_member("resolve");
                    assert(resolve_member && "Promise.resolve() method not found");
                    auto resolve_method_node = get_variant_member_node(resolve_member, variant_type_id);
                    auto resolve_method = get_fn(resolve_method_node);
                    llvm_builder.CreateCall(resolve_method->llvm_fn,
                                            {fn->async_reject_promise_ptr, ret_value});
                    auto promise_type_l = compile_type(return_type);
                    auto resolved_promise =
                        llvm_builder.CreateLoad(promise_type_l, fn->async_reject_promise_ptr);
                    llvm_builder.CreateStore(resolved_promise, fn->return_value);
                } else if (data.expr) {
                    auto ret_type = get_chitype(stmt);
                    compile_assignment_to_ptr(fn, data.expr, fn->return_value, ret_type);
                }
            }
        }
        // If return expression was a moved local, skip its destruction
        ast::Node *move_returned_var = nullptr;
        if (data.expr && data.expr->analysis.moved && data.expr->type == ast::NodeType::Identifier) {
            move_returned_var = data.expr->data.identifier.decl;
        }
        // Destroy all active block-local vars (inner to outer) before returning
        // Use innermost block's exit_flow — it has the correct flow state at this return point
        auto &return_flow = fn->active_blocks.back()->exit_flow;
        for (int i = fn->active_blocks.size() - 1; i >= 0; i--) {
            compile_block_cleanup(fn, fn->active_blocks[i], move_returned_var, return_flow);
        }
        if (fn->return_label) {
            llvm_builder.CreateBr(fn->return_label);
        } else {
            emit_cleanup_owners(fn);
            llvm_builder.CreateRetVoid();
        }
        scope->branched = true;
        break;
    }
    case ast::NodeType::ThrowStmt: {
        auto &data = stmt->data.throw_stmt;
        auto &llvm_builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();

        // Compile the error expression — yields &ErrorStruct (a reference)
        auto error_ref = compile_expr(fn, data.expr);
        auto expr_type = get_chitype(data.expr);
        auto elem_type = expr_type->get_elem(); // the concrete struct type

        // Convert &ErrorStruct to Error interface to get vtable and type_info
        auto rt_error = get_resolver()->get_context()->rt_error_type;
        auto impl = elem_type->data.struct_.interface_table[rt_error];
        assert(impl);
        auto vtable = m_ctx->impl_table[impl];
        assert(vtable);
        auto type_info = compile_type_info(elem_type);

        if (fn->try_block_landing) {
            // Inside a try-block: invoke cx_throw so the landing pad catches it
            auto throw_fn = get_system_fn("cx_throw");
            auto type_id =
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), elem_type->id);
            auto unreachable_b = fn->new_label("_throw_unreachable");
            llvm_builder.CreateInvoke(throw_fn->llvm_fn, unreachable_b, fn->try_block_landing,
                                      {type_info, error_ref, vtable, type_id});
            fn->use_label(unreachable_b);
            llvm_builder.CreateUnreachable();
        } else if (fn->async_reject_promise_ptr) {
            // In async context: convert throw to promise.reject() instead of cx_throw
            emit_async_promise_reject(fn, error_ref, vtable);
            llvm_builder.CreateCall(get_system_fn("cx_clear_panic_location")->llvm_fn, {});

            if (fn->return_label) {
                // Parent async function: store rejected promise and branch to return
                auto promise_type_l = compile_type(fn->async_promise_type);
                auto rejected_promise =
                    llvm_builder.CreateLoad(promise_type_l, fn->async_reject_promise_ptr);
                llvm_builder.CreateStore(rejected_promise, fn->return_value);
                llvm_builder.CreateBr(fn->return_label);
            } else {
                // Async continuation: just return void
                emit_cleanup_owners(fn);
                llvm_builder.CreateRetVoid();
            }
        } else {
            // Normal context: run cleanup for live locals/owners before cx_throw so the
            // throwing frame's destructors fire even though the Itanium unwinder will skip
            // this frame (it has no landingpad at the call site).
            auto &throw_flow = fn->active_blocks.back()->exit_flow;
            for (int i = fn->active_blocks.size() - 1; i >= 0; i--) {
                compile_block_cleanup(fn, fn->active_blocks[i], nullptr, throw_flow);
            }
            emit_cleanup_owners(fn);
            auto throw_fn = get_system_fn("cx_throw");
            auto type_id =
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), elem_type->id);
            llvm_builder.CreateCall(throw_fn->llvm_fn, {type_info, error_ref, vtable, type_id});
            llvm_builder.CreateUnreachable();
        }
        scope->branched = true;
        break;
    }
    case ast::NodeType::BranchStmt: {
        auto token = stmt->token;
        auto loop = fn->get_loop();
        auto &builder = *m_ctx->llvm_builder.get();
        // Destroy block-local vars between current scope and the loop boundary
        auto &break_flow = fn->active_blocks.back()->exit_flow;
        for (int i = fn->active_blocks.size() - 1; i >= (int)loop->active_blocks_depth; i--) {
            compile_block_cleanup(fn, fn->active_blocks[i], nullptr, break_flow);
        }
        if (token->type == TokenType::KW_BREAK) {
            builder.CreateBr(loop->end);
        }
        if (token->type == TokenType::KW_CONTINUE) {
            builder.CreateBr(loop->continue_target ? loop->continue_target : loop->start);
        }
        break;
    }
    case ast::NodeType::IfExpr: {
        compile_expr(fn, stmt);
        break;
    }
    case ast::NodeType::ForStmt: {
        auto &builder = *m_ctx->llvm_builder.get();
        auto &data = stmt->data.for_stmt;
        auto kind = data.effective_kind();
        if (kind == ast::ForLoopKind::IntRange) {
            auto &range = data.expr->data.range_expr;
            auto start_val = compile_expr(fn, range.start);
            auto end_val = compile_expr(fn, range.end);
            auto iter_type = start_val->getType();

            auto it = builder.CreateAlloca(iter_type, nullptr, "_range_iter");
            builder.CreateStore(start_val, it);

            if (data.bind) {
                add_var(data.bind, it);
            }

            auto loop = fn->push_loop();
            loop->start = fn->new_label("_for_start");
            loop->end = fn->new_label("_for_end");
            auto loop_main = fn->new_label("_for_main");
            builder.CreateBr(loop->start);

            fn->use_label(loop->start);
            auto cond = builder.CreateICmpSLT(builder.CreateLoad(iter_type, it), end_val);
            builder.CreateCondBr(cond, loop_main, loop->end);

            fn->use_label(loop_main);
            auto loop_post = fn->new_label("_for_post");
            loop->continue_target = loop_post;
            compile_block(fn, stmt, data.body, loop_post);

            fn->use_label(loop_post);
            auto cur = builder.CreateLoad(iter_type, it);
            builder.CreateStore(builder.CreateAdd(cur, llvm::ConstantInt::get(iter_type, 1)), it);
            builder.CreateBr(loop->start);

            fn->use_label(loop->end);
            fn->pop_loop();

        } else if (data.expr && get_chitype(data.expr) && ({
                       auto t = get_chitype(data.expr);
                       (t->kind == TypeKind::FixedArray) ||
                           (t->is_reference() && t->get_elem()->kind == TypeKind::FixedArray);
                   })) {
            auto expr_type = get_chitype(data.expr);
            auto arr_type = expr_type->is_reference() ? expr_type->get_elem() : expr_type;
            auto elem_type = arr_type->data.fixed_array.elem;
            auto fa_size = arr_type->data.fixed_array.size;
            auto arr_type_l = compile_type(arr_type);
            auto elem_type_l = compile_type(elem_type);
            auto arr_ref = compile_expr_ref(fn, data.expr);
            // If expr is a reference, load through it to get the array address
            auto arr_addr =
                expr_type->is_reference()
                    ? builder.CreateLoad(compile_type(expr_type), arr_ref.address, "_fa_deref")
                    : arr_ref.address;
            auto i32_ty = llvm::Type::getInt32Ty(m_ctx->llvm_module->getContext());

            // Allocate counter (entry block for domination in cleanup paths)
            auto it = fn->entry_alloca(i32_ty, "_fa_iter");
            builder.CreateStore(llvm::ConstantInt::get(i32_ty, 0), it);

            llvm::Value *item_var = nullptr;
            if (data.bind) {
                item_var =
                    fn->entry_alloca(compile_type(data.bind->resolved_type), "_bind_item_var");
                add_var(data.bind, item_var);
                // Allocate drop flag for maybe-moved bind variables
                if (fn->get_def() && fn->get_def()->flow.is_maybe_sunk(data.bind)) {
                    alloc_drop_flag(fn, data.bind, false);
                }
            }
            llvm::Value *index_var = nullptr;
            if (data.index_bind) {
                index_var = fn->entry_alloca(i32_ty, "_bind_index_var");
                add_var(data.index_bind, index_var);
            }

            auto loop = fn->push_loop();
            loop->start = fn->new_label("_for_start");
            loop->end = fn->new_label("_for_end");
            auto loop_main = fn->new_label("_for_main");
            builder.CreateBr(loop->start);

            fn->use_label(loop->start);
            auto size_val = llvm::ConstantInt::get(i32_ty, fa_size);
            auto cur_idx = builder.CreateLoad(i32_ty, it);
            auto cond = builder.CreateICmpULT(cur_idx, size_val);
            builder.CreateCondBr(cond, loop_main, loop->end);

            fn->use_label(loop_main);
            auto loop_post = fn->new_label("_for_post");
            loop->continue_target = loop_post;

            if (index_var) {
                builder.CreateStore(builder.CreateLoad(i32_ty, it), index_var);
            }
            if (item_var) {
                auto idx = builder.CreateLoad(i32_ty, it);
                auto zero = llvm::ConstantInt::get(i32_ty, 0);
                auto elem_ptr = builder.CreateGEP(arr_type_l, arr_addr, {zero, idx});
                if (data.bind_sigil != ast::SigilKind::None) {
                    // Reference bind: store pointer to element
                    builder.CreateStore(elem_ptr, item_var);
                } else {
                    // Value bind: copy element
                    auto value = builder.CreateLoad(elem_type_l, elem_ptr, "_item_value");
                    compile_copy_with_ref(fn, RefValue{elem_ptr, value}, item_var,
                                          get_chitype(data.bind));
                }
                // Mark bind alive after value write (for maybe-moved drop flag)
                set_drop_flag_alive(data.bind, true);
            }
            compile_block(fn, stmt, data.body, loop_post);

            fn->use_label(loop_post);
            auto cur = builder.CreateLoad(i32_ty, it);
            builder.CreateStore(builder.CreateAdd(cur, llvm::ConstantInt::get(i32_ty, 1)), it);
            builder.CreateBr(loop->start);

            fn->use_label(loop->end);
            fn->pop_loop();

        } else if (kind == ast::ForLoopKind::Range) {
            auto ptr = compile_dot_ptr(fn, data.expr);
            assert(ptr);
            auto sty = get_resolver()->resolve_struct_type(get_chitype(data.expr));
            auto beginp = sty->member_table.get("begin");
            auto endp = sty->member_table.get("end");
            auto nextp = sty->member_table.get("next");
            auto indexp = sty->member_table.get("index_mut");
            assert(beginp && endp && nextp && indexp);
            auto begin = *beginp;
            auto end = *endp;
            auto next = *nextp;
            auto index = *indexp;
            llvm::Value *item_var = nullptr;
            if (data.bind) {
                item_var =
                    fn->entry_alloca(compile_type(data.bind->resolved_type), "_bind_item_var");
                add_var(data.bind, item_var);
                // Allocate drop flag for maybe-moved bind variables
                if (fn->get_def() && fn->get_def()->flow.is_maybe_sunk(data.bind)) {
                    alloc_drop_flag(fn, data.bind, false);
                }
            }
            llvm::Value *index_var = nullptr;
            if (data.index_bind) {
                auto idx_type = llvm::Type::getInt32Ty(m_ctx->llvm_module->getContext());
                index_var = fn->entry_alloca(idx_type, "_bind_index_var");
                add_var(data.index_bind, index_var);
            }

            auto loop = fn->push_loop();
            auto iter_begin =
                builder.CreateCall(get_fn(begin->node)->llvm_fn, {ptr}, "_iter_begin");
            auto iter_end = builder.CreateCall(get_fn(end->node)->llvm_fn, {ptr}, "_iter_end");
            auto iter_type_l = iter_begin->getType();
            auto it = builder.CreateAlloca(iter_type_l, nullptr, "_iter_alloc");
            builder.CreateStore(iter_begin, it);

            loop->start = fn->new_label("_for_start");
            loop->end = fn->new_label("_for_end");
            auto loop_main = fn->new_label("_for_main");
            builder.CreateBr(loop->start);

            fn->use_label(loop->start);
            auto cond = builder.CreateICmpSLT(builder.CreateLoad(iter_type_l, it), iter_end);
            builder.CreateCondBr(cond, loop_main, loop->end);

            fn->use_label(loop_main);
            auto loop_post = fn->new_label("_for_post");
            loop->continue_target = loop_post;
            if (index_var) {
                auto iter_value = builder.CreateLoad(iter_type_l, it);
                // Convert iterator value to uint32 if needed
                auto idx_type = llvm::Type::getInt32Ty(m_ctx->llvm_module->getContext());
                auto idx_val = iter_value->getType() == idx_type
                                   ? iter_value
                                   : builder.CreateIntCast(iter_value, idx_type, false);
                builder.CreateStore(idx_val, index_var);
            }
            if (item_var) {
                auto item_ref =
                    builder.CreateCall(get_fn(index->node)->llvm_fn,
                                       {ptr, builder.CreateLoad(iter_type_l, it)}, "_iter_item");
                if (data.bind_sigil != ast::SigilKind::None) {
                    builder.CreateStore(item_ref, item_var);
                } else {
                    auto value = builder.CreateLoad(compile_type(data.bind->resolved_type),
                                                    item_ref, "_item_value");
                    compile_copy_with_ref(fn, RefValue{item_ref, value}, item_var,
                                          get_chitype(data.bind));
                }
                // Mark bind alive after value write (for maybe-moved drop flag)
                set_drop_flag_alive(data.bind, true);
            }
            compile_block(fn, stmt, data.body, loop_post);

            fn->use_label(loop_post);
            auto iter_value = builder.CreateLoad(iter_type_l, it);
            auto iter_next =
                builder.CreateCall(get_fn(next->node)->llvm_fn, {ptr, iter_value}, "_iter_next");
            builder.CreateStore(iter_next, it);
            builder.CreateBr(loop->start);

            fn->use_label(loop->end);
            fn->pop_loop();

        } else if (kind == ast::ForLoopKind::Iter) {
            // Iterator-based loop: call to_iter_mut(), then loop calling next()
            auto container_ptr = compile_dot_ptr(fn, data.expr);
            assert(container_ptr);
            auto sty = get_resolver()->resolve_struct_type(get_chitype(data.expr));
            auto iter_fn = *sty->member_table.get("to_iter_mut");
            auto iter_fn_type = iter_fn->resolved_type;
            auto iter_ret_type = iter_fn_type->data.fn.return_type;
            auto iter_type_l = compile_type(iter_ret_type);

            // Allocate space for the iterator
            auto iter_alloca = builder.CreateAlloca(iter_type_l, nullptr, "_iter_alloca");

            // Call to_iter_mut() — handle sret if needed
            auto iter_llvm_fn = get_fn(iter_fn->node)->llvm_fn;
            if (iter_fn_type->data.fn.should_use_sret()) {
                builder.CreateCall(iter_llvm_fn, {iter_alloca, container_ptr});
            } else {
                auto iter_val = builder.CreateCall(iter_llvm_fn, {container_ptr}, "_iter_val");
                builder.CreateStore(iter_val, iter_alloca);
            }

            // Look up next() on the iterator struct
            auto iter_sty = get_resolver()->resolve_struct_type(iter_ret_type);
            auto next_fn = *iter_sty->member_table.get("next");
            auto next_fn_type = next_fn->resolved_type;
            auto next_ret_type = next_fn_type->data.fn.return_type;
            auto next_ret_type_l = compile_type(next_ret_type);
            auto next_llvm_fn = get_fn(next_fn->node)->llvm_fn;
            bool next_uses_sret = next_fn_type->data.fn.should_use_sret();

            auto &llvm_ctx = *m_ctx->llvm_ctx.get();
            llvm::Value *item_var = nullptr;
            if (data.bind) {
                item_var =
                    fn->entry_alloca(compile_type(data.bind->resolved_type), "_bind_item_var");
                add_var(data.bind, item_var);
                // Allocate drop flag for maybe-moved bind variables
                if (fn->get_def() && fn->get_def()->flow.is_maybe_sunk(data.bind)) {
                    alloc_drop_flag(fn, data.bind, false);
                }
            }
            auto idx_type = llvm::Type::getInt32Ty(m_ctx->llvm_module->getContext());
            llvm::Value *index_var = nullptr;
            if (data.index_bind) {
                index_var = fn->entry_alloca(idx_type, "_bind_index_var");
                builder.CreateStore(llvm::ConstantInt::get(idx_type, 0), index_var);
                add_var(data.index_bind, index_var);
            }

            auto loop = fn->push_loop();
            loop->start = fn->new_label("_for_start");
            loop->end = fn->new_label("_for_end");
            auto loop_main = fn->new_label("_for_main");
            builder.CreateBr(loop->start);

            fn->use_label(loop->start);
            // Call next(&iter) → ?&mut T
            auto opt_alloca = builder.CreateAlloca(next_ret_type_l, nullptr, "_opt_alloca");
            if (next_uses_sret) {
                builder.CreateCall(next_llvm_fn, {opt_alloca, iter_alloca});
            } else {
                auto opt_result = builder.CreateCall(next_llvm_fn, {iter_alloca}, "_opt_result");
                builder.CreateStore(opt_result, opt_alloca);
            }
            // Check has_value (field 0)
            auto has_value_p = builder.CreateStructGEP(next_ret_type_l, opt_alloca, 0);
            auto has_value = builder.CreateLoad(llvm::Type::getInt1Ty(llvm_ctx), has_value_p);
            builder.CreateCondBr(has_value, loop_main, loop->end);

            fn->use_label(loop_main);
            auto loop_post = fn->new_label("_for_post");
            loop->continue_target = loop_post;
            if (item_var) {
                // Extract value (field 1) — this is &mut T (a pointer)
                auto value_p = builder.CreateStructGEP(next_ret_type_l, opt_alloca, 1);
                auto value = builder.CreateLoad(compile_type(data.bind->resolved_type), value_p,
                                                "_iter_item");
                builder.CreateStore(value, item_var);
                // Mark bind alive after value write (for maybe-moved drop flag)
                set_drop_flag_alive(data.bind, true);
            }
            compile_block(fn, stmt, data.body, loop_post);

            fn->use_label(loop_post);
            if (index_var) {
                auto cur = builder.CreateLoad(idx_type, index_var);
                builder.CreateStore(builder.CreateAdd(cur, llvm::ConstantInt::get(idx_type, 1)),
                                    index_var);
            }
            builder.CreateBr(loop->start);

            fn->use_label(loop->end);
            fn->pop_loop();

        } else {
            auto loop = fn->push_loop();
            if (data.init) {
                compile_stmt(fn, data.init);
            }
            loop->start = fn->new_label("_for_start");
            loop->end = fn->new_label("_for_end");
            auto loop_main = fn->new_label("_for_main");
            builder.CreateBr(loop->start);

            fn->use_label(loop->start);
            if (data.condition) {
                auto cond =
                    compile_assignment_to_type(fn, data.condition, get_system_types()->bool_);
                builder.CreateCondBr(cond, loop_main, loop->end);
            } else {
                builder.CreateBr(loop_main);
            }

            fn->use_label(loop_main);
            auto loop_post = fn->new_label("_for_post");
            loop->continue_target = loop_post;
            compile_block(fn, stmt, data.body, loop_post);
            fn->use_label(loop_post);
            if (data.post) {
                compile_stmt(fn, data.post);
            }
            builder.CreateBr(loop->start);

            fn->use_label(loop->end);
            fn->pop_loop();
        }
        break;
    }
    case ast::NodeType::WhileStmt: {
        auto &builder = *m_ctx->llvm_builder.get();
        auto &data = stmt->data.while_stmt;
        auto loop = fn->push_loop();
        loop->start = fn->new_label("_while_start");
        loop->end = fn->new_label("_while_end");
        loop->continue_target = loop->start;
        auto loop_main = fn->new_label("_while_main");
        builder.CreateBr(loop->start);

        fn->use_label(loop->start);
        if (data.condition) {
            auto cond = compile_assignment_to_type(fn, data.condition, get_system_types()->bool_);
            builder.CreateCondBr(cond, loop_main, loop->end);
        } else {
            builder.CreateBr(loop_main);
        }

        fn->use_label(loop_main);
        auto loop_post = fn->new_label("_while_post");
        compile_block(fn, stmt, data.body, loop_post);
        fn->use_label(loop_post);

        builder.CreateBr(loop->start);

        fn->use_label(loop->end);
        fn->pop_loop();
        break;
    }
    case ast::NodeType::Block: {
        compile_block(fn, stmt, stmt, nullptr);
        break;
    }
    case ast::NodeType::FnCallExpr: {
        compile_assignment_to_type(fn, stmt, nullptr);
        for (auto var : stmt->data.fn_call_expr.post_narrow_vars) {
            compile_stmt(fn, var);
        }
        auto call_type = get_chitype(stmt);
        if (call_type && call_type->kind == TypeKind::Never) {
            auto &builder = *m_ctx->llvm_builder.get();
            builder.CreateUnreachable();
            scope->branched = true;
        }
        break;
    }
    default:
        compile_assignment_to_type(fn, stmt, nullptr);
    }
}

void Compiler::compile_block_cleanup(Function *fn, ast::Block *block, ast::Node *skip_var,
                                     ast::FlowState &flow) {
    auto &builder = *m_ctx->llvm_builder.get();
    auto &llvm_ctx = *m_ctx->llvm_ctx.get();

    // Look up how far into `block`'s statement list we are at this cleanup point.
    // Stmt-scoped temps whose owning statement hasn't been reached yet are skipped —
    // their alloca is live but unfilled, so an early return from an earlier stmt must
    // not destroy them.
    int current_stmt_idx = INT_MAX; // normal block exit: all stmts done, no filter
    for (size_t ai = 0; ai < fn->active_blocks.size(); ai++) {
        if (fn->active_blocks[ai] == block) {
            current_stmt_idx = fn->active_block_stmt_idx[ai];
            break;
        }
    }

    for (int i = block->cleanup_vars.size() - 1; i >= 0; i--) {
        auto var = block->cleanup_vars[i];
        if (var == skip_var)
            continue; // Move-returned: skip destruction
        if (var->type == ast::NodeType::VarDecl &&
            var->data.var_decl.stmt_owner_index > current_stmt_idx)
            continue; // stmt-temp not yet born at this divergence point
        if (fn->async_frame_owned_vars.count(var))
            continue; // async frame owns this value; frame destructor handles it
        // Skip variables not yet compiled (e.g. early return before var decl).
        // Also skip stale entries from a previous generic instantiation of the same
        // AST (same node key, but the alloca belongs to a different LLVM function).
        if (!m_ctx->var_table.has_key(var))
            continue;
        if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(m_ctx->var_table.at(var))) {
            if (alloca->getFunction() != fn->llvm_fn)
                continue;
        }
        if (flow.is_sunk(var) && !flow.is_maybe_sunk(var)) {
            continue; // Definitely moved: skip destruction
        }
        // Check drop flag at runtime if one exists. Flags exist either for
        // maybe-moved vars (from flow analysis) or for let-bindings whose init
        // may unwind (so we destroy only if init completed).
        if (auto *flag = m_ctx->drop_flags.get(var)) {
            auto alive = builder.CreateLoad(llvm::Type::getInt1Ty(llvm_ctx), *flag, "alive");
            auto *do_destroy = llvm::BasicBlock::Create(llvm_ctx, "drop", fn->llvm_fn);
            auto *skip_destroy = llvm::BasicBlock::Create(llvm_ctx, "drop.skip", fn->llvm_fn);
            builder.CreateCondBr(alive, do_destroy, skip_destroy);
            builder.SetInsertPoint(do_destroy);
            compile_destruction(fn, get_var(var), var);
            builder.CreateBr(skip_destroy);
            builder.SetInsertPoint(skip_destroy);
        } else {
            compile_destruction(fn, get_var(var), var);
        }
    }
}

void Compiler::alloc_drop_flag(Function *fn, ast::Node *node, bool initial_alive) {
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto flag = fn->entry_alloca(llvm::Type::getInt1Ty(llvm_ctx), node->name + ".alive");
    {
        llvm::IRBuilder<> tmp(llvm_ctx);
        tmp.SetInsertPoint(flag->getNextNode());
        auto init = initial_alive ? llvm::ConstantInt::getTrue(llvm_ctx)
                                  : llvm::ConstantInt::getFalse(llvm_ctx);
        tmp.CreateStore(init, flag);
    }
    m_ctx->drop_flags[node] = flag;
}

void Compiler::set_drop_flag_alive(ast::Node *node, bool alive) {
    if (auto *flag = m_ctx->drop_flags.get(node)) {
        auto &llvm_ctx = *m_ctx->llvm_ctx;
        auto val = alive ? llvm::ConstantInt::getTrue(llvm_ctx)
                         : llvm::ConstantInt::getFalse(llvm_ctx);
        m_ctx->llvm_builder->CreateStore(val, *flag);
    }
}

void Compiler::mark_outlet_alive(Function *fn, ast::Node *outlet) {
    if (!outlet) {
        return;
    }
    if (fn && fn->async_machine) {
        if (auto alive_ptr =
                get_async_frame_var_alive_ptr(fn, *fn->async_machine, outlet)) {
            m_ctx->llvm_builder->CreateStore(
                llvm::ConstantInt::getTrue(*m_ctx->llvm_ctx), alive_ptr);
        }
    }
    // Only mark the drop flag alive when it belongs to the current function —
    // async state machines share outlet AST nodes across resume-point lambdas,
    // so a flag allocated in one lambda must not be touched from another.
    if (auto *flag = m_ctx->drop_flags.get(outlet)) {
        if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(*flag)) {
            if (fn && alloca->getFunction() == fn->llvm_fn) {
                set_drop_flag_alive(outlet, true);
            }
        }
    }
}

void Compiler::compile_destruction(Function *fn, llvm::Value *address, ast::Node *node) {
    // In managed memory mode, don't destroy heap-allocated objects locally - GC handles them
    if (is_fn_managed(fn) && node->is_heap_allocated()) {
        return;
    }

    auto type = get_chitype(node);
    compile_destruction_for_type(fn, address, type);
}

void Compiler::compile_destruction_for_type(Function *fn, llvm::Value *address, ChiType *type) {
    auto &builder = *m_ctx->llvm_builder;

    // Lambdas lower to an internal __CxLambda<...> struct that owns type-erased captures.
    if (type && type->kind == TypeKind::FnLambda) {
        auto internal = type->data.fn_lambda.internal;
        if (internal) {
            compile_destruction_for_type(fn, address, internal);
        }
        return;
    }

    // Resolve Subtype to final type
    auto original_type = type;
    while (type && type->kind == TypeKind::Subtype) {
        auto final_type = type->data.subtype.final_type;
        if (final_type) {
            type = final_type;
        } else {
            break;
        }
    }

    if (!type)
        return;

    if (type->kind == TypeKind::Array) {
        if (!type->data.array.internal) {
            get_resolver()->eval_struct_type(type);
        }
        auto internal = type->data.array.internal;
        if (!internal) {
            return;
        }
        auto dtor = generate_destructor(internal, nullptr);
        if (dtor) {
            builder.CreateCall(dtor->llvm_fn, {address});
        }
        return;
    }

    // Any: destroy inner value via TypeInfo destructor, free heap if not inlined
    if (type->kind == TypeKind::Any) {
        auto &llvm_ctx = *m_ctx->llvm_ctx;
        auto any_type_l = compile_type(type);
        auto ptr_type = get_llvm_ptr_type();
        auto i32_ty = llvm::Type::getInt32Ty(llvm_ctx);
        auto i8_ty = llvm::Type::getInt8Ty(llvm_ctx);

        // Load TypeInfo* from any.type (field 0)
        auto ti_gep = builder.CreateStructGEP(any_type_l, address, 0);
        auto ti_ptr = builder.CreateLoad(ptr_type, ti_gep, "any_ti");

        // Null check TypeInfo — skip if null (uninitialized any)
        auto ti_is_null = builder.CreateICmpEQ(ti_ptr, get_null_ptr());
        auto bb_has_ti = fn->new_label("any_has_ti");
        auto bb_done = fn->new_label("any_dtor_done");
        builder.CreateCondBr(ti_is_null, bb_done, bb_has_ti);
        fn->use_label(bb_has_ti);

        // Load destructor from TypeInfo (field 3: kind, size, data, destructor, copier, ...)
        auto ti_header_l = get_typeinfo_llvm_type();
        auto dtor_gep = builder.CreateStructGEP(ti_header_l, ti_ptr, 3);
        auto dtor_ptr = builder.CreateLoad(ptr_type, dtor_gep, "any_dtor");

        // Resolve data pointer: inlined → &any.data, not inlined → *(void**)&any.data
        auto inlined_gep = builder.CreateStructGEP(any_type_l, address, 1);
        auto inlined = builder.CreateLoad(i8_ty, inlined_gep, "any_inlined");
        auto is_inlined = builder.CreateICmpNE(inlined, llvm::ConstantInt::get(i8_ty, 0));
        auto data_gep = builder.CreateStructGEP(any_type_l, address, 3);

        auto bb_inlined = fn->new_label("any_data_inlined");
        auto bb_heap = fn->new_label("any_data_heap");
        auto bb_have_data = fn->new_label("any_have_data");
        builder.CreateCondBr(is_inlined, bb_inlined, bb_heap);

        fn->use_label(bb_inlined);
        auto inlined_data_ptr = (llvm::Value *)data_gep;
        builder.CreateBr(bb_have_data);

        fn->use_label(bb_heap);
        auto heap_ptr = builder.CreateLoad(ptr_type, data_gep, "any_heap_ptr");
        builder.CreateBr(bb_have_data);

        fn->use_label(bb_have_data);
        auto data_ptr = builder.CreatePHI(ptr_type, 2, "any_data_ptr");
        data_ptr->addIncoming(inlined_data_ptr, bb_inlined);
        data_ptr->addIncoming(heap_ptr, bb_heap);

        // Call destructor if non-null
        auto dtor_is_null = builder.CreateICmpEQ(dtor_ptr, get_null_ptr());
        auto bb_call_dtor = fn->new_label("any_call_dtor");
        auto bb_after_dtor = fn->new_label("any_after_dtor");
        builder.CreateCondBr(dtor_is_null, bb_after_dtor, bb_call_dtor);

        fn->use_label(bb_call_dtor);
        auto dtor_fn_type =
            llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type}, false);
        builder.CreateCall(dtor_fn_type, dtor_ptr, {data_ptr});
        builder.CreateBr(bb_after_dtor);

        fn->use_label(bb_after_dtor);
        // Free heap allocation if not inlined
        auto bb_free_heap = fn->new_label("any_free_heap");
        builder.CreateCondBr(is_inlined, bb_done, bb_free_heap);
        fn->use_label(bb_free_heap);
        auto free_fn = get_system_fn("cx_free");
        builder.CreateCall(free_fn->llvm_fn, {data_ptr});
        builder.CreateBr(bb_done);

        fn->use_label(bb_done);
        return;
    }

    // &move T RAII: destroy pointee + free (same as delete)
    if (type->kind == TypeKind::MoveRef) {
        auto elem_type = type->get_elem();
        auto ptr = builder.CreateLoad(compile_type(type), address);
        // Null check — pointer may be null if default-initialized
        auto is_null = builder.CreateICmpEQ(ptr, get_null_ptr());
        auto bb_destroy = fn->new_label("_moveref_destroy");
        auto bb_done = fn->new_label("_moveref_done");
        builder.CreateCondBr(is_null, bb_done, bb_destroy);
        fn->use_label(bb_destroy);
        compile_heap_free(fn, ptr, elem_type);
        builder.CreateBr(bb_done);
        fn->use_label(bb_done);
        return;
    }

    // Handle strings
    if (type->kind == TypeKind::String) {
        auto string_delete = get_system_fn("cx_string_delete");
        builder.CreateCall(string_delete->llvm_fn, {address});
        return;
    }

    // Handle interface destruction via vtable
    if (type->kind == TypeKind::Struct && ChiTypeStruct::is_interface(type)) {
        auto ref_type = get_resolver()->get_pointer_type(type, TypeKind::Reference);
        compile_interface_destruction(fn, address, ref_type);
        return;
    }

    if (auto dtor = generate_destructor(original_type, nullptr)) {
        builder.CreateCall(dtor->llvm_fn, {address});
    }
}

void Compiler::compile_heap_free(Function *fn, llvm::Value *ptr, ChiType *elem_type) {
    auto &builder = *m_ctx->llvm_builder;
    if (elem_type && ChiTypeStruct::is_interface(elem_type)) {
        auto data_ptr = builder.CreateExtractValue(ptr, {0}, "data_ptr");
        auto vtable_ptr = builder.CreateExtractValue(ptr, {1}, "vtable_ptr");
        call_vtable_destructor(fn, vtable_ptr, data_ptr);
        auto free_fn = get_system_fn("cx_free");
        builder.CreateCall(free_fn->llvm_fn, {data_ptr});
    } else {
        if (elem_type) {
            compile_destruction_for_type(fn, ptr, elem_type);
        }
        auto free_fn = get_system_fn("cx_free");
        builder.CreateCall(free_fn->llvm_fn, {ptr});
    }
}

void Compiler::compile_interface_destruction(Function *fn, llvm::Value *iface_address,
                                             ChiType *iface_ref_type) {
    auto &builder = *m_ctx->llvm_builder;
    auto iface_type_l = compile_type(iface_ref_type);
    auto ptr_type = get_llvm_ptr_type();

    // Extract data_ptr (field 0) and vtable_ptr (field 1)
    auto data_gep = builder.CreateStructGEP(iface_type_l, iface_address, 0);
    auto data_ptr = builder.CreateLoad(ptr_type, data_gep);
    auto vtable_gep = builder.CreateStructGEP(iface_type_l, iface_address, 1);
    auto vtable_ptr = builder.CreateLoad(ptr_type, vtable_gep);

    call_vtable_destructor(fn, vtable_ptr, data_ptr);

    auto free_fn = get_system_fn("cx_free");
    builder.CreateCall(free_fn->llvm_fn, {data_ptr});
}

void Compiler::call_vtable_destructor(Function *fn, llvm::Value *vtable_ptr,
                                      llvm::Value *data_ptr) {
    auto &builder = *m_ctx->llvm_builder;
    auto ptr_type = get_llvm_ptr_type();

    // Load destructor from vtable[1] (index 0=typeinfo, 1=destructor)
    auto dtor_gep = builder.CreateGEP(
        ptr_type, vtable_ptr, {llvm::ConstantInt::get(*m_ctx->llvm_ctx, llvm::APInt(32, 1))});
    auto dtor_ptr = builder.CreateLoad(ptr_type, dtor_gep);

    auto is_null = builder.CreateICmpEQ(dtor_ptr, get_null_ptr());
    auto then_bb = fn->new_label("dtor_call");
    auto merge_bb = fn->new_label("dtor_merge");
    builder.CreateCondBr(is_null, merge_bb, then_bb);

    fn->use_label(then_bb);
    auto dtor_fn_type =
        llvm::FunctionType::get(llvm::Type::getVoidTy(*m_ctx->llvm_ctx), {ptr_type}, false);
    builder.CreateCall(dtor_fn_type, dtor_ptr, {data_ptr});
    builder.CreateBr(merge_bb);

    fn->use_label(merge_bb);
}

void Compiler::call_vtable_copier(Function *fn, llvm::Value *vtable_ptr, llvm::Value *dest_data,
                                  llvm::Value *src_data) {
    auto &builder = *m_ctx->llvm_builder;
    auto ptr_type = get_llvm_ptr_type();

    // Load copier from vtable[2] (index 0=typeinfo, 1=destructor, 2=copier)
    auto copier_gep = builder.CreateGEP(
        ptr_type, vtable_ptr, {llvm::ConstantInt::get(*m_ctx->llvm_ctx, llvm::APInt(32, 2))});
    auto copier_fn_ptr = builder.CreateLoad(ptr_type, copier_gep, "copier_fn");
    auto copier_is_null = builder.CreateICmpEQ(copier_fn_ptr, get_null_ptr());
    auto bb_copy = fn->new_label("vtable_copy");
    auto bb_memcpy = fn->new_label("vtable_memcpy");
    auto bb_done = fn->new_label("vtable_copy_done");
    builder.CreateCondBr(copier_is_null, bb_memcpy, bb_copy);

    // Call copier(dest_data, src_data)
    fn->use_label(bb_copy);
    auto copier_fn_type = llvm::FunctionType::get(llvm::Type::getVoidTy(*m_ctx->llvm_ctx),
                                                  {ptr_type, ptr_type}, false);
    builder.CreateCall(copier_fn_type, copier_fn_ptr, {dest_data, src_data});
    builder.CreateBr(bb_done);

    // Fallback: shallow memcpy using typesize from vtable
    fn->use_label(bb_memcpy);
    auto typesize = load_typesize_from_vtable(vtable_ptr);
    builder.CreateMemCpy(dest_data, {}, src_data, {}, typesize);
    builder.CreateBr(bb_done);

    fn->use_label(bb_done);
}

llvm::ConstantPointerNull *Compiler::get_null_ptr() {
    return llvm::ConstantPointerNull::get(llvm::PointerType::get(*m_ctx->llvm_ctx, 0));
}

llvm::StructType *Compiler::get_typeinfo_llvm_type() {
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto ptr_type_l = llvm::PointerType::get(llvm_ctx, 0);
    constexpr size_t tidata_word_count =
        (sizeof(TypeInfoData) + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    auto tidata_type_l =
        llvm::ArrayType::get(llvm::Type::getInt64Ty(llvm_ctx), (uint32_t)tidata_word_count);
    auto i8_ty = llvm::Type::getInt8Ty(llvm_ctx);

    return llvm::StructType::get(
        llvm_ctx,
        {llvm::Type::getInt32Ty(llvm_ctx), llvm::Type::getInt32Ty(llvm_ctx), tidata_type_l,
         ptr_type_l, ptr_type_l, llvm::Type::getInt32Ty(llvm_ctx), ptr_type_l,
         llvm::Type::getInt32Ty(llvm_ctx), ptr_type_l, llvm::Type::getInt32Ty(llvm_ctx),
         llvm::ArrayType::get(i8_ty, sizeof(TypeInfo::name))},
        false);
}

llvm::Value *Compiler::load_typesize_from_vtable(llvm::Value *vtable_ptr) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto ptr_type = get_llvm_ptr_type();

    // vtable[0] = typeinfo pointer
    auto typeinfo_ptr = builder.CreateLoad(ptr_type, vtable_ptr, "typeinfo_ptr");

    // TypeInfo header starts with {i32 kind, i32 typesize, ...}
    // We only need to read the first two i32 fields
    auto ti_header_l = llvm::StructType::get(
        llvm_ctx, {llvm::Type::getInt32Ty(llvm_ctx), llvm::Type::getInt32Ty(llvm_ctx)}, true);
    auto size_gep = builder.CreateStructGEP(ti_header_l, typeinfo_ptr, 1);
    return builder.CreateLoad(llvm::Type::getInt32Ty(llvm_ctx), size_gep, "typesize");
}

llvm::Value *Compiler::find_interface_vtable(Function *fn, ChiType *iface_type) {
    auto &builder = *m_ctx->llvm_builder;

    if (!fn || !fn->container_subtype)
        return nullptr;

    auto container_type = fn->container_subtype->final_type;
    if (!container_type)
        return nullptr;
    container_type = eval_type(container_type);
    if (container_type->kind != TypeKind::Struct)
        return nullptr;

    auto &struct_data = container_type->data.struct_;

    // Find a fat pointer field (*T or &T) where T is the interface type
    auto iface_key = get_resolver()->format_type_id(iface_type);
    ChiStructMember *target_field = nullptr;
    ChiType *field_type = nullptr;
    for (auto field : struct_data.fields) {
        auto ftype = eval_type(field->resolved_type);
        if (ftype->is_pointer_like()) {
            auto elem = ftype->get_elem();
            if (elem && ChiTypeStruct::is_interface(elem) &&
                get_resolver()->format_type_id(elem) == iface_key) {
                target_field = field;
                field_type = ftype;
                break;
            }
        }
    }

    if (!target_field)
        return nullptr;

    auto container_type_l = compile_type(container_type);
    auto fat_ptr_type_l = compile_type(field_type);

    // Prefer function parameters over `this` (params are more likely to have valid data)
    if (fn->node && fn->node->type == ast::NodeType::FnDef) {
        auto proto_node = fn->node->data.fn_def.fn_proto;
        for (auto &param_info : fn->parameter_info) {
            if (param_info.kind != ParameterKind::Regular)
                continue;
            auto param_type = param_info.type;
            if (!param_type || !param_type->is_pointer_like())
                continue;
            auto param_elem = param_type->get_elem();
            if (!param_elem)
                continue;
            param_elem = eval_type(param_elem);
            if (get_resolver()->format_type_id(param_elem) !=
                get_resolver()->format_type_id(container_type))
                continue;

            // This param references the same container struct — use its fat pointer field
            auto param_node = proto_node->data.fn_proto.params[param_info.user_param_index];
            auto param_alloca = get_var(param_node);
            auto struct_ptr =
                builder.CreateLoad(get_llvm_ptr_type(), param_alloca, "param_struct_ptr");
            auto field_gep =
                builder.CreateStructGEP(container_type_l, struct_ptr, target_field->field_index);
            auto fat_ptr = builder.CreateLoad(fat_ptr_type_l, field_gep, "iface_fat_ptr");
            return builder.CreateExtractValue(fat_ptr, {1}, "vtable_ptr");
        }
    }

    // Fall back to `this` pointer
    if (fn->bind_ptr) {
        auto field_gep =
            builder.CreateStructGEP(container_type_l, fn->bind_ptr, target_field->field_index);
        auto fat_ptr = builder.CreateLoad(fat_ptr_type_l, field_gep, "iface_fat_ptr");
        return builder.CreateExtractValue(fat_ptr, {1}, "vtable_ptr");
    }

    return nullptr;
}

Function *Compiler::generate_destructor(ChiType *type, ChiType *container_type) {
    // Don't generate destructors for placeholder types
    if (type->is_placeholder) {
        return nullptr;
    }

    // Check if already generated
    auto existing = m_ctx->destructor_table.get(type);
    if (existing) {
        return *existing;
    }

    // Resolve subtype if needed
    auto resolved_type = type;
    while (resolved_type && resolved_type->kind == TypeKind::Subtype) {
        auto final_type = resolved_type->data.subtype.final_type;
        if (final_type) {
            resolved_type = final_type;
        } else {
            break;
        }
    }

    if (!resolved_type || resolved_type->is_placeholder) {
        return nullptr;
    }

    // Enums reduce to their base EnumValue layout
    if (resolved_type->kind == TypeKind::Enum) {
        resolved_type = resolved_type->data.enum_.base_value_type;
    }

    switch (resolved_type->kind) {
    case TypeKind::Optional:
        return generate_destructor_optional(type, resolved_type);
    case TypeKind::EnumValue:
        return generate_destructor_enum(type, resolved_type);
    case TypeKind::FixedArray:
        return generate_destructor_fixed_array(type, resolved_type);
    case TypeKind::Tuple:
        return generate_destructor_tuple(type, resolved_type);
    case TypeKind::Struct:
        return generate_destructor_struct(type, resolved_type);
    default:
        return nullptr;
    }
}

Function *Compiler::generate_destructor_fixed_array(ChiType *type, ChiType *resolved_type) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    auto elem = resolved_type->data.fixed_array.elem;
    if (!get_resolver()->type_needs_destruction(elem)) {
        return nullptr;
    }
    auto ptr_type = get_llvm_ptr_type();
    auto fn_type_l =
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type}, false);
    auto type_name = get_resolver()->format_type_display(type);
    auto fn_name = fmt::format("{}.__delete", type_name);
    auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage, fn_name,
                                          m_ctx->llvm_module.get());
    auto fn = new Function(m_ctx, llvm_fn, nullptr);
    fn->qualified_name = fn_name;
    m_ctx->functions.emplace(fn);
    m_ctx->destructor_table[type] = fn;

    auto saved_block = builder.GetInsertBlock();
    auto saved_point = builder.GetInsertPoint();

    auto i32_ty = llvm::Type::getInt32Ty(llvm_ctx);
    auto fa_size = resolved_type->data.fixed_array.size;
    auto arr_type_l = compile_type(resolved_type);

    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    auto loop_bb = llvm::BasicBlock::Create(llvm_ctx, "loop", llvm_fn);
    auto body_bb = llvm::BasicBlock::Create(llvm_ctx, "body", llvm_fn);
    auto end_bb = llvm::BasicBlock::Create(llvm_ctx, "end", llvm_fn);

    builder.SetInsertPoint(entry_bb);
    auto this_ptr = llvm_fn->getArg(0);
    auto counter = builder.CreateAlloca(i32_ty, nullptr, "i");
    builder.CreateStore(llvm::ConstantInt::get(i32_ty, fa_size), counter);
    builder.CreateBr(loop_bb);

    builder.SetInsertPoint(loop_bb);
    auto cur = builder.CreateLoad(i32_ty, counter);
    auto cond = builder.CreateICmpUGT(cur, llvm::ConstantInt::get(i32_ty, 0));
    builder.CreateCondBr(cond, body_bb, end_bb);

    builder.SetInsertPoint(body_bb);
    auto idx = builder.CreateSub(cur, llvm::ConstantInt::get(i32_ty, 1));
    builder.CreateStore(idx, counter);
    auto zero = llvm::ConstantInt::get(i32_ty, 0);
    auto elem_ptr = builder.CreateGEP(arr_type_l, this_ptr, {zero, idx});
    compile_destruction_for_type(fn, elem_ptr, elem);
    builder.CreateBr(loop_bb);

    builder.SetInsertPoint(end_bb);
    builder.CreateRetVoid();

    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }
    return fn;
}

Function *Compiler::generate_destructor_tuple(ChiType *type, ChiType *resolved_type) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    auto &elements = resolved_type->data.tuple.elements;
    bool any_destructible = false;
    for (auto elem : elements) {
        if (get_resolver()->type_needs_destruction(elem)) {
            any_destructible = true;
            break;
        }
    }
    if (!any_destructible) {
        return nullptr;
    }

    auto ptr_type = get_llvm_ptr_type();
    auto fn_type_l =
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type}, false);
    auto type_name = get_resolver()->format_type_display(type);
    auto fn_name = fmt::format("{}.__delete", type_name);
    auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage, fn_name,
                                          m_ctx->llvm_module.get());
    auto fn = new Function(m_ctx, llvm_fn, nullptr);
    fn->qualified_name = fn_name;
    m_ctx->functions.emplace(fn);
    m_ctx->destructor_table[type] = fn;

    auto saved_block = builder.GetInsertBlock();
    auto saved_point = builder.GetInsertPoint();

    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    builder.SetInsertPoint(entry_bb);
    auto this_ptr = llvm_fn->getArg(0);
    auto tuple_type_l = compile_type(resolved_type);

    for (int i = (int)elements.size() - 1; i >= 0; i--) {
        auto elem_type = elements[i];
        if (!get_resolver()->type_needs_destruction(elem_type)) {
            continue;
        }
        auto elem_ptr = builder.CreateStructGEP(tuple_type_l, this_ptr, (unsigned)i);
        compile_destruction_for_type(fn, elem_ptr, elem_type);
    }
    builder.CreateRetVoid();

    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }
    return fn;
}

Function *Compiler::generate_destructor_struct(ChiType *type, ChiType *resolved_type) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    auto struct_ptr_type = get_llvm_ptr_type();
    auto fn_type_l =
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {struct_ptr_type}, false);

    auto type_name = get_resolver()->format_type_display(type);
    auto fn_name = fmt::format("{}.__delete", type_name);

    auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage, fn_name,
                                          m_ctx->llvm_module.get());

    auto fn = new Function(m_ctx, llvm_fn, nullptr);
    fn->qualified_name = fn_name;
    m_ctx->functions.emplace(fn);
    m_ctx->destructor_table[type] = fn;

    auto saved_block = builder.GetInsertBlock();
    auto saved_point = builder.GetInsertPoint();

    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    builder.SetInsertPoint(entry_bb);

    auto this_ptr = llvm_fn->getArg(0);
    auto llvm_struct_type = compile_type(resolved_type);

    // Call user's delete() if defined
    auto user_destructor = ChiTypeStruct::get_destructor(resolved_type);
    if (user_destructor) {
        auto destructor_type = get_chitype(user_destructor->node);
        auto destructor_id = get_resolver()->resolve_global_id(user_destructor->node);
        auto destructor_fn_ptr = m_ctx->function_table.get(destructor_id);
        Function *destructor_fn = nullptr;
        if (!destructor_fn_ptr) {
            auto proto = user_destructor->node->data.fn_def.fn_proto;
            destructor_fn = compile_fn_proto(proto, user_destructor->node);
            if (type->kind == TypeKind::Subtype) {
                destructor_fn->container_subtype = &type->data.subtype;
                destructor_fn->container_type = type;
                if (auto entry = get_resolver()->get_generics()->struct_envs.get(type->global_id)) {
                    destructor_fn->type_env = &entry->subs;
                }
            }
            m_ctx->pending_fns.add(destructor_fn);
        } else {
            destructor_fn = *destructor_fn_ptr;
        }
        auto destructor_type_l = (llvm::FunctionType *)compile_type(destructor_type);
        // Guard the user destructor call: if the user's delete() or anything it
        // calls panics, cx_throw observes cx_destructor_depth > 0 and aborts
        // instead of throwing. Matches C++ noexcept-dtor / Rust double-panic.
        auto enter_fn = get_system_fn("cx_destructor_enter");
        auto leave_fn = get_system_fn("cx_destructor_leave");
        builder.CreateCall(enter_fn->llvm_fn, {});
        builder.CreateCall(destructor_type_l, destructor_fn->llvm_fn, {this_ptr});
        builder.CreateCall(leave_fn->llvm_fn, {});
    }

    // Destroy fields in reverse declaration order
    auto fields = resolved_type->data.struct_.own_fields();
    for (int i = fields.size() - 1; i >= 0; i--) {
        auto field = fields[i];
        auto field_type = field->resolved_type;
        auto resolved_field_type = field_type;

        while (resolved_field_type && resolved_field_type->kind == TypeKind::Subtype) {
            auto final_type = resolved_field_type->data.subtype.final_type;
            if (final_type) {
                resolved_field_type = final_type;
            } else {
                break;
            }
        }

        if (!resolved_field_type)
            continue;

        if (!get_resolver()->type_needs_destruction(resolved_field_type)) {
            continue;
        }

        auto field_gep = builder.CreateStructGEP(llvm_struct_type, this_ptr, field->field_index);
        // Pass original field_type to preserve Subtype info for container_subtype resolution
        compile_destruction_for_type(fn, field_gep, field_type);
    }

    builder.CreateRetVoid();

    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }

    return fn;
}


Function *Compiler::generate_copier(ChiType *type) {
    // Don't generate copiers for placeholder types
    if (type->is_placeholder) {
        return nullptr;
    }

    // Check if already generated
    auto existing = m_ctx->copier_table.get(type);
    if (existing) {
        return *existing;
    }

    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    // Resolve subtype if needed
    auto resolved_type = type;
    while (resolved_type && resolved_type->kind == TypeKind::Subtype) {
        auto final_type = resolved_type->data.subtype.final_type;
        if (final_type) {
            resolved_type = final_type;
        } else {
            break;
        }
    }

    if (!resolved_type || resolved_type->is_placeholder) {
        return nullptr;
    }

    if (resolved_type->kind != TypeKind::Struct) {
        return nullptr;
    }

    // Create function type: void __copy(T* dest, T* src)
    auto ptr_type = get_llvm_ptr_type();
    auto fn_type_l =
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type, ptr_type}, false);

    // Generate unique name for the copier
    auto type_name = get_resolver()->format_type_display(type);
    auto fn_name = fmt::format("{}.__copy", type_name);

    auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage, fn_name,
                                          m_ctx->llvm_module.get());

    // Create Function object
    auto fn = new Function(m_ctx, llvm_fn, nullptr);
    fn->qualified_name = fn_name;
    m_ctx->functions.emplace(fn);
    m_ctx->copier_table[type] = fn;

    // Save current insert point
    auto saved_block = builder.GetInsertBlock();
    auto saved_point = builder.GetInsertPoint();

    // Create entry block
    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    builder.SetInsertPoint(entry_bb);

    auto dest_ptr = llvm_fn->getArg(0);
    auto src_ptr = llvm_fn->getArg(1);

    // Delegate to compile_copy_with_ref — pure copy without destruct_old
    // The caller is responsible for destructing the old value first if needed
    compile_copy_with_ref(fn, RefValue::from_address(src_ptr), dest_ptr, resolved_type, nullptr,
                          false);

    builder.CreateRetVoid();

    // Restore insert point
    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }

    return fn;
}

Function *Compiler::generate_any_destructor(ChiType *type) {
    if (!get_resolver()->type_needs_destruction(type))
        return nullptr;

    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto ptr_type = get_llvm_ptr_type();

    // void __any_dtor_T(void* ptr)
    auto fn_type_l = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type}, false);
    auto type_name = get_resolver()->format_type_display(type);
    auto fn_name = fmt::format("{}.__any_dtor", type_name);
    auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage, fn_name,
                                          m_ctx->llvm_module.get());

    auto fn = new Function(m_ctx, llvm_fn, nullptr);
    fn->qualified_name = fn_name;
    m_ctx->functions.emplace(fn);

    auto saved_block = builder.GetInsertBlock();
    auto saved_point = builder.GetInsertPoint();
    auto saved_dbg = builder.getCurrentDebugLocation();

    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    builder.SetInsertPoint(entry_bb);
    builder.SetCurrentDebugLocation(llvm::DebugLoc());

    compile_destruction_for_type(fn, llvm_fn->getArg(0), type);
    builder.CreateRetVoid();

    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }
    builder.SetCurrentDebugLocation(saved_dbg);
    return fn;
}

Function *Compiler::generate_any_copier(ChiType *type) {
    // Check if type needs non-trivial copy (has destructor, Copy, string, lambda, etc.)
    bool needs_copier = get_resolver()->type_needs_destruction(type);
    if (!needs_copier && type->kind == TypeKind::Struct) {
        auto sty = get_resolver()->resolve_struct_type(eval_type(type));
        if (sty && sty->member_intrinsics.get(IntrinsicSymbol::Copy))
            needs_copier = true;
    }
    if (!needs_copier)
        return nullptr;

    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto ptr_type = get_llvm_ptr_type();

    // void __any_copy_T(void* dest, void* src)
    auto fn_type_l =
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type, ptr_type}, false);
    auto type_name = get_resolver()->format_type_display(type);
    auto fn_name = fmt::format("{}.__any_copy", type_name);
    auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage, fn_name,
                                          m_ctx->llvm_module.get());

    auto fn = new Function(m_ctx, llvm_fn, nullptr);
    fn->qualified_name = fn_name;
    m_ctx->functions.emplace(fn);

    auto saved_block = builder.GetInsertBlock();
    auto saved_point = builder.GetInsertPoint();
    auto saved_dbg = builder.getCurrentDebugLocation();

    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    builder.SetInsertPoint(entry_bb);
    builder.SetCurrentDebugLocation(llvm::DebugLoc());

    auto dest_ptr = llvm_fn->getArg(0);
    auto src_ptr = llvm_fn->getArg(1);
    compile_copy_with_ref(fn, RefValue::from_address(src_ptr), dest_ptr, type, nullptr, false);
    builder.CreateRetVoid();

    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }
    builder.SetCurrentDebugLocation(saved_dbg);
    return fn;
}

Function *Compiler::generate_destructor_optional(ChiType *type, ChiType *resolved_type) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    // Get element type
    auto elem_type = resolved_type->get_elem();
    if (!elem_type || !get_resolver()->type_needs_destruction(elem_type)) {
        return nullptr;
    }

    // Create function type: void __delete(T*)
    auto ptr_type = get_llvm_ptr_type();
    auto fn_type_l = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type}, false);

    // Generate unique name for the destructor
    auto type_name = get_resolver()->format_type_display(type);
    auto fn_name = fmt::format("{}.__delete", type_name);

    auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage, fn_name,
                                          m_ctx->llvm_module.get());

    // Create Function object
    auto fn = new Function(m_ctx, llvm_fn, nullptr);
    fn->qualified_name = fn_name;
    m_ctx->functions.emplace(fn);
    m_ctx->destructor_table[type] = fn;

    // Save current insert point
    auto saved_block = builder.GetInsertBlock();
    auto saved_point = builder.GetInsertPoint();

    // Create blocks
    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    auto destroy_bb = llvm::BasicBlock::Create(llvm_ctx, "destroy", llvm_fn);
    auto end_bb = llvm::BasicBlock::Create(llvm_ctx, "end", llvm_fn);

    // Entry: check has_value
    builder.SetInsertPoint(entry_bb);
    auto this_ptr = llvm_fn->getArg(0);
    auto opt_type_l = compile_type(resolved_type);
    auto has_value_ptr = builder.CreateStructGEP(opt_type_l, this_ptr, 0);
    auto has_value = builder.CreateLoad(llvm::Type::getInt1Ty(llvm_ctx), has_value_ptr);
    builder.CreateCondBr(has_value, destroy_bb, end_bb);

    // Destroy: call destruction on inner value
    builder.SetInsertPoint(destroy_bb);
    auto value_ptr = builder.CreateStructGEP(opt_type_l, this_ptr, 1);
    compile_destruction_for_type(fn, value_ptr, elem_type);
    builder.CreateBr(end_bb);

    // End: return
    builder.SetInsertPoint(end_bb);
    builder.CreateRetVoid();

    // Restore insert point
    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }

    return fn;
}

Function *Compiler::generate_destructor_enum(ChiType *type, ChiType *resolved_type) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    auto enum_ = resolved_type->data.enum_value.parent_enum();
    auto bvs = enum_->base_value_type->data.enum_value.resolved_struct;

    // Check if anything actually needs destruction
    if (!get_resolver()->type_needs_destruction(resolved_type)) {
        m_ctx->destructor_table[type] = nullptr;
        return nullptr;
    }

    // Create function type: void __delete(T*)
    auto ptr_type = get_llvm_ptr_type();
    auto fn_type_l = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type}, false);

    auto type_name = get_resolver()->format_type_display(type);
    auto fn_name = fmt::format("{}.__delete", type_name);

    auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage, fn_name,
                                          m_ctx->llvm_module.get());

    auto fn = new Function(m_ctx, llvm_fn, nullptr);
    fn->qualified_name = fn_name;
    m_ctx->functions.emplace(fn);
    m_ctx->destructor_table[type] = fn;

    // Save current insert point and debug location
    auto saved_block = builder.GetInsertBlock();
    auto saved_point = builder.GetInsertPoint();
    auto saved_dbg = builder.getCurrentDebugLocation();

    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    builder.SetInsertPoint(entry_bb);
    builder.SetCurrentDebugLocation(llvm::DebugLoc());

    auto this_ptr = llvm_fn->getArg(0);
    auto enum_type_l = compile_type(resolved_type);

    // 1. Destroy base_value_struct fields that need it (in reverse)
    if (bvs) {
        auto &fields = bvs->data.struct_.fields;
        for (int i = fields.size() - 1; i >= 0; i--) {
            auto field = fields[i];
            if (!get_resolver()->type_needs_destruction(field->resolved_type))
                continue;
            auto field_gep = builder.CreateStructGEP(enum_type_l, this_ptr, field->field_index);
            compile_destruction_for_type(fn, field_gep, field->resolved_type);
        }
    }

    // 2. Check if any variant has destructible fields
    bool any_variant_needs = false;
    for (auto variant : enum_->variants) {
        if (auto vs = variant->resolved_type->data.enum_value.variant_struct) {
            for (auto field : vs->data.struct_.fields) {
                if (get_resolver()->type_needs_destruction(field->resolved_type)) {
                    any_variant_needs = true;
                    break;
                }
            }
        }
        if (any_variant_needs)
            break;
    }

    if (any_variant_needs) {
        // 3. Load discriminator from field 0
        auto disc_gep = builder.CreateStructGEP(enum_type_l, this_ptr, 0);
        auto disc = builder.CreateLoad(compile_type(enum_->discriminator), disc_gep, "disc");

        // 4. Variant data index matches __data field_index used by compile_dot_access
        auto variant_data_idx = bvs ? (unsigned)bvs->data.struct_.fields.size() : 0u;

        auto bb_done = fn->new_label("enum_dtor_done");
        auto sw = builder.CreateSwitch(disc, bb_done, enum_->variants.size());

        for (auto variant : enum_->variants) {
            auto vs = variant->resolved_type->data.enum_value.variant_struct;
            if (!vs)
                continue;

            bool variant_needs = false;
            for (auto field : vs->data.struct_.fields) {
                if (get_resolver()->type_needs_destruction(field->resolved_type)) {
                    variant_needs = true;
                    break;
                }
            }
            if (!variant_needs)
                continue;

            auto bb = fn->new_label(fmt::format("enum_dtor_{}", variant->name));
            sw->addCase(
                llvm::ConstantInt::get((llvm::IntegerType *)compile_type(enum_->discriminator),
                                       variant->node->data.enum_variant.resolved_value),
                bb);
            fn->use_label(bb);

            auto data_gep = builder.CreateStructGEP(enum_type_l, this_ptr, variant_data_idx);
            auto vs_type_l = compile_type(vs);

            // Destroy variant fields in reverse
            auto &vfields = vs->data.struct_.fields;
            for (int i = vfields.size() - 1; i >= 0; i--) {
                auto field = vfields[i];
                if (!get_resolver()->type_needs_destruction(field->resolved_type))
                    continue;
                auto field_gep = builder.CreateStructGEP(vs_type_l, data_gep, field->field_index);
                compile_destruction_for_type(fn, field_gep, field->resolved_type);
            }
            builder.CreateBr(bb_done);
        }
        fn->use_label(bb_done);
    }

    builder.CreateRetVoid();

    // Restore insert point and debug location
    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }
    builder.SetCurrentDebugLocation(saved_dbg);

    return fn;
}

Function *Compiler::generate_copier_enum(ChiType *type) {
    // Don't generate copiers for placeholder types
    if (type->is_placeholder)
        return nullptr;

    // Check if already generated
    auto existing = m_ctx->copier_table.get(type);
    if (existing)
        return *existing;

    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    // Resolve to the EnumValue type
    auto resolved_type = type;
    while (resolved_type && resolved_type->kind == TypeKind::Subtype) {
        auto final_type = resolved_type->data.subtype.final_type;
        if (final_type)
            resolved_type = final_type;
        else
            break;
    }

    if (!resolved_type) {
        return nullptr;
    }

    if (resolved_type->kind == TypeKind::Enum) {
        resolved_type = resolved_type->data.enum_.base_value_type;
    }

    if (resolved_type->kind != TypeKind::EnumValue) {
        return nullptr;
    }

    auto enum_ = resolved_type->data.enum_value.parent_enum();
    auto bvs = enum_->base_value_type->data.enum_value.resolved_struct;

    // Check if any field needs deep copy (destructor OR Copy)
    bool needs_copier = get_resolver()->type_needs_destruction(resolved_type);
    if (!needs_copier) {
        // Also check if any field has Copy (copy semantics without destructor)
        auto check_field_copier = [&](ChiType *field_type) -> bool {
            auto sty = get_resolver()->resolve_struct_type(eval_type(field_type));
            return sty && sty->member_intrinsics.get(IntrinsicSymbol::Copy);
        };
        if (bvs) {
            for (auto field : bvs->data.struct_.fields) {
                if (check_field_copier(field->resolved_type)) {
                    needs_copier = true;
                    break;
                }
            }
        }
        if (!needs_copier) {
            for (auto variant : enum_->variants) {
                if (auto vs = variant->resolved_type->data.enum_value.variant_struct) {
                    for (auto field : vs->data.struct_.fields) {
                        if (check_field_copier(field->resolved_type)) {
                            needs_copier = true;
                            break;
                        }
                    }
                }
                if (needs_copier)
                    break;
            }
        }
    }
    if (!needs_copier) {
        m_ctx->copier_table[type] = nullptr;
        return nullptr;
    }

    // Create function type: void __copy(T* dest, T* src)
    auto ptr_type = get_llvm_ptr_type();
    auto fn_type_l =
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type, ptr_type}, false);

    auto type_name = get_resolver()->format_type_display(type);
    auto fn_name = fmt::format("{}.__copy", type_name);

    auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage, fn_name,
                                          m_ctx->llvm_module.get());

    auto fn = new Function(m_ctx, llvm_fn, nullptr);
    fn->qualified_name = fn_name;
    m_ctx->functions.emplace(fn);
    m_ctx->copier_table[type] = fn;

    // Save current insert point and debug location
    auto saved_block = builder.GetInsertBlock();
    auto saved_point = builder.GetInsertPoint();
    auto saved_dbg = builder.getCurrentDebugLocation();

    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    builder.SetInsertPoint(entry_bb);
    builder.SetCurrentDebugLocation(llvm::DebugLoc());

    auto dest_ptr = llvm_fn->getArg(0);
    auto src_ptr = llvm_fn->getArg(1);
    auto enum_type_l = compile_type(resolved_type);
    auto full_size = llvm_type_size(enum_type_l);

    // 1. memcpy entire struct first (preserves trivial fields including variant data),
    //    then deep-copy non-trivial fields on top
    builder.CreateMemCpy(dest_ptr, {}, src_ptr, {}, full_size);

    // 2. Copy base_value_struct fields (header + base shared fields)
    if (bvs) {
        for (auto field : bvs->data.struct_.fields) {
            auto src_gep = builder.CreateStructGEP(enum_type_l, src_ptr, field->field_index);
            auto dst_gep = builder.CreateStructGEP(enum_type_l, dest_ptr, field->field_index);
            auto field_type_l = compile_type(field->resolved_type);
            auto fval = builder.CreateLoad(field_type_l, src_gep);
            compile_copy(fn, fval, dst_gep, field->resolved_type, nullptr);
        }
    }

    // Helper: check if a field type needs deep copy (destructor or Copy)
    auto field_needs_deep_copy = [&](ChiType *field_type) -> bool {
        if (get_resolver()->type_needs_destruction(field_type))
            return true;
        auto sty = get_resolver()->resolve_struct_type(eval_type(field_type));
        return sty && sty->member_intrinsics.get(IntrinsicSymbol::Copy);
    };

    // 3. Check if any variant has fields needing deep copy
    bool any_variant_needs = false;
    for (auto variant : enum_->variants) {
        if (auto vs = variant->resolved_type->data.enum_value.variant_struct) {
            for (auto field : vs->data.struct_.fields) {
                if (field_needs_deep_copy(field->resolved_type)) {
                    any_variant_needs = true;
                    break;
                }
            }
        }
        if (any_variant_needs)
            break;
    }

    if (any_variant_needs) {
        // Load discriminator from src, switch for variant fields
        auto disc_gep = builder.CreateStructGEP(enum_type_l, src_ptr, 0);
        auto disc = builder.CreateLoad(compile_type(enum_->discriminator), disc_gep, "disc");

        // Variant data index matches __data field_index used by compile_dot_access
        auto variant_data_idx = bvs ? (unsigned)bvs->data.struct_.fields.size() : 0u;

        auto bb_done = fn->new_label("enum_copy_done");
        auto sw = builder.CreateSwitch(disc, bb_done, enum_->variants.size());

        for (auto variant : enum_->variants) {
            auto vs = variant->resolved_type->data.enum_value.variant_struct;
            if (!vs)
                continue;

            // For variants with only trivial fields, copy the byte array via memcpy
            bool variant_has_nontrivial = false;
            for (auto field : vs->data.struct_.fields) {
                if (field_needs_deep_copy(field->resolved_type)) {
                    variant_has_nontrivial = true;
                    break;
                }
            }

            auto bb = fn->new_label(fmt::format("enum_copy_{}", variant->name));
            sw->addCase(
                llvm::ConstantInt::get((llvm::IntegerType *)compile_type(enum_->discriminator),
                                       variant->node->data.enum_variant.resolved_value),
                bb);
            fn->use_label(bb);

            auto src_data = builder.CreateStructGEP(enum_type_l, src_ptr, variant_data_idx);
            auto dst_data = builder.CreateStructGEP(enum_type_l, dest_ptr, variant_data_idx);
            auto vs_type_l = compile_type(vs);

            if (!variant_has_nontrivial) {
                // All trivial — memcpy the variant data
                auto vs_size = llvm_type_size(vs_type_l);
                builder.CreateMemCpy(dst_data, {}, src_data, {}, vs_size);
            } else {
                // Deep copy each variant field
                for (auto field : vs->data.struct_.fields) {
                    auto sf = builder.CreateStructGEP(vs_type_l, src_data, field->field_index);
                    auto df = builder.CreateStructGEP(vs_type_l, dst_data, field->field_index);
                    auto fval = builder.CreateLoad(compile_type(field->resolved_type), sf);
                    compile_copy(fn, fval, df, field->resolved_type, nullptr);
                }
            }
            builder.CreateBr(bb_done);
        }
        fn->use_label(bb_done);
    }

    builder.CreateRetVoid();

    // Restore insert point and debug location
    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }
    builder.SetCurrentDebugLocation(saved_dbg);

    return fn;
}

Function *Compiler::generate_copier_fixed_array(ChiType *type) {
    auto existing = m_ctx->copier_table.get(type);
    if (existing)
        return *existing;

    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    auto elem = type->data.fixed_array.elem;
    if (!get_resolver()->type_needs_destruction(elem)) {
        return nullptr;
    }

    auto ptr_type = get_llvm_ptr_type();
    auto fn_type_l =
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type, ptr_type}, false);
    auto type_name = get_resolver()->format_type_display(type);
    auto fn_name = fmt::format("{}.__copy", type_name);
    auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage, fn_name,
                                          m_ctx->llvm_module.get());
    auto fn = new Function(m_ctx, llvm_fn, nullptr);
    fn->qualified_name = fn_name;
    m_ctx->functions.emplace(fn);
    m_ctx->copier_table[type] = fn;

    auto saved_block = builder.GetInsertBlock();
    auto saved_point = builder.GetInsertPoint();

    auto i32_ty = llvm::Type::getInt32Ty(llvm_ctx);
    auto fa_size = type->data.fixed_array.size;
    auto arr_type_l = compile_type(type);
    auto elem_type_l = compile_type(elem);

    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    auto loop_bb = llvm::BasicBlock::Create(llvm_ctx, "loop", llvm_fn);
    auto body_bb = llvm::BasicBlock::Create(llvm_ctx, "body", llvm_fn);
    auto end_bb = llvm::BasicBlock::Create(llvm_ctx, "end", llvm_fn);

    builder.SetInsertPoint(entry_bb);
    auto dest_ptr = llvm_fn->getArg(0);
    auto src_ptr = llvm_fn->getArg(1);
    auto counter = builder.CreateAlloca(i32_ty, nullptr, "i");
    builder.CreateStore(llvm::ConstantInt::get(i32_ty, 0), counter);
    builder.CreateBr(loop_bb);

    builder.SetInsertPoint(loop_bb);
    auto cur = builder.CreateLoad(i32_ty, counter);
    auto cond = builder.CreateICmpULT(cur, llvm::ConstantInt::get(i32_ty, fa_size));
    builder.CreateCondBr(cond, body_bb, end_bb);

    builder.SetInsertPoint(body_bb);
    auto zero = llvm::ConstantInt::get(i32_ty, 0);
    auto src_elem = builder.CreateGEP(arr_type_l, src_ptr, {zero, cur});
    auto dst_elem = builder.CreateGEP(arr_type_l, dest_ptr, {zero, cur});
    auto val = builder.CreateLoad(elem_type_l, src_elem);
    compile_copy(fn, val, dst_elem, elem, nullptr);
    auto next = builder.CreateAdd(cur, llvm::ConstantInt::get(i32_ty, 1));
    builder.CreateStore(next, counter);
    builder.CreateBr(loop_bb);

    builder.SetInsertPoint(end_bb);
    builder.CreateRetVoid();

    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }
    return fn;
}

Function *
Compiler::generate_destructor_continuation(llvm::StructType *capture_struct_type,
                                           ChiType *promise_type,
                                           const std::vector<ast::Node *> &captured_vars) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    // Create function type: void __delete(T*)
    auto ptr_type = get_llvm_ptr_type();
    auto fn_type_l = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type}, false);

    // Generate unique name
    static int continuation_dtor_counter = 0;
    auto fn_name = fmt::format("__continuation_delete_{}", continuation_dtor_counter++);

    auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage, fn_name,
                                          m_ctx->llvm_module.get());

    // Create Function object
    auto fn = new Function(m_ctx, llvm_fn, nullptr);
    fn->qualified_name = fn_name;
    m_ctx->functions.emplace(fn);

    // Save current insert point
    auto saved_block = builder.GetInsertBlock();
    auto saved_point = builder.GetInsertPoint();

    // Create entry block
    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    builder.SetInsertPoint(entry_bb);

    auto this_ptr = llvm_fn->getArg(0);

    // Destroy field 0: Promise
    if (get_resolver()->type_needs_destruction(promise_type)) {
        auto promise_gep = builder.CreateStructGEP(capture_struct_type, this_ptr, 0);
        compile_destruction_for_type(fn, promise_gep, promise_type);
    }

    // Destroy fields 1..N: captured variables
    for (size_t i = 0; i < captured_vars.size(); i++) {
        auto var = captured_vars[i];
        auto var_type = get_chitype(var);

        if (get_resolver()->type_needs_destruction(var_type)) {
            auto var_gep = builder.CreateStructGEP(capture_struct_type, this_ptr, i + 1);
            compile_destruction_for_type(fn, var_gep, var_type);
        }
    }

    builder.CreateRetVoid();

    // Restore insert point
    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }

    return fn;
}

Function *Compiler::generate_constructor(ChiType *struct_type, ChiType *container_type,
                                         bool managed_variant, ast::Module *context_module) {
    // Check if already generated
    auto &ctor_table = managed_variant ? m_ctx->managed_constructor_table : m_ctx->constructor_table;
    auto existing = ctor_table.get(struct_type);
    if (existing) {
        return *existing;
    }

    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    // Resolve subtype if needed
    auto resolved_type = struct_type;
    while (resolved_type && resolved_type->kind == TypeKind::Subtype) {
        auto final_type = resolved_type->data.subtype.final_type;
        if (final_type) {
            resolved_type = final_type;
        } else {
            break;
        }
    }

    if (!resolved_type || resolved_type->kind != TypeKind::Struct) {
        return nullptr;
    }

    // Check if any field has a default value (or is an embedded struct field
    // whose own type carries defaults that we'd recursively initialize).
    auto &struct_data = resolved_type->data.struct_;
    bool has_defaults = false;
    for (auto field : struct_data.fields) {
        if (!field->node)
            continue;
        auto &var_decl = field->node->data.var_decl;
        if (var_decl.expr) {
            has_defaults = true;
            break;
        }
        if (var_decl.is_embed && var_decl.is_field) {
            auto embed_struct = get_resolver()->eval_struct_type(field->resolved_type);
            if (embed_struct && embed_struct->kind == TypeKind::Struct) {
                for (auto embed_field : embed_struct->data.struct_.fields) {
                    if (embed_field->node && embed_field->node->data.var_decl.expr) {
                        has_defaults = true;
                        break;
                    }
                }
                if (has_defaults) break;
            }
        }
    }

    if (!has_defaults) {
        // No defaults - no need for __new
        ctor_table[struct_type] = nullptr;
        return nullptr;
    }

    // Create function type: void __new(T*)
    auto struct_ptr_type = get_llvm_ptr_type();
    auto fn_type_l =
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {struct_ptr_type}, false);

    // Generate unique name for the constructor
    auto type_name = get_resolver()->format_type_qualified_name(
        struct_type,
        resolved_type->data.struct_.node ? resolved_type->data.struct_.node->module->global_id()
                                         : "");
    auto fn_name =
        managed_variant ? fmt::format("{}.__new.managed", type_name) : fmt::format("{}.__new", type_name);

    auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage, fn_name,
                                          m_ctx->llvm_module.get());

    // Create Function object
    auto fn = new Function(m_ctx, llvm_fn, nullptr);
    fn->qualified_name = fn_name;
    if (managed_variant) {
        fn->module = context_module;
    } else if (resolved_type->data.struct_.node) {
        fn->module = resolved_type->data.struct_.node->module;
    }
    m_ctx->functions.emplace(fn);
    ctor_table[struct_type] = fn;

    // Save current insert point and debug location
    auto saved_block = builder.GetInsertBlock();
    auto saved_point = builder.GetInsertPoint();
    auto saved_dbg = builder.getCurrentDebugLocation();

    auto saved_dbg_scope_len =
        attach_generated_debug_info(llvm_fn, fn_name, resolved_type->data.struct_.node);

    // Create entry block
    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    builder.SetInsertPoint(entry_bb);
    builder.SetCurrentDebugLocation(llvm::DebugLoc());

    auto this_ptr = llvm_fn->getArg(0);

    // Initialize all fields with default values
    for (auto field : struct_data.fields) {
        if (field->node) {
            emit_dbg_location(field->node);
        }
        emit_default_field_initializer(fn, this_ptr, resolved_type, field);
    }

    builder.CreateRetVoid();

    m_ctx->dbg_scopes.resize(saved_dbg_scope_len);

    // Restore insert point and debug location
    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }
    builder.SetCurrentDebugLocation(saved_dbg);

    return fn;
}

llvm::Value *Compiler::compile_block(Function *fn, ast::Node *parent, ast::Node *block,
                                     label_t *end_label, llvm::Value *var) {
    assert(block->type == ast::NodeType::Block);
    auto &data = block->data.block;
    auto &builder = *m_ctx->llvm_builder.get();
    end_label = end_label ? end_label : fn->next_end_label;
    llvm::Value *result = nullptr;

    for (auto var : data.implicit_vars) {
        compile_stmt(fn, var);
    }

    auto scope = fn->push_scope();
    fn->push_active_block(&data);
    for (auto var : data.stmt_temp_vars) {
        compile_stmt(fn, var);
    }

    for (int i = 0; i < (int)data.statements.size(); i++) {
        fn->active_block_stmt_idx.back() = i;
        compile_stmt(fn, data.statements[i]);
    }
    if (data.return_expr) {
        fn->active_block_stmt_idx.back() = (int)data.statements.size();
        if (var && parent) {
            result = compile_assignment_to_type(fn, data.return_expr, get_chitype(parent));
        } else {
            result = compile_expr(fn, data.return_expr);
        }
    }

    // Destroy block-local vars (only if block didn't already branch away via return/break
    // and current BB isn't already terminated by a nested return/break)
    if (!scope->branched && !builder.GetInsertBlock()->getTerminator()) {
        compile_block_cleanup(fn, &data, nullptr, data.exit_flow);
    }
    fn->pop_active_block();
    fn->pop_scope();

    if (data.return_expr) {
        if (var) {
            builder.CreateStore(result, var);
        }
        builder.CreateBr(end_label);
    } else {
        auto bb = builder.GetInsertBlock();
        auto term = bb->getTerminator();
        if (!term && end_label) {
            builder.CreateBr(end_label);
        }
    }

    return result;
}

Function *Compiler::add_fn(llvm::Function *llvm_fn, ast::Node *node, ChiType *fn_type) {
    auto fn = new Function(get_context(), llvm_fn, node);
    fn->fn_type = fn_type ? fn_type : get_chitype(node);
    return m_ctx->add_fn(node, fn);
}

Function *Compiler::get_fn(ast::Node *node) {
    auto id = get_resolver()->resolve_global_id(node);
    auto entry = m_ctx->function_table.get(id);

    // If not found and we have type_env, the function type may have placeholder
    // container types that need substitution to find the correct specialized method
    if (!entry && m_fn && m_fn->type_env && node->resolved_type &&
        node->resolved_type->is_placeholder) {
        auto fn_type = eval_type(node->resolved_type);
        if (fn_type->kind == TypeKind::Fn && fn_type->data.fn.container_ref) {
            auto container = fn_type->data.fn.container_ref->get_elem();
            auto subst_id = fmt::format(
                "{}.{}.{}", node->module->global_id(),
                get_resolver()->format_type_qualified_name(
                    container, node->module->global_id()),
                node->name);
            entry = m_ctx->function_table.get(subst_id);
            // On-demand compile: the concrete method exists but hasn't been compiled yet
            if (!entry) {
                auto concrete_struct = container;
                if (concrete_struct->kind == TypeKind::Subtype) {
                    if (!concrete_struct->data.subtype.final_type)
                        get_resolver()->resolve_subtype(concrete_struct);
                    concrete_struct = concrete_struct->data.subtype.final_type;
                }
                if (concrete_struct && concrete_struct->kind == TypeKind::Struct) {
                    auto *member = concrete_struct->data.struct_.find_member(node->name);
                    if (!member)
                        member = concrete_struct->data.struct_.find_static_member(node->name);
                    if (member && member->node &&
                        member->node->type == ast::NodeType::FnDef) {
                        auto compiled = compile_fn_proto(
                            member->node->data.fn_def.fn_proto, member->node);
                        if (!member->node->declspec_ref().is_extern())
                            m_ctx->pending_fns.add(compiled);
                        entry = m_ctx->function_table.get(subst_id);
                    }
                }
            }
        }
    }

    if (!entry && node->type == ast::NodeType::GeneratedFn) {
        auto target = node;
        if (m_fn && m_fn->type_env && node->data.generated_fn.fn_subtype->is_placeholder) {
            auto &gfn = node->data.generated_fn;
            array<ChiType *> concrete_args;
            for (auto arg : gfn.fn_subtype->data.subtype.args) {
                // Substitute placeholders without collapsing Subtype wrappers
                // (eval_type would collapse to final_type Struct, losing nesting
                // info needed for the generic-depth check in get_fn_variant).
                auto substituted =
                    get_resolver()->type_placeholders_sub_map(arg, m_fn->type_env);
                concrete_args.add(substituted);
            }
            target = get_resolver()->get_fn_variant(get_resolver()->node_get_type(gfn.original_fn),
                                                    &concrete_args, gfn.original_fn);
        }

        auto fn = compile_fn_proto(target->data.generated_fn.fn_proto, target, "");
        m_ctx->pending_fns.add(fn);
        entry = m_ctx->function_table.get(get_resolver()->resolve_global_id(target));
    }

    // Fallback for inherited default interface methods: try struct-qualified key
    if (!entry && node->data.fn_def.body && node->resolved_type &&
        node->resolved_type->kind == TypeKind::Fn) {
        auto container_ref = node->resolved_type->data.fn.container_ref;
        if (container_ref) {
            auto container = container_ref->get_elem();
            if (container && ChiTypeStruct::is_interface(container) && m_fn &&
                m_fn->container_type) {
                auto struct_type = m_fn->container_type;
                if (struct_type->kind == TypeKind::Subtype)
                    struct_type = struct_type->data.subtype.final_type;
                auto key =
                    fmt::format("{}.{}.{}", node->module->global_id(),
                                get_resolver()->format_type_qualified_name(
                                    struct_type, node->module->global_id()),
                                node->name);
                entry = m_ctx->function_table.get(key);
            }
        }
    }

    // On-demand proto compilation: the function's module may not have been
    // compiled yet (e.g. a generic stdlib type like Box<T> references a user
    // type's copy during the stdlib module pass). Compile the proto now
    // and schedule the body for later. This is safe because compile_fn_proto
    // only creates the LLVM declaration — no recursion into get_fn.
    if (!entry && node->type == ast::NodeType::FnDef && node->resolved_type &&
        !node->resolved_type->is_placeholder) {
        auto compiled = compile_fn_proto(node->data.fn_def.fn_proto, node);
        // Only schedule body compilation for non-extern functions
        if (!node->declspec_ref().is_extern()) {
            m_ctx->pending_fns.add(compiled);
        }
        entry = m_ctx->function_table.get(id);
    }

    if (!entry) {
        panic("Function not found: {}", id);
    }
    return *entry;
}

Function *Compiler::get_fn(ast::Node *node, ChiType *struct_type) {
    if (struct_type) {
        auto key = fmt::format("{}.{}.{}", node->module->global_id(),
                               get_resolver()->format_type_qualified_name(
                                   struct_type, node->module->global_id()),
                               node->name);
        auto entry = m_ctx->function_table.get(key);
        if (entry)
            return *entry;
    }
    return get_fn(node);
}

Function *Compiler::get_managed_fn(Function *base_fn, ast::Module *context_module) {
    assert(base_fn && "managed base function required");
    assert(context_module && "managed function variant requires context module");

    auto entry = m_ctx->managed_function_table.get(base_fn);
    if (entry) {
        return *entry;
    }

    auto *node = base_fn->node;
    assert(node && "managed function node required");
    auto name = get_resolver()->resolve_qualified_name(node) + ".managed";
    auto proto_node = node->type == ast::NodeType::GeneratedFn ? node->data.generated_fn.fn_proto
                                                               : node->data.fn_def.fn_proto;
    ScopedCodegenState saved_state(this);
    auto fn = compile_fn_proto(proto_node, node, name, base_fn->fn_type, false, context_module);
    fn->type_env = base_fn->type_env;
    fn->container_type = base_fn->container_type;
    fn->container_subtype = base_fn->container_subtype;
    fn->default_method_struct = base_fn->default_method_struct;
    fn->specialized_subtype = base_fn->specialized_subtype;
    m_ctx->managed_function_table[base_fn] = fn;
    m_fn = fn;
    return compile_fn_def(node, fn);
}

Function *Compiler::compile_fn_proto(ast::Node *proto_node, ast::Node *fn, string name,
                                     ChiType *fn_type_override, bool register_global,
                                     ast::Module *module_override) {
    auto declspec = fn->declspec_ref();
    auto subtype =
        fn->type == ast::NodeType::GeneratedFn ? fn->data.generated_fn.fn_subtype : nullptr;
    m_fn_eval_subtype = subtype;
    auto ftype = fn_type_override ? fn_type_override : get_chitype(fn);
    bool use_default_global_key = register_global && name.empty() && !module_override &&
                                  !subtype && !fn_type_override &&
                                  fn->type == ast::NodeType::FnDef;

    // Determine bind parameter information
    bool has_bind = false;
    string bind_name = "";

    // Handle specialization first, then lambda processing
    if (subtype) {
        assert(subtype->kind == TypeKind::Subtype);
        auto &subtype_data = subtype->data.subtype;
        ftype = eval_type(subtype_data.final_type);
        assert(ftype && ftype->kind == TypeKind::Fn);

        // Create a unique name for the specialized function
        if (name.empty()) {
            name = get_resolver()->resolve_qualified_name(fn);
        }
        name += ".<";
        for (auto arg : subtype_data.args) {
            name += get_resolver()->format_type_id(arg) + ",";
        }
        name += ">";

        // Track this function specialization for comparison with GenericResolver
        // Use the node's global_id (same as GenericResolver uses)
        m_ctx->compiled_generic_fns.insert(get_resolver()->resolve_global_id(fn));
    } else {
        // Handle lambda types for non-specialized functions only
        if (ftype->kind == TypeKind::FnLambda) {
            // Always add bind parameter for lambdas (even if no captures)
            // to match the bound_fn signature created in resolver
            auto &lambda_data = ftype->data.fn_lambda;
            has_bind = true;
            bind_name = "_binds";
            ftype = lambda_data.bound_fn;
            assert(ftype);
        }
    }

    assert(!ftype->is_placeholder && "Compiling placeholder type");

    if (ftype->kind == TypeKind::Fn) {
        if (ftype->data.fn.container_ref && !declspec.is_static()) {
            has_bind = true;
            bind_name = "this";
        }
    }

    if (name.empty()) {
        name = get_resolver()->resolve_qualified_name(fn);
    }

    if (use_default_global_key) {
        auto fn_id = get_resolver()->resolve_global_id(fn);
        if (auto existing_entry = m_ctx->function_table.get(fn_id)) {
            return *existing_entry;
        }
    }

    llvm::FunctionType *ftype_l = nullptr;
    if (declspec.has_flag(ast::DECL_IS_ENTRY)) {
        // C `main` signature: `int main(int, char**)`. Must return int so libc's
        // `__libc_start_main` gets a defined exit code (otherwise %eax is whatever
        // the last call left behind — 0 by luck normally, 1 under valgrind).
        ftype_l = llvm::FunctionType::get(llvm::Type::getInt32Ty(*m_ctx->llvm_ctx),
                                          {llvm::Type::getInt32Ty(*m_ctx->llvm_ctx),
                                           llvm::PointerType::get(*m_ctx->llvm_ctx, 0)},
                                          false);
    } else {
        ftype_l = (llvm::FunctionType *)compile_type(ftype);
    }

    // For extern C functions, reuse existing declaration if already compiled
    if (declspec.is_extern()) {
        auto id = get_resolver()->resolve_global_id(fn);
        auto existing_entry = m_ctx->function_table.get(id);
        if (existing_entry) {
            return *existing_entry;
        }
    }

    auto fn_l = llvm::Function::Create(ftype_l, llvm::Function::ExternalLinkage, name,
                                       m_ctx->llvm_module.get());
    fn_l->addAttributeAtIndex(llvm::AttributeList::FunctionIndex,
                              llvm::Attribute::get(*m_ctx->llvm_ctx, llvm::Attribute::NoInline));
    fn_l->addFnAttr("frame-pointer", "all");
    if (get_settings()->sanitize_address && !declspec.is_extern()) {
        fn_l->addFnAttr(llvm::Attribute::SanitizeAddress);
    }

    Function *new_fn = nullptr;
    if (register_global) {
        new_fn = add_fn(fn_l, fn, ftype);
    } else {
        new_fn = new Function(get_context(), fn_l, fn);
        new_fn->fn_type = ftype;
        m_ctx->functions.emplace(new_fn);
    }

    auto fn_id = get_resolver()->resolve_global_id(fn);
    if (subtype) {
        new_fn->specialized_subtype = subtype;
    }
    if (auto entry = get_resolver()->get_generics()->fn_envs.get(fn_id)) {
        new_fn->type_env = &entry->subs;
    } else if (subtype && is_verbose_generics()) {
        print("WARNING: No TypeEnv found for function: {}\n", fn_id);
    }

    if (ftype->kind == TypeKind::Fn && ftype->data.fn.container_ref) {
        auto container = ftype->data.fn.container_ref->get_elem();
        if (!new_fn->container_type) {
            new_fn->container_type = container;
        }
        if (container && container->kind == TypeKind::Subtype) {
            new_fn->container_subtype = &container->data.subtype;
            if (!new_fn->type_env) {
                if (auto entry =
                        get_resolver()->get_generics()->struct_envs.get(container->global_id)) {
                    new_fn->type_env = &entry->subs;
                }
            }
        } else if (container && container->kind == TypeKind::Struct &&
                   !container->global_id.empty() && !new_fn->type_env) {
            // When the container is already resolved to a Struct (e.g. from resolve_subtype
            // creating a final_type for a generic specialization like Shared<Inner<int>>),
            // the Struct shares the same global_id as the original Subtype. Look up
            // struct_envs to recover the type substitution map and container context.
            if (auto entry = get_resolver()->get_generics()->struct_envs.get(
                    container->global_id)) {
                new_fn->type_env = &entry->subs;
                if (entry->subtype) {
                    new_fn->container_type = entry->subtype;
                    new_fn->container_subtype = &entry->subtype->data.subtype;
                }
            }
        }
    }

    // Build parameter information
    std::vector<ParameterInfo> param_info;
    int llvm_index = 0;

    // Add sret parameter if needed
    bool has_sret = ftype->data.fn.should_use_sret() && !declspec.is_extern() &&
                    !declspec.has_flag(ast::DECL_IS_ENTRY);
    if (has_sret) {
        param_info.emplace_back(ParameterKind::SRet, llvm_index++, -1, "sret");
        fn_l->getArg(llvm_index - 1)->setName("sret");
    }

    // Add bind parameter if needed
    if (has_bind) {
        param_info.emplace_back(ParameterKind::Bind, llvm_index++, -1, bind_name);
        if (llvm_index - 1 >= fn_l->arg_size()) {
            printf("ERROR: Bind param index %d >= arg_size %u\n", llvm_index - 1,
                   (unsigned)fn_l->arg_size());
        } else {
            auto bind_param = fn_l->getArg(llvm_index - 1);
            bind_param->setName(bind_name);
            new_fn->bind_ptr = bind_param;
        }
    }

    // Add regular user parameters
    for (int user_idx = 0; user_idx < proto_node->data.fn_proto.params.size(); user_idx++) {
        auto param = proto_node->data.fn_proto.params[user_idx];
        auto param_name = param->name;
        auto &info =
            param_info.emplace_back(ParameterKind::Regular, llvm_index++, user_idx, param_name);
        if (subtype) {
            info.type = ftype->data.fn.params[user_idx];
        } else {
            info.type = get_chitype(param);
        }
        if (llvm_index - 1 >= fn_l->arg_size()) {
            printf("ERROR: User param index %d >= arg_size %u\n", llvm_index - 1,
                   (unsigned)fn_l->arg_size());
        } else {
            fn_l->getArg(llvm_index - 1)->setName(param_name);
        }
    }

    // Store parameter information
    new_fn->parameter_info = std::move(param_info);

    // For specialized functions, always use the specialized name
    if (subtype) {
        new_fn->qualified_name = name;
        new_fn->specialized_subtype = subtype;
    }
    else if (!register_global && !name.empty()) {
        new_fn->qualified_name = name;
    }
    // For lambda functions, use the passed name instead of the empty qualified_name
    else if (fn && fn->type == ast::NodeType::FnDef &&
             fn->data.fn_def.fn_kind == ast::FnKind::Lambda && !name.empty()) {
        new_fn->qualified_name = name;
    }

    if (module_override) {
        new_fn->module = module_override;
    }

    new_fn->llvm_fn->setName(new_fn->get_llvm_name());
    m_fn_eval_subtype = nullptr;
    return new_fn;
}

void Compiler::compile_extern(ast::Node *node) {
    auto &data = node->data.extern_decl;
    for (auto member : data.members) {
        auto &fn_data = member->data.fn_def;
        auto fn = compile_fn_proto(fn_data.fn_proto, member);
        auto id = get_resolver()->resolve_global_id(member);
        m_ctx->function_table.emplace(id, fn);
    }
}

llvm::Type *Compiler::compile_type(ChiType *type) {
    assert(type && "compile_type called with null type");

    type = eval_type(type);
    if (type->kind == TypeKind::EnumValue && type->data.enum_value.member) {
        auto base_value_type = type->data.enum_value.parent_enum()->base_value_type;
        if (m_fn && m_fn->type_env && base_value_type && base_value_type->is_placeholder) {
            base_value_type = get_resolver()->type_placeholders_sub_map(base_value_type, m_fn->type_env);
        }
        return compile_type(base_value_type);
    }
    assert(type && "eval_type returned null in compile_type");
    auto key = get_resolver()->format_type_id(type);
    // *Interface, &Interface, Mut<Interface>, and bare Interface are all the same fat pointer type
    if ((type->kind == TypeKind::Pointer || type->kind == TypeKind::Reference ||
         type->kind == TypeKind::MutRef ||
         type->kind == TypeKind::MoveRef) &&
        type->data.pointer.elem && ChiTypeStruct::is_interface(type->data.pointer.elem)) {
        key = "FatIFacePointer<" + get_resolver()->format_type_id(type->data.pointer.elem) + ">";
    }
    auto it = m_ctx->type_table.get(key);
    if (it) {
        return *it;
    }

    auto compiled_type = _compile_type(type);
    m_ctx->type_table[key] = compiled_type;
    return compiled_type;
}

llvm::Type *Compiler::_compile_type(ChiType *type) {
    if (type->is_placeholder && type->kind == TypeKind::Placeholder) {
        assert(false && "compile_type called on unresolved placeholder type");
    }
    auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
    switch (type->kind) {
    case TypeKind::This: {
        return compile_type(type->eval());
    }
    case TypeKind::Never:
    case TypeKind::Void: {
        return llvm::Type::getVoidTy(llvm_ctx);
    }
    case TypeKind::Bool: {
        return llvm::Type::getInt1Ty(llvm_ctx);
    }
    case TypeKind::Byte: {
        return llvm::Type::getInt8Ty(llvm_ctx);
    }
    case TypeKind::Rune: {
        return llvm::Type::getInt32Ty(llvm_ctx);
    }
    case TypeKind::Int: {
        return llvm::Type::getIntNTy(llvm_ctx, type->data.int_.bit_count);
    }
    case TypeKind::Float: {
        if (type->data.float_.bit_count == 64) {
            return llvm::Type::getDoubleTy(llvm_ctx);
        } else {
            return llvm::Type::getFloatTy(llvm_ctx);
        }
    }
    case TypeKind::String: {
        return llvm::StructType::create(
            {
                compile_type(get_resolver()->get_system_types()->str_lit),
                llvm::Type::getInt32Ty(llvm_ctx),
                llvm::Type::getInt32Ty(llvm_ctx),
            },
            "String");
    }
    case TypeKind::FnLambda: {
        return compile_type(type->data.fn_lambda.internal);
    }
    case TypeKind::Fn: {
        auto &data = type->data.fn;
        auto ret_type_l = compile_type(data.return_type);
        std::vector<llvm::Type *> param_types = {};
        if (data.should_use_sret()) {
            param_types.push_back(ret_type_l->getPointerTo());
            ret_type_l = llvm::Type::getVoidTy(llvm_ctx);
        }
        if (data.container_ref && !data.is_static) {
            param_types.push_back(compile_type(data.container_ref));
        }
        // For extern C variadic functions, exclude the varargs parameter from the param list
        // (LLVM handles it separately with the isVarArg flag)
        auto param_count = data.params.size();
        if (data.is_variadic && data.is_extern) {
            param_count = data.get_va_start();
        }
        for (size_t i = 0; i < param_count; i++) {
            auto param = data.params[i];
            param_types.push_back(compile_type(param));
        }
        // For extern variadic functions, use LLVM's native variadic support
        bool is_llvm_vararg = data.is_variadic && data.is_extern;
        return llvm::FunctionType::get(ret_type_l, param_types, is_llvm_vararg);
    }
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef: {
        auto &data = type->data.pointer;
        // Interface references are fat pointers {data_ptr, vtable_ptr}
        if (data.elem && ChiTypeStruct::is_interface(data.elem)) {
            std::vector<llvm::Type *> members;
            members.push_back(get_llvm_ptr_type()); // [0] data
            members.push_back(get_llvm_ptr_type()); // [1] vtable
            return llvm::StructType::create(
                members, "FatIFacePointer<" + get_resolver()->format_type_display(data.elem) + ">");
        }
        return get_llvm_ptr_type();
    }
    case TypeKind::Optional: {
        auto &data = type->data.pointer;
        auto elem_type_l = compile_type(data.elem);
        std::vector<llvm::Type *> members;
        members.push_back(llvm::Type::getInt1Ty(llvm_ctx)); // bool has_value
        members.push_back(elem_type_l);                     // elem
        return llvm::StructType::create(members, get_resolver()->format_type_display(type));
    }
    case TypeKind::Array: {
        auto internal = get_resolver()->eval_struct_type(type);
        assert(internal);
        return compile_type(internal);
    }
    case TypeKind::Span: {
        auto internal = get_resolver()->eval_struct_type(type);
        assert(internal);
        return compile_type(internal);
    }
    case TypeKind::FixedArray: {
        auto elem_type_l = compile_type(type->data.fixed_array.elem);
        return llvm::ArrayType::get(elem_type_l, type->data.fixed_array.size);
    }
    case TypeKind::Any: {
        std::vector<llvm::Type *> members;
        members.push_back(get_llvm_ptr_type());
        members.push_back(llvm::Type::getInt8Ty(llvm_ctx));
        auto any_storage_offset = offsetof(CxAny, storage);
        auto any_inlined_offset = offsetof(CxAny, inlined);
        auto any_padding = any_storage_offset - any_inlined_offset - sizeof(bool);
        if (any_padding > 0) {
            members.push_back(
                llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx), (uint32_t)any_padding));
        }
        members.push_back(
            llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx), (uint32_t)sizeof(CxAnyStorage)));
        return llvm::StructType::create(members, "Any");
    }
    case TypeKind::Struct: {
        auto key = get_resolver()->format_type_id(type);
        auto &data = type->data.struct_;
        auto own = data.own_fields();
        if (!own.size()) {
            // Empty structs need a placeholder byte for LLVM allocations
            // (void type cannot be allocated)
            std::vector<llvm::Type *> members;
            members.push_back(llvm::Type::getInt8Ty(llvm_ctx));
            return llvm::StructType::create(members, get_resolver()->format_type_display(type));
        }

        std::vector<llvm::Type *> members;
        for (auto &member : own) {
            members.push_back(compile_type(member->resolved_type));
        }
        return llvm::StructType::create(members, get_resolver()->format_type_display(type));
    }
    // Promise is now a Chi-native struct (TypeKind::Subtype), no special handling needed
    case TypeKind::Subtype: {
        assert(type->data.subtype.final_type && "compile_type called on unresolved subtype");
        return compile_type(type->data.subtype.final_type);
    }
    case TypeKind::Unit: {
        // Unit type: 1-byte placeholder (like empty struct)
        std::vector<llvm::Type *> members;
        members.push_back(llvm::Type::getInt8Ty(llvm_ctx));
        return llvm::StructType::create(members, "Unit");
    }
    case TypeKind::Tuple: {
        std::vector<llvm::Type *> members;
        for (auto elem : type->data.tuple.elements) {
            members.push_back(compile_type(elem));
        }
        return llvm::StructType::create(members, get_resolver()->format_type_display(type));
    }
    case TypeKind::Null: {
        return get_llvm_ptr_type();
    }
    case TypeKind::Placeholder: {
        return compile_type(get_system_types()->void_);
    }
    case TypeKind::Infer: {
        // For Infer types, use the inferred type if available
        auto inferred = type->data.infer.inferred_type;
        if (inferred) {
            return compile_type(inferred);
        }
        // Fallback to void if no inferred type (should not happen)
        return compile_type(get_system_types()->void_);
    }
    case TypeKind::Enum: {
        return compile_type(type->data.enum_.base_value_type);
    }
    case TypeKind::EnumValue: {
        if (type->data.enum_value.member) {
            return compile_type(type->data.enum_value.parent_enum()->base_value_type);
        }

        auto enum_ = type->data.enum_value.parent_enum();
        if (enum_->compiled_data_size < 0) {
            if (enum_->resolved_generic) {
                compile_concrete_enum(enum_);
            } else {
                compile_enum(enum_->node);
            }
            assert(enum_->compiled_data_size >= 0);
        }

        auto base_value_struct = type->data.enum_value.resolved_struct;
        std::vector<llvm::Type *> members;
        for (auto member : base_value_struct->data.struct_.members) {
            if (member->is_field()) {
                members.push_back(compile_type(member->resolved_type));
            }
        }
        if (enum_->base_struct) {
            for (auto member : enum_->base_struct->data.struct_.members) {
                if (member->is_field()) {
                    members.push_back(compile_type(member->resolved_type));
                }
            }
        }

        // variant data field
        members.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx),
                                               std::max(enum_->compiled_data_size, 1)));
        return llvm::StructType::create(members, get_resolver()->format_type_display(type));
    }
    case TypeKind::Undefined:
    case TypeKind::ZeroInit: {
        return get_llvm_ptr_type();
    }
    default:
        panic("compile_type not implemented for TypeKind::{} ({})", (int)type->kind,
              get_resolver()->format_type_display(type));
    }
    return nullptr;
}

void Compiler::emit_dbg_location(ast::Node *node) {
    auto builder = m_ctx->llvm_builder.get();
    if (!node) {
        return builder->SetCurrentDebugLocation(llvm::DebugLoc());
    }
    assert(node->token);
    auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
    llvm::DIScope *scope = m_ctx->dbg_cu;
    if (m_ctx->dbg_scopes.size()) {
        scope = m_ctx->dbg_scopes.last();
    }
    auto line_no = node->token->pos.line_number();
    auto col_no = node->token->pos.col_number();
    builder->SetCurrentDebugLocation(
        llvm::DILocation::get(llvm_ctx, line_no, col_no, scope, nullptr));
}

size_t Compiler::attach_generated_debug_info(llvm::Function *llvm_fn, const std::string &fn_name,
                                              ast::Node *anchor_node) {
    auto *mod = anchor_node ? anchor_node->module : nullptr;
    auto cu = mod ? get_module_cu(mod) : m_ctx->dbg_cu;
    auto dbg_builder = m_ctx->dbg_builder.get();
    auto file = dbg_builder->createFile(cu->getFilename(), cu->getDirectory());
    auto line_no = anchor_node ? anchor_node->token->pos.line_number() : 0;
    auto sp_type = dbg_builder->createSubroutineType(dbg_builder->getOrCreateTypeArray({}));
    auto sp = dbg_builder->createFunction(
        file, fn_name, llvm::StringRef(), file, line_no, sp_type, line_no,
        llvm::DINode::FlagArtificial, llvm::DISubprogram::SPFlagDefinition);
    llvm_fn->setSubprogram(sp);
    auto saved_len = m_ctx->dbg_scopes.size();
    m_ctx->dbg_scopes.add(sp);
    return saved_len;
}

void Compiler::emit_runtime_assert(Function *fn, llvm::Value *cond, llvm::Value *msg,
                                   ast::Node *site) {
    auto builder = m_ctx->llvm_builder.get();
    auto assert_fn = get_system_fn("assert");
    auto opt_string_type =
        get_resolver()->get_wrapped_type(get_system_types()->string, TypeKind::Optional);
    auto opt_msg_ptr = builder->CreateAlloca(compile_type(opt_string_type), nullptr, "assert_msg");
    auto opt_type_l = compile_type(opt_string_type);
    auto has_value_p = builder->CreateStructGEP(opt_type_l, opt_msg_ptr, 0);
    builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt1Ty(*m_ctx->llvm_ctx), 1),
                         has_value_p);
    auto value_p = builder->CreateStructGEP(opt_type_l, opt_msg_ptr, 1);
    compile_copy(fn, msg, value_p, get_system_types()->string, nullptr);
    auto opt_msg = builder->CreateLoad(opt_type_l, opt_msg_ptr);
    emit_dbg_location(site);

    if (site && site->module && site->token) {
        auto set_loc_fn = get_system_fn("cx_set_panic_location");
        auto clear_loc_fn = get_system_fn("cx_clear_panic_location");
        auto file_value = compile_string_literal(site->module->display_path());
        auto file_ptr = builder->CreateAlloca(compile_type(get_system_types()->string));
        builder->CreateStore(file_value, file_ptr);
        auto line = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*m_ctx->llvm_ctx),
                                           (uint32_t)site->token->pos.line_number());
        auto col = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*m_ctx->llvm_ctx),
                                          (uint32_t)site->token->pos.col_number());
        builder->CreateCall(set_loc_fn->llvm_fn, {file_ptr, line, col});
        builder->CreateCall(assert_fn->llvm_fn, {cond, opt_msg});
        builder->CreateCall(clear_loc_fn->llvm_fn, {});
        return;
    }

    builder->CreateCall(assert_fn->llvm_fn, {cond, opt_msg});
}

void Compiler::dump_generics_comparison() {
    if (!is_verbose_generics()) {
        return;
    }

    auto *generics = get_resolver()->get_generics();

    print("\n=== Codegen Compiled Generics ===\n");
    print("Functions ({}):\n", m_ctx->compiled_generic_fns.size());
    for (auto &id : m_ctx->compiled_generic_fns) {
        print("  [codegen] {}\n", id);
    }
    print("Structs ({}):\n", m_ctx->compiled_generic_structs.size());
    for (auto &id : m_ctx->compiled_generic_structs) {
        print("  [codegen] {}\n", id);
    }

    print("\n=== Comparison ===\n");

    // Check for functions in GenericResolver but not compiled
    print("Functions in GenericResolver but NOT compiled:\n");
    int fn_missing = 0;
    for (auto &[id, entry] : generics->fn_envs.data) {
        if (m_ctx->compiled_generic_fns.find(id) == m_ctx->compiled_generic_fns.end()) {
            print("  MISSING: {} (name: {})\n", id, entry.name);
            fn_missing++;
        }
    }
    if (fn_missing == 0)
        print("  (none)\n");

    // Check for functions compiled but not in GenericResolver
    print("Functions compiled but NOT in GenericResolver:\n");
    int fn_extra = 0;
    for (auto &id : m_ctx->compiled_generic_fns) {
        if (!generics->fn_envs.has_key(id)) {
            print("  EXTRA: {}\n", id);
            fn_extra++;
        }
    }
    if (fn_extra == 0)
        print("  (none)\n");

    // Check for structs in GenericResolver but not compiled
    print("Structs in GenericResolver but NOT compiled:\n");
    int struct_missing = 0;
    for (auto &[id, entry] : generics->struct_envs.data) {
        if (m_ctx->compiled_generic_structs.find(id) == m_ctx->compiled_generic_structs.end()) {
            print("  MISSING: {} (name: {})\n", id, entry.name);
            struct_missing++;
        }
    }
    if (struct_missing == 0)
        print("  (none)\n");

    // Check for structs compiled but not in GenericResolver
    print("Structs compiled but NOT in GenericResolver:\n");
    int struct_extra = 0;
    for (auto &id : m_ctx->compiled_generic_structs) {
        if (!generics->struct_envs.has_key(id)) {
            print("  EXTRA: {}\n", id);
            struct_extra++;
        }
    }
    if (struct_extra == 0)
        print("  (none)\n");

    print("\n=== Summary ===\n");
    print("GenericResolver: {} fns, {} structs\n", generics->fn_envs.size(),
          generics->struct_envs.size());
    print("Codegen compiled: {} fns, {} structs\n", m_ctx->compiled_generic_fns.size(),
          m_ctx->compiled_generic_structs.size());
    print("Missing from codegen: {} fns, {} structs\n", fn_missing, struct_missing);
    print("Extra in codegen (not tracked): {} fns, {} structs\n", fn_extra, struct_extra);
}

void Compiler::emit_output() {
#ifdef __APPLE__
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
#else
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmParser();
    llvm::InitializeNativeTargetAsmPrinter();
#endif

    string error;
    auto target_triple = llvm::sys::getDefaultTargetTriple();
    auto target = llvm::TargetRegistry::lookupTarget(target_triple, error);
    if (!target) {
        print("error: {}", error);
        return exit(1);
    }

    auto cpu = "generic";
    auto features = "";

    llvm::TargetOptions opt;
    auto codegen_opt = llvm::CodeGenOpt::None;
    if (get_settings()->profile == CompilationProfile::Release) {
        codegen_opt = llvm::CodeGenOpt::Aggressive;
    }
    auto target_machine =
        target->createTargetMachine(target_triple, cpu, features, opt, llvm::Reloc::PIC_,
                                    std::nullopt, codegen_opt);
    auto module = m_ctx->llvm_module.get();
    module->setDataLayout(target_machine->createDataLayout());
    module->setTargetTriple(target_triple);

    // Add the current debug info version into the module.
    module->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                          llvm::DEBUG_METADATA_VERSION);
    // Darwin only supports dwarf2.
    if (llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin())
        module->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 2);

    auto settings = get_settings();
    std::error_code ec;
    llvm::raw_fd_ostream dest_obj(settings->output_obj_to_file, ec, llvm::sys::fs::OF_None);
    if (ec) {
        print("error: could not open file: {}", ec.message());
        return exit(1);
    }

    llvm::legacy::PassManager pass;
    if (settings->profile == CompilationProfile::Release) {
        llvm::LoopAnalysisManager loop_am;
        llvm::FunctionAnalysisManager function_am;
        llvm::CGSCCAnalysisManager cgscc_am;
        llvm::ModuleAnalysisManager module_am;
        llvm::PassBuilder pass_builder(target_machine);
        pass_builder.registerModuleAnalyses(module_am);
        pass_builder.registerCGSCCAnalyses(cgscc_am);
        pass_builder.registerFunctionAnalyses(function_am);
        pass_builder.registerLoopAnalyses(loop_am);
        pass_builder.crossRegisterProxies(loop_am, function_am, cgscc_am, module_am);

        auto module_pass =
            pass_builder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
        module_pass.run(*module, module_am);
    }

    if (settings->sanitize_address) {
        llvm::LoopAnalysisManager loop_am;
        llvm::FunctionAnalysisManager function_am;
        llvm::CGSCCAnalysisManager cgscc_am;
        llvm::ModuleAnalysisManager module_am;
        llvm::PassBuilder pass_builder(target_machine);
        pass_builder.registerModuleAnalyses(module_am);
        pass_builder.registerCGSCCAnalyses(cgscc_am);
        pass_builder.registerFunctionAnalyses(function_am);
        pass_builder.registerLoopAnalyses(loop_am);
        pass_builder.crossRegisterProxies(loop_am, function_am, cgscc_am, module_am);

        llvm::AddressSanitizerOptions asan_opts;
        asan_opts.UseAfterScope = true;
        llvm::ModulePassManager asan_mpm;
        asan_mpm.addPass(llvm::AddressSanitizerPass(asan_opts));
        asan_mpm.run(*module, module_am);
    }

    pass.add((llvm::Pass *)llvm::createVerifierPass());
    if (target_machine->addPassesToEmitFile(pass, dest_obj, nullptr,
                                            llvm::CodeGenFileType::CGFT_ObjectFile)) {
        print("error: target_machine can't emit a file of this type");
        return exit(1);
    }

    if (!settings->output_ir_to_file.empty()) {
        llvm::raw_fd_ostream ir_dest(settings->output_ir_to_file, ec, llvm::sys::fs::OF_None);
        module->print(ir_dest, nullptr);
    }

    pass.run(*module);
    m_ctx->dbg_builder->finalize();
    dest_obj.flush();
}

Function *Compiler::get_system_fn(const string &name) {
    if (m_ctx->system_functions.has_key(name)) {
        return m_ctx->system_functions.at(name);
    }
    auto module = m_ctx->compilation_ctx->rt_module;
    assert(module && "runtime module not initialized");
    auto node = module->scope->find_one(name);
    if (!node) {
        panic("system function not found: {}", name);
    }
    assert(node && node->type == ast::NodeType::FnDef);
    auto fn = get_fn(node);
    m_ctx->system_functions[name] = fn;
    return fn;
}

} // namespace codegen
} // namespace cx
