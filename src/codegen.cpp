#include "codegen.h"
#include "context.h"

namespace cx {
namespace codegen {

CodegenContext::~CodegenContext() {}
CodegenContext::CodegenContext(CompilationContext *compilation_ctx)
    : compilation_ctx(compilation_ctx), resolver(compilation_ctx->resolve_ctx.get()) {
    init_llvm();
}

Function *CodegenContext::add_fn(ast::Node *node, Function *fn) {
    functions.emplace(fn)->get();
    function_table[node] = fn;
    return fn;
}

Function::Function(CodegenContext *ctx, llvm::Function *llvm_fn, ast::Node *node)
    : ctx(ctx), llvm_fn(llvm_fn), node(node) {
    if (node) {
        qualified_name = node->name;
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
    if (type->kind == TypeKind::Subtype) {
        return type->data.subtype.resolved_struct;
    }
    return type;
}

ChiType *Compiler::get_chitype(ast::Node *node) { return eval_type(node->resolved_type); }

void Compiler::compile_module(ast::Module *module) {
    m_ctx->dbg_cu = m_ctx->dbg_builder->createCompileUnit(
        llvm::dwarf::DW_LANG_C,
        m_ctx->dbg_builder->createFile(module->filename, module->path, std::nullopt, std::nullopt),
        "Chi Compiler", 0, "", 0);

    m_ctx->pending_fns.clear();
    auto &root = module->root->data.root;
    for (auto decl : root.top_level_decls) {
        if (decl->type == ast::NodeType::FnDef) {
            compile_fn_def(decl);
        } else if (decl->type == ast::NodeType::StructDecl) {
            // compile_struct(decl);
        } else if (decl->type == ast::NodeType::ExternDecl) {
            compile_extern(decl);
        } else {
            panic("not implemented");
        }
    }

    while (m_ctx->pending_fns.size) {
        auto fn = m_ctx->pending_fns.pop();
        compile_fn_def(fn->node, fn);
    }
}

inline llvm::Type *Compiler::compile_type_of(cx::ast::Node *node) {
    return compile_type(get_chitype(node));
}

llvm::DISubroutineType *Compiler::compile_di_fn_type(Function *fn) {
    llvm::SmallVector<llvm::Metadata *, 8> types;
    for (auto param : fn->fn_type->data.fn.params) {
        types.push_back(compile_di_type(param));
    }
    return m_ctx->dbg_builder->createSubroutineType(
        m_ctx->dbg_builder->getOrCreateTypeArray(types));
}

llvm::DIType *Compiler::compile_di_type(ChiType *type) {
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
    case TypeKind::Reference: {
        auto &data = type->data.pointer;
        auto elem_type = compile_di_type(data.elem);
        return llvm_db.createPointerType(elem_type, 64);
    }
    case TypeKind::Array: {
        auto &data = type->data.array;
        auto elem_type = compile_di_type(data.elem);
        auto size = llvm_module.getDataLayout().getTypeAllocSize(compile_type(data.elem));
        return llvm_db.createArrayType(0, size, elem_type, {});
    }
    case TypeKind::Struct: {
        auto &data = type->data.struct_;
        if (!data.members.size) {
            return llvm_db.createBasicType("void", 0, llvm::dwarf::DW_ATE_address);
        }
        auto name = m_ctx->resolver.to_string(type);
        auto file = llvm_db.createFile(llvm_cu.getFilename(), llvm_cu.getDirectory());
        auto line_no = 0;
        if (data.node) {
            line_no = data.node->token->pos.line_number();
        }
        auto scope = llvm_db.createStructType(m_ctx->dbg_cu, name, file, line_no, 0, 0,
                                              llvm::DINode::FlagZero, nullptr, {});
        m_ctx->dbg_scopes.add(scope);
        return scope;
    }
    default:
        auto size = llvm_module.getDataLayout().getTypeAllocSize(compile_type(type));
        return llvm_db.createBasicType(m_ctx->resolver.to_string(type), size,
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
    auto dbg_builder = m_ctx->dbg_builder.get();
    auto unit =
        dbg_builder->createFile(m_ctx->dbg_cu->getFilename(), m_ctx->dbg_cu->getDirectory());
    llvm::DIScope *dctx = unit;
    auto line_no = fn_def.fn_proto->token->pos.line_number();
    auto sp = dbg_builder->createFunction(
        dctx, name, llvm::StringRef(), unit, 0, compile_di_fn_type(fn), line_no,
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
    // int skip = (fn_def.is_instance_method() ? 1 : 0) + fn->bind_offset;
    int skip = fn->bind_offset;
    for (uint32_t i = 0; i < fn->llvm_fn->arg_size(); i++) {
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
        builder.CreateStore(llvm_param, var);
        add_var(param, var);

        // debug info
        auto param_line_no = param->token->pos.line_number();
        auto dbg_var = dbg_builder->createParameterVariable(sp, llvm_param->getName(), i + skip + 1,
                                                            unit, param_line_no,
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
        fn->return_value = builder.CreateAlloca(return_type_l, nullptr, "_fn_ret");
    }

    // main function initialization
    auto is_entry = declspec_has_flag(fn_def.decl_spec, ast::DECL_IS_ENTRY);
    if (is_entry) {
        auto runtime_start = get_system_fn("cx_runtime_start");
        auto stack_marker = fn->return_value;
        if (!fn->return_value) {
            stack_marker = builder.CreateAlloca(llvm::IntegerType::getInt1Ty(llvm_ctx), nullptr,
                                                "_stack_marker");
        }
        builder.CreateCall(runtime_start->llvm_fn, {stack_marker});
    }

    // function body
    emit_dbg_location(fn_def.body);
    auto return_b = fn->new_label("_return");
    fn->return_label = return_b;
    if (fn_def.body) {
        compile_block(fn, node, fn_def.body);
    }
    if (fn->has_try) {
        fn->llvm_fn->setPersonalityFn(get_system_fn("cx_personality")->llvm_fn);
    }

    // return block
    fn->use_label(return_b);
    if (is_entry) {
        auto runtime_stop = get_system_fn("cx_runtime_stop");
        builder.CreateCall(runtime_stop->llvm_fn, {});
    }
    if (return_type->kind == TypeKind::Void) {
        builder.CreateRetVoid();
    } else {
        // auto ret_value = builder.CreateLoad(return_type_l, fn->return_value);
        auto value = builder.CreateLoad(return_type_l, fn->return_value);
        builder.CreateRet(value);
    }

    llvm::verifyFunction(*fn->llvm_fn);
    return fn;
}

llvm::Value *Compiler::compile_constant_value(Function *fn, const ConstantValue &value,
                                              ChiType *type) {
    auto t = compile_type(type);
    if (type->is_pointer()) {
        return llvm::ConstantPointerNull::get((llvm::PointerType *)t);
    }
    if (VARIANT_TRY(value, const_int_t, v)) {
        return llvm::ConstantInt::get(t, *v);
    } else if (VARIANT_TRY(value, const_float_t, v)) {
        return llvm::ConstantFP::get(t, *v);
    } else if (VARIANT_TRY(value, string, v)) {
        return compile_string_literal(fn, *v);
    }
    return nullptr;
}

llvm::Value *Compiler::compile_string_literal(Function *fn, const string &str) {
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
    auto str_struct =
        llvm::ConstantStruct::get((llvm::StructType *)str_type_l, {str_global, str_len});
    return str_struct;
}

llvm::Value *Compiler::compile_assignment_to_type(Function *fn, ast::Node *expr,
                                                  ChiType *dest_type) {
    auto src_value = compile_expr(fn, expr);
    auto src_type = get_chitype(expr);
    if (expr->type == ast::NodeType::ConstructExpr && src_type == dest_type) {
        return src_value;
    }
    auto value = src_value;
    if (dest_type) {
        value = compile_conversion(fn, src_value, src_type, dest_type);
    }
    // if (expr->type == ast::NodeType::FnCallExpr && src_type->kind != TypeKind::Void) {
    //     if (should_destroy_for_type(src_type)) {
    //         // auto addr = fn->insn_address_of(src_value);
    //         // compile_destruction_for_type(fn, addr, src_type);
    //     }
    // }
    return value;
}

llvm::Value *Compiler::compile_assignment_value(Function *fn, ast::Node *expr, ast::Node *dest) {
    return compile_assignment_to_type(fn, expr, get_chitype(dest));
}

llvm::Value *Compiler::compile_type_info(ChiType *type) {
    if (auto info = m_ctx->typeinfo_table.get(type)) {
        return *info;
    }
    auto type_l = compile_type(type);
    auto &llvm_ctx = *(m_ctx->llvm_ctx.get());
    auto &llvm_module = *(m_ctx->llvm_module.get());
    auto tidata_type_l = llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx), 32);
    auto ti_type_l = llvm::StructType::create(
        {llvm::Type::getInt32Ty(llvm_ctx), llvm::Type::getInt32Ty(llvm_ctx), tidata_type_l},
        "TypeInfo");
    auto typesize = llvm_module.getDataLayout().getTypeAllocSize(type_l);
    auto typedata = (uint8_t *)&type->data;
    llvm::Constant *typedata_arr_l =
        llvm::ConstantDataArray::get(llvm_ctx, llvm::ArrayRef<uint8_t>(typedata, 32));

    auto info_l = llvm::ConstantStruct::get(
        ti_type_l, {llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), (int32_t)type->kind),
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), (int32_t)typesize),
                    typedata_arr_l});
    auto info_global =
        new llvm::GlobalVariable(llvm_module, ti_type_l, true, llvm::GlobalValue::PrivateLinkage,
                                 info_l, "typeinfo." + get_resolver()->to_string(type));
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
    if (captures && captures->size) {
        auto bstruct = lambda_type->data.fn_lambda.bind_struct;
        assert(bstruct);
        auto bstruct_l = compile_type(bstruct);
        auto bind_var = builder.CreateAlloca(bstruct_l, nullptr, "lambda_captures");
        auto bind_size = llvm_module.getDataLayout().getTypeAllocSize(bstruct_l);
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
        auto data_gep = llvm_builder.CreateStructGEP(any_type_l, any_var, 1);
        llvm_builder.CreateStore(value, data_gep);
        return llvm_builder.CreateLoad(any_type_l, any_var);
    }
    case TypeKind::Bool: {
        if (from_type->kind == TypeKind::Optional) {
            auto from_type_l = compile_type(from_type);
            auto has_value_p =
                m_ctx->llvm_builder->CreateStructGEP(from_type_l, value, 0, "has_value");
            auto has_value =
                m_ctx->llvm_builder->CreateLoad(from_type_l->getStructElementType(0), has_value_p);
            return has_value;

        } else {
            return m_ctx->llvm_builder->CreateIntCast(value, compile_type(to_type), false);
        }
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

    default:
        // by default, do nothing
        return value;
    }
}

