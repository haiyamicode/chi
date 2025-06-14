/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include <filesystem>

#include "analyzer.h"
#include "ast_printer.h"
#include "builder.h"
#include "parser.h"
#include "util.h"

using namespace cx;
namespace fs = std::filesystem;

codegen::Compiler Builder::create_codegen_compiler() { return {m_codegen_ctx.get()}; }

Builder::Builder() : m_ctx(), m_codegen_ctx(nullptr) {
    m_codegen_ctx.reset(new codegen::CodegenContext(&m_ctx));
}

ast::Module *Builder::process_source(ast::Package *package, io::Buffer *src,
                                     const string &file_name) {
    return m_ctx.process_source(package, src, file_name);
}

ast::Module *Builder::process_file(ast::Package *package, const string &file_name) {
    auto src = io::Buffer::from_file(file_name);
    return process_source(package, &src, file_name);
}

ast::Module *Builder::build_runtime() {
    auto resolver = m_ctx.create_resolver();
    resolver.context_init_primitives();

    auto rt_file_path = m_ctx.init_rt_stdlib();
    auto rt_source = io::Buffer::from_file(rt_file_path);
    auto module = process_source(m_ctx.rt_package, &rt_source, rt_file_path);
    resolver.context_init_builtins(module);
    return module;
}

void Builder::build_single_file(const string &file_name) {
    auto runtime_module = build_runtime();

    auto package = add_package(".");
    package->name = "__main";

    auto module = process_file(package, file_name);
    if (m_ctx.flags & FLAG_PRINT_AST) {
        return;
    }

    auto compiler = create_codegen_compiler();
    auto settings = compiler.get_settings();
    settings->output_obj_to_file = get_tmp_file_path("main.o");
    settings->output_ir_to_file = get_tmp_file_path("main.ll");
    settings->lang_flags = module->get_lang_flags();

    compiler.compile_module(runtime_module);
    compiler.compile_module(module);
    compiler.emit_output();

    // produce executable
    auto cmd =
        fmt::format("c++ {} -g -o {} -lchrt", settings->output_obj_to_file, output_file_name);

    if (debug_mode) {
        print("running: {}\n", cmd);
    }
    auto result = system(cmd.c_str());
    if (result != 0) {
        print("error: failed to run command: {}\n", cmd);
    }

#if __APPLE__
    cmd = fmt::format("dsymutil {} -o {}.dSYM", output_file_name, output_file_name);
    if (debug_mode) {
        print("running: {}\n", cmd);
    }
    result = system(cmd.c_str());
    if (result != 0) {
        print("error: failed to run command: {}\n", cmd);
    }
#endif
}

void Builder::build_program(const string &entry_file_name) {
    uint32_t flags = FLAG_EXIT_ON_ERROR;
    if (build_mode == BuildMode::AST) {
        flags |= FLAG_PRINT_AST;
    }
    m_ctx.flags = flags;

    if (!working_dir.empty()) {
        if (!fs::exists(working_dir)) {
            fs::create_directories(working_dir);
        }
    }

    build_single_file(entry_file_name);
}

string Builder::get_tmp_file_path(const string &filename) {
    auto dir = !working_dir.empty() ? fs::path(working_dir) : fs::temp_directory_path();
    return dir / filename;
}