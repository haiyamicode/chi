#include "codegen.h"
#include "ast.h"
#include "context.h"
#include "enum.h"
#include "fmt/core.h"
#include "resolver.h"
#include "sema.h"
#include "util.h"

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

ChiType *Compiler::eval_type(ChiType *type) {
    if (type->is_placeholder && type->kind == TypeKind::Placeholder) {
        // Let's also check the placeholder's trait if it has one
        if (type->data.placeholder.trait) {
        }
    }

    // Handle struct/container type parameters using selective substitution
    if (type->is_placeholder && m_fn && m_fn->container_subtype) {
        // Use selective substitution to only replace placeholders that belong to the struct
        if (m_fn->fn_type && m_fn->fn_type->data.fn.container_ref) {
            auto container_ref = m_fn->fn_type->data.fn.container_ref;
            auto container_struct = get_resolver()->resolve_struct_type(container_ref);
            if (container_struct) {
                type = get_resolver()->type_placeholders_sub_selective(
                    type, m_fn->container_subtype, container_struct->node);
            }
        }
    }

    // Handle function generic placeholders using selective substitution
    if (type->is_placeholder && m_fn && m_fn->specialized_subtype &&
        m_fn->specialized_subtype->kind == TypeKind::Subtype) {
        // Use selective substitution to only replace placeholders that belong to the current
        // function
        if (m_fn->node) {
            auto old_type = type;
            type = get_resolver()->type_placeholders_sub_selective(
                type, &m_fn->specialized_subtype->data.subtype, m_fn->node->get_root_node());
        }
    }

    if (type->is_placeholder && type->kind == TypeKind::Fn && m_fn_eval_subtype &&
        m_fn_eval_subtype->kind == TypeKind::Subtype) {
        auto subtype_data = &m_fn_eval_subtype->data.subtype;
        auto old_type = type;
        type = get_resolver()->type_placeholders_sub_selective(type, subtype_data,
                                                               subtype_data->root_node);
    }

    // Handle placeholders with unknown source
    if (type->is_placeholder && type->kind == TypeKind::Placeholder &&
        !type->data.placeholder.source_decl) {

        // General fallback for other unknown source placeholders
        if (m_fn && m_fn->specialized_subtype &&
            m_fn->specialized_subtype->kind == TypeKind::Subtype) {
            type = get_resolver()->type_placeholders_sub(type,
                                                         &m_fn->specialized_subtype->data.subtype);
        }
        if (type->is_placeholder && m_fn && m_fn->container_subtype) {
            type = get_resolver()->type_placeholders_sub(type, m_fn->container_subtype);
        }
    }

    if (type->kind == TypeKind::Subtype) {
        return type->data.subtype.final_type;
    }
    if (type->kind == TypeKind::This) {
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
    return m_ctx->module_cu_table[module->global_id()];
}

void Compiler::compile_module(ast::Module *module) {
    for (auto import : module->imports) {
        compile_module(import);
    }

    auto module_cu = m_ctx->dbg_builder->createCompileUnit(
        llvm::dwarf::DW_LANG_C,
        m_ctx->dbg_builder->createFile(module->filename, module->path, std::nullopt, std::nullopt),
        "Chi Compiler", 0, "", 0);
    m_ctx->module_cu_table[module->global_id()] = module_cu;
    m_ctx->dbg_cu = module_cu;

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
            _compile_struct(node, subtype);
        }

    } else {
        _compile_struct(node, struct_type);
    }
}

