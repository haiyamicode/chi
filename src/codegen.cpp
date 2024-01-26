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

void CodegenContext::init_llvm() {
    llvm_ctx = std::make_unique<llvm::LLVMContext>();
    llvm_module = std::make_unique<llvm::Module>("main", *llvm_ctx);
    llvm_builder = std::make_unique<llvm::IRBuilder<>>(*llvm_ctx);
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
}

inline llvm::Type *Compiler::compile_type_of(cx::ast::Node *node) {
    return compile_type(get_chitype(node));
}

void Compiler::compile_fn_def(ast::Node *node) {
    auto &fn_def = node->data.fn_def;
    auto fn = compile_fn_proto(fn_def.fn_proto, node);
    if (!fn->llvm_fn->empty()) {
        return panic("function already compiled");
    }

    auto &builder = *m_ctx->llvm_builder.get();
    auto &llvm_ctx = *m_ctx->llvm_ctx.get();
    auto *entry_b = fn->new_label("_entry");
    fn->use_label(entry_b);

    auto &proto = fn_def.fn_proto->data.fn_proto;
    int skip = fn_def.is_instance_method() ? 1 : 0;
    for (uint32_t i = 0; i < proto.params.size; i++) {
        auto &param = proto.params[i];
        auto llvm_param = fn->llvm_fn->getArg(i);
        llvm_param->setName(param->name);
        auto var = builder.CreateAlloca(llvm_param->getType(), nullptr, param->name);
        builder.CreateStore(llvm_param, var);
        add_value(param, var);
    }

    // function return
    auto fn_type = get_chitype(fn->node);
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

    auto return_b = fn->new_label("_return");
    fn->return_label = return_b;
    if (fn_def.body) {
        compile_block(fn, node, fn_def.body);
    }

    // fn_end.label = llvm::BasicBlock::Create(*m_ctx->llvm_ctx, "_fn_end", fn->llvm_fn);

    // fn->pop_return_scope();
    // auto type = get_chitype(fn_def.fn_proto);

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
}

llvm::Value *Compiler::compile_constant_value(Function *fn, const ConstantValue &value,
                                              ChiType *type) {
    auto t = compile_type(type);
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
    auto str_type = compile_type(get_resolver()->get_system_types()->string);
    auto str_value = llvm::ConstantDataArray::getString(llvm_ctx, str);
    auto str_global = new llvm::GlobalVariable(llvm_module, str_type, true,
                                               llvm::GlobalValue::PrivateLinkage, str_value);
    auto str_len = llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), str.size());
    auto str_struct = llvm::ConstantStruct::getAnon({str_global, str_len});
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
    auto info_global = new llvm::GlobalVariable(llvm_module, ti_type_l, true,
                                                llvm::GlobalValue::PrivateLinkage, info_l);
    return info_global;
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
        auto ti_l = compile_type_info(from_type);
        auto from_type_l = compile_type(from_type);
        auto any_type_l = (llvm::StructType *)compile_type(to_type);
        auto any_val = (llvm::Value *)llvm_builder.CreateAlloca(any_type_l, nullptr, "localValue");
        auto ti_gep = llvm_builder.CreateStructGEP(any_type_l, any_val, 0);
        llvm_builder.CreateStore(ti_l, ti_gep);
        auto data_gep = llvm_builder.CreateStructGEP(any_type_l, any_val, 1);
        llvm_builder.CreateStore(value, data_gep);
        return any_val;
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
        return builder.CreateLoad(type_l, ref);
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
            return ref;
        }
        default:
            panic("not implemented: {}", PRINT_ENUM(data.op_type));
        }
    }
    default:
        panic("not implemented: {}", PRINT_ENUM(expr->type));
    }
    return nullptr;
}

llvm::Value *Compiler::compile_expr_ref(Function *fn, ast::Node *expr) {
    auto &builder = *m_ctx->llvm_builder.get();
    auto &data = expr->data.identifier;
    if (data.kind == ast::IdentifierKind::This) {
        return fn->llvm_fn->getArg(0);
    }
    if (data.decl->type == ast::NodeType::VarDecl) {
        auto &var = data.decl->data.var_decl;
        if (var.is_const) {
            return compile_constant_value(fn, var.resolved_value, get_chitype(data.decl));
        }
    }
    auto &val = get_value(data.decl);
    return val;
}

llvm::Value *Compiler::compile_fn_call(Function *fn, ast::Node *expr) {
    auto &data = expr->data.fn_call_expr;
    auto fn_ref = data.fn_ref_expr->data.identifier.decl;
    auto callee = get_fn(fn_ref);
    auto fn_type = get_chitype(callee->node);
    auto &fn_spec = fn_type->data.fn;
    auto is_variadic = fn_spec.is_variadic;
    auto va_start = fn_spec.get_va_start();

    std::vector<llvm::Value *> args;
    for (int i = 0; i < data.args.size; i++) {
        if (is_variadic && i >= va_start) {
            auto array_add_fn = get_system_fn("cx_array_construct");
        }
        auto arg = data.args[i];
        auto param_type = fn_spec.get_param_at(i);
        args.push_back(compile_assignment_to_type(fn, arg, param_type));
    }
    return m_ctx->llvm_builder->CreateCall(callee->llvm_fn, args);
}

