#include "codegen.h"

namespace cx {
namespace codegen {

CompilationContext::~CompilationContext() {}
CompilationContext::CompilationContext(ResolveContext *rctx) : resolver(rctx) { init_llvm(); }

Function *CompilationContext::add_fn(ast::Node *node, Function *fn) {
    functions.emplace(fn)->get();
    function_table[node] = fn;
    return fn;
}

Function::Function(CompilationContext *ctx, llvm::Function *llvm_fn, ast::Node *node)
    : ctx(ctx), llvm_fn(llvm_fn), node(node) {
    if (node) {
        qualified_name = node->name;
    }
}

void CompilationContext::init_llvm() {
    llvm_ctx = std::make_unique<llvm::LLVMContext>();
    llvm_module = std::make_unique<llvm::Module>("main", *llvm_ctx);
    llvm_builder = std::make_unique<llvm::IRBuilder<>>(*llvm_ctx);
}

Compiler::Compiler(CompilationContext *ctx) : m_ctx(ctx) {}

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

    llvm::BasicBlock *bb =
        llvm::BasicBlock::Create(*m_ctx->llvm_ctx, fn->get_llvm_name(), fn->llvm_fn);
    m_ctx->llvm_builder->SetInsertPoint(bb);

    auto &proto = fn_def.fn_proto->data.fn_proto;
    int skip = fn_def.is_instance_method() ? 1 : 0;
    for (uint32_t i = 0; i < proto.params.size; i++) {
        auto &param = proto.params[i];
        auto llvm_param = fn->llvm_fn->getArg(i);
        llvm_param->setName(param->name);
        add_value(param, llvm_param);
    }

    compile_block(fn, node, fn_def.body);
    auto type = get_chitype(fn_def.fn_proto);
    if (type->data.fn.return_type->kind == TypeKind::Void) {
        m_ctx->llvm_builder->CreateRetVoid();
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

llvm::Value *Compiler::compile_expr(Function *fn, ast::Node *expr) {
    switch (expr->type) {
    case ast::NodeType::FnCallExpr: {
        return compile_fn_call(fn, expr);
    }
    case ast::NodeType::Identifier: {
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
    case ast::NodeType::LiteralExpr: {
        auto value = get_resolver()->resolve_constant_value(expr);
        return compile_constant_value(fn, value, get_chitype(expr));
    }
    default:
        panic("not implemented: {}", PRINT_ENUM(expr->type));
    }
    return nullptr;
}

llvm::Value *Compiler::compile_fn_call(Function *fn, ast::Node *expr) {
    auto &data = expr->data.fn_call_expr;
    auto fn_ref = data.fn_ref_expr->data.identifier.decl;
    auto callee = get_fn(fn_ref);

    std::vector<llvm::Value *> args;
    for (auto arg : data.args) {
        args.push_back(compile_expr(fn, arg));
    }
    return m_ctx->llvm_builder->CreateCall(callee->llvm_fn, args);
}

void Compiler::compile_stmt(Function *fn, ast::Node *stmt) {
    switch (stmt->type) {
    case ast::NodeType::ReturnStmt: {
        auto &data = stmt->data.return_stmt;
        if (data.expr) {
            auto value = compile_expr(fn, data.expr);
            m_ctx->llvm_builder->CreateRet(value);
        } else {
            m_ctx->llvm_builder->CreateRetVoid();
        }
        break;
    }
    default:
        compile_expr(fn, stmt);
    }
}

void Compiler::compile_block(Function *fn, ast::Node *parent, ast::Node *block) {
    for (auto stmt : block->data.block.statements) {
        compile_stmt(fn, stmt);
    }
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
    case TypeKind::String: {
        return llvm::StructType::create(
            {llvm::Type::getInt8PtrTy(llvm_ctx), llvm::Type::getInt32Ty(llvm_ctx)}, "CxString");
    }
    case TypeKind::Fn: {
        auto &data = type->data.fn;
        auto ret_type = compile_type(data.return_type);
        std::vector<llvm::Type *> param_types = {};
        for (auto param : data.params) {
            param_types.push_back(compile_type(param));
        }
        return llvm::FunctionType::get(ret_type, param_types, data.is_variadic);
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

    auto &settings = m_ctx->settings;
    std::error_code ec;
    llvm::raw_fd_ostream dest_obj(settings.output_obj_to_file, ec, llvm::sys::fs::OF_None);
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

    if (!settings.output_ir_to_file.empty()) {
        llvm::raw_fd_ostream ir_dest(settings.output_ir_to_file, ec, llvm::sys::fs::OF_None);
        module->print(ir_dest, nullptr);
    }

    pass.run(*module);
    dest_obj.flush();
}

} // namespace codegen
} // namespace cx