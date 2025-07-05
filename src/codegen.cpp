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
    if (type->is_placeholder && m_fn && m_fn->container_subtype) {
        type = get_resolver()->type_placeholders_sub(type, m_fn->container_subtype);
    }
    if (type->kind == TypeKind::Array && type->data.array.elem->is_placeholder) {
        return get_resolver()->type_placeholders_sub(type, m_fn->container_subtype);
    }
    if (type->kind == TypeKind::Subtype) {
        return type->data.subtype.resolved_struct;
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
            auto fn = compile_fn_proto(decl->data.fn_def.fn_proto, decl);
            m_ctx->pending_fns.add(fn);
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
    auto struct_type = type->kind == TypeKind::Subtype ? type->data.subtype.resolved_struct : type;

    for (auto member : node->data.struct_decl.members) {
        if (member->type == ast::NodeType::FnDef) {
            auto fn_node = member;
            if (subtype) {
                auto subtype_member = struct_type->data.struct_.find_member(member->name);
                fn_node = subtype_member->node;
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
        if (member->is_method()) {
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
        return compile_di_type(type->data.subtype.resolved_struct);
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
    auto &fn_def = node->data.fn_def;
    if (!fn) {
        fn = compile_fn_proto(fn_def.fn_proto, node);
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

    auto &proto = fn_def.fn_proto->data.fn_proto;
    int skip = fn->bind_offset;
    llvm::Value *sret_arg = nullptr;
    for (uint32_t i = 0; i < fn->llvm_fn->arg_size(); i++) {
        if (i == fn->sret_offset) {
            auto llvm_param = fn->llvm_fn->getArg(i);
            sret_arg = llvm_param;
            continue;
        }

        if (i < fn->bind_offset) {
            auto llvm_param = fn->llvm_fn->getArg(i);
            auto var = builder.CreateAlloca(llvm_param->getType(), nullptr, llvm_param->getName());
            builder.CreateStore(llvm_param, var);
            fn->bind_ptr = builder.CreateLoad(llvm_param->getType(), var);
            continue;
        }

        auto idx = i - skip;
        auto param = proto.params[idx];
        auto llvm_param = fn->llvm_fn->getArg(i);
        auto var = compile_alloc(fn, param);
        emit_dbg_location(param);
        compile_copy(fn, llvm_param, var, get_chitype(param));
        add_var(param, var);

        // debug info
        auto param_line_no = param->token->pos.line_number();
        auto dbg_var = dbg_builder->createParameterVariable(sp, llvm_param->getName(), i + skip + 1,
                                                            file, param_line_no,
                                                            compile_di_type(get_chitype(param)));
        dbg_builder->insertDeclare(var, dbg_var, dbg_builder->createExpression(),
                                   llvm::DILocation::get(sp->getContext(), param_line_no, 0, sp),
                                   builder.GetInsertBlock());
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
        compile_block(fn, node, fn_def.body, return_b);
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
    if (type->is_pointer()) {
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
    builder.CreateStore(fn_ptr, fn_ptr_gep);
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

        for (auto capture : *captures) {
            auto ref = get_var(capture);
            auto capture_gep =
                builder.CreateStructGEP(bstruct_l, bind_var, capture->escape.local_index);
            builder.CreateStore(ref, capture_gep);
        }
    } else {
        builder.CreateStore(llvm::ConstantPointerNull::get(
                                llvm::PointerType::get(llvm::Type::getInt8Ty(*m_ctx->llvm_ctx), 0)),
                            data_gep);
        builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*m_ctx->llvm_ctx), 0),
                            size_gep);
    }
    return builder.CreateLoad(struct_type_l, var);
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
        if (from_type->kind == TypeKind::Pointer) {
            return m_ctx->llvm_builder->CreatePtrToInt(value, compile_type(to_type));
        } else {
            return m_ctx->llvm_builder->CreateIntCast(value, compile_type(to_type), false);
        }
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

static llvm::BinaryOperator::BinaryOps get_binop(TokenType op) {
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
            auto llvm_op = get_binop(data.op_type);
            auto lhs = compile_expr(fn, data.op1);
            auto rhs = compile_expr(fn, data.op2);
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
        auto lambda_fn = compile_fn_proto(data.fn_proto, expr, fn->qualified_name + "__anonymous");
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
        return compile_expr(fn, data.expr);
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
        auto sty = get_resolver()->resolve_struct_type(type);
        auto &builder = *m_ctx->llvm_builder;
        auto copyp = sty->member_intrinsics.get(IntrinsicSymbol::CopyFrom);
        if (copyp) {
            auto copy = *copyp;
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
            auto call = builder.CreateCall(get_fn(copy->node)->llvm_fn, {
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
    if (ctn_type->is_pointer()) {
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
        } else if (type->is_pointer()) {
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
        auto fn = get_fn(data.decl);
        auto type_l = compile_type(get_chitype(iden));
        return RefValue::from_value(fn->llvm_fn);
    }

normal:
    // handle captured variables
    if (iden->escape.is_capture()) {
        assert(fn->bind_ptr);
        auto field_idx = data.decl->escape.local_index;
        assert(field_idx >= 0);
        auto bstruct = get_chitype(fn->node)->data.fn_lambda.bind_struct;
        auto bstruct_l = (llvm::StructType *)compile_type(bstruct);
        auto gep = builder.CreateStructGEP(bstruct_l, fn->bind_ptr, field_idx);
        auto ref = builder.CreateLoad(bstruct_l->elements()[field_idx], gep);
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

        auto bind_gep = builder.CreateStructGEP(lambda_type_l, ref.address, 2);
        auto bind_ptr = builder.CreateLoad(lambda_type_l->elements()[2], bind_gep);
        args.push_back(bind_ptr);

        for (int i = 0; i < data.args.len; i++) {
            auto arg = data.args[i];
            auto param_type = fn_spec.get_param_at(i);
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
        auto ctn_type = get_chitype(dot_expr.expr);
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
        auto callee_fn = get_fn(fn_decl);
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

llvm::Value *Compiler::compile_alloc(Function *fn, ast::Node *decl, bool is_new) {
    auto &llvm_builder = *m_ctx->llvm_builder.get();
    auto &llvm_ctx = *m_ctx->llvm_ctx.get();
    auto &llvm_module = *m_ctx->llvm_module.get();
    auto var_type_l = compile_type_of(decl);

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
            auto ret_value = compile_assignment_value(fn, data.expr, stmt);
            compile_copy(fn, ret_value, fn->return_value, get_chitype(stmt), data.expr);
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
    default:
        compile_assignment_to_type(fn, stmt, nullptr);
    }
}

void Compiler::compile_destruction(Function *fn, llvm::Value *address, ast::Node *node) {
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
    auto declspec = fn->declspec();
    auto ftype = get_chitype(fn);
    int bind_offset = 0;
    string bind_name = "";
    if (ftype->kind == TypeKind::FnLambda) {
        bind_offset = 1;
        ftype = ftype->data.fn_lambda.bound_fn;
        bind_name = "_binds";
        assert(ftype);
    }
    if (ftype->kind == TypeKind::Fn) {
        if (ftype->data.fn.container_ref) {
            bind_offset = 1;
            bind_name = "this";
        }
    }
    if (name.empty()) {
        name = proto_node->name;
    }
    auto ftype_l = (llvm::FunctionType *)compile_type(ftype);
    auto fn_l = llvm::Function::Create(ftype_l, llvm::Function::ExternalLinkage, name,
                                       m_ctx->llvm_module.get());
    fn_l->addAttributeAtIndex(llvm::AttributeList::FunctionIndex,
                              llvm::Attribute::get(*m_ctx->llvm_ctx, llvm::Attribute::NoInline));

    // sret mechanism
    auto sret_offset = -1;
    if (ftype->data.fn.should_use_sret() && !declspec.is_extern()) {
        bind_offset++;
        sret_offset = 0;
    }

    // Set names for all arguments.
    for (int i = 0; i < fn_l->arg_size(); i++) {
        if (i == sret_offset) {
            fn_l->getArg(i)->setName("sret");
            // fn_l->getArg(i)->addAttr(llvm::Attribute::StructRet);
        } else if (i == bind_offset - 1) {
            fn_l->getArg(i)->setName(bind_name);
        } else {
            auto index = i - bind_offset;
            auto arg = fn_l->arg_begin() + i;
            arg->setName(proto_node->data.fn_proto.params[index]->name);
        }
    }

    auto new_fn = add_fn(fn_l, fn, ftype);
    new_fn->bind_offset = bind_offset;
    new_fn->sret_offset = sret_offset;
    new_fn->llvm_fn->setName(new_fn->get_llvm_name());
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
    if (!type->name) {
        auto stringid = get_resolver()->to_string(type);
        auto it = m_ctx->anon_type_table.get(stringid);
        if (it) {
            return *it;
        }
    } else {
        auto it = m_ctx->type_table.get(type->id);
        if (it) {
            return *it;
        }
    }
    auto compiled_type = _compile_type(type);
    m_ctx->type_table[type->id] = compiled_type;

    if (!type->name) {
        auto stringid = get_resolver()->to_string(type);
        m_ctx->anon_type_table[stringid] = compiled_type;
    }
    return compiled_type;
}

llvm::Type *Compiler::_compile_type(ChiType *type) {
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
    case TypeKind::Int: {
        return llvm::Type::getIntNTy(llvm_ctx, type->data.int_.bit_count);
    }
    case TypeKind::Float: {
        return llvm::Type::getFloatTy(llvm_ctx);
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
        for (auto param : data.params) {
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
        // auto &data = type->data.array;
        // auto elem_type_l = compile_type(data.elem);
        // std::vector<llvm::Type *> members;
        // members.push_back(get_llvm_ptr_type());              // void *data
        // members.push_back(llvm::Type::getInt32Ty(llvm_ctx)); // uint32_t size
        // members.push_back(llvm::Type::getInt32Ty(llvm_ctx)); // uint32_t capacity
        // members.push_back(llvm::Type::getInt8Ty(llvm_ctx));  // uint8_t flags
        // return llvm::StructType::create(members, get_resolver()->to_string(type, true));
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
        return compile_type(type->data.subtype.resolved_struct);
    }
    case TypeKind::Placeholder: {
        return compile_type(get_system_types()->void_);
    }
    case TypeKind::Enum: {
        return _compile_type(type->data.enum_.base_value_type);
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