llvm::Value *Compiler::compile_alloc(Function *fn, ast::Node *decl) {
    auto &llvm_builder = *m_ctx->llvm_builder.get();
    auto &llvm_ctx = *m_ctx->llvm_ctx.get();
    auto &llvm_module = *m_ctx->llvm_module.get();
    auto var_type_l = compile_type_of(decl);

    if (is_managed() && decl->is_heap_allocated()) {
        auto ptr_type_l = llvm::PointerType::get(llvm::Type::getInt8Ty(llvm_ctx), 0);
        auto var_p = llvm_builder.CreateAlloca(ptr_type_l, nullptr, decl->name);
        auto gc_alloc = get_system_fn("cx_gc_alloc");
        auto size = llvm_module.getDataLayout().getTypeAllocSize(var_type_l);
        std::vector<llvm::Value *> args = {
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_ctx), size),
            llvm::ConstantPointerNull::get(
                llvm::PointerType::get(llvm::Type::getInt8Ty(llvm_ctx), 0)),
        };
        auto result = llvm_builder.CreateCall(gc_alloc->llvm_fn, args);
        // auto p = llvm_builder.CreateLoad(ptr_type_l, result);
        llvm_builder.CreateStore(result, var_p);
        return var_p;
    } else {
        return llvm_builder.CreateAlloca(var_type_l, nullptr, decl->name);
    }
}

void Compiler::compile_stmt(Function *fn, ast::Node *stmt) {
    switch (stmt->type) {
    case ast::NodeType::VarDecl: {
        auto &data = stmt->data.var_decl;
        auto value = compile_assignment_to_type(fn, data.expr, get_chitype(stmt));
        auto &llvm_builder = *m_ctx->llvm_builder.get();
        auto &llvm_ctx = *m_ctx->llvm_ctx.get();
        auto &llvm_module = *m_ctx->llvm_module.get();
        auto var = compile_alloc(fn, stmt);
        if (is_managed() && stmt->is_heap_allocated()) {
            // auto p = llvm_builder.CreateLoad(
            //     llvm::PointerType::get(llvm::Type::getInt8Ty(llvm_ctx), 0), var);
            llvm_builder.CreateStore(value, var);
        } else {
            llvm_builder.CreateStore(value, var);
        }
        add_value(stmt, var);
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
        //     llvm::ConstantInt::get(llvm::IntegerType::getInt1Ty(llvm_ctx), 1), fn->is_returning);
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
    //     llvm::ConstantInt::get(llvm::IntegerType::getInt1Ty(llvm_ctx), 0), fn->is_returning);
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

Function *Compiler::add_fn(llvm::Function *llvm_fn, ast::Node *node) {
    auto fn = new Function(get_context(), llvm_fn, node);
    return m_ctx->add_fn(node, fn);
}

Function *Compiler::get_fn(ast::Node *node) { return m_ctx->function_table.at(node); }

Function *Compiler::compile_fn_proto(ast::Node *node, ast::Node *fn) {
    auto flags = fn->data.fn_def.decl_spec.flags;
    auto ftype = get_chitype(node);
    auto ftype_l = (llvm::FunctionType *)compile_type(ftype);
    auto fn_l = llvm::Function::Create(ftype_l, llvm::Function::ExternalLinkage, node->name,
                                       m_ctx->llvm_module.get());
    // Set names for all arguments.
    unsigned Idx = 0;
    for (auto &arg : fn_l->args())
        arg.setName(node->data.fn_proto.params[Idx++]->name);

    return add_fn(fn_l, fn);
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
    auto it = m_ctx->type_table.get(type->id);
    if (it) {
        return *it;
    }
    auto compiled_type = _compile_type(type);
    m_ctx->type_table[type->id] = compiled_type;
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
        return llvm::StructType::create(
            {llvm::Type::getInt8PtrTy(llvm_ctx), llvm::Type::getInt32Ty(llvm_ctx)}, "CxString");
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
    case TypeKind::Array: {
        auto &data = type->data.array;
        auto elem_type_l = compile_type(data.elem);
        std::vector<llvm::Type *> memberes;
        memberes.push_back(llvm::Type::getInt8PtrTy(llvm_ctx)); // void *data
        memberes.push_back(llvm::Type::getInt32Ty(llvm_ctx));   // uint32_t size
        memberes.push_back(llvm::Type::getInt32Ty(llvm_ctx));   // uint32_t capacity
        memberes.push_back(llvm::Type::getInt8Ty(llvm_ctx));    // uint8_t flags
        return llvm::StructType::create(memberes, "CxArray");
    }
    case TypeKind::Any: {
        std::vector<llvm::Type *> members;
        members.push_back(llvm::Type::getInt8PtrTy(llvm_ctx));
        members.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_ctx), 16));
        return llvm::StructType::create(members, "CxAny");
    }
    default:
        panic("not implemented");
    }
    return nullptr;
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

    auto settings = get_settings();
    std::error_code ec;
    llvm::raw_fd_ostream dest_obj(settings->output_obj_to_file, ec, llvm::sys::fs::OF_None);
    if (ec) {
        print("error: could not open file: {}", ec.message());
        return exit(1);
    }

    llvm::legacy::PassManager pass;
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