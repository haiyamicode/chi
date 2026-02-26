#include "codegen.h"
#include "ast.h"
#include "context.h"
#include "enum.h"
#include "fmt/core.h"
#include "resolver.h"
#include "sema.h"
#include "util.h"
#include <set>

namespace cx {
namespace codegen {

typedef llvm::ArrayRef<llvm::Type *> TypeArray;

CodegenContext::~CodegenContext() {}
CodegenContext::CodegenContext(CompilationContext *compilation_ctx)
    : compilation_ctx(compilation_ctx), resolver(&compilation_ctx->resolve_ctx) {
    init_llvm();
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
    auto &builder = *ctx->llvm_builder.get();
    auto &block = llvm_fn->getEntryBlock();
    llvm::IRBuilder<> tmp(&block, block.begin());
    auto var = tmp.CreateAlloca(ty, 0, name);
    tmp.CreateMemSet(var, llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(*ctx->llvm_ctx), 0),
                     ctx->llvm_module->getDataLayout().getTypeAllocSize(ty), {});
    return var;
}

void CodegenContext::init_llvm() {
    llvm_ctx = std::make_unique<llvm::LLVMContext>();
    llvm_module = std::make_unique<llvm::Module>("main", *llvm_ctx);
    llvm_builder = std::make_unique<llvm::IRBuilder<>>(*llvm_ctx);
    dbg_builder = std::make_unique<llvm::DIBuilder>(*llvm_module);
}

Compiler::Compiler(CodegenContext *ctx) : m_ctx(ctx) {}

// Check if a type contains placeholder descendants in its params
// (is_placeholder may be false even when params contain placeholders)
static bool has_placeholder_descendants(ChiType *type) {
    switch (type->kind) {
    case TypeKind::Fn:
        for (auto p : type->data.fn.params) {
            if (p->is_placeholder || p->kind == TypeKind::Infer)
                return true;
        }
        if (type->data.fn.return_type &&
            (type->data.fn.return_type->is_placeholder ||
             type->data.fn.return_type->kind == TypeKind::Infer))
            return true;
        return false;
    case TypeKind::FnLambda:
        return has_placeholder_descendants(type->data.fn_lambda.fn);
    default:
        return false;
    }
}

ChiType *Compiler::eval_type(ChiType *type) {
    // Handle Infer types - extract the inferred concrete type
    if (type->kind == TypeKind::Infer && type->data.infer.inferred_type) {
        return eval_type(type->data.infer.inferred_type);
    }

    // Use TypeEnv from GenericResolver (set by compile_fn_proto)
    if (m_fn && m_fn->type_env && (type->is_placeholder || has_placeholder_descendants(type))) {
        type = get_resolver()->type_placeholders_sub_map(type, m_fn->type_env);
    }

    // Handle m_fn_eval_subtype (for function proto compilation, before m_fn exists)
    if (type->is_placeholder && m_fn_eval_subtype &&
        m_fn_eval_subtype->kind == TypeKind::Subtype) {
        type = get_resolver()->type_placeholders_sub(type, &m_fn_eval_subtype->data.subtype);
    }

    // Resolve special type kinds
    if (type->kind == TypeKind::Subtype) {
        return type->data.subtype.final_type;
    }
    if (type->kind == TypeKind::This && m_fn) {
        return m_fn->fn_type->data.fn.container_ref;
    }
    return type;
}

llvm::Type *Compiler::get_llvm_ptr_type() {
    auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
    return llvm::Type::getInt8PtrTy(llvm_ctx);
}

ChiType *Compiler::get_chitype(ast::Node *node) {
    if (m_fn && node->orig_type && node->orig_type->kind == TypeKind::This) {
        return m_fn->fn_type->data.fn.container_ref;
    }
    return eval_type(node->resolved_type);
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
    llvm::DICompileUnit* module_cu = nullptr;

    if (!is_virtual) {
        module_cu = m_ctx->dbg_builder->createCompileUnit(
            llvm::dwarf::DW_LANG_C,
            m_ctx->dbg_builder->createFile(module->filename, module->path, std::nullopt, std::nullopt),
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
                auto global = new llvm::GlobalVariable(
                    *m_ctx->llvm_module, var_type_l, false,
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

    while (m_ctx->pending_fns.len) {
        auto list = m_ctx->pending_fns;
        m_ctx->pending_fns.clear();
        for (auto fn : list) {
            m_fn = fn;
            compile_fn_def(fn->node, fn);
            m_fn = nullptr;
        }
    }
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
                if (!subtype_member) continue;
                fn_node = subtype_member->node;
            }

            auto fn_type = get_chitype(fn_node);

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
                            if (auto entry = get_resolver()->get_generics()->struct_envs.get(subtype->global_id)) {
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
                if (auto entry = get_resolver()->get_generics()->struct_envs.get(subtype->global_id)) {
                    fn->type_env = &entry->subs;
                } else if (getenv("DUMP_GENERICS")) {
                    print("WARNING: No TypeEnv found for struct method, struct: {}\n", subtype->global_id);
                }
            } else {
                fn->container_type = type;
            };
            m_ctx->pending_fns.add(fn);
        }
    }

    // Generate __delete and __copy before vtables (vtable needs these fn ptrs)
    if (get_resolver()->type_needs_destruction(type)) {
        generate_destructor(type, nullptr);
    }
    generate_copier(type);

    if (!subtype) {
        if (struct_type->data.struct_.interfaces.len) {
            compile_struct_vtables(struct_type);
        }
    }

    for (auto member : struct_type->data.struct_.members) {
        if (member->is_method() && !member->resolved_type->is_placeholder) {
            auto method_fn = get_fn(member->node);
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
    for (auto variant : node->data.enum_decl.variants) {
        auto variant_display_name = variant->resolved_type->get_display_name();
        auto display_name_str = (llvm::Constant *)compile_string_literal(variant_display_name);
        auto display_name_var =
            new llvm::GlobalVariable(*m_ctx->llvm_module, display_name_str->getType(), true,
                                     llvm::GlobalValue::InternalLinkage, display_name_str,
                                     fmt::format("{}.display_name", variant_display_name));
        auto variant_value = llvm::ConstantStruct::get(
            (llvm::StructType *)header_type_l,
            {
                llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(*m_ctx->llvm_ctx),
                                       variant->data.enum_variant.resolved_value),
                display_name_var,
            });
        auto var = new llvm::GlobalVariable(*m_ctx->llvm_module, header_type_l, true,
                                            llvm::GlobalValue::InternalLinkage, variant_value,
                                            fmt::format("{}.constant", variant_display_name));
        auto member_type = variant->resolved_type->data.enum_value.member;
        m_ctx->enum_variant_table[variant->data.enum_variant.resolved_enum_variant] = var;
    }

    // compile base struct methods
    if (auto base_struct = enum_type->data.enum_.base_struct) {
        for (auto member : base_struct->data.struct_.members) {
            if (member->is_method()) {
                auto fn_node = member->node;
                auto fn = compile_fn_proto(fn_node->data.fn_def.fn_proto, fn_node);
                m_ctx->pending_fns.add(fn);
            }
        }
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
    for (auto variant : enum_data->variants) {
        auto variant_display_name = variant->resolved_type->get_display_name();
        auto display_name_str = (llvm::Constant *)compile_string_literal(variant_display_name);
        auto display_name_var =
            new llvm::GlobalVariable(*m_ctx->llvm_module, display_name_str->getType(), true,
                                     llvm::GlobalValue::InternalLinkage, display_name_str,
                                     fmt::format("{}.display_name", variant_display_name));
        auto variant_value = llvm::ConstantStruct::get(
            (llvm::StructType *)header_type_l,
            {
                llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(*m_ctx->llvm_ctx),
                                       variant->node->data.enum_variant.resolved_value),
                display_name_var,
            });
        auto var = new llvm::GlobalVariable(*m_ctx->llvm_module, header_type_l, true,
                                            llvm::GlobalValue::InternalLinkage, variant_value,
                                            fmt::format("{}.constant", variant_display_name));
        m_ctx->enum_variant_table[variant] = var;
    }

    // Compile base struct methods (same as compile_enum)
    if (auto base_struct = enum_data->base_struct) {
        for (auto member : base_struct->data.struct_.members) {
            if (member->is_method()) {
                auto fn_node = member->node;
                auto fn = compile_fn_proto(fn_node->data.fn_def.fn_proto, fn_node);
                m_ctx->pending_fns.add(fn);
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
    case TypeKind::Bool: {
        return llvm_db.createBasicType("bool", 8, llvm::dwarf::DW_ATE_boolean);
    }
    case TypeKind::Char: {
        return llvm_db.createBasicType("char", 8, llvm::dwarf::DW_ATE_unsigned_char);
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
    case TypeKind::Struct: {
        auto &data = type->data.struct_;
        if (!data.fields.len) {
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
        auto scope = m_ctx->dbg_scopes.len ? m_ctx->dbg_scopes.last() : nullptr;
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
    if (!fn->llvm_fn->empty()) {
        panic("function already compiled");
        return nullptr;
    }

    // debug info
    auto name = fn->qualified_name;
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
            if (idx >= fn_proto.params.len) {
                printf("ERROR: idx %d >= proto.params.len %lu\n", idx, fn_proto.params.len);
                continue;
            }

            auto param = fn_proto.params[idx];
            auto param_type = param_info.type;
            // Never heap-allocate parameters - use stack allocation regardless of escape status
            // Parameters are function arguments and should always be stack-local
            auto var = fn->entry_alloca(compile_type(param_type), param->name);

            emit_dbg_location(param);

            compile_copy(fn, llvm_param, var, param_type);
            add_var(param, var);

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
        auto stack_marker = fn->return_value;
        if (!fn->return_value) {
            stack_marker = builder.CreateAlloca(llvm::IntegerType::getInt1Ty(llvm_ctx), nullptr,
                                                "_stack_marker");
        }
        builder.CreateCall(runtime_start->llvm_fn, {stack_marker});

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
                auto value = compile_assignment_to_type(fn, var_data.expr, var_type);
                if (value) {
                    compile_copy(fn, value, global_var, var_type, var_data.expr);
                }
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
    if (fn->get_def()->has_try_or_cleanup()) {
        fn->llvm_fn->setPersonalityFn(get_system_fn("cx_personality")->llvm_fn);
    }

    // clean up & return (block-local vars are destroyed at block exit or inline at return sites)
    fn->use_label(return_b);
    // Destroy any caught error objects still owned (from diverging catch blocks)
    for (auto &owner : fn->error_owner_vars) {
        auto owned_ptr = builder.CreateLoad(builder.getPtrTy(), owner.ptr_var);
        auto is_nonnull = builder.CreateICmpNE(
            owned_ptr, llvm::ConstantPointerNull::get(builder.getPtrTy()));
        auto do_free_b = fn->new_label("_err_cleanup");
        auto skip_free_b = fn->new_label("_err_cleanup_done");
        builder.CreateCondBr(is_nonnull, do_free_b, skip_free_b);
        fn->use_label(do_free_b);
        if (owner.concrete_type) {
            compile_destruction_for_type(fn, owned_ptr, owner.concrete_type);
        }
        builder.CreateCall(get_system_fn("cx_free")->llvm_fn, {owned_ptr});
        builder.CreateBr(skip_free_b);
        fn->use_label(skip_free_b);
    }
    for (auto ptr : fn->vararg_pointers) {
        emit_dbg_location(fn->node);
        builder.CreateCall(get_system_fn("cx_array_delete")->llvm_fn, {ptr});
    }
    if (is_entry) {
        auto runtime_stop = get_system_fn("cx_runtime_stop");
        builder.CreateCall(runtime_stop->llvm_fn, {});
    }
    if (return_type->kind == TypeKind::Void || return_type->kind == TypeKind::Never ||
        fn->use_sret()) {
        builder.CreateRetVoid();
    } else {
        auto value = builder.CreateLoad(return_type_l, fn->return_value);
        builder.CreateRet(value);
    }

    // cleanup landing pad (for handling exceptions)
    if (fn->cleanup_landing_label) {
        fn->use_label(fn->cleanup_landing_label);
        auto landing = builder.CreateLandingPad(m_ctx->get_caught_result_type(), 0);
        landing->setCleanup(true);
        builder.CreateExtractValue(landing, {0});
        builder.CreateExtractValue(landing, {1});
        // Destroy function body block's vars (conservative: covers all function-level locals)
        if (fn->get_def()->body) {
            compile_block_cleanup(fn, &fn->get_def()->body->data.block);
        }
        for (auto &owner : fn->error_owner_vars) {
            auto owned_ptr = builder.CreateLoad(builder.getPtrTy(), owner.ptr_var);
            auto is_nonnull = builder.CreateICmpNE(
                owned_ptr, llvm::ConstantPointerNull::get(builder.getPtrTy()));
            auto do_free_b = fn->new_label("_err_cleanup");
            auto skip_free_b = fn->new_label("_err_cleanup_done");
            builder.CreateCondBr(is_nonnull, do_free_b, skip_free_b);
            fn->use_label(do_free_b);
            if (owner.concrete_type) {
                compile_destruction_for_type(fn, owned_ptr, owner.concrete_type);
            }
            builder.CreateCall(get_system_fn("cx_free")->llvm_fn, {owned_ptr});
            builder.CreateBr(skip_free_b);
            fn->use_label(skip_free_b);
        }
        fn->insn_noop();
        builder.CreateResume(landing);
    }

    llvm::verifyFunction(*fn->llvm_fn);
    return fn;
}

llvm::Value *Compiler::compile_constant_value(Function *fn, const ConstantValue &value,
                                              ChiType *type, llvm::Type *llvm_type) {
    auto t = llvm_type ? llvm_type : compile_type(type);
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
    auto str_value = llvm::ConstantDataArray::getString(llvm_ctx, str, true);  // true = add null terminator
    auto char_array_type = llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx), str.size() + 1);
    auto str_global = new llvm::GlobalVariable(llvm_module, char_array_type, true,
                                               llvm::GlobalValue::PrivateLinkage, str_value, "cstr");
    // Return pointer to first element (char*)
    return llvm::ConstantExpr::getPointerCast(str_global, llvm::Type::getInt8PtrTy(llvm_ctx));
}

llvm::Value *Compiler::compile_assignment_to_type(Function *fn, ast::Node *expr,
                                                  ChiType *dest_type) {
    auto src_type = get_chitype(expr);

    // Check if src_type has placeholder type params (for generic struct field defaults)
    auto has_placeholder_params = [](ChiType *type) -> bool {
        if (type->is_placeholder) return true;
        if (type->kind == TypeKind::Struct && type->data.struct_.type_params.len > 0) {
            return true;
        }
        return false;
    };

    // Handle ConstructExpr with placeholder type - use dest_type for compilation
    if (expr->type == ast::NodeType::ConstructExpr && has_placeholder_params(src_type) &&
        dest_type && !has_placeholder_params(dest_type)) {
        auto &builder = *m_ctx->llvm_builder.get();
        auto ptr = fn->entry_alloca(compile_type(dest_type), "placeholder_tmp");
        compile_construction(fn, ptr, dest_type, expr);
        return builder.CreateLoad(compile_type(dest_type), ptr);
    }

    auto src_value = compile_expr(fn, expr);
    if (src_type->kind == TypeKind::Undefined) {
        return nullptr;
    }
    if (expr->type == ast::NodeType::ConstructExpr && src_type == dest_type) {
        return src_value;
    }
    auto value = src_value;
    if (dest_type) {
        emit_dbg_location(expr);
        value = compile_conversion(fn, src_value, src_type, dest_type);
    }
    return value;
}

void Compiler::compile_assignment_to_ptr(Function *fn, ast::Node *expr, llvm::Value *dest,
                                         ChiType *dest_type) {
    auto src_type = get_chitype(expr);
    // RVO: construct directly at destination when types match
    if (expr->type == ast::NodeType::ConstructExpr && src_type == dest_type) {
        compile_construction(fn, dest, dest_type, expr);
        return;
    }
    auto value = compile_assignment_to_type(fn, expr, dest_type);
    if (value) {
        compile_store_or_copy(fn, value, dest, dest_type, expr);
    }
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

void Compiler::compile_struct_vtables(ChiType *type) {
    array<CompiledVtable> vtables = {};
    int32_t count = 0;
    std::vector<llvm::Constant *> methods;

    assert(type->kind == TypeKind::Struct);

    // Get destructor for this concrete type (may be null)
    auto dtor_it = m_ctx->destructor_table.get(type);
    llvm::Constant *dtor_ptr = dtor_it && *dtor_it
        ? (llvm::Constant *)(*dtor_it)->llvm_fn
        : get_null_ptr();

    // Get copier for this concrete type (may be null)
    auto copier_it = m_ctx->copier_table.get(type);
    llvm::Constant *copier_ptr = copier_it && *copier_it
        ? (llvm::Constant *)(*copier_it)->llvm_fn
        : get_null_ptr();

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
                auto method_fn = get_fn(method->node);
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
            m_ctx->impl_table[vtable.impl] = llvm::ConstantExpr::getGetElementPtr(
                get_llvm_ptr_type(), global, idx);
        }
    }
}

llvm::Value *Compiler::compile_type_info(ChiType *type) {
    if (auto info = m_ctx->typeinfo_table.get(type)) {
        return *info;
    }

    auto type_l = compile_type(type);
    auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
    auto &llvm_module = *(m_ctx->llvm_module.get());
    auto tidata_type_l =
        llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx), sizeof(TypeInfoData));

    // Compile meta table
    auto meta_table_len = 0;
    TypeMetaEntry *meta_table_data = nullptr;

    if (type->kind == TypeKind::Struct || type->kind == TypeKind::Array) {
        auto sty = get_resolver()->resolve_struct_type(type);
        for (auto member : sty->members) {
            if (member->is_method() && member->vtable_index >= 0) {
                auto new_len = meta_table_len + 1;
                meta_table_data = reallocate_nonzero(meta_table_data, meta_table_len, new_len);
                auto new_entry = &meta_table_data[meta_table_len];
                new_entry->vtable_index = member->vtable_index;
                new_entry->symbol = member->symbol;
                auto member_name = member->get_name();
                new_entry->name_len = member_name.size();
                memset(new_entry->name, 0, sizeof(meta_table_data->name));
                memcpy(new_entry->name, member_name.data(), sizeof(member_name));
                meta_table_len = new_len;
            }
        }
    }

    auto meta_table_size = sizeof(TypeMetaEntry) * meta_table_len;
    auto ti_meta_table_l = llvm::ConstantDataArray::get(
        llvm_ctx, llvm::ArrayRef<uint8_t>((uint8_t *)meta_table_data, meta_table_size));
    auto ti_meta_table_type_l =
        llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx), meta_table_size);

    auto typesize = llvm_type_size(type_l);

    // For reference/pointer types, store elem TypeInfo* in the data field.
    // For other types, serialize the data as raw bytes.
    llvm::Constant *typedata_l;
    llvm::Type *tidata_actual_l;
    if ((type->kind == TypeKind::Reference || type->kind == TypeKind::MutRef ||
         type->kind == TypeKind::MoveRef || type->kind == TypeKind::Pointer ||
         type->kind == TypeKind::Optional) &&
        type->get_elem()) {
        auto ptr_type_l = llvm::PointerType::get(llvm_ctx, 0);
        tidata_actual_l = ptr_type_l;
        typedata_l = (llvm::Constant *)compile_type_info(type->get_elem());
    } else {
        tidata_actual_l = tidata_type_l;
        auto typedata = (uint8_t *)&type->data;
        typedata_l = llvm::ConstantDataArray::get(
            llvm_ctx, llvm::ArrayRef<uint8_t>(typedata, sizeof(TypeInfoData)));
    }

    auto ptr_type_l = llvm::PointerType::get(llvm_ctx, 0);

    // Generate destructor/copier wrappers for any type-erasure
    auto dtor_fn = generate_any_destructor(type);
    llvm::Constant *dtor_ptr = dtor_fn ? (llvm::Constant *)dtor_fn->llvm_fn : get_null_ptr();
    auto copy_fn = generate_any_copier(type);
    llvm::Constant *copy_ptr = copy_fn ? (llvm::Constant *)copy_fn->llvm_fn : get_null_ptr();

    auto ti_type_l = llvm::StructType::create(
        {llvm::Type::getInt32Ty(llvm_ctx), llvm::Type::getInt32Ty(llvm_ctx), tidata_actual_l,
         ptr_type_l, ptr_type_l,
         llvm::Type::getInt32Ty(llvm_ctx), ti_meta_table_type_l},
        "TypeInfo", true);

    auto info_l = llvm::ConstantStruct::get(
        ti_type_l,
        {
            /* kind */
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), (int32_t)type->kind),
            /* typesize */
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), (int32_t)typesize),
            /* data */
            typedata_l,
            /* destructor */
            dtor_ptr,
            /* copier */
            copy_ptr,
            /* vtable_len */
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), meta_table_len),
            /* vtable */
            ti_meta_table_l,
        });

    auto info_global =
        new llvm::GlobalVariable(llvm_module, ti_type_l, true, llvm::GlobalValue::PrivateLinkage,
                                 info_l, "typeinfo." + get_resolver()->format_type_display(type));
    return info_global;
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

    if (captures && captures->len) {
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
    if (captures && captures->len) {
        auto bstruct = lambda_type->data.fn_lambda.bind_struct;
        auto bstruct_l = compile_type(bstruct);

        // Create bind_struct on stack to hold captures
        auto bind_var = builder.CreateAlloca(bstruct_l, nullptr, "bind_struct");

        // Zero-init bind struct before populating
        auto bind_size_bytes = llvm_type_size(bstruct_l);
        builder.CreateMemSet(bind_var,
            llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(*m_ctx->llvm_ctx), 0),
            bind_size_bytes, {});

        // Store captures into bind_struct
        for (int i = 0; i < captures->len; i++) {
            auto &cap = (*captures)[i];
            auto capture = cap.decl;
            auto capture_gep = builder.CreateStructGEP(bstruct_l, bind_var, i);

            // Get source address of the captured variable
            llvm::Value *src_addr = nullptr;
            auto &current_captures = fn->node->data.fn_def.captures;
            for (int j = 0; j < current_captures.len; j++) {
                if (current_captures[j].decl == capture && fn->bind_ptr) {
                    auto current_fn_type = get_chitype(fn->node);
                    if (current_fn_type->kind == TypeKind::FnLambda) {
                        auto current_bstruct = current_fn_type->data.fn_lambda.bind_struct;
                        auto current_bstruct_l =
                            (llvm::StructType *)compile_type(current_bstruct);
                        auto nested_gep =
                            builder.CreateStructGEP(current_bstruct_l, fn->bind_ptr, j);
                        src_addr = builder.CreateLoad(current_bstruct_l->elements()[j],
                                                       nested_gep);
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
        }

        // Generate destructor for CxCapture cleanup
        llvm::Value *dtor_ptr = llvm::ConstantPointerNull::get(builder.getInt8PtrTy());
        if (get_resolver()->type_needs_destruction(bstruct)) {
            if (auto dtor = generate_destructor(bstruct, nullptr)) {
                dtor_ptr = builder.CreateBitCast(dtor->llvm_fn, builder.getInt8PtrTy());
            }
        }

        // Allocate type-erased capture box and store bind struct into it
        auto captures_ti = compile_type_info(bstruct);
        auto captures_ti_ptr = builder.CreateBitCast(captures_ti, builder.getInt8PtrTy());

        auto [capture_ptr, payload_data_ptr] = compile_cxcapture_create(bind_size, captures_ti_ptr, dtor_ptr);

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

// Helper to check if an expression contains an await
static bool contains_await(ast::Node *node) {
    if (!node) return false;
    if (node->type == ast::NodeType::AwaitExpr) return true;

    switch (node->type) {
    case ast::NodeType::DestructureDecl:
        return contains_await(node->data.destructure_decl.expr);
    case ast::NodeType::VarDecl: {
        auto &data = node->data.var_decl;
        return contains_await(data.expr);
    }
    case ast::NodeType::ReturnStmt:
        return contains_await(node->data.return_stmt.expr);
    case ast::NodeType::ThrowStmt:
        return contains_await(node->data.throw_stmt.expr);
    case ast::NodeType::BinOpExpr:
        return contains_await(node->data.bin_op_expr.op1) ||
               contains_await(node->data.bin_op_expr.op2);
    case ast::NodeType::FnCallExpr:
        for (int i = 0; i < node->data.fn_call_expr.args.len; i++) {
            if (contains_await(node->data.fn_call_expr.args[i])) return true;
        }
        return contains_await(node->data.fn_call_expr.fn_ref_expr);
    case ast::NodeType::UnaryOpExpr:
        return contains_await(node->data.unary_op_expr.op1);
    default:
        return false;
    }
}

// Find the await expression within a node
static ast::Node *find_await_expr(ast::Node *node) {
    if (!node) return nullptr;
    if (node->type == ast::NodeType::AwaitExpr) return node;

    switch (node->type) {
    case ast::NodeType::DestructureDecl:
        return find_await_expr(node->data.destructure_decl.expr);
    case ast::NodeType::VarDecl:
        return find_await_expr(node->data.var_decl.expr);
    case ast::NodeType::ReturnStmt:
        return find_await_expr(node->data.return_stmt.expr);
    case ast::NodeType::ThrowStmt:
        return find_await_expr(node->data.throw_stmt.expr);
    case ast::NodeType::BinOpExpr: {
        auto lhs = find_await_expr(node->data.bin_op_expr.op1);
        return lhs ? lhs : find_await_expr(node->data.bin_op_expr.op2);
    }
    case ast::NodeType::FnCallExpr:
        for (int i = 0; i < node->data.fn_call_expr.args.len; i++) {
            auto arg = find_await_expr(node->data.fn_call_expr.args[i]);
            if (arg) return arg;
        }
        return find_await_expr(node->data.fn_call_expr.fn_ref_expr);
    case ast::NodeType::UnaryOpExpr:
        return find_await_expr(node->data.unary_op_expr.op1);
    default:
        return nullptr;
    }
}

void Compiler::collect_vars_used_in_node(ast::Node *node, std::set<ast::Node *> &vars) {
    if (!node) return;

    switch (node->type) {
    case ast::NodeType::Identifier: {
        auto decl = node->data.identifier.decl;
        if (decl && decl->type == ast::NodeType::VarDecl) {
            vars.insert(decl);
        }
        break;
    }
    case ast::NodeType::DestructureDecl:
        collect_vars_used_in_node(node->data.destructure_decl.expr, vars);
        break;
    case ast::NodeType::VarDecl:
        collect_vars_used_in_node(node->data.var_decl.expr, vars);
        break;
    case ast::NodeType::ReturnStmt:
        collect_vars_used_in_node(node->data.return_stmt.expr, vars);
        break;
    case ast::NodeType::ThrowStmt:
        collect_vars_used_in_node(node->data.throw_stmt.expr, vars);
        break;
    case ast::NodeType::BinOpExpr:
        collect_vars_used_in_node(node->data.bin_op_expr.op1, vars);
        collect_vars_used_in_node(node->data.bin_op_expr.op2, vars);
        break;
    case ast::NodeType::FnCallExpr:
        collect_vars_used_in_node(node->data.fn_call_expr.fn_ref_expr, vars);
        for (int i = 0; i < node->data.fn_call_expr.args.len; i++) {
            collect_vars_used_in_node(node->data.fn_call_expr.args[i], vars);
        }
        break;
    case ast::NodeType::AwaitExpr:
        collect_vars_used_in_node(node->data.await_expr.expr, vars);
        break;
    case ast::NodeType::Block:
        for (int i = 0; i < node->data.block.statements.len; i++) {
            collect_vars_used_in_node(node->data.block.statements[i], vars);
        }
        break;
    case ast::NodeType::UnaryOpExpr:
        collect_vars_used_in_node(node->data.unary_op_expr.op1, vars);
        break;
    default:
        break;
    }
}

std::vector<AsyncSegment> Compiler::collect_async_segments(ast::Node *body) {
    std::vector<AsyncSegment> segments;
    AsyncSegment current;

    auto &stmts = body->data.block.statements;
    for (int i = 0; i < stmts.len; i++) {
        auto stmt = stmts[i];
        if (contains_await(stmt)) {
            // This statement has an await - it ends the current segment
            current.await_expr = find_await_expr(stmt);
            current.await_value_type = get_chitype(current.await_expr);

            // If the await is in a var decl, store it for later use
            if (stmt->type == ast::NodeType::VarDecl) {
                current.await_var_decl = stmt;
            } else {
                current.stmts.push_back(stmt);
            }

            segments.push_back(current);
            current = AsyncSegment();
        } else {
            current.stmts.push_back(stmt);
        }
    }

    // Add the final segment (code after last await, including return)
    if (!current.stmts.empty()) {
        segments.push_back(current);
    }

    // Now compute vars_to_capture for each segment
    // A var defined in segment i needs to be captured if used in segment j (j > i)
    // Note: await_var_decl for segment i is NOT captured by segment i - it's passed as the value parameter
    std::set<ast::Node *> defined_vars;
    for (size_t i = 0; i < segments.size(); i++) {
        // Collect vars defined in this segment's statements
        for (auto stmt : segments[i].stmts) {
            if (stmt->type == ast::NodeType::VarDecl) {
                defined_vars.insert(stmt);
            }
        }

        // For subsequent segments, find which vars they use
        for (size_t j = i + 1; j < segments.size(); j++) {
            std::set<ast::Node *> used_vars;
            for (auto stmt : segments[j].stmts) {
                collect_vars_used_in_node(stmt, used_vars);
            }
            // Also check the await expression itself (argument to await)
            if (segments[j].await_expr) {
                collect_vars_used_in_node(segments[j].await_expr->data.await_expr.expr, used_vars);
            }

            // Any var in defined_vars that is in used_vars must be captured
            for (auto var : used_vars) {
                if (defined_vars.count(var)) {
                    segments[i].vars_to_capture.insert(var);
                }
            }
        }

        // Add await_var_decl to defined_vars AFTER computing this segment's captures
        // This is because await_var_decl is passed as value parameter, not captured
        if (segments[i].await_var_decl) {
            defined_vars.insert(segments[i].await_var_decl);
        }
    }

    return segments;
}

llvm::Function *Compiler::create_continuation_fn_decl(AsyncContext &ctx, int segment_index) {
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto &llvm_module = *m_ctx->llvm_module;
    auto parent_fn = ctx.parent_fn;

    // Get the previous segment's await value type (this is what we receive as callback arg)
    auto &prev_segment = ctx.segments[segment_index - 1];
    auto value_type_l = compile_type(prev_segment.await_value_type);

    // Create the continuation function: void __fnname_cont_N(void* data, T value)
    // This matches the lambda calling convention: bound_fn(data, arg0, arg1, ...)
    auto void_type = llvm::Type::getVoidTy(llvm_ctx);
    auto ptr_type = llvm::PointerType::get(llvm_ctx, 0);
    auto fn_type = llvm::FunctionType::get(void_type, {ptr_type, value_type_l}, false);
    auto fn_name = parent_fn->qualified_name + "__cont_" + std::to_string(segment_index);
    auto llvm_fn = llvm::Function::Create(fn_type, llvm::Function::PrivateLinkage, fn_name, llvm_module);

    // Create a Function object for code generation
    auto cont_fn = new Function(m_ctx, llvm_fn, parent_fn->node);
    cont_fn->qualified_name = fn_name;

    // Set up entry block
    auto entry_b = cont_fn->new_label("_entry");
    cont_fn->use_label(entry_b);

    // Store the Function object for later body generation
    ctx.continuation_fn_objects.push_back(cont_fn);

    return llvm_fn;
}

std::pair<llvm::StructType *, std::vector<ast::Node *>>
Compiler::get_continuation_capture_info(AsyncContext &ctx, int segment_index) {
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    int prev_segment_idx = segment_index - 1;
    auto &prev_segment = ctx.segments[prev_segment_idx];

    // Build capture struct type: { result_promise, captured_vars... }
    // Store Promise by VALUE (not pointer) so it survives after stack frame is gone
    std::vector<llvm::Type *> capture_types;
    capture_types.push_back(ctx.promise_struct_type); // result promise VALUE

    std::vector<ast::Node *> captured_vars_ordered;
    for (auto var : prev_segment.vars_to_capture) {
        captured_vars_ordered.push_back(var);
        capture_types.push_back(compile_type(get_chitype(var)));
    }

    auto capture_struct_type = llvm::StructType::get(llvm_ctx, capture_types);
    return {capture_struct_type, captured_vars_ordered};
}

void Compiler::generate_async_continuation_body(AsyncContext &ctx, int segment_index) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    auto &segment = ctx.segments[segment_index];

    // Get the continuation function object (index in continuation arrays is segment_index - 1)
    int cont_idx = segment_index - 1;
    auto cont_fn = ctx.continuation_fn_objects[cont_idx];
    auto llvm_fn = ctx.continuations[cont_idx];

    // Position builder at entry block (already set up in create_continuation_fn_decl)
    builder.SetInsertPoint(&llvm_fn->getEntryBlock());

    // Get the arguments: data (captures) and value (resolved promise value)
    auto data_arg = llvm_fn->getArg(0);
    auto value_arg = llvm_fn->getArg(1);

    // Get capture struct type and ordered list of captured variables
    auto [capture_struct_type, captured_vars_ordered] = get_continuation_capture_info(ctx, segment_index);
    auto captures_ptr = builder.CreateBitCast(data_arg, capture_struct_type->getPointerTo());

    // Previous segment (needed for await_var_decl)
    auto &prev_segment = ctx.segments[segment_index - 1];

    // Get pointer to the captured result promise (stored by value in captures)
    auto result_promise_ptr = builder.CreateStructGEP(capture_struct_type, captures_ptr, 0);

    // Extract captured variables and add to var table
    map<ast::Node *, llvm::Value *> local_vars;
    for (size_t i = 0; i < captured_vars_ordered.size(); i++) {
        auto var = captured_vars_ordered[i];
        auto var_type = get_chitype(var);
        auto var_type_l = compile_type(var_type);

        // Allocate local storage for the captured variable
        auto local_alloc = cont_fn->entry_alloca(var_type_l, var->name);
        auto captured_value_ptr = builder.CreateStructGEP(capture_struct_type, captures_ptr, i + 1);
        auto captured_value = builder.CreateLoad(var_type_l, captured_value_ptr);
        builder.CreateStore(captured_value, local_alloc);

        local_vars[var] = local_alloc;
        add_var(var, local_alloc);
    }

    // Handle the await result from previous segment
    if (prev_segment.await_var_decl) {
        auto var = prev_segment.await_var_decl;
        auto var_type = get_chitype(var);
        auto var_type_l = compile_type(var_type);

        // Allocate storage for the await result
        auto var_alloc = cont_fn->entry_alloca(var_type_l, var->name);

        // value_arg is the value itself (passed by value from the callback)
        builder.CreateStore(value_arg, var_alloc);

        local_vars[var] = var_alloc;
        add_var(var, var_alloc);
    }

    // Check if this is the final segment (contains return)
    bool is_final = (segment_index == (int)ctx.segments.size() - 1);

    // Push scope for continuation body
    cont_fn->push_scope();

    // Compile the segment's statements
    for (auto stmt : segment.stmts) {
        if (stmt->type == ast::NodeType::ReturnStmt) {
            // For return in async function, resolve the result promise
            auto &data = stmt->data.return_stmt;
            // Compile return value, or synthesize Unit{} for bare `return;`
            llvm::Value *ret_value;
            if (data.expr) {
                ret_value = compile_expr(cont_fn, data.expr);
            } else {
                auto inner_type = get_resolver()->get_promise_value_type(ctx.promise_type);
                ret_value = llvm::Constant::getNullValue(compile_type(inner_type));
            }

            auto promise_struct = get_resolver()->resolve_struct_type(ctx.promise_type);
            std::optional<TypeId> variant_type_id = std::nullopt;
            if (ctx.promise_type->kind == TypeKind::Subtype && !ctx.promise_type->is_placeholder) {
                variant_type_id = ctx.promise_type->id;
            }
            auto resolve_member = promise_struct->find_member("resolve");
            assert(resolve_member && "Promise.resolve() method not found");
            auto resolve_method_node = get_variant_member_node(resolve_member, variant_type_id);
            auto resolve_method = get_fn(resolve_method_node);
            builder.CreateCall(resolve_method->llvm_fn, {result_promise_ptr, ret_value});
        } else if (contains_await(stmt)) {
            // This segment has another await - chain to next continuation
            auto await_expr = find_await_expr(stmt);
            emit_promise_chain(cont_fn, ctx, await_expr, segment_index + 1, local_vars, result_promise_ptr);
        } else {
            compile_stmt(cont_fn, stmt);
        }
    }

    // If segment has an await (not final), chain to next continuation
    if (segment.await_expr && !is_final) {
        emit_promise_chain(cont_fn, ctx, segment.await_expr, segment_index + 1, local_vars, result_promise_ptr);
    }

    cont_fn->pop_scope();
    builder.CreateRetVoid();

    // Clean up temporary vars from var_table
    for (auto &kv : local_vars.data) {
        m_ctx->var_table.unset(kv.first);
    }

    llvm::verifyFunction(*llvm_fn);
}

llvm::Value *Compiler::build_continuation_lambda(Function *fn, AsyncContext &ctx, int segment_index,
                                                  map<ast::Node *, llvm::Value *> &local_vars,
                                                  llvm::Value *result_promise_ptr) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    // Get capture struct type and ordered list of captured variables
    auto [capture_struct_type, captured_vars_ordered] = get_continuation_capture_info(ctx, segment_index);
    auto capture_size = m_ctx->llvm_module->getDataLayout().getTypeAllocSize(capture_struct_type);
    auto ptr_type = llvm::PointerType::get(llvm_ctx, 0);

    // Generate destructor for capture struct (called by cx_capture_release when refcount hits 0)
    llvm::Value *dtor_ptr = llvm::ConstantPointerNull::get(ptr_type);
    bool needs_destruction = get_resolver()->type_needs_destruction(ctx.promise_type);
    if (!needs_destruction) {
        for (auto var : captured_vars_ordered) {
            if (get_resolver()->type_needs_destruction(get_chitype(var))) {
                needs_destruction = true;
                break;
            }
        }
    }
    if (needs_destruction) {
        auto dtor = generate_destructor_continuation(capture_struct_type, ctx.promise_type, captured_vars_ordered);
        if (dtor) {
            dtor_ptr = builder.CreateBitCast(dtor->llvm_fn, ptr_type);
        }
    }

    // Allocate capture via CxCapture (refcounted, type-erased)
    auto null_ti = llvm::ConstantPointerNull::get(ptr_type); // no Chi type for ad-hoc capture struct
    auto [capture_ptr, captures_payload_ptr] = compile_cxcapture_create((uint32_t)capture_size, null_ti, dtor_ptr);
    auto captures_ptr = builder.CreateBitCast(captures_payload_ptr, capture_struct_type->getPointerTo());

    // Zero-init the payload before populating (needed for compile_copy on Promise,
    // since copy_from may release the old value which would be garbage otherwise)
    builder.CreateMemSet(captures_payload_ptr,
        llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(llvm_ctx), 0),
        capture_size, {});

    // Store result promise VALUE (not pointer) - copy it so it survives after stack frame is gone
    auto result_gep = builder.CreateStructGEP(capture_struct_type, captures_ptr, 0);
    auto result_promise_val = builder.CreateLoad(ctx.promise_struct_type, result_promise_ptr);
    compile_copy(fn, result_promise_val, result_gep, ctx.promise_type);

    // Store captured variables
    for (size_t i = 0; i < captured_vars_ordered.size(); i++) {
        auto var = captured_vars_ordered[i];
        auto var_type = get_chitype(var);
        auto var_type_l = compile_type(var_type);

        llvm::Value *var_ptr;
        if (local_vars.has_key(var)) {
            var_ptr = local_vars[var];
        } else {
            var_ptr = get_var(var);
        }
        auto var_value = builder.CreateLoad(var_type_l, var_ptr);

        auto var_gep = builder.CreateStructGEP(capture_struct_type, captures_ptr, i + 1);
        builder.CreateStore(var_value, var_gep);
    }

    // Build __CxLambda struct
    emit_dbg_location(fn->node);
    auto cont_fn_llvm = ctx.continuations[segment_index - 1];
    auto [lambda_alloca, lambda_struct_type_l] = compile_cxlambda_init(fn, cont_fn_llvm, (uint32_t)capture_size);
    compile_cxlambda_set_captures(lambda_alloca, capture_ptr);

    return builder.CreateLoad(lambda_struct_type_l, lambda_alloca);
}

void Compiler::emit_promise_chain(Function *fn, AsyncContext &ctx, ast::Node *await_expr,
                                   int next_segment_index, map<ast::Node *, llvm::Value *> &local_vars,
                                   llvm::Value *result_promise_ptr) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    // Compile the await expression to get the promise
    auto awaited_promise = compile_expr(fn, await_expr->data.await_expr.expr);

    // Get promise type info
    auto promise_type = get_chitype(await_expr->data.await_expr.expr);
    auto promise_struct_type = get_resolver()->resolve_struct_type(promise_type);
    auto promise_type_l = compile_type(promise_type);

    // Allocate storage for the promise and properly copy it
    // Using compile_copy ensures copy_from is called, which increments Shared ref count
    auto awaited_promise_ptr = fn->entry_alloca(promise_type_l, "awaited");
    // Zero-initialize the destination before copy to avoid garbage in Shared.data
    auto size = m_ctx->llvm_module->getDataLayout().getTypeAllocSize(promise_type_l);
    builder.CreateMemSet(awaited_promise_ptr,
        llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(llvm_ctx), 0),
        size, {});
    compile_copy(fn, awaited_promise, awaited_promise_ptr, promise_type, await_expr->data.await_expr.expr);

    // Build continuation lambda for next segment
    auto lambda = build_continuation_lambda(fn, ctx, next_segment_index, local_vars, result_promise_ptr);

    // Call Promise.then(callback) to register the continuation
    // Use variant lookup to get the specialized Promise<T>.then() method
    auto then_member = promise_struct_type->find_member("then");
    assert(then_member && "Promise.then() method not found");
    std::optional<TypeId> variant_type_id = std::nullopt;
    if (promise_type->kind == TypeKind::Subtype && !promise_type->is_placeholder) {
        variant_type_id = promise_type->id;
    }
    auto then_method_node = get_variant_member_node(then_member, variant_type_id);
    auto then_method = get_fn(then_method_node);
    emit_dbg_location(await_expr);
    builder.CreateCall(then_method->llvm_fn, {awaited_promise_ptr, lambda});
}

