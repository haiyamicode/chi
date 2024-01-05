#include "codegen.h"

namespace cx {
namespace codegen {

CompilationContext::~CompilationContext() {}

Compiler::Compiler(CompilationContext *ctx) : m_ctx(ctx) {}

void Compiler::compile_module(ast::Module *module) {
    auto &root = module->root->data.root;
    for (auto decl : root.top_level_decls) {
        if (decl->type == ast::NodeType::FnDef) {
            // add_fn_node(decl);
        } else if (decl->type == ast::NodeType::StructDecl) {
            // compile_struct(decl);
        }
    }
}

void Compiler::compile_internals() {
    auto rctx = m_ctx->resolver.get_context();
    for (auto method : rctx->internal_methods) {
        // add_internal_method_fn(method->resolved_type->data.fn.container, method);
    }
}

} // namespace codegen
} // namespace cx