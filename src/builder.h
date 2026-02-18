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
    CompilationContext m_ctx;
    box<codegen::CodegenContext> m_codegen_ctx;
    codegen::Compiler create_codegen_compiler();

  public:
    string output_file_name;
    string working_dir;
    bool debug_mode = false;
    bool safe_mode = false;
    BuildMode build_mode = BuildMode::Run;

    Builder();
    Builder(const Builder &) = delete;
    Builder &operator=(const Builder &) = delete;

    CompilationContext *get_context() { return &m_ctx; }

    ast::Package *add_package(string id_path) { return m_ctx.add_package(id_path); }

    ast::Module *process_source(ast::Package *package, io::Buffer *src, const string &file_name);
    ast::Module *process_file(ast::Package *package, const string &file_name);

    ast::Module *build_runtime();
    void build_single_file(const string &file_name);

    void build_program(const string &entry_file_name);

    void build_package(const string &entry_file_name);

  private:
    string get_tmp_file_path(const string &filename);
};

} // namespace cx