void Compiler::_compile_struct(ast::Node *node, ChiType *type) {
    auto subtype = type->kind == TypeKind::Subtype ? type : nullptr;
    auto struct_type = type->kind == TypeKind::Subtype ? type->data.subtype.final_type : type;

    for (auto member : node->data.struct_decl.members) {
        if (member->type == ast::NodeType::FnDef) {
            auto fn_node = member;
            if (subtype) {
                auto subtype_member = struct_type->data.struct_.find_member(member->name);
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
                        m_ctx->pending_fns.add(fn);
                    }
                }
                continue;
            }

            auto fn = compile_fn_proto(fn_node->data.fn_def.fn_proto, fn_node);
            if (subtype) {
                fn->container_subtype = &subtype->data.subtype;
                fn->container_type = subtype;
            } else {
                fn->container_type = type;
            };
            m_ctx->pending_fns.add(fn);
        }
    }

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
    case TypeKind::Void: {
        return llvm_db.createBasicType("void", 0, llvm::dwarf::DW_ATE_address);
    }
    case TypeKind::Bool: {
        return llvm_db.createBasicType("bool", 8, llvm::dwarf::DW_ATE_boolean);
    }
    case TypeKind::Char: {
        return llvm_db.createBasicType("char", 8, llvm::dwarf::DW_ATE_unsigned_char);
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
    case TypeKind::MutRef: {
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
        auto name = m_ctx->resolver.to_string(type, true);
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
        return llvm_db.createBasicType(m_ctx->resolver.to_string(type, true), size,
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
            auto var = compile_alloc(fn, param, param_type);

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
    if (return_type->kind != TypeKind::Void) {
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

    // clean up & return
    fn->use_label(return_b);
    for (auto var : fn->get_def()->cleanup_vars) {
        compile_destruction(fn, get_var(var), var);
    }
    for (auto ptr : fn->vararg_pointers) {
        emit_dbg_location(fn->node);
        builder.CreateCall(get_system_fn("cx_array_delete")->llvm_fn, {ptr});
    }
    if (is_entry) {
        auto runtime_stop = get_system_fn("cx_runtime_stop");
        builder.CreateCall(runtime_stop->llvm_fn, {});
    }
    if (return_type->kind == TypeKind::Void || fn->use_sret()) {
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
        for (auto var : fn->get_def()->cleanup_vars) {
            compile_destruction(fn, get_var(var), var);
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

llvm::Value *Compiler::compile_assignment_to_type(Function *fn, ast::Node *expr,
                                                  ChiType *dest_type) {
    auto src_value = compile_expr(fn, expr);
    auto src_type = get_chitype(expr);
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
    for (auto impl : type->data.struct_.interfaces) {
        auto vtable = vtables.add({});
        vtable->offset = count;
        vtable->impl = impl;
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
                                           "vtables." + get_resolver()->to_string(type, true));

    for (auto &vtable : vtables) {
        m_ctx->impl_table[vtable.impl] = global + vtable.offset;
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

    auto ti_type_l = llvm::StructType::create(
        {llvm::Type::getInt32Ty(llvm_ctx), llvm::Type::getInt32Ty(llvm_ctx), tidata_type_l,
         llvm::Type::getInt32Ty(llvm_ctx), ti_meta_table_type_l},
        "TypeInfo", true);

    auto typesize = llvm_type_size(type_l);
    auto typedata = (uint8_t *)&type->data;
    llvm::Constant *typedata_arr_l = llvm::ConstantDataArray::get(
        llvm_ctx, llvm::ArrayRef<uint8_t>(typedata, sizeof(TypeInfoData)));

    auto info_l = llvm::ConstantStruct::get(
        ti_type_l,
        {
            /* kind */
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), (int32_t)type->kind),
            /* typesize */
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), (int32_t)typesize),
            /* data */
            typedata_arr_l,
            /* vtable_len */
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), meta_table_len),
            /* vtable */
            ti_meta_table_l,
        });

    auto info_global =
        new llvm::GlobalVariable(llvm_module, ti_type_l, true, llvm::GlobalValue::PrivateLinkage,
                                 info_l, "typeinfo." + get_resolver()->to_string(type, true));
    return info_global;
}

llvm::Value *Compiler::compile_lambda_alloc(Function *fn, ChiType *lambda_type, llvm::Value *fn_ptr,
                                            NodeList *captures) {
    auto &builder = *(m_ctx->llvm_builder.get());
    auto &llvm_module = *(m_ctx->llvm_module.get());

    auto struct_type = lambda_type->data.fn_lambda.internal;
    auto struct_type_l = compile_type(struct_type);
    auto var = builder.CreateAlloca(struct_type_l, nullptr, "lambda");
    auto fn_ptr_gep = builder.CreateStructGEP(struct_type_l, var, 0);
    auto size_gep = builder.CreateStructGEP(struct_type_l, var, 1);
    auto data_gep = builder.CreateStructGEP(struct_type_l, var, 2);

    // load captures
    if (captures && captures->len) {
        auto bstruct = lambda_type->data.fn_lambda.bind_struct;
        assert(bstruct);
        auto bstruct_l = compile_type(bstruct);
        auto bind_var = builder.CreateAlloca(bstruct_l, nullptr, "lambda_captures");
        auto bind_size = llvm_type_size(bstruct_l);
        builder.CreateStore(bind_var, data_gep);
        builder.CreateStore(
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*m_ctx->llvm_ctx), bind_size), size_gep);

        // For lambdas with captures, use the original function directly
        // The captures are handled by the binding struct
        builder.CreateStore(fn_ptr, fn_ptr_gep);

        for (int i = 0; i < captures->len; i++) {
            auto capture = (*captures)[i];
            llvm::Value *ref;

            // Check if the capture exists in current function's variable table AND
            // the variable was declared in the current function (not a parent function)
            if (m_ctx->var_table.get(capture) && capture->parent_fn == fn->node) {
                ref = get_var(capture);
            } else {
                // Handle nested captures: the variable should be in the current function's captures
                // since capture propagation ensures all intermediate functions capture the variable
                bool found = false;

                // Find the capture index in the current function's captures
                auto &current_captures = fn->node->data.fn_def.captures;
                for (int j = 0; j < current_captures.len; j++) {
                    if (current_captures[j] == capture) {
                        // This capture should be accessible from the current function's bind_ptr
                        if (fn->bind_ptr) {
                            auto current_fn_type = get_chitype(fn->node);
                            if (current_fn_type->kind == TypeKind::FnLambda) {
                                auto current_bstruct = current_fn_type->data.fn_lambda.bind_struct;
                                auto current_bstruct_l =
                                    (llvm::StructType *)compile_type(current_bstruct);

                                auto capture_gep =
                                    builder.CreateStructGEP(current_bstruct_l, fn->bind_ptr, j);
                                ref = builder.CreateLoad(current_bstruct_l->elements()[j],
                                                         capture_gep);
                                found = true;
                                break;
                            }
                        }
                    }
                }

                if (!found) {
                    // The variable is not available in current scope - this should not happen
                    // if capture propagation is working correctly
                    ref = llvm::ConstantPointerNull::get(
                        llvm::PointerType::get(llvm::Type::getInt8Ty(*m_ctx->llvm_ctx), 0));
                }
            }

            auto capture_gep = builder.CreateStructGEP(bstruct_l, bind_var, i);
            builder.CreateStore(ref, capture_gep);
        }
    } else {
        // For lambdas without captures, we still generate a proxy function for consistent calling
        // convention Generate proxy function that ignores the binding struct parameter
        auto proxy_fn = generate_lambda_proxy_function(fn, fn_ptr, lambda_type, nullptr);
        builder.CreateStore(proxy_fn, fn_ptr_gep);

        builder.CreateStore(llvm::ConstantPointerNull::get(
                                llvm::PointerType::get(llvm::Type::getInt8Ty(*m_ctx->llvm_ctx), 0)),
                            data_gep);
        builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*m_ctx->llvm_ctx), 0),
                            size_gep);
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
    case ast::NodeType::VarDecl: {
        auto &data = node->data.var_decl;
        return contains_await(data.expr);
    }
    case ast::NodeType::ReturnStmt:
        return contains_await(node->data.return_stmt.expr);
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
    case ast::NodeType::VarDecl:
        return find_await_expr(node->data.var_decl.expr);
    case ast::NodeType::ReturnStmt:
        return find_await_expr(node->data.return_stmt.expr);
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
    case ast::NodeType::VarDecl:
        collect_vars_used_in_node(node->data.var_decl.expr, vars);
        break;
    case ast::NodeType::ReturnStmt:
        collect_vars_used_in_node(node->data.return_stmt.expr, vars);
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

    // Create the continuation function: void __fnname_cont_N(void* data, void* value)
    auto void_type = llvm::Type::getVoidTy(llvm_ctx);
    auto ptr_type = llvm::PointerType::get(llvm_ctx, 0);
    auto fn_type = llvm::FunctionType::get(void_type, {ptr_type, ptr_type}, false);
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
    auto ptr_type = llvm::PointerType::get(llvm_ctx, 0);

    // Previous segment that ended with an await
    int prev_segment_idx = segment_index - 1;
    auto &prev_segment = ctx.segments[prev_segment_idx];

    // Build the capture struct type for this continuation
    // { result_promise_ptr, captured_var1, captured_var2, ... }
    std::vector<llvm::Type *> capture_types;
    capture_types.push_back(ptr_type); // result promise pointer

    std::vector<ast::Node *> captured_vars_ordered;
    for (auto var : prev_segment.vars_to_capture) {
        captured_vars_ordered.push_back(var);
        capture_types.push_back(compile_type(get_chitype(var)));
    }

    auto capture_struct_type = llvm::StructType::get(llvm_ctx, capture_types);
    auto captures_ptr = builder.CreateBitCast(data_arg, capture_struct_type->getPointerTo());

    // Extract result promise pointer
    auto result_promise_ptr_ptr = builder.CreateStructGEP(capture_struct_type, captures_ptr, 0);
    auto result_promise_ptr = builder.CreateLoad(ptr_type, result_promise_ptr_ptr);

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

        // Load value from value_arg (which is a pointer to the value)
        auto result_value = builder.CreateLoad(var_type_l, value_arg);
        builder.CreateStore(result_value, var_alloc);

        local_vars[var] = var_alloc;
        add_var(var, var_alloc);
    }

    // Check if this is the final segment (contains return)
    bool is_final = (segment_index == (int)ctx.segments.size() - 1);

    // Compile the segment's statements
    for (auto stmt : segment.stmts) {
        if (stmt->type == ast::NodeType::ReturnStmt) {
            // For return in async function, resolve the result promise
            auto &data = stmt->data.return_stmt;
            if (data.expr) {
                auto ret_value = compile_expr(cont_fn, data.expr);
                auto value_type = get_chitype(data.expr);
                auto value_type_l = compile_type(value_type);

                // Box the value on heap
                auto malloc_fn = m_ctx->llvm_module->getFunction("cx_malloc");
                auto value_size = m_ctx->llvm_module->getDataLayout().getTypeAllocSize(value_type_l);
                auto value_size_l = llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), value_size);
                auto null_ptr = llvm::ConstantPointerNull::get(llvm::PointerType::get(llvm_ctx, 0));
                auto boxed_value = builder.CreateCall(malloc_fn, {value_size_l, null_ptr}, "boxed");
                builder.CreateStore(ret_value, boxed_value);

                // Call cx_promise_resolve
                auto resolve_fn = get_system_fn("cx_promise_resolve");
                builder.CreateCall(resolve_fn->llvm_fn, {result_promise_ptr, boxed_value});
            } else {
                // void return
                auto resolve_fn = get_system_fn("cx_promise_resolve");
                auto null_ptr = llvm::ConstantPointerNull::get(llvm::PointerType::get(llvm_ctx, 0));
                builder.CreateCall(resolve_fn->llvm_fn, {result_promise_ptr, null_ptr});
            }
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

    int prev_segment_idx = segment_index - 1;
    auto &prev_segment = ctx.segments[prev_segment_idx];

    // Build capture struct type: { result_promise_ptr, captured_vars... }
    std::vector<llvm::Type *> capture_types;
    auto ptr_type = llvm::PointerType::get(llvm_ctx, 0);
    capture_types.push_back(ptr_type); // result promise pointer

    std::vector<ast::Node *> captured_vars_ordered;
    for (auto var : prev_segment.vars_to_capture) {
        captured_vars_ordered.push_back(var);
        capture_types.push_back(compile_type(get_chitype(var)));
    }

    auto capture_struct_type = llvm::StructType::get(llvm_ctx, capture_types);
    auto capture_size = m_ctx->llvm_module->getDataLayout().getTypeAllocSize(capture_struct_type);

    // Allocate capture struct on heap
    auto malloc_fn = m_ctx->llvm_module->getFunction("cx_malloc");
    auto size_l = llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), capture_size);
    auto null_ptr = llvm::ConstantPointerNull::get(ptr_type);
    auto capture_alloc = builder.CreateCall(malloc_fn, {size_l, null_ptr}, "captures");
    auto captures_ptr = builder.CreateBitCast(capture_alloc, capture_struct_type->getPointerTo());

    // Store result promise pointer (passed as parameter)
    auto result_ptr_gep = builder.CreateStructGEP(capture_struct_type, captures_ptr, 0);
    builder.CreateStore(result_promise_ptr, result_ptr_gep);

    // Store captured variables
    for (size_t i = 0; i < captured_vars_ordered.size(); i++) {
        auto var = captured_vars_ordered[i];
        auto var_type = get_chitype(var);
        auto var_type_l = compile_type(var_type);

        // Get the variable value (either from local_vars or var_table)
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

    // Build CxLambda struct: { ptr, size, data }
    auto lambda_type = get_system_types()->lambda;
    auto lambda_type_l = compile_type(lambda_type);
    auto lambda_struct_type = (llvm::StructType *)lambda_type_l;

    auto lambda_alloc = fn->entry_alloca(lambda_struct_type, "lambda");

    // ptr = continuation function (segment N uses continuation at index N-1)
    auto cont_fn = ctx.continuations[segment_index - 1];
    auto fn_ptr_gep = builder.CreateStructGEP(lambda_struct_type, lambda_alloc, 0);
    builder.CreateStore(cont_fn, fn_ptr_gep);

    // size = capture struct size
    auto size_gep = builder.CreateStructGEP(lambda_struct_type, lambda_alloc, 1);
    builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), capture_size), size_gep);

    // data = captures pointer
    auto data_gep = builder.CreateStructGEP(lambda_struct_type, lambda_alloc, 2);
    builder.CreateStore(capture_alloc, data_gep);

    return builder.CreateLoad(lambda_struct_type, lambda_alloc);
}