llvm::Value *Compiler::compile_expr(Function *fn, ast::Node *expr) {
    switch (expr->type) {
    case ast::NodeType::FnCallExpr: {
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
        return compile_constant_value(fn, value, get_chitype(expr));
    }
    case ast::NodeType::UnaryOpExpr: {
        auto &data = expr->data.unary_op_expr;
        auto &builder = *m_ctx->llvm_builder.get();
        switch (data.op_type) {
        case TokenType::LNOT: {
            if (data.is_suffix) {
                auto ptr = compile_expr(fn, data.op1);
                auto elem_type = get_chitype(data.op1)->get_elem();
                auto elem_type_l = compile_type(elem_type);
                auto value = builder.CreateLoad(elem_type_l, ptr);
                return value;
            } else {
                panic("not implemented");
            }
        }
        case TokenType::AND: {
            auto ref = compile_expr_ref(fn, data.op1);
            assert(ref.address);
            return ref.address;
        }
        default:
            panic("not implemented: {}", PRINT_ENUM(data.op_type));
        }
    }
    case ast::NodeType::TryExpr: {
        fn->has_try = true;
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
        auto landing = builder.CreateLandingPad(llvm::Type::getInt8PtrTy(llvm_ctx), 1);
        landing->addClause(llvm::ConstantPointerNull::get(
            llvm::PointerType::get(llvm::Type::getInt8Ty(llvm_ctx), 0)));
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
        auto &data = expr->data.construct_expr;
        return compile_alloc(fn, expr, data.is_new);
    }
    case ast::NodeType::BinOpExpr: {
        auto &data = expr->data.bin_op_expr;
        auto &builder = *m_ctx->llvm_builder.get();
        switch (data.op_type) {
        case TokenType::ASS: {
            auto value = compile_assignment_value(fn, data.op2, data.op1);
            auto ref = compile_expr_ref(fn, data.op1);
            assert(ref.address);
            builder.CreateStore(value, ref.address);
            return value;
        }
        default:
            panic("not implemented: {}", PRINT_ENUM(data.op_type));
        }
        return nullptr;
    }
    default:
        panic("not implemented: {}", PRINT_ENUM(expr->type));
    }
    return nullptr;
}

