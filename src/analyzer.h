#pragma once

#include "context.h"
#include "parser.h"
#include "resolver.h"
#include "sema.h"

namespace cx {

struct ScanResult {
    ast::Module *module = nullptr;
    Pos pos;
    ast::Node *fn = nullptr;
    ast::Node *block = nullptr;
    Scope *scope = nullptr;
    ast::Node *stmt = nullptr;
    ast::Node *dot_expr = nullptr;
    bool is_dot = false;
    Token *token = nullptr;
    ast::Node *decl = nullptr;
};

class Analyzer {
    CompilationContext m_ctx;

  public:
    bool debug_mode = false;

    Analyzer();
    CompilationContext *get_context() { return &m_ctx; }
    Resolver get_resolver() { return m_ctx.create_resolver(); }

    ast::Package *add_package() { return m_ctx.add_package(); }

    ast::Module *process_source(ast::Package *package, io::Buffer *src, const string &file_name);
    ast::Module *process_file(ast::Package *package, const string &file_name);

    void build_runtime();
    ast::Module *analyze_package_file(ast::Package *package, const string &file_name);
    ast::Module *analyze_file(const string &entry_file_name);

    ScanResult scan(ast::Module *module, Pos cursor_pos);
    bool scan(ast::Node *node, Pos cursor_pos, ScanResult *result);
};

} // namespace cx
