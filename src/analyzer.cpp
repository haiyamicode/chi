/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */
#include <filesystem>

#include "analyzer.h"
#include "ast_printer.h"
#include "parser.h"
#include "util.h"

using namespace cx;
namespace fs = std::filesystem;

Analyzer::Analyzer() {}

ast::Module *Analyzer::process_source(ast::Package *package, io::Buffer *src,
                                      const string &file_name) {
    return m_ctx.process_source(package, src, file_name);
}

ast::Module *Analyzer::process_file(ast::Package *package, const string &file_name) {
    auto src = io::Buffer::from_file(file_name);
    return process_source(package, &src, file_name);
}

void Analyzer::build_runtime() {
    auto resolver = m_ctx.create_resolver();
    resolver.context_init_primitives();

    auto package = m_ctx.add_package();
    package->kind = PackageKind::BUILTIN;
    auto rt_path = m_ctx.get_stdlib_path("runtime.xc");
    auto rt_source = io::Buffer::from_file(rt_path);
    auto module = process_source(package, &rt_source, rt_path);
    resolver.context_init_builtins(module);
}

ast::Module *Analyzer::analyze_package_file(ast::Package *package, const string &file_name) {
    build_runtime();
    return process_file(package, file_name);
}

ast::Module *Analyzer::analyze_file(const string &entry_file_name) {
    return analyze_package_file(m_ctx.add_package(), entry_file_name);
}