RefValue Compiler::compile_expr_ref(Function *fn, ast::Node *expr) {
    switch (expr->type) {
    case ast::NodeType::Identifier:
        return compile_iden_ref(fn, expr);
    case ast::NodeType::DotExpr: {
        auto &data = expr->data.dot_expr;
        auto type = get_chitype(data.expr);
        llvm::Value *ptr = nullptr;
        if (type->is_pointer()) {
            type = type->get_elem();
            ptr = compile_expr(fn, data.expr);
        } else {
            auto ref = compile_expr_ref(fn, data.expr);
            if (type->kind == TypeKind::Fn) {
                auto struct_type = get_resolver()->get_lambda_for_fn(type);
                ptr = ref.value;
            } else {
                ptr = ref.address;
            }
        }

        auto &builder = *m_ctx->llvm_builder.get();
        auto struct_type = type;
        assert(ptr);
        auto struct_type_l = compile_type(struct_type);
        auto field_index = data.resolved_member->field_index;
        auto gep = builder.CreateStructGEP(struct_type_l, ptr, field_index);
        auto member_type_l = compile_type(data.resolved_member->resolved_type);
        return RefValue::from_address(gep);
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
        return RefValue::from_value(fn->llvm_fn->getArg(0));
    }
    if (data.decl->type == ast::NodeType::VarDecl) {
        auto &var = data.decl->data.var_decl;
        if (var.is_const) {
            return RefValue::from_value(
                compile_constant_value(fn, var.resolved_value, get_chitype(data.decl)));
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
    if (data.decl->parent_fn != fn->node) {
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

llvm::Value *Compiler::compile_fn_call(Function *fn, ast::Node *expr, InvokeInfo *invoke) {
    auto &data = expr->data.fn_call_expr;
    auto &builder = *m_ctx->llvm_builder.get();
    auto ref = compile_expr_ref(fn, data.fn_ref_expr);

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

        for (int i = 0; i < data.args.size; i++) {
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

    assert(data.fn_ref_expr->type == ast::NodeType::Identifier);
    auto fn_ref = data.fn_ref_expr->data.identifier.decl;
    auto callee = get_fn(fn_ref);
    auto fn_type = get_chitype(callee->node);
    auto &fn_spec = fn_type->data.fn;
    auto is_variadic = fn_spec.is_variadic;
    auto va_start = fn_spec.get_va_start();

    std::vector<llvm::Value *> args;
    for (int i = 0; i < data.args.size; i++) {
        if (is_variadic && i >= va_start) {
            auto array_add_fn = get_system_fn("cx_array_new");
        }
        auto arg = data.args[i];
        auto param_type = fn_spec.get_param_at(i);
        args.push_back(compile_assignment_to_type(fn, arg, param_type));
    }

    if (invoke) {
        return builder.CreateInvoke(callee->llvm_fn, invoke->normal, invoke->landing, args);
    }
    return builder.CreateCall(callee->llvm_fn, args);
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
        auto size = llvm_module.getDataLayout().getTypeAllocSize(var_type_l);
        std::vector<llvm::Value *> args = {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), size),
            llvm::ConstantPointerNull::get(
                llvm::PointerType::get(llvm::Type::getInt8Ty(llvm_ctx), 0)),
        };
        auto result = llvm_builder.CreateCall(alloc_fn->llvm_fn, args);
        return result;
    }
    return llvm_builder.CreateAlloca(var_type_l, nullptr, decl->name);
}

void Compiler::compile_stmt(Function *fn, ast::Node *stmt) {
    emit_dbg_location(stmt);

    switch (stmt->type) {
    case ast::NodeType::VarDecl: {
        auto &data = stmt->data.var_decl;
        auto value = compile_assignment_to_type(fn, data.expr, get_chitype(stmt));
        auto &llvm_builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();
        auto &llvm_module = *m_ctx->llvm_module.get();
        auto var = compile_alloc(fn, stmt);
        llvm_builder.CreateStore(value, var);
        add_var(stmt, var);
        break;
    }
    case ast::NodeType::ReturnStmt: {
        auto &data = stmt->data.return_stmt;
        auto &llvm_builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();
        assert(fn->return_label);
        auto scope = fn->get_scope();
        if (scope->returned) {
            return;
        }

        // fn->is_returning = llvm_builder.CreateStore(
        //     llvm::ConstantInt::get(llvm::IntegerType::getInt1Ty(llvm_ctx), 1),
        //     fn->is_returning);
        if (data.expr) {
            auto ret_value = compile_assignment_value(fn, data.expr, stmt);
            llvm_builder.CreateStore(ret_value, fn->return_value);
        }
        llvm_builder.CreateBr(fn->return_label);
        scope->returned = true;
        break;
    }
    default:
        compile_assignment_to_type(fn, stmt, nullptr);
    }
}

void Compiler::compile_block(Function *fn, ast::Node *parent, ast::Node *block) {
    // TODO: implement destructors

    assert(block->type == ast::NodeType::Block);
    auto &builder = *m_ctx->llvm_builder.get();
    // array<ast::Node *> vars; // vars to destroy

    auto scope = fn->push_scope();
    for (auto stmt : block->data.block.statements) {
        // if (stmt->type == ast::NodeType::VarDecl) {
        //     if (should_destroy(stmt)) {
        //         vars.add(stmt);
        //         ret_scope->emplace_back().var = stmt;
        //     }
        // }
        compile_stmt(fn, stmt);
    }
    fn->pop_scope();
    if (!fn->scope_stack.size() && !scope->returned) {
        builder.CreateBr(fn->return_label);
    }

    // call destructors
    // auto &llvm_builder = *m_ctx->llvm_builder.get();
    // auto &llvm_ctx = *m_ctx->llvm_ctx.get();
    // fn->is_returning = llvm_builder.CreateStore(
    //     llvm::ConstantInt::get(llvm::IntegerType::getInt1Ty(llvm_ctx), 0),
    //     fn->is_returning);
    // if (vars.size) {
    //     for (long i = vars.size - 1; i >= 0; i--) {
    //         auto var = vars[i];
    //         if (var == ret_scope->back().var) {
    //             ret_scope->back().label =
    //                 llvm::BasicBlock::Create(*m_ctx->llvm_ctx, "", fn->llvm_fn);
    //             ret_scope->pop_back();
    //         }
    //         // TODO
    //         // compile_destruction(fn, get_value(var), var);
    //     }
    // }

    // auto bb = llvm::BasicBlock::Create(*m_ctx->llvm_ctx, "", fn->llvm_fn);
    // ret_scope->back().label = bb;
    // ret_scope->pop_back();
    // assert(ret_scope->empty());
    // fn->pop_return_scope();
    // llvm_builder.CreateCondBr(fn->is_returning, fn->get_return_label(), bb);
}

Function *Compiler::add_fn(llvm::Function *llvm_fn, ast::Node *node, ChiType *fn_type) {
    auto fn = new Function(get_context(), llvm_fn, node);
    fn->fn_type = fn_type ? fn_type : get_chitype(node);
    return m_ctx->add_fn(node, fn);
}

Function *Compiler::get_fn(ast::Node *node) { return m_ctx->function_table.at(node); }

Function *Compiler::compile_fn_proto(ast::Node *node, ast::Node *fn, string name) {
    auto flags = fn->data.fn_def.decl_spec.flags;
    auto ftype = get_chitype(node);
    int bind_offset = 0;
    if (ftype->kind == TypeKind::FnLambda) {
        bind_offset = 1;
        ftype = ftype->data.fn_lambda.bound_fn;
        assert(ftype);
    }
    if (name.empty()) {
        name = node->name;
    }
    auto ftype_l = (llvm::FunctionType *)compile_type(ftype);
    auto fn_l = llvm::Function::Create(ftype_l, llvm::Function::ExternalLinkage, name,
                                       m_ctx->llvm_module.get());
    // Set names for all arguments.
    for (int i = 0; i < fn_l->arg_size(); i++) {
        if (i == 0 && bind_offset) {
            fn_l->arg_begin()->setName("_binds");
        } else {
            auto index = i - bind_offset;
            auto arg = fn_l->arg_begin() + i;
            arg->setName(node->data.fn_proto.params[index]->name);
        }
    }

    auto new_fn = add_fn(fn_l, fn, ftype);
    new_fn->bind_offset = bind_offset;
    return new_fn;
}

void Compiler::compile_extern(ast::Node *node) {
    auto &data = node->data.extern_decl;
    for (auto member : data.members) {
        auto fn_data = member->data.fn_def;
        auto fn = compile_fn_proto(fn_data.fn_proto, member);
        m_ctx->function_table.emplace(member, fn);
    }
}

llvm::Type *Compiler::compile_type(ChiType *type) {
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
        return llvm::StructType::create({compile_type(get_resolver()->get_system_types()->str_lit),
                                         llvm::Type::getInt32Ty(llvm_ctx)},
                                        "CxString");
    }
    case TypeKind::FnLambda: {
        return compile_type(type->data.fn_lambda.internal);
    }
    case TypeKind::Fn: {
        auto &data = type->data.fn;
        auto ret_type_l = compile_type(data.return_type);
        std::vector<llvm::Type *> param_types = {};
        for (auto param : data.params) {
            param_types.push_back(compile_type(param));
        }
        return llvm::FunctionType::get(ret_type_l, param_types, data.is_variadic);
    }
    case TypeKind::Pointer:
    case TypeKind::Reference: {
        auto &data = type->data.pointer;
        auto elem_type_l = compile_type(data.elem);
        return llvm::PointerType::get(elem_type_l, 0);
    }
    case TypeKind::Optional: {
        auto &data = type->data.pointer;
        auto elem_type_l = compile_type(data.elem);
        std::vector<llvm::Type *> members;
        members.push_back(llvm::Type::getInt1Ty(llvm_ctx)); // bool has_value
        members.push_back(elem_type_l);                     // elem
        return llvm::StructType::create(members, get_resolver()->to_string(type));
    }
    case TypeKind::Array: {
        auto &data = type->data.array;
        auto elem_type_l = compile_type(data.elem);
        std::vector<llvm::Type *> members;
        members.push_back(llvm::Type::getInt8PtrTy(llvm_ctx)); // void *data
        members.push_back(llvm::Type::getInt32Ty(llvm_ctx));   // uint32_t size
        members.push_back(llvm::Type::getInt32Ty(llvm_ctx));   // uint32_t capacity
        members.push_back(llvm::Type::getInt8Ty(llvm_ctx));    // uint8_t flags
        return llvm::StructType::create(members, get_resolver()->to_string(type));
    }
    case TypeKind::Any: {
        std::vector<llvm::Type *> members;
        members.push_back(llvm::Type::getInt8PtrTy(llvm_ctx));
        members.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx), 16));
        return llvm::StructType::create(members, "CxAny");
    }
    case TypeKind::Result: {
        auto &data = type->data.result;
        return compile_type(data.internal);
    }
    case TypeKind::Struct: {
        auto &data = type->data.struct_;
        if (!data.members.size) {
            return compile_type(get_system_types()->void_);
        }
        std::vector<llvm::Type *> members;
        for (auto &member : data.members) {
            members.push_back(compile_type(member->resolved_type));
        }
        return llvm::StructType::create(members, get_resolver()->to_string(type));
    }
    case TypeKind::Error: {
        // TODO: implement actual error type
        return llvm::Type::getInt8PtrTy(llvm_ctx);
    }
    case TypeKind::Promise: {
        return compile_type(type->data.promise.internal);
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
    if (m_ctx->dbg_scopes.size) {
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
    auto runtime_pkg = m_ctx->compilation_ctx->packages[0].get();
    assert(runtime_pkg->kind == ast::PackageKind::BUILTIN);
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