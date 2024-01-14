/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "codegen.h"
#include "parser.h"
#include "resolver.h"
#include "sema.h"

namespace cx {
struct BuildContext {
    box<ResolveContext> resolve_ctx;
    box<codegen::CompilationContext> codegen_ctx;

    BuildContext(Allocator *allocator);

    Resolver create_resolver() { return {resolve_ctx.get()}; }

    codegen::Compiler create_compiler();
};

enum class BuildMode { Run, Executable, AST, Fuzz };

class Builder : Allocator {
    bool m_debug_mode = false;
    BuildMode m_build_mode = BuildMode::Run;
    string m_output_file_name;
    string m_working_dir;
    array<ast::Package> m_packages;
    array<box<ast::Node>> m_ast_nodes;
    array<box<ChiType>> m_types;
    BuildContext m_ctx;

  public:
    Builder();

    ast::Package *add_package() { return m_packages.emplace(); }

    Node *create_node(NodeType type);

    ChiType *create_type(TypeKind kind);

    void set_debug_mode(bool value) { m_debug_mode = value; }

    void set_assembly_mode(bool value) {}

    void set_build_mode(BuildMode value);

    void set_output_file_name(const string &value) { m_output_file_name = value; }
    void set_working_dir(const string &value) { m_working_dir = value; }

    ast::Module *process_source(ast::Package *package, io::Buffer *src, const string &file_name);
    ast::Module *process_file(ast::Package *package, const string &file_name);

    void build_runtime();
    void build_single_file(ast::Package *package, const string &file_name);

    void build_program(const string &entry_file_name);

  private:
    string get_tmp_file_path(const string &filename);
};

} // namespace cx
