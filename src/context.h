#pragma once

#include "ast.h"
#include "resolver.h"

namespace cx {

enum ProcessingFlags : uint32_t {
    FLAG_NONE = 0,
    FLAG_PRINT_AST = 1 << 0,
    FLAG_EXIT_ON_ERROR = 1 << 1,
    FLAG_SAVE_TOKENS = 1 << 2
};

struct CompilationContext : public Context {
    CompilationContext(const CompilationContext &) = delete;
    CompilationContext &operator=(const CompilationContext &) = delete;

    array<box<Token>> tokens = {};
    array<box<ast::Node>> ast_nodes = {};
    array<box<ChiType>> types = {};
    array<box<Scope>> scopes = {};
    array<box<ast::DeclSpec>> decl_specs = {};
    array<box<ChiStructMember>> struct_members = {};
    array<box<InterfaceImpl>> interface_impls = {};
    uint32_t flags = 0;
    array<string> file_extensions = {"xc", "x"};
    string root_path = "";
    array<box<ast::Package>> packages = {};
    ResolveContext resolve_ctx;
    map<string, ast::Module *> module_map = {};

    explicit CompilationContext() : resolve_ctx(this) {
        auto rootenv = std::getenv("CHI_ROOT");
        if (rootenv) {
            root_path = rootenv;
        }
    }

    ast::Node *create_node(ast::NodeType type) {
        auto node = ast_nodes.emplace(new ast::Node(type))->get();
        node->id = ast_nodes.len;
        return node;
    }

    ast::DeclSpec *create_decl_spec() { return decl_specs.emplace(new ast::DeclSpec())->get(); }

    ChiType *create_type(TypeKind kind) {
        return types.emplace(new ChiType(kind, types.len + 1))->get();
    }

    Scope *create_scope(Scope *parent) { return scopes.emplace(new Scope(parent))->get(); }

    ast::Package *add_package() { return packages.emplace(new ast::Package())->get(); }

    Resolver create_resolver() { return {&resolve_ctx}; }

    Token *create_token() { return tokens.emplace(new Token())->get(); }

    ast::Module *module_from_path(ast::Package *package, const string &path, bool import = false);

    optional<ModulePathInfo> find_module_path(const string &path, const string &base_path = "");

    ast::Module *process_source(ast::Package *package, io::Buffer *src, const string &file_name);

    string get_stdlib_path(string path) {
        return fs::path(root_path) / fs::path("src") / fs::path("stdlib") / path;
    }

    ChiStructMember *create_struct_member() {
        return struct_members.emplace(new ChiStructMember())->get();
    }

    InterfaceImpl *create_interface_impl() {
        return interface_impls.emplace(new InterfaceImpl())->get();
    }
};
} // namespace cx