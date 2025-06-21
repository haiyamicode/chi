#pragma once

#include "ast.h"
#include "resolver.h"
#include "sema.h"

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
    array<box<ChiEnumMember>> enum_members = {};
    array<box<InterfaceImpl>> interface_impls = {};
    uint32_t flags = 0;
    array<string> file_extensions = {"xc", "x"};
    string root_path = "";
    array<box<ast::Package>> packages = {};
    map<string, ast::Package *> package_map = {};
    ResolveContext resolve_ctx;
    map<string, ast::Module *> module_map = {};

    // default packages
    ast::Package *stdlib_package = nullptr;
    ast::Package *rt_package = nullptr;

    explicit CompilationContext();

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

    ast::Package *add_package(string id_path) {
        auto package = packages.emplace(new ast::Package())->get();
        package->id_path = id_path;
        package_map[id_path] = package;
        return package;
    }

    string init_rt_stdlib();

    Resolver create_resolver() { return {&resolve_ctx}; }

    Token *create_token() { return tokens.emplace(new Token())->get(); }

    ast::Module *module_from_path(ast::Package *package, const string &path, bool import = false);

    optional<ModulePathInfo> find_module_path(const string &path, const string &base_path = "");

    ast::Package *get_or_create_package(const string &id_path) {
        auto p = package_map.get(id_path);
        if (p) {
            return *p;
        }
        return add_package(id_path);
    }

    std::pair<string, string> parse_import_path(const string &path);

    optional<ModulePathInfo> find_module_at_path(const string &path, const string &package_path);

    ast::Module *process_source(ast::Package *package, io::Buffer *src, const string &file_name);

    string get_stdlib_path(string path) {
        return fs::path(root_path) / fs::path("src") / fs::path("stdlib") / path;
    }

    ChiStructMember *create_struct_member() {
        return struct_members.emplace(new ChiStructMember())->get();
    }

    ChiEnumMember *create_enum_member() { return enum_members.emplace(new ChiEnumMember())->get(); }

    InterfaceImpl *create_interface_impl() {
        return interface_impls.emplace(new InterfaceImpl())->get();
    }
};
} // namespace cx