#pragma once

#include "parser.h"
#include "resolver.h"
#include "sema.h"

namespace cx {
struct AnalyzerContext {
    box<ResolveContext> resolve_ctx;
    AnalyzerContext(Allocator *allocator);
    Resolver create_resolver() { return {resolve_ctx.get()}; }
};

class Analyzer : Allocator {
    bool m_debug_mode = false;
    array<ast::Package> m_packages;
    array<box<ast::Node>> m_ast_nodes;
    array<box<ChiType>> m_types;
    AnalyzerContext m_ctx;

  public:
    Analyzer();

    ast::Package *add_package() { return m_packages.emplace(); }

    Node *create_node(NodeType type);

    ChiType *create_type(TypeKind kind);

    void set_debug_mode(bool value) { m_debug_mode = value; }

    ast::Module *process_source(ast::Package *package, io::Buffer *src, const string &file_name);
    ast::Module *process_file(ast::Package *package, const string &file_name);

    void build_runtime();
    ast::Module *analyze_package_file(ast::Package *package, const string &file_name);
    ast::Module *analyze_file(const string &entry_file_name);
};

} // namespace cx
