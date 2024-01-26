#pragma once

#include "context.h"
#include "parser.h"
#include "resolver.h"
#include "sema.h"

namespace cx {

class Analyzer {
    CompilationContext m_ctx;

  public:
    bool debug_mode = false;

    Analyzer();
    CompilationContext *get_context() { return &m_ctx; }

    ast::Package *add_package() { return m_ctx.add_package(); }

    ast::Module *process_source(ast::Package *package, io::Buffer *src, const string &file_name);
    ast::Module *process_file(ast::Package *package, const string &file_name);

    void build_runtime();
    ast::Module *analyze_package_file(ast::Package *package, const string &file_name);
    ast::Module *analyze_file(const string &entry_file_name);
};

} // namespace cx
