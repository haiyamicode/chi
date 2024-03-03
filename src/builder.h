/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "codegen.h"
#include "context.h"
#include "parser.h"
#include "resolver.h"
#include "sema.h"

namespace cx {
enum class BuildMode { Run, Executable, AST, Fuzz };

class Builder {
    box<codegen::CodegenContext> m_codegen_ctx;
    codegen::Compiler create_codegen_compiler();
    CompilationContext m_ctx;

  public:
    string output_file_name;
    string working_dir;
    bool debug_mode = false;
    BuildMode build_mode = BuildMode::Run;

    Builder();

    CompilationContext *get_context() { return &m_ctx; }

    ast::Package *add_package() { return m_ctx.add_package(); }

    ast::Module *process_source(ast::Package *package, io::Buffer *src, const string &file_name);
    ast::Module *process_file(ast::Package *package, const string &file_name);

    ast::Module *build_runtime();
    void build_single_file(const string &file_name);

    void build_program(const string &entry_file_name);

  private:
    string get_tmp_file_path(const string &filename);
};

} // namespace cx