void Compiler::compile_async_fn_body(Function *fn) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    auto fn_def = fn->get_def();
    auto body = fn_def->body;

    // Get return type (Promise<T>)
    auto return_type = fn->fn_type->data.fn.return_type;
    assert(get_resolver()->is_promise_type(return_type));

    // Promise<T> is now a Chi-native struct, compile it directly
    auto promise_struct_type_l = (llvm::StructType *)compile_type(return_type);

    // Create AsyncContext
    AsyncContext ctx;
    ctx.parent_fn = fn;
    ctx.promise_type = return_type;
    ctx.promise_struct_type = promise_struct_type_l;
    ctx.segments = collect_async_segments(body);

    // Check if there are any awaits - if the first segment has no await_expr, there are no awaits
    if (ctx.segments.empty() || !ctx.segments[0].await_expr) {
        // No awaits - just compile normally
        compile_block(fn, fn->node, body, fn->return_label);
        return;
    }

    // Allocate result promise
    ctx.result_promise_ptr = fn->entry_alloca(promise_struct_type_l, "result_promise");

    // Initialize promise by calling Promise.new() method
    // Use variant lookup to get the specialized Promise<T>.new() method
    auto promise_struct = get_resolver()->resolve_struct_type(return_type);
    auto new_member = promise_struct->find_member("new");
    assert(new_member && "Promise.new() method not found");
    std::optional<TypeId> variant_type_id = std::nullopt;
    if (return_type->kind == TypeKind::Subtype && !return_type->is_placeholder) {
        variant_type_id = return_type->id;
    }
    auto new_method_node = get_variant_member_node(new_member, variant_type_id);
    auto new_method = get_fn(new_method_node);
    builder.CreateCall(new_method->llvm_fn, {ctx.result_promise_ptr});

    // Create label for first segment code before generating continuations
    auto first_segment_label = fn->new_label("first_segment");

    // Phase 1: Create all continuation function declarations first
    // Note: this changes the builder's insert point to the continuation functions
    for (size_t i = 1; i < ctx.segments.size(); i++) {
        auto llvm_fn = create_continuation_fn_decl(ctx, i);
        ctx.continuations.push_back(llvm_fn);
    }

    // Restore builder to parent function entry and add branch to first segment
    builder.SetInsertPoint(&fn->llvm_fn->getEntryBlock());
    builder.CreateBr(first_segment_label);

    // Phase 2: Generate all continuation function bodies
    for (size_t i = 1; i < ctx.segments.size(); i++) {
        generate_async_continuation_body(ctx, i);
    }

    // Restore builder position to parent function's first segment label
    fn->use_label(first_segment_label);

    // Now compile the first segment (code before first await)
    auto &first_segment = ctx.segments[0];
    map<ast::Node *, llvm::Value *> local_vars;

    // Push scope for first segment
    fn->push_scope();

    for (auto stmt : first_segment.stmts) {
        if (stmt->type == ast::NodeType::VarDecl) {
            compile_stmt(fn, stmt);
            local_vars[stmt] = get_var(stmt);
        } else {
            compile_stmt(fn, stmt);
        }
    }

    // Handle the first await - chain to continuation for segment 1
    if (first_segment.await_expr) {
        emit_promise_chain(fn, ctx, first_segment.await_expr, 1, local_vars, ctx.result_promise_ptr);
    }

    fn->pop_scope();

    // Store result promise in return_value
    auto result_promise = builder.CreateLoad(promise_struct_type_l, ctx.result_promise_ptr);
    builder.CreateStore(result_promise, fn->return_value);

    // Jump to return label
    builder.CreateBr(fn->return_label);
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
        } else if (from_type->kind == TypeKind::Char || from_type->kind == TypeKind::Rune) {
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
        if (to_type->kind == TypeKind::Bool || to_type->kind == TypeKind::Char || to_type->kind == TypeKind::Rune) {
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
        if (from_type->kind == TypeKind::Bool || from_type->kind == TypeKind::Char || from_type->kind == TypeKind::Rune) {
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
                                          ChiType *to_type) {
    // never is the bottom type — unreachable code, return undef
    if (from_type->kind == TypeKind::Never) {
        return llvm::UndefValue::get(compile_type(to_type));
    }

    switch (to_type->kind) {
    case TypeKind::Any: {
        if (from_type->kind == TypeKind::Any) {
            return value;
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
        auto inlined = type_size <= sizeof(CxAny::data);
        llvm_builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_ctx), inlined),
                                 inlined_gep);
        auto data_gep = llvm_builder.CreateStructGEP(any_type_l, any_var, 2);
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
            auto from_type_l = compile_type(from_type);
            auto has_value = builder.CreateExtractValue(value, {0}, "has_value");
            return has_value;
        }
        case TypeKind::Result: {
            // Result is truthy when err.has_value is false (no error)
            // Result internal struct: { ?E err, T value }
            // ?E is { bool has_value, E }
            auto has_err = builder.CreateExtractValue(value, {0, 0}, "has_err");
            return builder.CreateXor(
                has_err, llvm::ConstantInt::getTrue(compile_type(get_system_types()->bool_)));
        }
        case TypeKind::Pointer:
        case TypeKind::Reference:
        case TypeKind::MutRef:
        case TypeKind::MoveRef: {
            auto elem = from_type->get_elem();
            if (elem && ChiTypeStruct::is_interface(elem)) {
                // Fat pointer struct {data_ptr, vtable_ptr} — check data_ptr (field 0) for null
                auto data_ptr = builder.CreateExtractValue(value, {0}, "data_ptr");
                return builder.CreateICmp(
                    llvm::CmpInst::Predicate::ICMP_NE, data_ptr,
                    get_null_ptr());
            }
            return builder.CreateICmp(
                llvm::CmpInst::Predicate::ICMP_NE, value,
                llvm::ConstantPointerNull::get((llvm::PointerType *)compile_type(from_type)));
        }
        default:
            return builder.CreateIntCast(value, compile_type(to_type), false);
        }
        break;
    }
    case TypeKind::Int: {
        if (from_type->kind == TypeKind::Float) {
            return compile_number_conversion(fn, value, from_type, to_type);
        }
        if (from_type->kind == TypeKind::Pointer) {
            return m_ctx->llvm_builder->CreatePtrToInt(value, compile_type(to_type));
        } else {
            return compile_number_conversion(fn, value, from_type, to_type);
        }
    }
    case TypeKind::Char:
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
        // For FnLambda -> FnLambda, just pass through (no retain here - compile_copy handles it)
        return value;
    }
    case TypeKind::Optional: {
        if (from_type->kind == TypeKind::Pointer) {
            // null pointer -> null optional
            return llvm::ConstantAggregateZero::get(compile_type(to_type));
        }
        if (from_type->kind == TypeKind::Optional) {
            // Same optional type - return as-is
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
        auto value_p = builder.CreateStructGEP(opt_type_l, opt_ptr, 1);
        auto inner_value = compile_conversion(fn, value, from_type, to_type->get_elem());
        builder.CreateStore(inner_value, value_p);
        return builder.CreateLoad(opt_type_l, opt_ptr);
    }
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef: {
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
            if (from_elem && from_elem->kind == TypeKind::Struct && !ChiTypeStruct::is_interface(from_elem)) {
                // &Concrete → &Interface: look up vtable from impl table
                auto impl = from_elem->data.struct_.interface_table[to_elem];
                assert(impl);
                auto vtable = m_ctx->impl_table[impl];
                assert(vtable);
                builder.CreateStore(vtable, vtable_p);
            } else {
                // *void → *Interface: no vtable yet, null
                builder.CreateStore(
                    get_null_ptr(),
                    vtable_p);
            }
            return builder.CreateLoad(iface_type_l, vp);
        }
        // Int to pointer
        if (from_type->is_int_like()) {
            return m_ctx->llvm_builder->CreateIntToPtr(value, compile_type(to_type));
        }
        return value;
    }
    case TypeKind::Struct: {
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
            return llvm::CmpInst::Predicate::FCMP_ONE;
        default:
            panic("not implemented: {}", PRINT_ENUM(op));
        }
    }
    auto is_unsigned = (type->kind == TypeKind::Int && type->data.int_.is_unsigned) ||
                       type->kind == TypeKind::Pointer ||
                       type->kind == TypeKind::Char || type->kind == TypeKind::Rune;
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
    switch (expr->type) {
    case ast::NodeType::FnCallExpr: {
        auto &call_data = expr->data.fn_call_expr;

        // Handle optional chaining method calls: obj?.method()
        if (call_data.fn_ref_expr->type == ast::NodeType::DotExpr &&
            call_data.fn_ref_expr->data.dot_expr.is_optional_chain) {
            auto &dot_data = call_data.fn_ref_expr->data.dot_expr;
            auto result_type = get_chitype(expr);
            auto result_type_l = compile_type(result_type);
            return compile_optional_branch(fn, dot_data.expr, result_type_l, "optcall",
                [&](llvm::Value *) {
                    auto call_result = compile_fn_call(fn, expr);
                    // compile_fn_call returns T; wrap T -> ?T if needed
                    if (call_result->getType() == result_type_l) return call_result;
                    return compile_conversion(fn, call_result, result_type->get_elem(), result_type);
                },
                [&]() -> llvm::Value * {
                    return llvm::ConstantAggregateZero::get(result_type_l);
                });
        }

        if (fn->get_def()->has_cleanup) {
            auto &builder = *m_ctx->llvm_builder.get();
            InvokeInfo invoke;
            auto next_label = fn->new_label("_invoke_next");
            invoke.normal = next_label;
            if (!fn->cleanup_landing_label) {
                fn->cleanup_landing_label = fn->new_label("_cleanup_landing");
            }
            invoke.landing = fn->cleanup_landing_label;
            auto result = compile_fn_call(fn, expr, &invoke);
            fn->use_label(next_label);
            fn->has_cleanup_invoke = true;
            if (invoke.sret) {
                return builder.CreateLoad(invoke.sret_type, invoke.sret);
            }
            return result;
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
                    auto assert = get_system_fn("assert");
                    emit_dbg_location(expr);
                    auto msg = compile_string_literal("unwrapping null optional");
                    auto opt_msg = compile_conversion(
                        fn, msg, get_system_types()->string,
                        get_resolver()->get_wrapped_type(get_system_types()->string,
                                                         TypeKind::Optional));
                    auto value = builder.CreateCall(assert->llvm_fn, {has_value, opt_msg});
                    auto value_p = builder.CreateStructGEP(opt_type_l, ref.address, 1);
                    return builder.CreateLoad(compile_type(expr->resolved_type), value_p);
                }

                if (data.op1->resolved_type->kind == TypeKind::Result) {
                    // Result! — force unwrap value (panic if error)
                    auto ref = compile_expr_ref(fn, data.op1);
                    auto result_type = data.op1->resolved_type;
                    auto result_type_l = compile_type(result_type);
                    // Result internal struct: { ?E err, T value }
                    // err is ?E which is { bool has_value, E }
                    auto err_type = result_type->data.result.internal->data.struct_.fields[0]->resolved_type;
                    auto err_type_l = compile_type(err_type);
                    auto err_p = builder.CreateStructGEP(result_type_l, ref.address, 0);
                    auto has_err_p = builder.CreateStructGEP(err_type_l, err_p, 0);
                    auto has_err = builder.CreateLoad(compile_type(get_system_types()->bool_), has_err_p);
                    // Assert no error: has_err must be false → negate for assert
                    auto is_ok = builder.CreateXor(
                        has_err, llvm::ConstantInt::getTrue(compile_type(get_system_types()->bool_)));
                    auto assert = get_system_fn("assert");
                    emit_dbg_location(expr);
                    auto msg = compile_string_literal("unwrapping error Result");
                    auto opt_msg = compile_conversion(
                        fn, msg, get_system_types()->string,
                        get_resolver()->get_wrapped_type(get_system_types()->string,
                                                         TypeKind::Optional));
                    builder.CreateCall(assert->llvm_fn, {is_ok, opt_msg});
                    auto value_p = builder.CreateStructGEP(result_type_l, ref.address, 1);
                    return builder.CreateLoad(compile_type(expr->resolved_type), value_p);
                }
                if (data.resolved_call) {
                    auto ref_ptr = compile_fn_call(fn, data.resolved_call);
                    auto elem = expr->resolved_type;
                    if (ChiTypeStruct::is_interface(elem)) return ref_ptr;
                    return builder.CreateLoad(compile_type(elem), ref_ptr);
                }
                panic("unreachable: suffix ! on non-optional/result type");
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
            assert(ref.address);
            auto result = ref.address;
            // For &move: null out the source after taking the pointer
            if (data.op_type == TokenType::MOVEREF) {
                auto ptr_val = builder.CreateLoad(compile_type(get_chitype(data.op1)), ref.address);
                builder.CreateStore(
                    llvm::ConstantPointerNull::get(
                        (llvm::PointerType *)compile_type(get_chitype(data.op1))),
                    ref.address);
                return ptr_val;
            }
            return result;
        }
        case TokenType::KW_MOVE: {
            // move x — bitwise load + zero source (skip copy_from)
            auto ref = compile_expr_ref(fn, data.op1);
            assert(ref.address);
            auto type = get_chitype(data.op1);
            auto type_l = compile_type(type);
            auto value = builder.CreateLoad(type_l, ref.address);
            auto size = llvm_type_size(type_l);
            builder.CreateMemSet(
                ref.address,
                llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(*m_ctx->llvm_ctx), 0),
                size, {});
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
            auto lhs = compile_comparator(fn, data.op1);
            auto rhs = compile_comparator(fn, data.op2);
            auto cmpop = get_cmpop(data.op_type, get_chitype(data.op1));
            return builder.CreateCmp(cmpop, lhs, rhs);
        }
        case TokenType::QUES_QUES: {
            auto result_type_l = compile_type(get_chitype(expr));
            return compile_optional_branch(fn, data.op1, result_type_l, "coalesce",
                [&](llvm::Value *unwrapped_ptr) {
                    return builder.CreateLoad(result_type_l, unwrapped_ptr);
                },
                [&]() {
                    auto rhs = compile_expr(fn, data.op2);
                    return compile_conversion(fn, rhs, get_chitype(data.op2), get_chitype(expr));
                });
        }
        case TokenType::ASS: {
            auto ref = compile_expr_ref(fn, data.op1);
            assert(ref.address);
            auto dest_type = get_chitype(data.op1);
            auto src_type = get_chitype(data.op2);
            bool destruct_old = !data.is_initializing;
            // Fallback for cloned AST nodes (e.g. subtype variants):
            // check if this is a field being initialized inside a constructor
            if (destruct_old && fn->node) {
                auto var = data.op1->get_decl();
                if (var && var->type == ast::NodeType::VarDecl &&
                    var->data.var_decl.is_field &&
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
            bool in_place = data.op2->escape.moved &&
                data.op2->type == ast::NodeType::ConstructExpr &&
                data.op2->data.construct_expr.resolved_outlet;
            // If in-place, destruct old BEFORE RHS overwrites dest
            if (in_place && destruct_old) {
                compile_destruction_for_type(fn, ref.address, dest_type);
            }

            // Get RHS as ref to preserve source address for efficient copy
            // (avoids load + temp alloca when source already has an address, e.g. ptr!)
            auto src_ref = compile_expr_ref(fn, data.op2);

            // Only load the value when needed (type conversion)
            if (dest_type && src_type != dest_type) {
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
            compile_store_or_copy(fn, val, ref.address, dest_type, data.op2,
                                  destruct_old);
            return val;
        }
        default: {
            auto target_type = get_chitype(expr);
            auto lhs_type = get_chitype(data.op1);
            auto rhs_type = get_chitype(data.op2);

            // Pointer arithmetic
            if (lhs_type->kind == TypeKind::Pointer || rhs_type->kind == TypeKind::Pointer) {
                auto lhs = compile_expr(fn, data.op1);
                auto rhs = compile_expr(fn, data.op2);

                if (lhs_type->kind == TypeKind::Pointer && rhs_type->is_int_like()) {
                    // ptr + n / ptr - n
                    auto elem_type_l = compile_type(lhs_type->get_elem());
                    auto index = rhs;
                    if (data.op_type == TokenType::SUB) {
                        index = builder.CreateNeg(index);
                    }
                    return builder.CreateGEP(elem_type_l, lhs, {index});
                }
                if (lhs_type->is_int_like() && rhs_type->kind == TypeKind::Pointer) {
                    // n + ptr
                    auto elem_type_l = compile_type(rhs_type->get_elem());
                    return builder.CreateGEP(elem_type_l, rhs, {lhs});
                }
                if (lhs_type->kind == TypeKind::Pointer &&
                    rhs_type->kind == TypeKind::Pointer) {
                    // ptr - ptr → ptrdiff
                    auto elem_type_l = compile_type(lhs_type->get_elem());
                    auto lhs_int = builder.CreatePtrToInt(lhs, builder.getInt64Ty());
                    auto rhs_int = builder.CreatePtrToInt(rhs, builder.getInt64Ty());
                    auto diff_bytes = builder.CreateSub(lhs_int, rhs_int);
                    auto elem_size = llvm::ConstantInt::get(builder.getInt64Ty(),
                                                            llvm_type_size(elem_type_l));
                    return builder.CreateSDiv(diff_bytes, elem_size);
                }
            }

            auto struct_type = get_resolver()->eval_struct_type(target_type);

            // For specialized generic functions, check if we need operator method calls
            // with the concrete types instead of using primitive arithmetic
            if (struct_type) {
                // Try to resolve operator method with concrete types
                IntrinsicSymbol intrinsic_symbol =
                    Resolver::get_operator_intrinsic_symbol(data.op_type);

                if (intrinsic_symbol != IntrinsicSymbol::None) {
                    auto resolver = get_resolver();
                    // Create a minimal scope for the method call resolution
                    ResolveScope dummy_scope = {};
                    auto method_call = resolver->try_resolve_operator_method(
                        intrinsic_symbol, lhs_type, rhs_type, data.op1, data.op2, expr,
                        dummy_scope);
                    if (method_call.has_value()) {
                        return compile_fn_call(fn, method_call->call_node);
                    }
                }
            }

            // Check if this is an interface method call for an operator (from resolver)
            if (data.resolved_call) {
                return compile_fn_call(fn, data.resolved_call);
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

        auto result_type = get_chitype(expr);
        bool is_void = result_type->kind == TypeKind::Void;
        llvm::Type *result_type_l = nullptr;
        llvm::Value *result_var = nullptr;

        if (!is_void) {
            result_type_l = compile_type(result_type);
            result_var = fn->entry_alloca(result_type_l, "try_result");

            // Zero-initialize the result
            auto size = llvm_type_size(result_type_l);
            builder.CreateMemSet(
                result_var, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_ctx), 0),
                size.getFixedValue(), llvm::MaybeAlign());
        }

        auto continue_b = fn->new_label("_try_continue");
        auto normal_b = fn->new_label("_try_normal");
        auto landing_b = fn->new_label("_try_landing");

        InvokeInfo invoke = {};
        invoke.normal = normal_b;
        invoke.landing = landing_b;
        auto value = compile_fn_call(fn, data.expr, &invoke);

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

        auto error_data = builder.CreateCall(get_error_data_fn->llvm_fn, {}, "error_data");
        auto error_vtable = builder.CreateCall(get_error_vtable_fn->llvm_fn, {}, "error_vtable");

        if (data.catch_block) {
            // === CATCH BLOCK MODE: try f() catch (...) { block } → yields T ===

            // Save error_data pointer for cleanup (initialized to null, set on catch path)
            // This alloca is in the entry block, so it persists for function-level cleanup
            auto error_data_var = fn->entry_alloca(builder.getPtrTy(), "error_owner");
            builder.CreateStore(
                llvm::ConstantPointerNull::get(builder.getPtrTy()), error_data_var);

            // Type check if specific catch
            ChiType *catch_type = nullptr;
            if (data.catch_expr) {
                catch_type = get_resolver()->to_value_type(get_chitype(data.catch_expr));
                auto caught_type_id = builder.CreateCall(get_error_type_id_fn->llvm_fn, {}, "caught_type_id");
                auto expected_id = llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(llvm_ctx), catch_type->id);
                auto type_matches = builder.CreateICmpEQ(caught_type_id, expected_id, "type_matches");

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
                data.catch_block->data.block.implicit_vars.len = 0;
            }

            // Compile catch block with a cleanup label instead of continue_b
            auto catch_cleanup_b = fn->new_label("_catch_cleanup");
            compile_block(fn, expr, data.catch_block, catch_cleanup_b, result_var);

            // === CATCH CLEANUP: destroy error object after catch block ===
            fn->use_label(catch_cleanup_b);
            {
                auto owned_ptr = builder.CreateLoad(builder.getPtrTy(), error_data_var, "err_owned");
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
                builder.CreateStore(
                    llvm::ConstantPointerNull::get(builder.getPtrTy()), error_data_var);
                builder.CreateBr(skip_free_b);

                fn->use_label(skip_free_b);
            }
            builder.CreateBr(continue_b);

            // Register error_data_var for function-level cleanup (diverge path)
            // This handles cases where the catch block returns/breaks
            fn->error_owner_vars.push_back({error_data_var, catch_type});
        } else {
            // === RESULT MODE: try f() → Result<T, E> ===

            if (data.catch_expr) {
                auto catch_type = get_resolver()->to_value_type(get_chitype(data.catch_expr));
                auto caught_type_id = builder.CreateCall(get_error_type_id_fn->llvm_fn, {}, "caught_type_id");
                auto expected_id = llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(llvm_ctx), catch_type->id);
                auto type_matches = builder.CreateICmpEQ(caught_type_id, expected_id, "type_matches");

                auto match_b = fn->new_label("_try_type_match");
                auto nomatch_b = fn->new_label("_try_type_nomatch");
                builder.CreateCondBr(type_matches, match_b, nomatch_b);

                // Type doesn't match: re-throw
                fn->use_label(nomatch_b);
                builder.CreateResume(landing);

                // Type matches: populate result with &ConcreteError
                fn->use_label(match_b);
                auto err_type = result_type->data.result.internal->data.struct_.fields[0]->resolved_type;
                auto err_type_l = compile_type(err_type);
                auto err_p = builder.CreateStructGEP(result_type_l, result_var, 0);
                auto has_err_p = builder.CreateStructGEP(err_type_l, err_p, 0);
                builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt1Ty(llvm_ctx), 1), has_err_p);
                auto err_value_p = builder.CreateStructGEP(err_type_l, err_p, 1);
                builder.CreateStore(error_data, err_value_p);
            } else {
                // Catch-all: populate result with Error interface
                auto err_type = result_type->data.result.internal->data.struct_.fields[0]->resolved_type;
                auto err_type_l = compile_type(err_type);
                auto err_p = builder.CreateStructGEP(result_type_l, result_var, 0);
                auto has_err_p = builder.CreateStructGEP(err_type_l, err_p, 0);
                builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt1Ty(llvm_ctx), 1), has_err_p);
                auto err_value_p = builder.CreateStructGEP(err_type_l, err_p, 1);
                auto iface_type_l = compile_type(err_type->get_elem());
                auto data_p = builder.CreateStructGEP(iface_type_l, err_value_p, 0);
                builder.CreateStore(error_data, data_p);
                auto vtable_p = builder.CreateStructGEP(iface_type_l, err_value_p, 1);
                builder.CreateStore(error_vtable, vtable_p);
            }
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
            // Result mode: store value into Result.value field (index 1)
            auto value_p = builder.CreateStructGEP(result_type_l, result_var, 1);
            if (invoke.sret) {
                auto sret_loaded = builder.CreateLoad(invoke.sret_type, invoke.sret);
                builder.CreateStore(sret_loaded, value_p);
            } else if (value && !value->getType()->isVoidTy()) {
                builder.CreateStore(value, value_p);
            }
        }
        builder.CreateBr(continue_b);

        // === CONTINUE ===
        fn->use_label(continue_b);
        if (is_void) {
            return nullptr;
        }
        return builder.CreateLoad(result_type_l, result_var, "try_result");
    }
    case ast::NodeType::AwaitExpr: {
        // For now, implement a simple synchronous await that reads the value directly
        // from an already-resolved Promise. Full async implementation will use continuations.
        auto &data = expr->data.await_expr;
        auto &builder = *m_ctx->llvm_builder.get();

        // Get the Promise type info
        auto promise_type = get_chitype(data.expr);
        auto promise_struct_type_l = compile_type(promise_type->data.promise.internal);

        // Compile the promise expression and store it
        auto promise_val = compile_expr(fn, data.expr);
        auto promise_ptr = builder.CreateAlloca(promise_struct_type_l, nullptr, "await_promise");
        builder.CreateStore(promise_val, promise_ptr);

        // Get the value type (what we're unwrapping from Promise<T>)
        auto result_type = get_chitype(expr);
        auto result_type_l = compile_type(result_type);

        // Promise struct layout: {state, value, error, on_resolve, on_reject}
        // value is at index 1, and it's a pointer to the actual value
        // GEP to get the value pointer field (index 1)
        auto value_ptr_ptr = builder.CreateStructGEP(promise_struct_type_l, promise_ptr, 1, "await_value_ptr");
        auto value_ptr = builder.CreateLoad(get_llvm_ptr_type(), value_ptr_ptr, "await_value_addr");

        // Load the actual value
        auto result = builder.CreateLoad(result_type_l, value_ptr, "await_result");
        return result;
    }
    case ast::NodeType::DotExpr: {
        auto &dot_data = expr->data.dot_expr;

        if (dot_data.is_optional_chain) {
            auto &builder = *m_ctx->llvm_builder.get();
            auto opt_type = get_chitype(dot_data.expr);
            auto result_type = get_chitype(expr);
            auto result_type_l = compile_type(result_type);
            return compile_optional_branch(fn, dot_data.expr, result_type_l, "optchain",
                [&](llvm::Value *unwrapped_ptr) {
                    auto unwrapped_type = opt_type->get_elem();
                    llvm::Value *struct_ptr;
                    ChiType *struct_type;
                    if (unwrapped_type->is_pointer_like()) {
                        struct_type = unwrapped_type->get_elem();
                        struct_ptr = builder.CreateLoad(compile_type(unwrapped_type), unwrapped_ptr);
                    } else {
                        struct_type = unwrapped_type;
                        struct_ptr = unwrapped_ptr;
                    }
                    auto gep = compile_dot_access(fn, struct_ptr, struct_type, dot_data.resolved_struct_member);
                    auto field_type = dot_data.resolved_struct_member->resolved_type;
                    auto field_val = builder.CreateLoad(compile_type(field_type), gep);
                    return compile_conversion(fn, field_val, field_type, result_type);
                },
                [&]() -> llvm::Value * {
                    return llvm::ConstantAggregateZero::get(result_type_l);
                });
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
        auto ptr = data.resolved_outlet ? compile_expr_ref(fn, data.resolved_outlet).address
                                        : compile_alloc(fn, expr, data.is_new, type);
        compile_construction(fn, ptr, type, expr);
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
            auto ptr = compile_expr(fn, data.expr);
            auto ptr_type = get_chitype(data.expr);
            auto elem_type = (ptr_type && ptr_type->is_pointer_like()) ? ptr_type->get_elem() : nullptr;
            compile_heap_free(fn, ptr, elem_type);
            return nullptr;
        }
        default:
            panic("not implemented: {}", PRINT_ENUM(data.prefix->type));
        }
        return nullptr;
    }
    case ast::NodeType::CastExpr: {
        auto &data = expr->data.cast_expr;
        auto from_value = compile_expr(fn, data.expr);
        auto from_type = get_chitype(data.expr);
        return compile_conversion(fn, from_value, from_type, get_chitype(expr));
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
        return compile_expr(fn, expr->data.child_expr);
    }
    case ast::NodeType::SwitchExpr: {
        auto &data = expr->data.switch_expr;
        auto expr_value = compile_comparator(fn, data.expr);
        auto comparator_type = expr_value->getType();
        auto ret_type = get_chitype(expr);
        llvm::Value *var = nullptr;
        if (ret_type->kind != TypeKind::Void) {
            if (data.resolved_outlet) {
                var = compile_expr_ref(fn, data.resolved_outlet).address;
            } else {
                var = compile_alloc(fn, expr, false);
            }
        }
        auto &builder = *m_ctx->llvm_builder;
        auto default_label = fn->new_label("_switch_default");

        auto switch_b = builder.CreateSwitch(expr_value, default_label, data.cases.len);

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

        for (int i = 0; i < data.cases.len; i++) {
            fn->use_label(case_labels[i]);
            auto scase = data.cases[i];
            compile_block(fn, scase, scase->data.case_expr.body, done_label, var);
        }

        fn->use_label(done_label);
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
    switch (type->kind) {
    case TypeKind::EnumValue: {
        auto ref = compile_expr_ref(fn, expr);
        auto address = ref.address ? ref.address : ref.value;
        auto gep = builder.CreateStructGEP(compile_type(type), address, 0);
        return builder.CreateLoad(llvm::IntegerType::getInt32Ty(*m_ctx->llvm_ctx), gep,
                                  "_load_enum_value");
    }
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
    if (!value) return;
    if (expr->escape.moved) {
        // Moved temporary — destruct old if needed, then bitwise store
        if (destruct_old)
            compile_destruction_for_type(fn, dest, type);
        m_ctx->llvm_builder->CreateStore(value, dest);
    } else {
        // Named value — deep copy via copy_from
        compile_copy_with_ref(fn, RefValue::from_value(value), dest, type, expr, destruct_old);
    }
}

void Compiler::compile_copy_with_ref(Function *fn, RefValue src, llvm::Value *dest, ChiType *type,
                                     ast::Node *expr, bool destruct_old) {
    auto &builder = *m_ctx->llvm_builder;
    assert(src.value || src.address);

    // Lazy load: derive value from address when needed
    auto ensure_value = [&]() {
        if (!src.value && src.address) {
            src.value = builder.CreateLoad(compile_type(type), src.address);
        }
        return src.value;
    };

    // Result delegates to its internal struct type
    if (type->kind == TypeKind::Result) {
        compile_copy_with_ref(fn, src, dest, type->data.result.internal, expr, destruct_old);
        return;
    }

    // Optional: copy has_value flag + deep-copy inner value
    if (type->kind == TypeKind::Optional) {
        auto elem_type = type->get_elem();
        if (elem_type && get_resolver()->type_needs_destruction(elem_type)) {
            if (destruct_old) {
                compile_destruction_for_type(fn, dest, type);
            }
            auto opt_type_l = compile_type(type);
            auto from_address = src.address;
            if (!from_address) {
                from_address = builder.CreateAlloca(opt_type_l, nullptr, "_opt_copy_src");
                builder.CreateStore(ensure_value(), from_address);
            }
            // Copy has_value (field 0)
            auto src_hv = builder.CreateStructGEP(opt_type_l, from_address, 0);
            auto dst_hv = builder.CreateStructGEP(opt_type_l, dest, 0);
            auto hv = builder.CreateLoad(llvm::Type::getInt1Ty(*m_ctx->llvm_ctx), src_hv);
            builder.CreateStore(hv, dst_hv);
            // Deep-copy inner value (field 1) only if has_value
            auto bb_copy = fn->new_label("opt_copy_value");
            auto bb_done = fn->new_label("opt_copy_done");
            builder.CreateCondBr(hv, bb_copy, bb_done);
            fn->use_label(bb_copy);
            auto src_val = builder.CreateStructGEP(opt_type_l, from_address, 1);
            auto dst_val = builder.CreateStructGEP(opt_type_l, dest, 1);
            auto inner = builder.CreateLoad(compile_type(elem_type), src_val);
            compile_copy(fn, inner, dst_val, elem_type, expr);
            builder.CreateBr(bb_done);
            fn->use_label(bb_done);
            return;
        }
        // For Optional with trivially-copyable inner type, fall through to default
    }

    // EnumValue: delegate to generated copier (switch on discriminator, deep-copy variant fields)
    if (type->kind == TypeKind::EnumValue) {
        auto copier = generate_copier_enum(type);
        if (copier) {
            if (destruct_old) {
                compile_destruction_for_type(fn, dest, type);
            }
            auto from_address = src.address;
            if (!from_address) {
                from_address = builder.CreateAlloca(compile_type(type), nullptr, "_enum_copy_src");
                builder.CreateStore(ensure_value(), from_address);
            }
            builder.CreateCall(copier->llvm_fn, {dest, from_address});
            return;
        }
        // No copier needed — fall through to default bitwise store
    }

    switch (type->kind) {
    case TypeKind::String: {
        auto from_address = src.address ? src.address : nullptr;
        if (!from_address) {
            from_address = builder.CreateAlloca(compile_type(type), nullptr, "_op_str_copy");
            builder.CreateStore(ensure_value(), from_address);
        }
        auto copy_fn = get_system_fn("cx_string_copy");
        auto call = builder.CreateCall(copy_fn->llvm_fn, {dest, from_address});
        call->setDebugLoc(m_ctx->llvm_builder->getCurrentDebugLocation());
        emit_dbg_location(expr);
        return;
    }
    case TypeKind::FnLambda: {
        // For lambda types, we need to manually copy fields and increment refcount
        // because __CxLambda<T1> and __CxLambda<T2> can't use copy_from across types
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

        // Retain type-erased captures (null-safe).
        auto retain_fn = get_system_fn("cx_capture_retain");
        builder.CreateCall(retain_fn->llvm_fn, {captures_ptr});

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
    case TypeKind::Struct: {
        // Interface copy via vtable dispatch
        // dest and src.address are fat pointer struct VALUES {data_ptr, vtable_ptr}
        if (ChiTypeStruct::is_interface(type)) {
            // dest is the ADDRESS of the fat pointer {data_ptr, vtable_ptr}
            // Load the fat pointer to extract data and vtable pointers
            auto iface_type_l = llvm::StructType::get(*m_ctx->llvm_ctx, {get_llvm_ptr_type(), get_llvm_ptr_type()});
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
        if (!sty) break;  // Not a struct type, fall through to default copy
        auto &builder = *m_ctx->llvm_builder;
        auto copy_fn_p = sty->member_intrinsics.get(IntrinsicSymbol::CopyFrom);
        if (copy_fn_p) {
            auto copy_fn = *copy_fn_p;
            auto from_address = src.address ? src.address : nullptr;
            if (!from_address) {
                from_address = builder.CreateAlloca(compile_type(type), nullptr, "_op_copy_from");
                builder.CreateStore(ensure_value(), from_address);
            }
            if (destruct_old) {
                compile_destruction_for_type(fn, dest, type);
            }
            auto size = llvm_type_size(compile_type(type));
            builder.CreateMemSet(
                dest, llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(*m_ctx->llvm_ctx), 0),
                size, {});
            // Use variant lookup to get the specialized copy_from method
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
            emit_dbg_location(expr);
            return;
        }

        // For structs without CopyFrom, check if any field needs destruction
        // (transitively — handles nested structs with CopyFrom/destructor fields).
        // If so, copy field-by-field to ensure proper deep copy semantics.
        if (type->kind == TypeKind::Struct) {
            auto &data = type->data.struct_;
            bool needs_field_copy = false;

            for (auto field : data.fields) {
                if (get_resolver()->type_needs_destruction(field->resolved_type)) {
                    needs_field_copy = true;
                    break;
                }
            }

            if (needs_field_copy) {
                if (destruct_old) {
                    compile_destruction_for_type(fn, dest, type);
                }
                auto from_address = src.address;
                if (!from_address) {
                    from_address = builder.CreateAlloca(compile_type(type), nullptr, "_struct_copy_src");
                    builder.CreateStore(ensure_value(), from_address);
                }

                auto llvm_type = compile_type(type);
                for (auto field : data.fields) {
                    auto field_src_gep = builder.CreateStructGEP(llvm_type, from_address, field->field_index);
                    auto field_dest_gep = builder.CreateStructGEP(llvm_type, dest, field->field_index);
                    auto field_llvm_type = compile_type(field->resolved_type);
                    auto field_value = builder.CreateLoad(field_llvm_type, field_src_gep);
                    compile_copy(fn, field_value, field_dest_gep, field->resolved_type, expr);
                }
                return;
            }
        }
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
        auto ti_header_l = llvm::StructType::get(llvm_ctx, {i32_ty, i32_ty, ptr_type, ptr_type, ptr_type}, true);
        auto size_gep = builder.CreateStructGEP(ti_header_l, ti_ptr, 1);
        auto typesize = builder.CreateLoad(i32_ty, size_gep, "any_typesize");
        auto copier_gep = builder.CreateStructGEP(ti_header_l, ti_ptr, 4);
        auto copier_ptr = builder.CreateLoad(ptr_type, copier_gep, "any_copier");

        auto src_data_gep = builder.CreateStructGEP(any_type_l, src_addr, 2);
        auto dst_data_gep = builder.CreateStructGEP(any_type_l, dest, 2);

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
            auto copier_fn_type = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_ctx), {ptr_type, ptr_type}, false);
            builder.CreateCall(copier_fn_type, copier_ptr, {dst_data_gep, src_data_gep});
            builder.CreateBr(bb_done);
        }

        // Heap path: malloc new buffer, copy into it, store pointer in dest.data
        fn->use_label(bb_heap);
        {
            auto src_heap_ptr = builder.CreateLoad(ptr_type, src_data_gep, "src_heap_ptr");
            auto malloc_fn = get_system_fn("cx_malloc");
            auto new_buf = builder.CreateCall(malloc_fn->llvm_fn, {typesize, get_null_ptr()}, "new_heap_buf");

            auto copier_is_null = builder.CreateICmpEQ(copier_ptr, get_null_ptr());
            auto bb_h_memcpy = fn->new_label("any_cp_heap_memcpy");
            auto bb_h_copier = fn->new_label("any_cp_heap_copier");
            auto bb_h_done = fn->new_label("any_cp_heap_done");
            builder.CreateCondBr(copier_is_null, bb_h_memcpy, bb_h_copier);

            fn->use_label(bb_h_memcpy);
            builder.CreateMemCpy(new_buf, {}, src_heap_ptr, {}, typesize);
            builder.CreateBr(bb_h_done);

            fn->use_label(bb_h_copier);
            auto copier_fn_type = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_ctx), {ptr_type, ptr_type}, false);
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

        for (auto field_init : expr->data.construct_expr.field_inits) {
            auto &data = field_init->data.field_init_expr;
            auto gep = compile_dot_access(fn, dest, type, data.resolved_field);
            data.compiled_field_address = gep;
            auto value =
                compile_assignment_to_type(fn, data.value, data.resolved_field->resolved_type);
            builder.CreateStore(value, gep);
            emit_dbg_location(expr);
        }
        break;
    }
    case TypeKind::Array: {
        auto array_struct_type = get_resolver()->eval_struct_type(type);
        return compile_construction(fn, dest, array_struct_type, expr);
    }
    case TypeKind::Subtype: {
        // Resolve generic struct instantiation to its concrete struct type
        auto struct_type = get_resolver()->eval_struct_type(type);
        return compile_construction(fn, dest, struct_type, expr);
    }
    case TypeKind::Struct: {
        // Call __new to initialize fields with default values
        auto generated_ctor = generate_constructor(type, nullptr);
        if (generated_ctor) {
            builder.CreateCall(generated_ctor->llvm_fn, {dest});
        }

        auto constructor = ChiTypeStruct::get_constructor(type);
        if (constructor) {
            auto variant_type_id = resolve_variant_type_id(m_fn, expr->resolved_type);
            auto constructor_node = get_variant_member_node(constructor, variant_type_id);

            auto constructor_type = get_chitype(constructor_node);
            auto id = get_resolver()->resolve_global_id(constructor_node);
            auto entry = m_ctx->function_table.get(id);
            assert(entry && "constructor not compiled");
            auto constructor_fn = *entry;
            auto constructor_type_l = (llvm::FunctionType *)compile_type(constructor_type);
            auto args = std::vector<llvm::Value *>{dest};
            // Track temporaries created for constructor arguments
            std::vector<std::pair<llvm::Value *, ast::Node *>> arg_temporaries;
            auto remaining_args = compile_fn_args(fn, constructor_fn,
                                                  expr->data.construct_expr.items, expr,
                                                  &arg_temporaries);
            args.insert(args.end(), remaining_args.begin(), remaining_args.end());

            // Compile default args for missing params (e.g. = {} on generic field)
            if (constructor_node->type == ast::NodeType::FnDef) {
                auto &proto = constructor_node->data.fn_def.fn_proto->data.fn_proto;
                for (size_t i = expr->data.construct_expr.items.len;
                     i < proto.params.len; i++) {
                    auto default_val = proto.params[i]->data.param_decl.default_value;
                    if (!default_val) break;
                    auto param_type = constructor_fn->fn_type->data.fn.get_param_at(i);
                    auto val = compile_assignment_to_type(fn, default_val, param_type);
                    args.push_back(val);
                }
            }
            builder.CreateCall(constructor_type_l, constructor_fn->llvm_fn, args);
            emit_dbg_location(expr);
            // Destroy temporaries after the constructor call completes
            for (auto &[temp_ptr, temp_node] : arg_temporaries) {
                compile_destruction(fn, temp_ptr, temp_node);
            }
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
            for (auto src_field : spread_type->data.struct_.fields) {
                auto tgt_field = get_resolver()->get_struct_member(type, src_field->get_name());
                if (!tgt_field) continue; // validated by resolver
                if (overridden.count(tgt_field->field_index)) continue;

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
            auto field_gep =
                builder.CreateStructGEP(compile_type(type), dest, data.resolved_field->field_index);
            data.compiled_field_address = field_gep;
            auto value =
                compile_assignment_to_type(fn, data.value, data.resolved_field->resolved_type);
            builder.CreateStore(value, field_gep);
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
               expr->data.construct_expr.items.len == 1);
        auto value = expr->data.construct_expr.items[0];
        auto opt_type = get_chitype(expr);
        auto has_value_p = builder.CreateStructGEP(compile_type(opt_type), dest, 0);
        builder.CreateStore(
            llvm::ConstantInt::get(llvm::IntegerType::getInt1Ty(*m_ctx->llvm_ctx), 1), has_value_p);
        auto value_p = builder.CreateStructGEP(compile_type(opt_type), dest, 1);
        builder.CreateStore(compile_assignment_to_type(fn, value, opt_type->get_elem()), value_p);
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

void Compiler::compile_destructure_fields(Function *fn, array<ast::Node *> &fields,
                                          llvm::Value *source_ptr, ChiType *source_type) {
    auto &builder = *m_ctx->llvm_builder;
    auto struct_type_l = compile_type(source_type);
    size_t var_idx = 0;

    for (auto field_node : fields) {
        auto &field_data = field_node->data.destructure_field;
        auto member = field_data.resolved_field;
        auto field_ptr = builder.CreateStructGEP(struct_type_l, source_ptr, member->field_index);

        if (field_data.nested) {
            // Nested destructuring: recurse
            auto &nested = field_data.nested->data.destructure_decl;
            compile_destructure_fields(fn, nested.fields, field_ptr, member->resolved_type);
        } else {
            // Allocate binding variable, copy field value into it
            auto &gen_vars = field_node->parent->data.destructure_decl.generated_vars;
            assert(var_idx < gen_vars.len);
            auto var_node = gen_vars[var_idx++];
            auto var_type = get_chitype(var_node);
            auto var_type_l = compile_type(var_type);
            auto var_ptr = compile_alloc(fn, var_node);
            add_var(var_node, var_ptr);

            auto field_value = builder.CreateLoad(var_type_l, field_ptr);
            compile_store_or_copy(fn, field_value, var_ptr, var_type, field_node);
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
    return ref.address ? ref.address : ref.value;
}

llvm::Value *Compiler::compile_optional_branch(
    Function *fn, ast::Node *opt_expr, llvm::Type *result_type_l, const char *label,
    std::function<llvm::Value *(llvm::Value *unwrapped_ptr)> on_has_value,
    std::function<llvm::Value *()> on_null) {
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
    switch (expr->type) {
    case ast::NodeType::FnDef: {
        auto &builder = *m_ctx->llvm_builder.get();
        auto lambda_val = compile_expr(fn, expr);
        auto lambda_ptr = fn->entry_alloca(lambda_val->getType(), "lambda_literal");
        builder.CreateStore(lambda_val, lambda_ptr);
        return RefValue::from_address(lambda_ptr);
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

        // Narrowing redirect: use the narrowed var's GEP alias
        if (data.narrowed_var) {
            return RefValue::from_address(get_var(data.narrowed_var));
        }

        auto type = get_chitype(data.expr);

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
        } else if (data.resolved_dot_kind == DotKind::MethodToLambda) {
            // Create a method lambda using proxy function approach
            auto lambda_type = get_chitype(expr);
            assert(lambda_type->kind == TypeKind::FnLambda);

            // Generate a proxy function that wraps the method call
            auto proxy_fn =
                generate_method_proxy_function(fn, data.resolved_struct_member, lambda_type);

            // Get bind struct size
            auto bind_struct_type_l = compile_type(lambda_type->data.fn_lambda.bind_struct);
            auto struct_size =
                (uint32_t)m_ctx->llvm_module->getDataLayout().getTypeAllocSize(bind_struct_type_l);

            // Initialize __CxLambda struct
            auto [lambda_alloca, lambda_struct_type_l] = compile_cxlambda_init(fn, proxy_fn, struct_size);

            // Get the instance pointer
            llvm::Value *instance_ptr;
            if (type->is_pointer_like()) {
                instance_ptr = compile_expr(fn, data.expr);
            } else {
                auto ref = compile_expr_ref(fn, data.expr);
                instance_ptr = ref.address;
            }

            // Create binding struct to hold the instance pointer
            auto bind_alloca = builder.CreateAlloca(bind_struct_type_l, nullptr, "method_bind");

            // Store instance pointer in binding struct
            auto instance_gep = builder.CreateStructGEP(bind_struct_type_l, bind_alloca, 0);
            builder.CreateStore(instance_ptr, instance_gep);

            // Allocate type-erased capture box and store bind struct into it
            auto bind_struct_chi = lambda_type->data.fn_lambda.bind_struct;
            auto captures_ti = compile_type_info(bind_struct_chi);
            auto captures_ti_ptr = builder.CreateBitCast(captures_ti, builder.getInt8PtrTy());

            llvm::Value *dtor_ptr = llvm::ConstantPointerNull::get(builder.getInt8PtrTy());
            if (get_resolver()->type_needs_destruction(bind_struct_chi)) {
                if (auto dtor = generate_destructor(bind_struct_chi, nullptr)) {
                    dtor_ptr = builder.CreateBitCast(dtor->llvm_fn, builder.getInt8PtrTy());
                }
            }

            auto [capture_ptr, payload_data_ptr] = compile_cxcapture_create(struct_size, captures_ti_ptr, dtor_ptr);

            auto bind_struct_value = builder.CreateLoad(bind_struct_type_l, bind_alloca);
            auto payload_typed_ptr =
                builder.CreateBitCast(payload_data_ptr, bind_struct_type_l->getPointerTo());
            builder.CreateStore(bind_struct_value, payload_typed_ptr);

            compile_cxlambda_set_captures(lambda_alloca, capture_ptr);

            return RefValue::from_address(lambda_alloca);
        } else if (type->is_pointer_like()) {
            type = type->get_elem();
            ptr = compile_expr(fn, data.expr);
        } else {
            // Check if this is module member access (e.g., sdl.SDL_Init)
            if (data.expr->type == ast::NodeType::Identifier &&
                data.expr->data.identifier.decl &&
                data.expr->data.identifier.decl->type == ast::NodeType::ImportDecl) {
                // Module member access - use resolved_decl if available
                if (data.resolved_decl) {
                    // Resolver already found the declaration, compile it directly
                    return compile_expr_ref(fn, data.resolved_decl);
                }
                // Fallback: look up the member from the module directly
                auto import_decl = data.expr->data.identifier.decl;
                auto imported_module = import_decl->data.import_decl.resolved_module;
                if (!imported_module || !imported_module->scope) {
                    panic("ImportDecl for '{}' has no resolved module", data.expr->name);
                }
                auto member_name = data.field->str;
                auto member_node = imported_module->scope->find_one(member_name);
                if (!member_node) {
                    panic("Module '{}' has no member '{}'", data.expr->name, member_name);
                }
                return compile_expr_ref(fn, member_node);
            }

            auto ref = compile_expr_ref(fn, data.expr);
            if (type->kind == TypeKind::Fn) {
                ptr = ref.value;
            } else {
                ptr = ref.address;
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
            // For pointer-to-interface deref, return the fat pointer variable's address
            // so interface copy can update both data and vtable
            auto op1_type = get_chitype(data.op1);
            if (op1_type && op1_type->is_pointer_like() &&
                op1_type->get_elem() && ChiTypeStruct::is_interface(op1_type->get_elem())) {
                auto ref = compile_expr_ref(fn, data.op1);
                return RefValue::from_address(ref.address);
            }
            return RefValue::from_address(compile_expr(fn, data.op1));
        }
        case TokenType::LNOT: {
            if (data.is_suffix) {
                if (data.op1->resolved_type->kind == TypeKind::Optional) {
                    auto ref = compile_expr_ref(fn, data.op1);
                    auto has_value_p = builder.CreateStructGEP(
                        compile_type(data.op1->resolved_type), ref.address, 0);
                    builder.CreateStore(
                        llvm::ConstantInt::get(llvm::IntegerType::getInt1Ty(*m_ctx->llvm_ctx), 1),
                        has_value_p);
                    auto value_p = builder.CreateStructGEP(compile_type(data.op1->resolved_type),
                                                           ref.address, 1);
                    return RefValue::from_address(value_p);
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
            return RefValue::from_value(compile_expr(fn, expr));
        default:
            panic("compile_expr_ref UnaryOpExpr not implemented: {}", PRINT_ENUM(data.op_type));
        }
    }
    case ast::NodeType::IndexExpr: {
        auto &builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();
        auto &data = expr->data.index_expr;
        auto type = get_chitype(data.expr);
        auto subscript = compile_expr(fn, data.subscript);
        switch (type->kind) {
        case TypeKind::Pointer: {
            auto ptr = compile_expr(fn, data.expr);
            auto zero = llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(llvm_ctx), 0);
            return RefValue::from_address(
                builder.CreateGEP(compile_type(type->get_elem()), ptr, {subscript}));
        }
        case TypeKind::Struct: {
            auto ref = compile_expr_ref(fn, data.expr);
            auto method = data.resolved_method;
            auto variant_type_id = resolve_variant_type_id(fn, data.expr->resolved_type);
            auto method_node = get_variant_member_node(method, variant_type_id);
            auto index_fn = get_fn(method_node);
            auto call = builder.CreateCall(index_fn->llvm_fn, {ref.address, subscript});
            emit_dbg_location(expr);
            return RefValue::from_address(call);
        }
        default:
            panic("not implemented: {}", PRINT_ENUM(type->kind));
        }
    }
    case ast::NodeType::FnCallExpr: {
        auto &data = expr->data.fn_call_expr;
        auto &builder = *m_ctx->llvm_builder.get();
        auto ret = compile_fn_call(fn, expr);
        auto var = compile_alloc(fn, expr);
        builder.CreateStore(ret, var);
        emit_dbg_location(expr);
        return RefValue::from_address(var);
    }
    // Rvalue expressions - return value only (no meaningful address)
    case ast::NodeType::ParenExpr:
        return compile_expr_ref(fn, expr->data.child_expr);
    case ast::NodeType::SwitchExpr:
    case ast::NodeType::BinOpExpr:
    case ast::NodeType::ConstructExpr:
    case ast::NodeType::LiteralExpr:
    case ast::NodeType::CastExpr:
        return RefValue::from_value(compile_expr(fn, expr));
    default:
        panic("compile_expr_ref not implemented: {}", PRINT_ENUM(expr->type));
    }
    return {};
}

RefValue Compiler::compile_iden_ref(Function *fn, ast::Node *iden) {
    auto &builder = *m_ctx->llvm_builder.get();
    assert(iden->type == ast::NodeType::Identifier);
    auto &data = iden->data.identifier;

    if (data.kind == ast::IdentifierKind::This) {
        return RefValue::from_value(fn->get_this_arg());
    }
    // Unwrap ImportSymbol to reach the actual declaration
    auto *resolved_decl = data.decl;
    if (resolved_decl->type == ast::NodeType::ImportSymbol && resolved_decl->data.import_symbol.resolved_decl) {
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
            return RefValue::from_value(lambda_value);
        }

        // Otherwise, return the raw function pointer
        auto type_l = compile_type(iden_type);
        return RefValue::from_value(fn_obj->llvm_fn);
    }
    if (data.decl->type == ast::NodeType::ImportDecl) {
        // Module identifiers (import aliases) should not be compiled directly
        // They should only appear as part of DotExpr for module member access
        panic("Cannot compile module identifier '{}' directly - module member access should be resolved",
              iden->name);
    }

normal:
    // handle captured variables
    if (iden->escape.is_capture()) {
        assert(fn->bind_ptr);
        assert(iden->escape.capture_path.len > 0);

        // The capture_path[0] represents the current function's capture
        // We just need to access the immediate capture from current function's bind_ptr
        auto &immediate_capture = iden->escape.capture_path[0];
        auto capture_idx = immediate_capture.capture_index;

        // Get the binding structure for the current function
        auto fn_type = get_chitype(fn->node);
        assert(fn_type->kind == TypeKind::FnLambda);
        auto bstruct = fn_type->data.fn_lambda.bind_struct;
        auto bstruct_l = (llvm::StructType *)compile_type(bstruct);

        auto gep = builder.CreateStructGEP(bstruct_l, fn->bind_ptr, capture_idx);

        auto field_type = bstruct->data.struct_.fields[capture_idx]->resolved_type;
        if (field_type->kind != TypeKind::Reference) {
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
    std::vector<std::pair<llvm::Value *, ast::Node *>> *out_temporaries) {
    std::vector<llvm::Value *> call_args = {};
    auto &builder = *m_ctx->llvm_builder.get();
    llvm::Value *va_ptr = nullptr;
    ChiType *va_type = nullptr;
    auto &fn_spec = callee->fn_type->data.fn;
    auto va_start = fn_spec.get_va_start();

    bool is_variadic = callee->fn_type->data.fn.is_variadic;
    bool is_extern = callee->fn_type->data.fn.is_extern;

    // Only use array-based variadic for Chi functions, not extern C functions
    if (is_variadic && !is_extern) {
        auto array_type = fn_spec.params.last();
        va_type = array_type->get_elem();
        va_ptr = fn->entry_alloca(compile_type(array_type), "vararg_array");
        emit_dbg_location(fn_call);
        auto init_fn = get_system_fn("cx_array_new");
        builder.CreateCall(init_fn->llvm_fn, {va_ptr});
    }

    for (int i = 0; i < args.len; i++) {
        if (is_variadic && !is_extern && i >= va_start) {
            emit_dbg_location(args[i]);
            auto add_fn = get_system_fn("cx_array_add");
            auto arg = compile_assignment_to_type(fn, args[i], va_type);
            auto tsize =
                m_ctx->llvm_module->getDataLayout().getTypeAllocSize(compile_type(va_type));
            auto tsize_l = llvm::ConstantInt::get(*m_ctx->llvm_ctx, llvm::APInt(32, tsize));
            auto ptr = builder.CreateCall(add_fn->llvm_fn, {va_ptr, tsize_l});
            builder.CreateStore(arg, ptr);
            continue;
        }
        // For extern variadic functions, pass variadic args directly
        if (is_variadic && is_extern && i >= va_start) {
            // Compile the argument without wrapping in array
            auto arg = compile_expr(fn, args[i]);
            call_args.push_back(arg);
            continue;
        }
        auto arg_node = args[i];
        auto param_type = fn_spec.get_param_at(i);

        // Check if this argument is a ConstructExpr that will create a temporary
        // that needs destruction after the call
        if (out_temporaries && arg_node->type == ast::NodeType::ConstructExpr) {
            auto &construct_data = arg_node->data.construct_expr;
            // Only track non-new constructs without an outlet (these create temporaries)
            if (!construct_data.is_new && !construct_data.resolved_outlet) {
                auto arg_type = get_chitype(arg_node);
                auto destructor = ChiTypeStruct::get_destructor(arg_type);
                if (destructor) {
                    // Compile the construct expr to get temporary address
                    auto temp_ptr = compile_alloc(fn, arg_node, false, arg_type);
                    compile_construction(fn, temp_ptr, arg_type, arg_node);
                    auto value = builder.CreateLoad(compile_type(arg_type), temp_ptr);
                    call_args.push_back(value);
                    // Track for destruction after call
                    out_temporaries->push_back({temp_ptr, arg_node});
                    continue;
                }
            }
        }
        // For C variadic args (param_type is nullptr), compile the expression directly
        if (param_type) {
            call_args.push_back(compile_assignment_to_type(fn, arg_node, param_type));
        } else {
            call_args.push_back(compile_expr(fn, arg_node));
        }
    }

    if (va_ptr) {
        call_args.push_back(builder.CreateLoad(compile_type(fn_spec.params.last()), va_ptr));
        fn->vararg_pointers.add(va_ptr);
    }

    return call_args;
}

llvm::Value *Compiler::compile_fn_call(Function *fn, ast::Node *expr, InvokeInfo *invoke,
                                       llvm::Value *sret_dest) {
    auto &data = expr->data.fn_call_expr;
    auto &builder = *m_ctx->llvm_builder.get();

    // Handle __copy_from(dest, src, destruct_old) intrinsic
    {
        auto callee_decl = data.fn_ref_expr->get_decl();
        if (callee_decl && callee_decl->name == "__copy_from" && data.args.len == 3) {
            auto dest_ptr = compile_expr(fn, data.args[0]);
            auto src_ptr = compile_expr(fn, data.args[1]);
            auto elem_type = get_chitype(data.args[0])->get_elem();
            assert(elem_type && "first arg of __copy_from must be a pointer type");
            // Third arg must be a compile-time bool literal
            bool destruct_old = true;
            if (data.args[2]->type == ast::NodeType::LiteralExpr &&
                data.args[2]->token->type == TokenType::BOOL) {
                destruct_old = data.args[2]->token->val.b;
            }

            if (ChiTypeStruct::is_interface(elem_type)) {
                // For interface types, dest_ptr and src_ptr are fat pointers {data, vtable}.
                auto dest_data = builder.CreateExtractValue(dest_ptr, {0}, "dest_data");
                auto src_data = builder.CreateExtractValue(src_ptr, {0}, "src_data");
                auto src_vtable = builder.CreateExtractValue(src_ptr, {1}, "src_vtable");
                call_vtable_copier(fn, src_vtable, dest_data, src_data);
            } else {
                compile_copy_with_ref(fn, RefValue::from_address(src_ptr), dest_ptr, elem_type,
                                      nullptr, destruct_old);
            }
            if (invoke) {
                builder.CreateBr(invoke->normal);
            }
            return nullptr;
        }
    }

    if (data.fn_ref_expr->resolved_type->kind == TypeKind::FnLambda) {
        auto ref = compile_expr_ref(fn, data.fn_ref_expr);
        auto lambda_type = get_chitype(data.fn_ref_expr);
        auto &fn_spec = lambda_type->data.fn_lambda.bound_fn->data.fn;
        auto bound_fn_type_l =
            (llvm::FunctionType *)compile_type(lambda_type->data.fn_lambda.bound_fn);
        std::vector<llvm::Value *> args = {};

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
            sret_var = fn->entry_alloca(return_type_l, "lambda_sret");
            args.push_back(sret_var);
        }

        // Always pass binding struct pointer as argument for all lambdas
        args.push_back(data_ptr);

        for (int i = 0; i < data.args.len; i++) {
            auto arg = data.args[i];
            // User arguments always start from parameter index 1 (after binding struct)
            int param_index = i + 1;
            auto param_type = fn_spec.get_param_at(param_index);
            args.push_back(compile_assignment_to_type(fn, arg, param_type));
        }

        llvm::FunctionCallee callee(bound_fn_type_l, fn_ptr);
        llvm::Value *ret = nullptr;
        if (invoke) {
            ret = builder.CreateInvoke(callee, invoke->normal, invoke->landing, args);
            if (use_sret) {
                // For invoke with sret, the load happens later in the normal block
                invoke->sret = sret_var;
                invoke->sret_type = compile_type(return_type);
            }
            // Invoke is a terminator - return value will be loaded by caller if needed
            return ret;
        } else {
            ret = builder.CreateCall(callee, args);
            // For sret, load and return the struct value
            if (use_sret) {
                return builder.CreateLoad(compile_type(return_type), sret_var);
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
                auto concrete_member = concrete_struct->find_member(name);
                assert(concrete_member && "concrete member not found during codegen");
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
    llvm::Value *va_ptr = nullptr;
    ChiType *va_type = nullptr;
    // Only use array-based variadic for Chi functions, not extern C functions
    if (is_variadic && !is_extern) {
        auto array_type = fn_spec.params.last();
        va_type = array_type->get_elem();
        va_ptr = fn->entry_alloca(compile_type(array_type), "vararg_array");
        emit_dbg_location(data.fn_ref_expr);
        auto init_fn = get_system_fn("cx_array_new");
        builder.CreateCall(init_fn->llvm_fn, {va_ptr});
    }

    llvm::FunctionCallee callee;
    llvm::Value *ctn_ptr = nullptr;
    if (fn_spec.container_ref && !fn_decl->declspec().is_static()) {
        auto dot_expr = data.fn_ref_expr->data.dot_expr;
        if (!ctn_type) {
            ctn_type = get_chitype(dot_expr.expr);
        }
        // Unwrap Optional for ?. method calls
        if (ctn_type->kind == TypeKind::Optional) {
            ctn_type = ctn_type->get_elem();
        }
        auto ctn_type_l = compile_type(ctn_type);
        auto ptr = compile_dot_ptr(fn, dot_expr.expr);

        if (!fn_decl->data.fn_def.body) {
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
                if (param == ctn_type_l) {
                    param_types.push_back(get_llvm_ptr_type()); // thin pointer for 'this'
                } else {
                    param_types.push_back(param);
                }
            }
            auto dispatch_fn_type = llvm::FunctionType::get(
                orig_fn_type->getReturnType(), param_types, orig_fn_type->isVarArg());
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
            callee_fn = get_fn(fn_decl);
        }

        callee = callee_fn->llvm_fn;
    }

    // Track temporaries created for struct arguments that need destruction after the call
    std::vector<std::pair<llvm::Value *, ast::Node *>> arg_temporaries;

    for (int i = 0; i < data.args.len; i++) {
        if (is_variadic && !is_extern && i >= va_start) {
            emit_dbg_location(data.args[i]);

            // Check for pack expansion - use Array.copy_from to append all elements
            if (data.args[i]->type == ast::NodeType::PackExpansion) {
                auto &pack_data = data.args[i]->data.pack_expansion;
                auto src_ptr = compile_expr_ref(fn, pack_data.expr).address;

                // Call dest_array.copy_from(&src_array)
                auto array_type = fn_spec.params.last(); // variadic param is Array<any>
                auto array_struct = get_resolver()->resolve_struct_type(array_type);
                auto copy_from_member = array_struct->find_member("copy_from");
                assert(copy_from_member && "Array.copy_from() method not found");
                auto copy_from_method_node = get_variant_member_node(copy_from_member, std::nullopt);
                auto copy_from_fn = get_fn(copy_from_method_node);
                builder.CreateCall(copy_from_fn->llvm_fn, {va_ptr, src_ptr});
                continue;
            }

            auto add_fn = get_system_fn("cx_array_add");
            auto arg = compile_assignment_to_type(fn, data.args[i], va_type);
            auto tsize =
                m_ctx->llvm_module->getDataLayout().getTypeAllocSize(compile_type(va_type));
            auto tsize_l = llvm::ConstantInt::get(*m_ctx->llvm_ctx, llvm::APInt(32, tsize));
            auto ptr = builder.CreateCall(add_fn->llvm_fn, {va_ptr, tsize_l});
            builder.CreateStore(arg, ptr);
            continue;
        }
        // For extern variadic functions, pass variadic args directly
        if (is_variadic && is_extern && i >= va_start) {
            // Compile the argument without wrapping in array
            auto arg = compile_expr(fn, data.args[i]);
            args.push_back(arg);
            continue;
        }
        auto arg = data.args[i];
        auto param_type = fn_spec.get_param_at(i);

        // Check if this argument is a ConstructExpr that will create a temporary
        // that needs destruction after the call
        if (arg->type == ast::NodeType::ConstructExpr) {
            auto &construct_data = arg->data.construct_expr;
            // Only track non-new constructs without an outlet (these create temporaries)
            if (!construct_data.is_new && !construct_data.resolved_outlet) {
                auto arg_type = get_chitype(arg);
                auto destructor = ChiTypeStruct::get_destructor(arg_type);
                if (destructor) {
                    // Compile the construct expr to get temporary address
                    auto temp_ptr = compile_alloc(fn, arg, false, arg_type);
                    compile_construction(fn, temp_ptr, arg_type, arg);
                    auto value = builder.CreateLoad(compile_type(arg_type), temp_ptr);
                    args.push_back(value);
                    // Track for destruction after call
                    arg_temporaries.push_back({temp_ptr, arg});
                    continue;
                }
            }
        }
        // For C variadic args (param_type is nullptr), compile the expression directly
        if (param_type) {
            args.push_back(compile_assignment_to_type(fn, arg, param_type));
        } else {
            args.push_back(compile_expr(fn, arg));
        }
    }
    // Compile default values for missing arguments
    if (fn_decl->type == ast::NodeType::FnDef) {
        auto &proto = fn_decl->data.fn_def.fn_proto->data.fn_proto;
        for (size_t i = data.args.len; i < proto.params.len; i++) {
            auto default_val = proto.params[i]->data.param_decl.default_value;
            if (!default_val) break;
            auto param_type = fn_spec.get_param_at(i);
            args.push_back(compile_assignment_to_type(fn, default_val, param_type));
        }
    }

    if (va_ptr) {
        args.push_back(builder.CreateLoad(compile_type(fn_spec.params.last()), va_ptr));
        fn->vararg_pointers.add(va_ptr);
    }

    emit_dbg_location(expr);
    auto return_type = fn_type->data.fn.return_type;
    auto sret_type = fn_type->data.fn.should_use_sret() ? compile_type(return_type) : nullptr;
    auto result = create_fn_call_invoke(callee, args, sret_type, invoke, sret_dest);

    // Destroy temporaries after the call completes.
    // When using invoke, the invoke terminates the current block, so we must
    // switch to the normal continuation block before emitting destruction code.
    if (!arg_temporaries.empty()) {
        if (invoke) {
            fn->use_label(invoke->normal);
        }
        for (auto &[temp_ptr, temp_node] : arg_temporaries) {
            compile_destruction(fn, temp_ptr, temp_node);
        }
    }

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
        ret = builder.CreateInvoke(callee, invoke->normal, invoke->landing, args);
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
        builder.CreateStore(ret, sret_dest);
        return nullptr;
    }
    return sret_type ? builder.CreateLoad(sret_type, sret_var) : ret;
}

std::optional<TypeId> Compiler::resolve_variant_type_id(Function *fn, ChiType *type) {
    if (!type)
        return std::nullopt;

    // Unwrap pointer-like and optional types
    while (type && (type->kind == TypeKind::Pointer || type->kind == TypeKind::Reference ||
                    type->kind == TypeKind::MutRef || type->kind == TypeKind::MoveRef ||
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

    // Also handle specialized_subtype for generic function parameters (e.g., Promise<T> in promise<T>)
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

llvm::Value *Compiler::generate_method_proxy_function(Function *fn, ChiStructMember *method_member,
                                                      ChiType *lambda_type) {
    auto &builder = *m_ctx->llvm_builder.get();

    // Save current insert point
    auto saved_insert_point = builder.GetInsertBlock();

    // Get the original method function
    auto method_fn = get_fn(method_member->node);
    auto method_type = method_member->resolved_type;
    auto &method_spec = method_type->data.fn;

    // Create function type for the proxy: (bind_struct*, user_args...) -> return_type
    auto bound_fn_type = lambda_type->data.fn_lambda.bound_fn;
    auto proxy_fn_type = (llvm::FunctionType *)compile_type(bound_fn_type);

    // Create the proxy function
    auto proxy_name = fmt::format("__method_proxy_{}_{}",
                                  method_member->parent_struct->display_name.value_or("unnamed"),
                                  method_member->get_name());
    auto proxy_llvm_fn = llvm::Function::Create(proxy_fn_type, llvm::Function::InternalLinkage,
                                                proxy_name, m_ctx->llvm_module.get());

    // Create function body
    auto entry_bb = llvm::BasicBlock::Create(*m_ctx->llvm_ctx, "entry", proxy_llvm_fn);
    builder.SetInsertPoint(entry_bb);

    // Extract instance pointer from binding struct (first argument)
    auto bind_struct_ptr = proxy_llvm_fn->args().begin();
    auto bind_struct_type = compile_type(lambda_type->data.fn_lambda.bind_struct);
    auto instance_gep = builder.CreateStructGEP(bind_struct_type, bind_struct_ptr, 0);
    auto instance_ptr = builder.CreateLoad(bind_struct_type->getStructElementType(0), instance_gep);

    // Prepare arguments for the original method call
    std::vector<llvm::Value *> method_args;

    // First argument is the instance pointer
    method_args.push_back(instance_ptr);

    // Add user arguments (skip first argument which is the binding struct)
    auto arg_iter = proxy_llvm_fn->args().begin();
    ++arg_iter; // skip binding struct
    for (; arg_iter != proxy_llvm_fn->args().end(); ++arg_iter) {
        method_args.push_back(&*arg_iter);
    }

    // Call the original method
    auto result = builder.CreateCall(method_fn->llvm_fn, method_args);

    // Return the result
    if (method_spec.return_type->kind == TypeKind::Void) {
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
    // - If the original LLVM function has more params than original_fn_type, it's a lambda with _binds

    std::vector<llvm::Value *> original_args;
    auto proxy_arg_count = proxy_llvm_fn->arg_size();

    // Get the LLVM function from the original_fn_ptr
    auto original_llvm_fn = llvm::dyn_cast<llvm::Function>(original_fn_ptr);
    auto original_fn_type_l = (llvm::FunctionType *)compile_type(original_fn_type);

    // Determine if we should pass the _binds parameter
    // If the original function has more parameters than the original type spec, it has _binds
    bool original_has_binds = original_llvm_fn &&
                             (original_llvm_fn->arg_size() > original_fn_type_l->getNumParams());

    // Check if the function uses sret (struct return)
    bool use_sret = bound_fn_type->data.fn.should_use_sret();

    // Start index: skip sret arg (if present), then skip _binds if original doesn't take it
    unsigned start_idx = use_sret ? 1 : 0;  // Skip sret if present
    if (!original_has_binds) {
        start_idx++;  // Skip _binds
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
        auto sret_arg = proxy_llvm_fn->getArg(0);  // First arg is the sret pointer
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

llvm::Value *Compiler::compile_alloc(Function *fn, ast::Node *decl, bool is_new, ChiType *type) {
    auto &llvm_builder = *m_ctx->llvm_builder.get();
    auto &llvm_ctx = *m_ctx->llvm_ctx.get();
    auto &llvm_module = *m_ctx->llvm_module.get();
    auto var_type_l = type ? compile_type(type) : compile_type_of(decl);

    // Debug: Show what type compile_alloc is using
    auto chi_type = get_chitype(decl);
    assert(!chi_type->is_placeholder && "compile_alloc called on placeholder type");

    Function *alloc_fn = nullptr;
    if (is_managed()) {
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

        if (is_managed() && alloc_fn->qualified_name == "cx_gc_alloc") {
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

    switch (stmt->type) {
    case ast::NodeType::DestructureDecl: {
        auto &data = stmt->data.destructure_decl;
        auto &builder = *m_ctx->llvm_builder.get();

        // Evaluate RHS and store in temp
        auto source_type = get_chitype(data.expr);
        auto temp_ptr = compile_alloc(fn, data.temp_var);
        add_var(data.temp_var, temp_ptr);
        auto rhs_value = compile_assignment_to_type(fn, data.expr, source_type);
        if (rhs_value) {
            compile_store_or_copy(fn, rhs_value, temp_ptr, source_type, data.expr);
        }

        // Extract fields
        compile_destructure_fields(fn, data.fields, temp_ptr, source_type);
        break;
    }
    case ast::NodeType::VarDecl: {
        auto &data = stmt->data.var_decl;
        auto &llvm_builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();
        auto &llvm_module = *m_ctx->llvm_module.get();

        // Narrowed variable: GEP alias into original optional/result value field
        if (data.narrowed_from) {
            auto ref = compile_expr_ref(fn, data.narrowed_from);
            if (ref.address) {
                auto original_type_l = compile_type(get_chitype(data.narrowed_from));
                auto value_ptr = llvm_builder.CreateStructGEP(original_type_l, ref.address, 1);
                add_var(stmt, value_ptr);
                break;
            }
        }

        auto var = compile_alloc(fn, stmt);
        add_var(stmt, var);
        auto var_type = get_chitype(stmt);

        if (data.expr) {
            if (data.expr->type == ast::NodeType::FnCallExpr) {
                auto &fn_call_data = data.expr->data.fn_call_expr;
                // Only use direct sret for regular function calls, not lambdas or optional chains
                bool is_lambda = fn_call_data.fn_ref_expr->resolved_type->kind == TypeKind::FnLambda;
                bool is_optional_chain = fn_call_data.fn_ref_expr->type == ast::NodeType::DotExpr &&
                                         fn_call_data.fn_ref_expr->data.dot_expr.is_optional_chain;
                if (!is_lambda && !is_optional_chain) {
                    // Pass var directly as sret destination - avoids intermediate copy
                    compile_fn_call(fn, data.expr, nullptr, var);
                } else {
                    // Lambda calls - use original path
                    auto value = compile_assignment_to_type(fn, data.expr, var_type);
                    if (value)
                        compile_store_or_copy(fn, value, var, var_type, data.expr);
                }
            } else {
                // For all other expressions, use original path
                auto value = compile_assignment_to_type(fn, data.expr, var_type);
                if (value) {
                    compile_store_or_copy(fn, value, var, var_type, data.expr);
                    // For lambda expressions with captures, compile_copy retains the captures
                    // but the temp value (from compile_lambda_alloc) already has refcount 1.
                    // Release the temp's captures to transfer ownership to the variable.
                    if (!data.expr->escape.moved && var_type->kind == TypeKind::FnLambda) {
                        auto captures_ptr = llvm_builder.CreateExtractValue(value, {2});
                        auto release_fn = get_system_fn("cx_capture_release");
                        llvm_builder.CreateCall(release_fn->llvm_fn, {captures_ptr});
                    }
                }
            }
        }
        break;
    }
    case ast::NodeType::ReturnStmt: {
        auto &data = stmt->data.return_stmt;
        auto &llvm_builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();
        assert(fn->return_label);
        auto scope = fn->get_scope();

        {
            // Check if this is an async function returning T (wrapped to Promise<T>)
            bool is_async = fn->node && fn->node->type == ast::NodeType::FnDef &&
                            fn->node->data.fn_def.is_async();
            auto return_type = fn->fn_type->data.fn.return_type;

            if (is_async && get_resolver()->is_promise_type(return_type)) {
                // For async functions, wrap the return value in a resolved Promise
                auto promise_struct = get_resolver()->resolve_struct_type(return_type);
                auto return_type_l = compile_type(return_type);

                // Zero-initialize return_value before calling Promise.new()
                auto size = m_ctx->llvm_module->getDataLayout().getTypeAllocSize(return_type_l);
                llvm_builder.CreateMemSet(fn->return_value,
                    llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(*m_ctx->llvm_ctx), 0),
                    size, {});

                std::optional<TypeId> variant_type_id = std::nullopt;
                if (return_type->kind == TypeKind::Subtype && !return_type->is_placeholder) {
                    variant_type_id = return_type->id;
                }

                // Call Promise.new() to initialize promise at return_value
                auto new_member = promise_struct->find_member("new");
                assert(new_member && "Promise.new() method not found");
                auto new_method_node = get_variant_member_node(new_member, variant_type_id);
                auto new_method = get_fn(new_method_node);
                llvm_builder.CreateCall(new_method->llvm_fn, {fn->return_value});

                // Compile return value, or synthesize Unit{} for bare `return;`
                auto inner_type = get_resolver()->get_promise_value_type(return_type);
                llvm::Value *ret_value;
                if (data.expr) {
                    ret_value = compile_assignment_to_type(fn, data.expr, inner_type);
                } else {
                    ret_value = llvm::Constant::getNullValue(compile_type(inner_type));
                }

                // Call Promise.resolve(value)
                auto resolve_member = promise_struct->find_member("resolve");
                assert(resolve_member && "Promise.resolve() method not found");
                auto resolve_method_node = get_variant_member_node(resolve_member, variant_type_id);
                auto resolve_method = get_fn(resolve_method_node);
                llvm_builder.CreateCall(resolve_method->llvm_fn, {fn->return_value, ret_value});
            } else if (data.expr) {
                auto ret_type = get_chitype(stmt);
                compile_assignment_to_ptr(fn, data.expr, fn->return_value, ret_type);
            }
        }
        // Destroy all active block-local vars (inner to outer) before returning
        for (int i = fn->active_blocks.size() - 1; i >= 0; i--) {
            compile_block_cleanup(fn, fn->active_blocks[i]);
        }
        llvm_builder.CreateBr(fn->return_label);
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

        // Call cx_throw(type_info, data_ptr, vtable_ptr, type_id)
        auto throw_fn = get_system_fn("cx_throw");
        auto type_id = llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(llvm_ctx), elem_type->id);
        llvm_builder.CreateCall(throw_fn->llvm_fn,
                                {type_info, error_ref, vtable, type_id});
        llvm_builder.CreateUnreachable();
        scope->branched = true;
        break;
    }
    case ast::NodeType::BranchStmt: {
        auto token = stmt->token;
        auto loop = fn->get_loop();
        auto &builder = *m_ctx->llvm_builder.get();
        // Destroy block-local vars between current scope and the loop boundary
        for (int i = fn->active_blocks.size() - 1; i >= (int)loop->active_blocks_depth; i--) {
            compile_block_cleanup(fn, fn->active_blocks[i]);
        }
        if (token->type == TokenType::KW_BREAK) {
            builder.CreateBr(loop->end);
        }
        if (token->type == TokenType::KW_CONTINUE) {
            builder.CreateBr(loop->continue_target ? loop->continue_target : loop->start);
        }
        break;
    }
    case ast::NodeType::IfStmt: {
        auto &data = stmt->data.if_stmt;
        auto &builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();
        auto &llvm_module = *m_ctx->llvm_module.get();
        auto cond = compile_assignment_to_type(fn, data.condition, get_system_types()->bool_);
        auto bb = builder.GetInsertBlock();
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
        compile_block(fn, stmt, data.then_block, end_b);

        if (data.else_node) {
            fn->use_label(else_b);
            if (data.else_node->type == ast::NodeType::Block) {
                compile_block(fn, stmt, data.else_node, end_b);
            } else {
                compile_stmt(fn, data.else_node);
                builder.CreateBr(end_b);
            }
        }

        fn->use_label(end_b);
        for (auto var : data.post_narrow_vars) {
            compile_stmt(fn, var);
        }
        break;
    }
    case ast::NodeType::ForStmt: {
        auto &builder = *m_ctx->llvm_builder.get();
        auto &data = stmt->data.for_stmt;
        if (data.kind == ast::ForLoopKind::IntRange) {
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
            builder.CreateStore(
                builder.CreateAdd(cur, llvm::ConstantInt::get(iter_type, 1)), it);
            builder.CreateBr(loop->start);

            fn->use_label(loop->end);
            fn->pop_loop();

        } else if (data.kind == ast::ForLoopKind::Range) {
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
                item_var = builder.CreateAlloca(compile_type(data.bind->resolved_type), nullptr,
                                                "_bind_item_var");
                add_var(data.bind, item_var);
            }
            llvm::Value *index_var = nullptr;
            if (data.index_bind) {
                auto idx_type = llvm::Type::getInt32Ty(m_ctx->llvm_module->getContext());
                index_var = builder.CreateAlloca(idx_type, nullptr, "_bind_index_var");
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

        } else if (data.kind == ast::ForLoopKind::Iter) {
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

            llvm::Value *item_var = nullptr;
            if (data.bind) {
                item_var = builder.CreateAlloca(compile_type(data.bind->resolved_type), nullptr,
                                                "_bind_item_var");
                add_var(data.bind, item_var);
            }
            auto idx_type = llvm::Type::getInt32Ty(m_ctx->llvm_module->getContext());
            llvm::Value *index_var = nullptr;
            if (data.index_bind) {
                index_var = builder.CreateAlloca(idx_type, nullptr, "_bind_index_var");
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
            auto has_value = builder.CreateLoad(
                llvm::Type::getInt1Ty(m_ctx->llvm_module->getContext()), has_value_p);
            builder.CreateCondBr(has_value, loop_main, loop->end);

            fn->use_label(loop_main);
            auto loop_post = fn->new_label("_for_post");
            loop->continue_target = loop_post;
            if (item_var) {
                // Extract value (field 1) — this is &mut T (a pointer)
                auto value_p = builder.CreateStructGEP(next_ret_type_l, opt_alloca, 1);
                auto value = builder.CreateLoad(compile_type(data.bind->resolved_type),
                                                value_p, "_iter_item");
                builder.CreateStore(value, item_var);
            }
            compile_block(fn, stmt, data.body, loop_post);

            fn->use_label(loop_post);
            if (index_var) {
                auto cur = builder.CreateLoad(idx_type, index_var);
                builder.CreateStore(
                    builder.CreateAdd(cur, llvm::ConstantInt::get(idx_type, 1)), index_var);
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

void Compiler::compile_block_cleanup(Function *fn, ast::Block *block) {
    auto *fn_def = fn->get_def();
    for (int i = block->cleanup_vars.len - 1; i >= 0; i--) {
        auto var = block->cleanup_vars[i];
        if (fn_def->is_sunk(var)) continue;
        // Skip variables not yet compiled (e.g. early return before var decl)
        if (!m_ctx->var_table.has_key(var)) continue;
        compile_destruction(fn, get_var(var), var);
    }
}

void Compiler::compile_destruction(Function *fn, llvm::Value *address, ast::Node *node) {
    // In managed memory mode, don't destroy heap-allocated objects locally - GC handles them
    if (is_managed() && node->is_heap_allocated()) {
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

    if (!type) return;

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
        auto ti_header_l = llvm::StructType::get(llvm_ctx, {i32_ty, i32_ty, ptr_type, ptr_type}, true);
        auto dtor_gep = builder.CreateStructGEP(ti_header_l, ti_ptr, 3);
        auto dtor_ptr = builder.CreateLoad(ptr_type, dtor_gep, "any_dtor");

        // Resolve data pointer: inlined → &any.data, not inlined → *(void**)&any.data
        auto inlined_gep = builder.CreateStructGEP(any_type_l, address, 1);
        auto inlined = builder.CreateLoad(i8_ty, inlined_gep, "any_inlined");
        auto is_inlined = builder.CreateICmpNE(inlined, llvm::ConstantInt::get(i8_ty, 0));
        auto data_gep = builder.CreateStructGEP(any_type_l, address, 2);

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
        auto dtor_fn_type = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type}, false);
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

    // Handle optionals, structs, results, and enum values via generated __delete
    if (type->kind != TypeKind::Optional && type->kind != TypeKind::Struct &&
        type->kind != TypeKind::Result && type->kind != TypeKind::EnumValue) {
        return;
    }

    auto dtor = generate_destructor(original_type, nullptr);
    if (dtor) {
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
    auto dtor_gep = builder.CreateGEP(ptr_type, vtable_ptr,
        {llvm::ConstantInt::get(*m_ctx->llvm_ctx, llvm::APInt(32, 1))});
    auto dtor_ptr = builder.CreateLoad(ptr_type, dtor_gep);

    auto is_null = builder.CreateICmpEQ(dtor_ptr, get_null_ptr());
    auto then_bb = fn->new_label("dtor_call");
    auto merge_bb = fn->new_label("dtor_merge");
    builder.CreateCondBr(is_null, merge_bb, then_bb);

    fn->use_label(then_bb);
    auto dtor_fn_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(*m_ctx->llvm_ctx), {ptr_type}, false);
    builder.CreateCall(dtor_fn_type, dtor_ptr, {data_ptr});
    builder.CreateBr(merge_bb);

    fn->use_label(merge_bb);
}

void Compiler::call_vtable_copier(Function *fn, llvm::Value *vtable_ptr,
                                  llvm::Value *dest_data, llvm::Value *src_data) {
    auto &builder = *m_ctx->llvm_builder;
    auto ptr_type = get_llvm_ptr_type();

    // Load copier from vtable[2] (index 0=typeinfo, 1=destructor, 2=copier)
    auto copier_gep = builder.CreateGEP(ptr_type, vtable_ptr,
        {llvm::ConstantInt::get(*m_ctx->llvm_ctx, llvm::APInt(32, 2))});
    auto copier_fn_ptr = builder.CreateLoad(ptr_type, copier_gep, "copier_fn");
    auto copier_is_null = builder.CreateICmpEQ(copier_fn_ptr, get_null_ptr());
    auto bb_copy = fn->new_label("vtable_copy");
    auto bb_memcpy = fn->new_label("vtable_memcpy");
    auto bb_done = fn->new_label("vtable_copy_done");
    builder.CreateCondBr(copier_is_null, bb_memcpy, bb_copy);

    // Call copier(dest_data, src_data)
    fn->use_label(bb_copy);
    auto copier_fn_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(*m_ctx->llvm_ctx), {ptr_type, ptr_type}, false);
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

llvm::Value *Compiler::load_typesize_from_vtable(llvm::Value *vtable_ptr) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto ptr_type = get_llvm_ptr_type();

    // vtable[0] = typeinfo pointer
    auto typeinfo_ptr = builder.CreateLoad(ptr_type, vtable_ptr, "typeinfo_ptr");

    // TypeInfo struct is packed: {i32 kind, i32 typesize, ...}
    // We only need to read the first two i32 fields
    auto ti_header_l = llvm::StructType::get(
        llvm_ctx, {llvm::Type::getInt32Ty(llvm_ctx), llvm::Type::getInt32Ty(llvm_ctx)}, true);
    auto size_gep = builder.CreateStructGEP(ti_header_l, typeinfo_ptr, 1);
    return builder.CreateLoad(llvm::Type::getInt32Ty(llvm_ctx), size_gep, "typesize");
}

llvm::Value *Compiler::find_interface_vtable(Function *fn, ChiType *iface_type) {
    auto &builder = *m_ctx->llvm_builder;

    if (!fn || !fn->container_subtype) return nullptr;

    auto container_type = fn->container_subtype->final_type;
    if (!container_type) return nullptr;
    container_type = eval_type(container_type);
    if (container_type->kind != TypeKind::Struct) return nullptr;

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

    if (!target_field) return nullptr;

    auto container_type_l = compile_type(container_type);
    auto fat_ptr_type_l = compile_type(field_type);

    // Prefer function parameters over `this` (params are more likely to have valid data)
    if (fn->node && fn->node->type == ast::NodeType::FnDef) {
        auto proto_node = fn->node->data.fn_def.fn_proto;
        for (auto &param_info : fn->parameter_info) {
            if (param_info.kind != ParameterKind::Regular) continue;
            auto param_type = param_info.type;
            if (!param_type || !param_type->is_pointer_like()) continue;
            auto param_elem = param_type->get_elem();
            if (!param_elem) continue;
            param_elem = eval_type(param_elem);
            if (get_resolver()->format_type_id(param_elem) !=
                get_resolver()->format_type_id(container_type)) continue;

            // This param references the same container struct — use its fat pointer field
            auto param_node = proto_node->data.fn_proto.params[param_info.user_param_index];
            auto param_alloca = get_var(param_node);
            auto struct_ptr = builder.CreateLoad(get_llvm_ptr_type(), param_alloca, "param_struct_ptr");
            auto field_gep = builder.CreateStructGEP(container_type_l, struct_ptr,
                                                     target_field->field_index);
            auto fat_ptr = builder.CreateLoad(fat_ptr_type_l, field_gep, "iface_fat_ptr");
            return builder.CreateExtractValue(fat_ptr, {1}, "vtable_ptr");
        }
    }

    // Fall back to `this` pointer
    if (fn->bind_ptr) {
        auto field_gep = builder.CreateStructGEP(container_type_l, fn->bind_ptr,
                                                 target_field->field_index);
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

    // Handle Optional types
    if (resolved_type->kind == TypeKind::Optional) {
        return generate_destructor_optional(type, resolved_type);
    }

    // Handle Result types — owns the error object's heap allocation
    if (resolved_type->kind == TypeKind::Result) {
        return generate_destructor_result(type, resolved_type);
    }

    // Handle EnumValue types — switch on discriminator to destroy variant fields
    if (resolved_type->kind == TypeKind::EnumValue) {
        return generate_destructor_enum(type, resolved_type);
    }

    if (resolved_type->kind != TypeKind::Struct) {
        return nullptr;
    }

    // Create function type: void __delete(T*)
    auto struct_ptr_type = get_llvm_ptr_type();
    auto fn_type_l = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {struct_ptr_type}, false);

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

    // Create entry block
    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    builder.SetInsertPoint(entry_bb);

    auto this_ptr = llvm_fn->getArg(0);
    auto llvm_struct_type = compile_type(resolved_type);

    // 1. Call user's delete() if it exists
    auto user_destructor = ChiTypeStruct::get_destructor(resolved_type);
    if (user_destructor) {
        auto destructor_type = get_chitype(user_destructor->node);
        auto destructor_id = get_resolver()->resolve_global_id(user_destructor->node);
        auto destructor_fn_ptr = m_ctx->function_table.get(destructor_id);
        Function *destructor_fn = nullptr;
        if (!destructor_fn_ptr) {
            // Compile on demand
            auto proto = user_destructor->node->data.fn_def.fn_proto;
            destructor_fn = compile_fn_proto(proto, user_destructor->node);
            // Set container subtype so generic type parameters can be resolved
            if (type->kind == TypeKind::Subtype) {
                destructor_fn->container_subtype = &type->data.subtype;
                destructor_fn->container_type = type;
                // Look up TypeEnv from GenericResolver
                if (auto entry = get_resolver()->get_generics()->struct_envs.get(type->global_id)) {
                    destructor_fn->type_env = &entry->subs;
                }
            }
            m_ctx->pending_fns.add(destructor_fn);
        } else {
            destructor_fn = *destructor_fn_ptr;
        }
        auto destructor_type_l = (llvm::FunctionType *)compile_type(destructor_type);
        builder.CreateCall(destructor_type_l, destructor_fn->llvm_fn, {this_ptr});
    }

    // 2. Destroy fields that need destruction (in reverse order)
    auto &struct_data = resolved_type->data.struct_;
    auto fields = struct_data.fields;

    // Iterate in reverse order
    for (int i = fields.len - 1; i >= 0; i--) {
        auto field = fields[i];
        auto field_type = field->resolved_type;
        auto resolved_field_type = field_type;

        // Resolve Subtype for checking
        while (resolved_field_type && resolved_field_type->kind == TypeKind::Subtype) {
            auto final_type = resolved_field_type->data.subtype.final_type;
            if (final_type) {
                resolved_field_type = final_type;
            } else {
                break;
            }
        }

        if (!resolved_field_type) continue;

        // Check if field needs destruction using resolver's utility
        if (!get_resolver()->type_needs_destruction(resolved_field_type)) {
            continue;
        }

        auto field_gep = builder.CreateStructGEP(llvm_struct_type, this_ptr, field->field_index);
        // Pass original field_type to preserve Subtype info for container_subtype resolution
        compile_destruction_for_type(fn, field_gep, field_type);
    }

    builder.CreateRetVoid();

    // Restore insert point
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
    auto fn_type_l = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type, ptr_type}, false);

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
    compile_copy_with_ref(fn, RefValue::from_address(src_ptr), dest_ptr, resolved_type, nullptr, false);

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
    // Check if type needs non-trivial copy (has destructor, CopyFrom, string, lambda, etc.)
    bool needs_copier = get_resolver()->type_needs_destruction(type);
    if (!needs_copier && type->kind == TypeKind::Struct) {
        auto sty = get_resolver()->resolve_struct_type(eval_type(type));
        if (sty && sty->member_intrinsics.get(IntrinsicSymbol::CopyFrom))
            needs_copier = true;
    }
    if (!needs_copier)
        return nullptr;

    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;
    auto ptr_type = get_llvm_ptr_type();

    // void __any_copy_T(void* dest, void* src)
    auto fn_type_l = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {ptr_type, ptr_type}, false);
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

Function *Compiler::generate_destructor_result(ChiType *type, ChiType *resolved_type) {
    // TODO: implement once TypeInfo destructor is in place
    m_ctx->destructor_table[type] = nullptr;
    return nullptr;
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
        for (int i = fields.len - 1; i >= 0; i--) {
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
        if (any_variant_needs) break;
    }

    if (any_variant_needs) {
        // 3. Load discriminator from field 0
        auto disc_gep = builder.CreateStructGEP(enum_type_l, this_ptr, 0);
        auto disc = builder.CreateLoad(compile_type(enum_->discriminator), disc_gep, "disc");

        // 4. Variant data index matches __data field_index used by compile_dot_access
        auto variant_data_idx = bvs ? (unsigned)bvs->data.struct_.fields.len : 0u;

        auto bb_done = fn->new_label("enum_dtor_done");
        auto sw = builder.CreateSwitch(disc, bb_done, enum_->variants.len);

        for (auto variant : enum_->variants) {
            auto vs = variant->resolved_type->data.enum_value.variant_struct;
            if (!vs) continue;

            bool variant_needs = false;
            for (auto field : vs->data.struct_.fields) {
                if (get_resolver()->type_needs_destruction(field->resolved_type)) {
                    variant_needs = true;
                    break;
                }
            }
            if (!variant_needs) continue;

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
            for (int i = vfields.len - 1; i >= 0; i--) {
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

    if (!resolved_type || resolved_type->kind != TypeKind::EnumValue) {
        return nullptr;
    }

    auto enum_ = resolved_type->data.enum_value.parent_enum();
    auto bvs = enum_->base_value_type->data.enum_value.resolved_struct;

    // Check if any field needs deep copy (destructor OR CopyFrom)
    bool needs_copier = get_resolver()->type_needs_destruction(resolved_type);
    if (!needs_copier) {
        // Also check if any field has CopyFrom (copy semantics without destructor)
        auto check_field_copier = [&](ChiType *field_type) -> bool {
            auto sty = get_resolver()->resolve_struct_type(eval_type(field_type));
            return sty && sty->member_intrinsics.get(IntrinsicSymbol::CopyFrom);
        };
        if (bvs) {
            for (auto field : bvs->data.struct_.fields) {
                if (check_field_copier(field->resolved_type)) { needs_copier = true; break; }
            }
        }
        if (!needs_copier) {
            for (auto variant : enum_->variants) {
                if (auto vs = variant->resolved_type->data.enum_value.variant_struct) {
                    for (auto field : vs->data.struct_.fields) {
                        if (check_field_copier(field->resolved_type)) { needs_copier = true; break; }
                    }
                }
                if (needs_copier) break;
            }
        }
    }
    if (!needs_copier) {
        m_ctx->copier_table[type] = nullptr;
        return nullptr;
    }

    // Create function type: void __copy(T* dest, T* src)
    auto ptr_type = get_llvm_ptr_type();
    auto fn_type_l = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx),
                                              {ptr_type, ptr_type}, false);

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

    // 1. memset dest to 0
    auto zero = llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(llvm_ctx), 0);
    builder.CreateMemSet(dest_ptr, zero, full_size, {});

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

    // Helper: check if a field type needs deep copy (destructor or CopyFrom)
    auto field_needs_deep_copy = [&](ChiType *field_type) -> bool {
        if (get_resolver()->type_needs_destruction(field_type))
            return true;
        auto sty = get_resolver()->resolve_struct_type(eval_type(field_type));
        return sty && sty->member_intrinsics.get(IntrinsicSymbol::CopyFrom);
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
        if (any_variant_needs) break;
    }

    if (any_variant_needs) {
        // Load discriminator from src, switch for variant fields
        auto disc_gep = builder.CreateStructGEP(enum_type_l, src_ptr, 0);
        auto disc = builder.CreateLoad(compile_type(enum_->discriminator), disc_gep, "disc");

        // Variant data index matches __data field_index used by compile_dot_access
        auto variant_data_idx = bvs ? (unsigned)bvs->data.struct_.fields.len : 0u;

        auto bb_done = fn->new_label("enum_copy_done");
        auto sw = builder.CreateSwitch(disc, bb_done, enum_->variants.len);

        for (auto variant : enum_->variants) {
            auto vs = variant->resolved_type->data.enum_value.variant_struct;
            if (!vs) continue;

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

Function *Compiler::generate_destructor_continuation(llvm::StructType *capture_struct_type,
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

Function *Compiler::generate_constructor(ChiType *struct_type, ChiType *container_type) {
    // Check if already generated
    auto existing = m_ctx->constructor_table.get(struct_type);
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

    // Check if any field has a default value
    auto &struct_data = resolved_type->data.struct_;
    bool has_defaults = false;
    for (auto field : struct_data.fields) {
        if (!field->node) continue;
        auto default_expr = field->node->data.var_decl.expr;
        if (default_expr) {
            has_defaults = true;
            break;
        }
    }

    if (!has_defaults) {
        // No defaults - no need for __new
        m_ctx->constructor_table[struct_type] = nullptr;
        return nullptr;
    }

    // Create function type: void __new(T*)
    auto struct_ptr_type = get_llvm_ptr_type();
    auto fn_type_l = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_ctx), {struct_ptr_type}, false);

    // Generate unique name for the constructor
    auto type_name = get_resolver()->format_type_display(struct_type);
    auto fn_name = fmt::format("{}.__new", type_name);

    auto llvm_fn = llvm::Function::Create(fn_type_l, llvm::Function::InternalLinkage, fn_name,
                                          m_ctx->llvm_module.get());

    // Create Function object
    auto fn = new Function(m_ctx, llvm_fn, nullptr);
    fn->qualified_name = fn_name;
    m_ctx->functions.emplace(fn);
    m_ctx->constructor_table[struct_type] = fn;

    // Save current insert point
    auto saved_block = builder.GetInsertBlock();
    auto saved_point = builder.GetInsertPoint();

    // Create entry block
    auto entry_bb = llvm::BasicBlock::Create(llvm_ctx, "entry", llvm_fn);
    builder.SetInsertPoint(entry_bb);

    auto this_ptr = llvm_fn->getArg(0);
    auto llvm_struct_type = compile_type(resolved_type);

    // Initialize all fields with default values
    for (auto field : struct_data.fields) {
        if (!field->node) continue;
        auto default_expr = field->node->data.var_decl.expr;
        if (!default_expr) continue;

        auto field_gep = builder.CreateStructGEP(llvm_struct_type, this_ptr, field->field_index);

        // Clear resolved_outlet on ConstructExpr to avoid using stale outlets
        if (default_expr->type == ast::NodeType::ConstructExpr) {
            std::function<void(ast::Node *)> clear_outlets = [&](ast::Node *node) {
                if (!node) return;
                if (node->type == ast::NodeType::ConstructExpr) {
                    node->data.construct_expr.resolved_outlet = nullptr;
                    for (auto item : node->data.construct_expr.items) {
                        clear_outlets(item);
                    }
                }
            };
            clear_outlets(default_expr);
        }
        compile_assignment_to_ptr(fn, default_expr, field_gep, field->resolved_type);
    }

    builder.CreateRetVoid();

    // Restore insert point
    if (saved_block) {
        builder.SetInsertPoint(saved_block, saved_point);
    }

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
    fn->active_blocks.push_back(&data);
    for (auto stmt : data.statements) {
        compile_stmt(fn, stmt);
    }
    if (data.return_expr) {
        result = compile_expr(fn, data.return_expr);
    }

    // Destroy block-local vars (only if block didn't already branch away via return/break
    // and current BB isn't already terminated by a nested return/break)
    if (!scope->branched && !builder.GetInsertBlock()->getTerminator()) {
        compile_block_cleanup(fn, &data);
    }
    fn->active_blocks.pop_back();
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
            // Build ID using the container's global_id (includes module prefix)
            auto container_id = container->global_id.empty()
                ? get_resolver()->format_type_id(container)
                : container->global_id;
            auto subst_id = fmt::format("{}.{}.{}",
                node->module->global_id(),
                container_id,
                node->name);
            entry = m_ctx->function_table.get(subst_id);
        }
    }

    // Placeholder GeneratedFn with type_env: the concrete variant exists on the
    // original fn_def but wasn't compiled yet (created after codegen passed over
    // the function's declaration). Compile its proto now; body goes to pending_fns.
    if (!entry && m_fn && m_fn->type_env &&
        node->type == ast::NodeType::GeneratedFn &&
        node->data.generated_fn.fn_subtype->is_placeholder) {
        auto &gfn = node->data.generated_fn;
        array<ChiType *> concrete_args;
        for (auto arg : gfn.fn_subtype->data.subtype.args)
            concrete_args.add(eval_type(arg));

        auto variant = get_resolver()->get_fn_variant(
            get_resolver()->node_get_type(gfn.original_fn), &concrete_args, gfn.original_fn);
        auto fn = compile_fn_proto(gfn.original_fn->data.fn_def.fn_proto, variant, "");
        m_ctx->pending_fns.add(fn);
        entry = m_ctx->function_table.get(get_resolver()->resolve_global_id(variant));
    }

    if (!entry) {
        panic("Function not found: {}", id);
    }
    return *entry;
}

Function *Compiler::compile_fn_proto(ast::Node *proto_node, ast::Node *fn, string name) {
    auto declspec = fn->declspec_ref();
    auto subtype =
        fn->type == ast::NodeType::GeneratedFn ? fn->data.generated_fn.fn_subtype : nullptr;
    m_fn_eval_subtype = subtype;
    auto ftype = get_chitype(fn);

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

    auto ftype_l = (llvm::FunctionType *)compile_type(ftype);

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

    auto new_fn = add_fn(fn_l, fn, ftype);

    // Store the specialized type and look up TypeEnv from GenericResolver
    if (subtype) {
        new_fn->specialized_subtype = subtype;
        // Look up the TypeEnv for this function specialization
        auto fn_id = get_resolver()->resolve_global_id(fn);
        if (auto entry = get_resolver()->get_generics()->fn_envs.get(fn_id)) {
            new_fn->type_env = &entry->subs;
        } else if (getenv("DUMP_GENERICS")) {
            print("WARNING: No TypeEnv found for function: {}\n", fn_id);
        }
    }

    // Build parameter information
    std::vector<ParameterInfo> param_info;
    int llvm_index = 0;

    // Add sret parameter if needed
    bool has_sret = ftype->data.fn.should_use_sret() && !declspec.is_extern();
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
    for (int user_idx = 0; user_idx < proto_node->data.fn_proto.params.len; user_idx++) {
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
    // For lambda functions, use the passed name instead of the empty qualified_name
    else if (fn && fn->type == ast::NodeType::FnDef &&
             fn->data.fn_def.fn_kind == ast::FnKind::Lambda && !name.empty()) {
        new_fn->qualified_name = name;
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
    if (type->kind == TypeKind::EnumValue) {
        if (type->data.enum_value.member) {
            return compile_type(type->data.enum_value.parent_enum()->base_value_type);
        }
    }

    type = eval_type(type);
    auto key = get_resolver()->format_type_id(type);
    // *Interface, &Interface, Mut<Interface>, and bare Interface are all the same fat pointer type
    if ((type->kind == TypeKind::Pointer || type->kind == TypeKind::Reference ||
         type->kind == TypeKind::MutRef || type->kind == TypeKind::MoveRef) &&
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
    assert(!type->is_placeholder && "compile_type called on placeholder type");
    auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
    switch (type->kind) {
    case TypeKind::This: {
        if (m_fn && m_fn->container_subtype) {
            return m_fn->get_this_arg()->getType();
        }
        return compile_type(type->get_elem());
    }
    case TypeKind::Never:
    case TypeKind::Void: {
        return llvm::Type::getVoidTy(llvm_ctx);
    }
    case TypeKind::Bool: {
        return llvm::Type::getInt1Ty(llvm_ctx);
    }
    case TypeKind::Char: {
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
        auto param_count = data.params.len;
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
                members,
                "FatIFacePointer<" + get_resolver()->format_type_display(data.elem) + ">");
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
        return compile_type(type->data.array.internal);
    }
    case TypeKind::Any: {
        std::vector<llvm::Type *> members;
        members.push_back(get_llvm_ptr_type());
        members.push_back(llvm::Type::getInt8Ty(llvm_ctx));
        members.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx), 23));
        return llvm::StructType::create(members, "Any");
    }
    case TypeKind::Result: {
        auto &data = type->data.result;
        return compile_type(data.internal);
    }
    case TypeKind::Struct: {
        auto key = get_resolver()->format_type_id(type);
        auto &data = type->data.struct_;
        if (!data.fields.len) {
            // Empty structs need a placeholder byte for LLVM allocations
            // (void type cannot be allocated)
            std::vector<llvm::Type *> members;
            members.push_back(llvm::Type::getInt8Ty(llvm_ctx));
            return llvm::StructType::create(members, get_resolver()->format_type_display(type));
        }

        std::vector<llvm::Type *> members;
        for (auto &member : data.fields) {
            members.push_back(compile_type(member->resolved_type));
        }
        return llvm::StructType::create(members, get_resolver()->format_type_display(type));
    }
    // Promise is now a Chi-native struct (TypeKind::Subtype), no special handling needed
    case TypeKind::Subtype: {
        return compile_type(type->data.subtype.final_type);
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
    case TypeKind::Undefined: {
        return get_llvm_ptr_type();
    }
    default:
        panic("not implemented");
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
    if (m_ctx->dbg_scopes.len) {
        scope = m_ctx->dbg_scopes.last();
    }
    auto line_no = node->token->pos.line_number();
    auto col_no = node->token->pos.col_number();
    builder->SetCurrentDebugLocation(
        llvm::DILocation::get(llvm_ctx, line_no, col_no, scope, nullptr));
}

void Compiler::dump_generics_comparison() {
    if (!getenv("DUMP_GENERICS")) {
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
    if (fn_missing == 0) print("  (none)\n");

    // Check for functions compiled but not in GenericResolver
    print("Functions compiled but NOT in GenericResolver:\n");
    int fn_extra = 0;
    for (auto &id : m_ctx->compiled_generic_fns) {
        if (!generics->fn_envs.has_key(id)) {
            print("  EXTRA: {}\n", id);
            fn_extra++;
        }
    }
    if (fn_extra == 0) print("  (none)\n");

    // Check for structs in GenericResolver but not compiled
    print("Structs in GenericResolver but NOT compiled:\n");
    int struct_missing = 0;
    for (auto &[id, entry] : generics->struct_envs.data) {
        if (m_ctx->compiled_generic_structs.find(id) == m_ctx->compiled_generic_structs.end()) {
            print("  MISSING: {} (name: {})\n", id, entry.name);
            struct_missing++;
        }
    }
    if (struct_missing == 0) print("  (none)\n");

    // Check for structs compiled but not in GenericResolver
    print("Structs compiled but NOT in GenericResolver:\n");
    int struct_extra = 0;
    for (auto &id : m_ctx->compiled_generic_structs) {
        if (!generics->struct_envs.has_key(id)) {
            print("  EXTRA: {}\n", id);
            struct_extra++;
        }
    }
    if (struct_extra == 0) print("  (none)\n");

    print("\n=== Summary ===\n");
    print("GenericResolver: {} fns, {} structs\n",
          generics->fn_envs.size(), generics->struct_envs.size());
    print("Codegen compiled: {} fns, {} structs\n",
          m_ctx->compiled_generic_fns.size(), m_ctx->compiled_generic_structs.size());
    print("Missing from codegen: {} fns, {} structs\n", fn_missing, struct_missing);
    print("Extra in codegen (not tracked): {} fns, {} structs\n", fn_extra, struct_extra);
}

void Compiler::emit_output() {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

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
    auto target_machine =
        target->createTargetMachine(target_triple, cpu, features, opt, llvm::Reloc::PIC_);
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
    auto runtime_pkg = m_ctx->compilation_ctx->rt_package;
    auto module = runtime_pkg->modules[0].get();
    auto list = module->scope->get_all();
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
