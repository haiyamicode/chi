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
#include "runtime.h"
#include "util.h"

using namespace cx;
namespace fs = std::filesystem;

codegen::Compiler Builder::create_codegen_compiler() { return {m_codegen_ctx.get()}; }

Builder::Builder() : m_ctx(), m_codegen_ctx(nullptr) {
    m_codegen_ctx.reset(new codegen::CodegenContext(&m_ctx));
}

ast::Module *Builder::process_source(ast::Package *package, io::Buffer *src,
                                     const string &file_name) {
    auto parts = string_split(file_name, ".");
    auto module = m_ctx.module_from_path(package, file_name);

    Tokenization tokenization;
    Lexer lexer(src, &tokenization);
    lexer.tokenize();
    if (tokenization.error) {
        print("{}:{}:{}: error: {}\n", module->path, tokenization.error_pos.line + 1,
              tokenization.error_pos.col + 1, *tokenization.error);
        exit(0);
    }

    auto resolver = m_ctx.create_resolver();
    ScopeResolver scope_resolver(&resolver);
    module->scope = scope_resolver.get_scope();
    ParseContext pc;
    pc.resolver = &scope_resolver;
    pc.module = module;
    pc.allocator = &m_ctx;
    pc.debug_mode = debug_mode;
    pc.add_token_results(tokenization.tokens);

    Parser parser(&pc);
    parser.parse();

    if (build_mode == BuildMode::AST) {
        print_ast(module->root);
    }

    resolver.resolve(package);
    return module;
}

ast::Module *Builder::process_file(ast::Package *package, const string &file_name) {
    auto src = io::Buffer::from_file(file_name);
    return process_source(package, &src, file_name);
}

void Builder::build_runtime() {
    auto resolver = m_ctx.create_resolver();
    resolver.context_init_primitives();

    auto package = add_package();
    package->name = "__runtime";
    package->kind = PackageKind::BUILTIN;
    auto rt_source = io::Buffer::from_string(runtime::source);
    auto module = process_source(package, &rt_source, "runtime.xc");
    resolver.context_init_builtins(module);

    auto compiler = create_codegen_compiler();
    compiler.compile_module(module);
}

void Builder::build_single_file(ast::Package *package, const string &file_name) {
    auto module = process_file(package, file_name);
    auto compiler = create_codegen_compiler();
    auto settings = compiler.get_settings();
    settings->output_obj_to_file = get_tmp_file_path("main.o");
    settings->output_ir_to_file = get_tmp_file_path("main.ll");
    settings->lang_flags = module->get_lang_flags();
    compiler.compile_module(module);
    compiler.emit_output();

    // produce executable
    auto cmd = fmt::format("c++ {} -o {} -lchrt", settings->output_obj_to_file, output_file_name);
    if (debug_mode) {
        print("running: {}\n", cmd);
    }
    auto result = system(cmd.c_str());
    if (result != 0) {
        print("error: failed to run command: {}\n", cmd);
    }
}

void Builder::build_program(const string &entry_file_name) {
    build_runtime();

    if (!working_dir.empty()) {
        if (!fs::exists(working_dir)) {
            fs::create_directories(working_dir);
        }
    }
    auto package = add_package();
    package->name = "__main";
    build_single_file(package, entry_file_name);
}

string Builder::get_tmp_file_path(const string &filename) {
    auto dir = !working_dir.empty() ? fs::path(working_dir) : fs::temp_directory_path();
    return dir / filename;
}