void Compiler::emit_promise_chain(Function *fn, AsyncContext &ctx, ast::Node *await_expr,
                                   int next_segment_index, map<ast::Node *, llvm::Value *> &local_vars,
                                   llvm::Value *result_promise_ptr) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    // Compile the await expression to get the promise
    auto awaited_promise = compile_expr(fn, await_expr->data.await_expr.expr);

    // Allocate storage for the promise
    auto awaited_promise_ptr = fn->entry_alloca(ctx.promise_struct_type, "awaited");
    builder.CreateStore(awaited_promise, awaited_promise_ptr);

    // Build continuation lambda for next segment
    auto lambda = build_continuation_lambda(fn, ctx, next_segment_index, local_vars, result_promise_ptr);

    // Store lambda and call cx_promise_then
    auto lambda_ptr = fn->entry_alloca(compile_type(get_system_types()->lambda), "cont_lambda");
    builder.CreateStore(lambda, lambda_ptr);

    auto then_fn = get_system_fn("cx_promise_then");
    auto null_ptr = llvm::ConstantPointerNull::get(llvm::PointerType::get(llvm_ctx, 0));
    builder.CreateCall(then_fn->llvm_fn, {awaited_promise_ptr, lambda_ptr, null_ptr});
}

void Compiler::compile_async_fn_body(Function *fn) {
    auto &builder = *m_ctx->llvm_builder;
    auto &llvm_ctx = *m_ctx->llvm_ctx;

    auto fn_def = fn->get_def();
    auto body = fn_def->body;

    // Get return type (Promise<T>)
    auto return_type = fn->fn_type->data.fn.return_type;
    assert(return_type->kind == TypeKind::Promise);

    auto promise_struct_type_l = (llvm::StructType *)compile_type(return_type->data.promise.internal);

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

    // Initialize promise: cx_promise_new(&result)
    auto promise_new_fn = get_system_fn("cx_promise_new");
    builder.CreateCall(promise_new_fn->llvm_fn, {ctx.result_promise_ptr});

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
        } else if (from_type->kind == TypeKind::Char) {
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
        if (to_type->kind == TypeKind::Bool || to_type->kind == TypeKind::Char) {
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
        if (from_type->kind == TypeKind::Bool || from_type->kind == TypeKind::Char) {
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

    panic("number conversion not implemented");
    return nullptr;
}

llvm::Value *Compiler::compile_conversion(Function *fn, llvm::Value *value, ChiType *from_type,
                                          ChiType *to_type) {
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
        case TypeKind::Pointer:
        case TypeKind::Reference: {
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
    case TypeKind::Float: {
        return compile_number_conversion(fn, value, from_type, to_type);
    }
    case TypeKind::FnLambda: {
        if (from_type->kind == TypeKind::Fn) {
            return compile_lambda_alloc(fn, to_type, value, nullptr);
        }
        return value;
    }
    case TypeKind::Optional: {
        if (from_type->kind == TypeKind::Pointer) {
            return llvm::ConstantAggregateZero::get(compile_type(to_type));
        }
    }
    case TypeKind::Struct: {
        if (ChiTypeStruct::is_interface(to_type)) {
            if (from_type->kind == TypeKind::Pointer || from_type->kind == TypeKind::Reference) {
                auto strct = from_type->get_elem();
                auto &builder = *m_ctx->llvm_builder;
                auto vp = builder.CreateAlloca(compile_type(to_type));
                auto impl = strct->data.struct_.interface_table[to_type];
                assert(impl);
                auto vtable = m_ctx->impl_table[impl];
                assert(vtable);
                auto iface_type_l = compile_type(to_type);
                auto ti_p = builder.CreateStructGEP(iface_type_l, vp, 0);
                builder.CreateStore(compile_type_info(from_type), ti_p);
                auto data_p = builder.CreateStructGEP(iface_type_l, vp, 1);
                builder.CreateStore(value, data_p);
                auto vtable_p = builder.CreateStructGEP(iface_type_l, vp, 2);
                builder.CreateStore(vtable, vtable_p);
                return builder.CreateLoad(iface_type_l, vp);
            }
        }
    }

    default:
        // by default, do nothing
        return value;
    }
}

static llvm::CmpInst::Predicate get_cmpop(TokenType op, ChiType *type) {
    auto is_unsigned = type->kind == TypeKind::Int && type->data.int_.is_unsigned;
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
        if (fn->get_def()->cleanup_vars.len) {
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
                    auto value = builder.CreateCall(
                        assert->llvm_fn,
                        {has_value, compile_string_literal("unwrapping null optional")});
                    auto value_p = builder.CreateStructGEP(opt_type_l, ref.address, 1);
                    return builder.CreateLoad(compile_type(expr->resolved_type), value_p);
                }

                // unwrap pointer
                auto ptr = compile_expr(fn, data.op1);
                auto elem_type = get_chitype(data.op1)->get_elem();
                auto elem_type_l = compile_type(elem_type);
                auto value = builder.CreateLoad(elem_type_l, ptr);
                return value;
            } else {
                auto value = compile_assignment_to_type(fn, data.op1, get_system_types()->bool_);
                return builder.CreateXor(
                    value, llvm::ConstantInt::getTrue(compile_type(get_system_types()->bool_)));
            }
        }
        case TokenType::AND:
        case TokenType::MUTREF: {
            auto ref = compile_expr_ref(fn, data.op1);
            assert(ref.address);
            return ref.address;
        }
        case TokenType::INC:
        case TokenType::DEC: {
            auto ref = compile_expr_ref(fn, data.op1);
            auto value = builder.CreateLoad(compile_type(get_chitype(data.op1)), ref.address);
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
            auto cmpop = get_cmpop(data.op_type, get_chitype(expr));
            return builder.CreateCmp(cmpop, lhs, rhs);
        }
        case TokenType::ASS: {
            auto ref = compile_expr_ref(fn, data.op1);
            auto value = compile_assignment_value(fn, data.op2, data.op1);
            assert(ref.address);
            if (!data.op2->escape.moved) {
                compile_copy(fn, value, ref.address, get_chitype(data.op2), data.op2);
            }
            return value;
        }
        default: {
            auto target_type = get_chitype(expr);
            auto lhs_type = get_chitype(data.op1);
            auto rhs_type = get_chitype(data.op2);
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
        auto &llvm_module = *m_ctx->llvm_module.get();
        auto result_type_l = compile_type(get_chitype(expr));
        llvm::Value *result_var = builder.CreateAlloca(result_type_l, nullptr, "try_result");
        auto continue_b = fn->new_label("_try_continue");
        auto normal_b = fn->new_label("_try_normal");
        auto landing_b = fn->new_label("_try_landing");

        InvokeInfo invoke = {};
        invoke.normal = normal_b;
        invoke.landing = landing_b;
        auto value = compile_fn_call(fn, data.expr, &invoke);

        fn->use_label(invoke.landing);
        auto caught_type_l = m_ctx->get_caught_result_type();
        auto landing = builder.CreateLandingPad(caught_type_l, 1);
        auto null_ptr = llvm::ConstantPointerNull::get(
            llvm::PointerType::get(llvm::Type::getInt8Ty(llvm_ctx), 0));
        landing->addClause(null_ptr);
        builder.CreateBr(continue_b);
        // auto result_err_field_type_l = result_type_l->getStructElementType(0);
        // auto has_err_type_l = result_err_field_type_l->getStructElementType(0);
        // auto err_p = builder.CreateStructGEP(result_type_l, result_var, 0);
        // auto err_has_value_p = builder.CreateStructGEP(result_err_field_type_l, err_p, 0);
        // builder.CreateStore(llvm::ConstantInt::get(has_err_type_l, 1), err_has_value_p);
        // builder.CreateBr(main_b);

        fn->use_label(invoke.normal);
        builder.CreateBr(continue_b);
        // auto value_type_l = compile_type(get_chitype(data.expr));
        // auto value_p = builder.CreateStructGEP(result_type_l, result_var, 1);
        // builder.CreateStore(value, value_p);

        fn->use_label(continue_b);
        // fn->insn_noop();

        return result_var;
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
        auto member = expr->data.dot_expr.resolved_struct_member;
        auto ref = compile_expr_ref(fn, expr);
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
        m_ctx->pending_fns.add(lambda_fn);
        return compile_lambda_alloc(fn, get_chitype(expr), lambda_fn->llvm_fn, &data.captures);
    }
    case ast::NodeType::ConstructExpr: {
        auto &builder = *m_ctx->llvm_builder.get();
        auto &data = expr->data.construct_expr;
        auto ptr = data.resolved_outlet ? compile_expr_ref(fn, data.resolved_outlet).address
                                        : compile_alloc(fn, expr, data.is_new);
        auto type = data.is_new ? get_chitype(expr)->get_elem() : get_chitype(expr);
        compile_construction(fn, ptr, type, expr);
        return data.is_new ? ptr : builder.CreateLoad(compile_type(type), ptr);
    }
    case ast::NodeType::PrefixExpr: {
        auto &data = expr->data.prefix_expr;
        auto &llvm_ctx = *m_ctx->llvm_ctx;
        auto &builder = *m_ctx->llvm_builder;

        switch (data.prefix->type) {
        case TokenType::KW_SIZEOF: {
            return llvm::ConstantInt::get(llvm::IntegerType::getInt32Ty(llvm_ctx),
                                          llvm_type_size(compile_type_of(data.expr)));
        }
        case TokenType::KW_DELETE: {
            emit_dbg_location(expr);
            auto free_fn = get_system_fn("cx_free");
            auto ptr = compile_expr(fn, data.expr);
            builder.CreateCall(free_fn->llvm_fn, {ptr});
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

void Compiler::compile_copy_with_ref(Function *fn, RefValue src, llvm::Value *dest, ChiType *type,
                                     ast::Node *expr) {
    auto &builder = *m_ctx->llvm_builder;
    assert(src.value);

    switch (type->kind) {
    case TypeKind::String: {
        auto from_address = src.address ? src.address : nullptr;
        if (!from_address) {
            from_address = builder.CreateAlloca(compile_type(type), nullptr, "_op_str_copy");
            builder.CreateStore(src.value, from_address);
        }
        auto copy_fn = get_system_fn("cx_string_copy");
        auto call = builder.CreateCall(copy_fn->llvm_fn, {dest, from_address});
        call->setDebugLoc(m_ctx->llvm_builder->getCurrentDebugLocation());
        emit_dbg_location(expr);
        return;
    }
    case TypeKind::Array:
    case TypeKind::Struct: {
        auto sty = get_resolver()->resolve_struct_type(eval_type(type));
        auto &builder = *m_ctx->llvm_builder;
        auto copy_fn_p = sty->member_intrinsics.get(IntrinsicSymbol::CopyFrom);
        if (copy_fn_p) {
            auto copy_fn = *copy_fn_p;
            auto from_address = src.address ? src.address : nullptr;
            if (!from_address) {
                from_address = builder.CreateAlloca(compile_type(type), nullptr, "_op_copy_from");
                builder.CreateStore(src.value, from_address);
            }
            auto size = llvm_type_size(compile_type(type));
            builder.CreateMemSet(
                dest, llvm::ConstantInt::get(llvm::IntegerType::getInt8Ty(*m_ctx->llvm_ctx), 0),
                size, {});
            auto loc = m_ctx->llvm_builder->getCurrentDebugLocation();
            auto call = builder.CreateCall(get_fn(copy_fn->node)->llvm_fn, {
                                                                               dest,
                                                                               from_address,
                                                                           });
            call->setDebugLoc(loc);
            emit_dbg_location(expr);
            return;
        }
        break;
    }
    default:
        break;
    }

    auto size = llvm_type_size(compile_type(type));
    if (size > 0) {
        builder.CreateStore(src.value, dest);
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
        auto &data = type->data.struct_;
        auto constructor = ChiTypeStruct::get_constructor(type);
        if (constructor) {
            auto constructor_fn = get_fn(constructor->node);
            auto constructor_type = get_chitype(constructor->node);
            auto constructor_type_l = (llvm::FunctionType *)compile_type(constructor_type);
            auto args = std::vector<llvm::Value *>{dest};
            auto remaining_args =
                compile_fn_args(fn, constructor_fn, expr->data.construct_expr.items, expr);
            args.insert(args.end(), remaining_args.begin(), remaining_args.end());
            builder.CreateCall(constructor_type_l, constructor_fn->llvm_fn, args);
            emit_dbg_location(expr);
        }

        for (auto field_init : expr->data.construct_expr.field_inits) {
            auto &data = field_init->data.field_init_expr;
            auto field_gep =
                builder.CreateStructGEP(compile_type(type), dest, data.resolved_field->field_index);
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

llvm::Value *Compiler::compile_dot_ptr(Function *fn, ast::Node *expr) {
    auto ctn_type = get_chitype(expr);
    if (ctn_type->is_pointer_like()) {
        return compile_expr(fn, expr);
    }
    auto ref = compile_expr_ref(fn, expr);
    return ref.address ? ref.address : ref.value;
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
    case ast::NodeType::VarDecl:
        return RefValue::from_address(get_var(expr));
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
            auto lambda_struct_type = compile_type(lambda_type->data.fn_lambda.internal);

            // Allocate lambda structure
            auto lambda_alloca = builder.CreateAlloca(lambda_struct_type, nullptr, "method_lambda");

            // Generate a proxy function that wraps the method call
            auto proxy_fn =
                generate_method_proxy_function(fn, data.resolved_struct_member, lambda_type);

            // Get the instance pointer
            llvm::Value *instance_ptr;
            if (type->is_pointer_like()) {
                instance_ptr = compile_expr(fn, data.expr);
            } else {
                auto ref = compile_expr_ref(fn, data.expr);
                instance_ptr = ref.address;
            }

            // Create binding struct to hold the instance pointer
            auto bind_struct_type = compile_type(lambda_type->data.fn_lambda.bind_struct);
            auto bind_alloca = builder.CreateAlloca(bind_struct_type, nullptr, "method_bind");

            // Store instance pointer in binding struct
            auto instance_gep = builder.CreateStructGEP(bind_struct_type, bind_alloca, 0);
            builder.CreateStore(instance_ptr, instance_gep);

            // Fill lambda structure: {ptr, size, data}
            // ptr = proxy function pointer
            auto fn_ptr_gep = builder.CreateStructGEP(lambda_struct_type, lambda_alloca, 0);
            auto fn_ptr_cast = builder.CreateBitCast(proxy_fn, builder.getInt8PtrTy());
            builder.CreateStore(fn_ptr_cast, fn_ptr_gep);

            // size = size of binding struct
            auto size_gep = builder.CreateStructGEP(lambda_struct_type, lambda_alloca, 1);
            auto struct_size =
                m_ctx->llvm_module->getDataLayout().getTypeAllocSize(bind_struct_type);
            builder.CreateStore(builder.getInt32(struct_size), size_gep);

            // data = binding struct pointer (heap allocated)
            auto data_gep = builder.CreateStructGEP(lambda_struct_type, lambda_alloca, 2);
            auto bind_void_ptr = builder.CreateBitCast(bind_alloca, builder.getInt8PtrTy());
            builder.CreateStore(bind_void_ptr, data_gep);

            return RefValue::from_address(lambda_alloca);
        } else if (type->is_pointer_like()) {
            type = type->get_elem();
            ptr = compile_expr(fn, data.expr);
        } else {
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
        case TokenType::MUL:
            return RefValue::from_address(compile_expr(fn, data.op1));
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
                return RefValue::from_address(compile_expr(fn, data.op1));
            }
            panic("unreachable");
            break;
        }
        default:
            panic("operator not implemented: {}", PRINT_ENUM(data.op_type));
        }
        return {};
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
            auto fn = get_fn(method->node);
            auto call = builder.CreateCall(fn->llvm_fn, {ref.address, subscript});
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
    default:
        panic("not implemented: {}", PRINT_ENUM(expr->type));
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
    if (data.decl->type == ast::NodeType::VarDecl) {
        auto &var = data.decl->data.var_decl;
        if (var.is_const && var.resolved_value.has_value() && !data.decl->parent_fn) {
            return RefValue::from_value(
                compile_constant_value(fn, *var.resolved_value, get_chitype(data.decl)));
        } else {
            goto normal;
        }
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

        // Access the capture from current function's bind_ptr
        auto gep = builder.CreateStructGEP(bstruct_l, fn->bind_ptr, capture_idx);
        auto ref = builder.CreateLoad(bstruct_l->elements()[capture_idx], gep);

        return RefValue::from_address(ref);
    }
    return RefValue::from_address(get_var(data.decl));
}

std::vector<llvm::Value *> Compiler::compile_fn_args(Function *fn, Function *callee,
                                                     array<ast::Node *> args, ast::Node *fn_call) {
    std::vector<llvm::Value *> call_args = {};
    auto &builder = *m_ctx->llvm_builder.get();
    llvm::Value *va_ptr = nullptr;
    ChiType *va_type = nullptr;
    auto &fn_spec = callee->fn_type->data.fn;
    auto va_start = fn_spec.get_va_start();

    bool is_variadic = callee->fn_type->data.fn.is_variadic;
    if (is_variadic) {
        auto array_type = fn_spec.params.last();
        va_type = array_type->get_elem();
        va_ptr = fn->entry_alloca(compile_type(array_type), "vararg_array");
        emit_dbg_location(fn_call);
        auto init_fn = get_system_fn("cx_array_new");
        builder.CreateCall(init_fn->llvm_fn, {va_ptr});
    }

    for (int i = 0; i < args.len; i++) {
        if (is_variadic && i >= va_start) {
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
        auto arg = args[i];
        auto param_type = fn_spec.get_param_at(i);
        call_args.push_back(compile_assignment_to_type(fn, arg, param_type));
    }

    if (va_ptr) {
        call_args.push_back(builder.CreateLoad(compile_type(fn_spec.params.last()), va_ptr));
        fn->vararg_pointers.add(va_ptr);
    }

    return call_args;
}

llvm::Value *Compiler::compile_fn_call(Function *fn, ast::Node *expr, InvokeInfo *invoke) {
    auto &data = expr->data.fn_call_expr;
    auto &builder = *m_ctx->llvm_builder.get();

    if (data.fn_ref_expr->resolved_type->kind == TypeKind::FnLambda) {
        auto ref = compile_expr_ref(fn, data.fn_ref_expr);
        auto lambda_type = get_chitype(data.fn_ref_expr);
        auto lambda_type_l = (llvm::StructType *)compile_type(lambda_type);
        auto fn_gep = builder.CreateStructGEP(lambda_type_l, ref.address, 0);
        auto fn_ptr = (llvm::Value *)builder.CreateLoad(lambda_type_l->elements()[0], fn_gep);
        auto &fn_spec = lambda_type->data.fn_lambda.bound_fn->data.fn;
        auto bound_fn_type_l =
            (llvm::FunctionType *)compile_type(lambda_type->data.fn_lambda.bound_fn);
        std::vector<llvm::Value *> args = {};

        // Standard lambda calling - same for all lambdas now
        auto &lambda_data = lambda_type->data.fn_lambda;
        auto data_gep = builder.CreateStructGEP(lambda_type_l, ref.address, 2);
        auto data_ptr = builder.CreateLoad(lambda_type_l->elements()[2], data_gep);

        // Always pass binding struct as first argument for all lambdas
        // This ensures consistent calling convention
        args.push_back(data_ptr);

        for (int i = 0; i < data.args.len; i++) {
            auto arg = data.args[i];
            // User arguments always start from parameter index 1 (after binding struct)
            int param_index = i + 1;
            auto param_type = fn_spec.get_param_at(param_index);
            args.push_back(compile_assignment_to_type(fn, arg, param_type));
        }

        llvm::FunctionCallee callee(bound_fn_type_l, fn_ptr);
        if (invoke) {
            return m_ctx->llvm_builder->CreateInvoke(callee, invoke->normal, invoke->landing, args);
        }
        return builder.CreateCall(callee, args);
    }

    auto container_type_id =
        fn->container_type ? std::optional{fn->container_type->id} : std::nullopt;
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
    auto va_start = fn_spec.get_va_start();

    std::vector<llvm::Value *> args;
    llvm::Value *va_ptr = nullptr;
    ChiType *va_type = nullptr;
    if (is_variadic) {
        auto array_type = fn_spec.params.last();
        va_type = array_type->get_elem();
        va_ptr = fn->entry_alloca(compile_type(array_type), "vararg_array");
        emit_dbg_location(data.fn_ref_expr);
        auto init_fn = get_system_fn("cx_array_new");
        builder.CreateCall(init_fn->llvm_fn, {va_ptr});
    }

    llvm::FunctionCallee callee;
    llvm::Value *ctn_ptr = nullptr;
    if (fn_spec.container_ref) {
        auto dot_expr = data.fn_ref_expr->data.dot_expr;
        if (!ctn_type) {
            ctn_type = get_chitype(dot_expr.expr);
        }
        auto ctn_type_l = compile_type(ctn_type);
        auto ptr = compile_dot_ptr(fn, dot_expr.expr);

        if (!fn_decl->data.fn_def.body) {
            // handle interface
            auto data_gep = builder.CreateStructGEP(ctn_type_l, ptr, 1);
            auto vtable_gep = builder.CreateStructGEP(ctn_type_l, ptr, 2);
            auto data_ptr = builder.CreateLoad(ctn_type_l->getStructElementType(1), data_gep);
            auto vtable_ptr = builder.CreateLoad(ctn_type_l->getStructElementType(2), vtable_gep);
            ctn_ptr = data_ptr;
            auto index = llvm::ConstantInt::get(
                *m_ctx->llvm_ctx, llvm::APInt(32, dot_expr.resolved_struct_member->method_index));
            auto fn_gep = builder.CreateGEP(
                llvm::PointerType::get(compile_type(get_system_types()->void_ptr), 0), vtable_ptr,
                {index});
            auto callee_ptr =
                builder.CreateLoad(compile_type(get_system_types()->void_ptr), fn_gep);
            callee = {(llvm::FunctionType *)compile_type_of(fn_decl), callee_ptr};
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

    for (int i = 0; i < data.args.len; i++) {
        if (is_variadic && i >= va_start) {
            emit_dbg_location(data.args[i]);
            auto add_fn = get_system_fn("cx_array_add");
            auto arg = compile_assignment_to_type(fn, data.args[i], va_type);
            auto tsize =
                m_ctx->llvm_module->getDataLayout().getTypeAllocSize(compile_type(va_type));
            auto tsize_l = llvm::ConstantInt::get(*m_ctx->llvm_ctx, llvm::APInt(32, tsize));
            auto ptr = builder.CreateCall(add_fn->llvm_fn, {va_ptr, tsize_l});
            builder.CreateStore(arg, ptr);
            continue;
        }
        auto arg = data.args[i];
        auto param_type = fn_spec.get_param_at(i);
        args.push_back(compile_assignment_to_type(fn, arg, param_type));
    }
    if (va_ptr) {
        args.push_back(builder.CreateLoad(compile_type(fn_spec.params.last()), va_ptr));
        fn->vararg_pointers.add(va_ptr);
    }

    emit_dbg_location(expr);
    auto return_type = fn_type->data.fn.return_type;
    auto sret_type = fn_type->data.fn.should_use_sret() ? compile_type(return_type) : nullptr;
    return create_fn_call_invoke(callee, args, sret_type, invoke);
}

llvm::Value *Compiler::create_fn_call_invoke(llvm::FunctionCallee callee,
                                             std::vector<llvm::Value *> args, llvm::Type *sret_type,
                                             InvokeInfo *invoke) {
    auto &builder = *m_ctx->llvm_builder.get();
    auto &llvm_ctx = *m_ctx->llvm_ctx.get();
    llvm::Value *sret_var = nullptr;
    if (sret_type) {
        sret_var = builder.CreateAlloca(sret_type, nullptr, "sret");
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

    return sret_type ? builder.CreateLoad(sret_type, sret_var) : ret;
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
    std::vector<llvm::Value *> original_args;

    // Skip the first argument (binding struct) and pass the rest to the original function
    auto arg_count = proxy_llvm_fn->arg_size();

    // Start from index 1 to skip the binding struct (index 0)
    for (unsigned i = 1; i < arg_count; ++i) {
        auto arg = proxy_llvm_fn->getArg(i);
        original_args.push_back(arg);
    }

    // Call the original function
    auto original_fn_type_l = (llvm::FunctionType *)compile_type(original_fn_type);
    llvm::FunctionCallee original_callee(original_fn_type_l, original_fn_ptr);
    auto result = builder.CreateCall(original_callee, original_args);

    // Return the result
    if (original_fn_spec.return_type->kind == TypeKind::Void) {
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
        std::vector<llvm::Value *> args = {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), size),
            llvm::ConstantPointerNull::get(
                llvm::PointerType::get(llvm::Type::getInt8Ty(llvm_ctx), 0)),
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
    case ast::NodeType::VarDecl: {
        auto &data = stmt->data.var_decl;
        auto &llvm_builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();
        auto &llvm_module = *m_ctx->llvm_module.get();
        auto var = compile_alloc(fn, stmt);
        add_var(stmt, var);
        auto var_type = get_chitype(stmt);
        auto value = compile_assignment_to_type(fn, data.expr, var_type);
        if (value && !data.expr->escape.moved) {
            compile_copy(fn, value, var, get_chitype(stmt), data.expr);
        }
        break;
    }
    case ast::NodeType::ReturnStmt: {
        auto &data = stmt->data.return_stmt;
        auto &llvm_builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();
        assert(fn->return_label);
        auto scope = fn->get_scope();

        if (data.expr) {
            // Check if this is an async function returning T (wrapped to Promise<T>)
            bool is_async = fn->node && fn->node->type == ast::NodeType::FnDef &&
                            fn->node->data.fn_def.is_async();
            auto return_type = fn->fn_type->data.fn.return_type;

            if (is_async && return_type->kind == TypeKind::Promise) {
                // For async functions, wrap the return value in a Promise
                auto value_type = return_type->data.promise.value;
                auto value_type_l = compile_type(value_type);
                auto promise_struct_type_l = compile_type(return_type->data.promise.internal);

                // Compile the expression value
                auto ret_value = compile_assignment_value(fn, data.expr, stmt);

                // Allocate memory for the value on the heap
                auto malloc_fn = m_ctx->llvm_module->getFunction("cx_malloc");
                auto value_size = m_ctx->llvm_module->getDataLayout().getTypeAllocSize(value_type_l);
                auto value_size_l = llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), value_size);
                auto null_ptr = llvm::ConstantPointerNull::get(llvm::PointerType::get(llvm_ctx, 0));
                auto value_ptr = llvm_builder.CreateCall(malloc_fn, {value_size_l, null_ptr}, "value_ptr");
                llvm_builder.CreateStore(ret_value, value_ptr);

                // Initialize Promise fields in return_value
                // state = CX_PROMISE_RESOLVED (1)
                auto state_ptr = llvm_builder.CreateStructGEP(promise_struct_type_l, fn->return_value, 0, "state_ptr");
                llvm_builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), 1), state_ptr);

                // value = value_ptr
                auto value_field_ptr = llvm_builder.CreateStructGEP(promise_struct_type_l, fn->return_value, 1, "value_field_ptr");
                llvm_builder.CreateStore(value_ptr, value_field_ptr);

                // error = nullptr
                auto error_ptr = llvm_builder.CreateStructGEP(promise_struct_type_l, fn->return_value, 2, "error_ptr");
                llvm_builder.CreateStore(null_ptr, error_ptr);

                // on_resolve and on_reject are left as zero-initialized
            } else {
                auto ret_value = compile_assignment_value(fn, data.expr, stmt);
                compile_copy(fn, ret_value, fn->return_value, get_chitype(stmt), data.expr);
            }
        }
        llvm_builder.CreateBr(fn->return_label);
        scope->branched = true;
        break;
    }
    case ast::NodeType::BranchStmt: {
        auto token = stmt->token;
        auto loop = fn->get_loop();
        auto &builder = *m_ctx->llvm_builder.get();
        if (token->type == TokenType::KW_BREAK) {
            builder.CreateBr(loop->end);
        }
        if (token->type == TokenType::KW_CONTINUE) {
            builder.CreateBr(loop->start);
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
        break;
    }
    case ast::NodeType::ForStmt: {
        auto &builder = *m_ctx->llvm_builder.get();
        auto &data = stmt->data.for_stmt;
        if (data.kind == ast::ForLoopKind::Range) {
            auto ptr = compile_dot_ptr(fn, data.expr);
            assert(ptr);
            auto sty = get_resolver()->resolve_struct_type(get_chitype(data.expr));
            auto beginp = sty->member_table.get("begin");
            auto endp = sty->member_table.get("end");
            auto nextp = sty->member_table.get("next");
            auto indexp = sty->member_table.get("index");
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
    default:
        compile_assignment_to_type(fn, stmt, nullptr);
    }
}

void Compiler::compile_destruction(Function *fn, llvm::Value *address, ast::Node *node) {
    // In managed memory mode, don't destroy heap-allocated objects locally - GC handles them
    if (is_managed() && node->is_heap_allocated()) {
        return;
    }

    auto type = get_chitype(node);
    if (type->kind == TypeKind::String) {
        auto &builder = *m_ctx->llvm_builder;
        emit_dbg_location(node);
        auto string_delete = get_system_fn("cx_string_delete");
        builder.CreateCall(string_delete->llvm_fn, {address});
        return;
    }

    auto destructor = ChiTypeStruct::get_destructor(type);
    if (destructor) {
        auto &builder = *m_ctx->llvm_builder;
        auto destructor_type = get_chitype(destructor->node);
        auto destructor_fn = get_fn(destructor->node);
        auto destructor_type_l = (llvm::FunctionType *)compile_type(destructor_type);
        auto args = std::vector<llvm::Value *>{address};
        emit_dbg_location(node);
        builder.CreateCall(destructor_type_l, destructor_fn->llvm_fn, args);
    }
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
    for (auto stmt : data.statements) {
        compile_stmt(fn, stmt);
    }
    if (data.return_expr) {
        result = compile_expr(fn, data.return_expr);
    }
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
            name += get_resolver()->to_string(arg, false) + ",";
        }
        name += ">";
    } else {
        // Handle lambda types for non-specialized functions only
        if (ftype->kind == TypeKind::FnLambda) {
            // Only add bind parameter if lambda has captures
            auto &lambda_data = ftype->data.fn_lambda;
            has_bind = lambda_data.captures.len > 0;
            ftype = lambda_data.bound_fn;
            if (has_bind) {
                bind_name = "_binds";
            }
            assert(ftype);
        }
    }

    assert(!ftype->is_placeholder && "Compiling placeholder type");

    if (ftype->kind == TypeKind::Fn) {
        if (ftype->data.fn.container_ref) {
            has_bind = true;
            bind_name = "this";
        }
    }

    if (name.empty()) {
        name = get_resolver()->resolve_qualified_name(fn);
    }

    auto ftype_l = (llvm::FunctionType *)compile_type(ftype);
    auto fn_l = llvm::Function::Create(ftype_l, llvm::Function::ExternalLinkage, name,
                                       m_ctx->llvm_module.get());
    fn_l->addAttributeAtIndex(llvm::AttributeList::FunctionIndex,
                              llvm::Attribute::get(*m_ctx->llvm_ctx, llvm::Attribute::NoInline));

    auto new_fn = add_fn(fn_l, fn, ftype);

    // Store the specialized type for lookup if this is a specialized function
    if (subtype) {
        new_fn->specialized_subtype = subtype;
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
    auto key = get_resolver()->to_string(type);
    auto it = m_ctx->type_table.get(key);
    if (it) {
        return *it;
    }

    auto compiled_type = _compile_type(type);
    m_ctx->type_table[key] = compiled_type;
    return compiled_type;
}

llvm::Type *Compiler::_compile_type(ChiType *type) {
    auto key = get_resolver()->to_string(type);
    assert(!type->is_placeholder && "compile_type called on placeholder type");
    auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
    switch (type->kind) {
    case TypeKind::This: {
        if (m_fn && m_fn->container_subtype) {
            return m_fn->get_this_arg()->getType();
        }
        return compile_type(type->get_elem());
    }
    case TypeKind::Void: {
        return llvm::Type::getVoidTy(llvm_ctx);
    }
    case TypeKind::Bool: {
        return llvm::Type::getInt1Ty(llvm_ctx);
    }
    case TypeKind::Char: {
        return llvm::Type::getInt8Ty(llvm_ctx);
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
        if (data.container_ref) {
            param_types.push_back(compile_type(data.container_ref));
        }
        for (size_t i = 0; i < data.params.len; i++) {
            auto param = data.params[i];
            param_types.push_back(compile_type(param));
        }
        return llvm::FunctionType::get(ret_type_l, param_types, false);
    }
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef: {
        auto &data = type->data.pointer;
        return get_llvm_ptr_type();
        // auto elem_type_l = compile_type(data.elem);
        // return llvm::PointerType::get(elem_type_l, 0);
    }
    case TypeKind::Optional: {
        auto &data = type->data.pointer;
        auto elem_type_l = compile_type(data.elem);
        std::vector<llvm::Type *> members;
        members.push_back(llvm::Type::getInt1Ty(llvm_ctx)); // bool has_value
        members.push_back(elem_type_l);                     // elem
        return llvm::StructType::create(members, get_resolver()->to_string(type, true));
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
        auto key = get_resolver()->to_string(type);
        auto &data = type->data.struct_;
        if (data.kind == ContainerKind::Interface) {
            std::vector<llvm::Type *> members;
            members.push_back(get_llvm_ptr_type()); // typeinfo
            members.push_back(get_llvm_ptr_type()); // data
            members.push_back(get_llvm_ptr_type()); // vtable
            return llvm::StructType::create(members, get_resolver()->to_string(type, true));
        }
        if (!data.fields.len) {
            return compile_type(get_system_types()->void_);
        }

        std::vector<llvm::Type *> members;
        for (auto &member : data.fields) {
            members.push_back(compile_type(member->resolved_type));
        }
        return llvm::StructType::create(members, get_resolver()->to_string(type, true));
    }
    case TypeKind::Error: {
        // TODO: implement actual error type
        return get_llvm_ptr_type();
    }
    case TypeKind::Promise: {
        return compile_type(type->data.promise.internal);
    }
    case TypeKind::Subtype: {
        return compile_type(type->data.subtype.final_type);
    }
    case TypeKind::Placeholder: {
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
            compile_enum(enum_->node);
            assert(enum_->compiled_data_size >= 0);
        }
        auto base_value_struct = enum_->base_value_type->data.enum_value.resolved_struct;
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
        return llvm::StructType::create(members, get_resolver()->to_string(type, true));
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