#pragma once

#include "ast.h"
#include "resolver.h"

namespace cx {
struct CompilationContext : public Allocator {
    box<ResolveContext> resolve_ctx;
    array<box<ast::Package>> packages = {};
    array<box<ast::Node>> ast_nodes = {};
    array<box<ChiType>> types = {};
    array<box<Scope>> scopes = {};
    array<box<Token>> tokens = {};

    CompilationContext() : resolve_ctx(new ResolveContext(this)) {}

    ast::Node *create_node(ast::NodeType type) {
        return ast_nodes.emplace(new ast::Node(type))->get();
    }

    ChiType *create_type(TypeKind kind) {
        return types.emplace(new ChiType(kind, types.size + 1))->get();
    }

    Scope *create_scope(Scope *parent) { return scopes.emplace(new Scope(parent))->get(); }

    ast::Package *add_package() { return packages.emplace(new ast::Package())->get(); }

    Resolver create_resolver() { return {resolve_ctx.get()}; }

    Token *create_token() { return tokens.emplace(new Token())->get(); }

    ast::Module *module_from_path(ast::Package *package, const string &path);
};
} // namespace cx