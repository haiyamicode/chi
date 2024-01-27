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
#include "runtime.h"
#include "util.h"

using namespace cx;
namespace fs = std::filesystem;

Analyzer::Analyzer() {}

ast::Module *Analyzer::process_source(ast::Package *package, io::Buffer *src,
                                      const string &file_name) {
    auto module = m_ctx.module_from_path(package, file_name);

    Tokenization tokenization;
    Lexer lexer(src, &tokenization);
    lexer.tokenize();
    if (tokenization.error) {
        module->errors.add({*tokenization.error, tokenization.error_pos});
        return module;
    }

    auto resolver = m_ctx.create_resolver();
    resolver.get_context()->error_handler = [module](Error error) { module->errors.add(error); };

    ScopeResolver scope_resolver(&resolver);
    module->scope = scope_resolver.get_scope();
    ParseContext pc;
    pc.resolver = &scope_resolver;
    pc.module = module;
    pc.allocator = &m_ctx;
    pc.debug_mode = debug_mode;
    pc.error_handler = [module](Error error) { module->errors.add(error); };
    pc.add_token_results(tokenization.tokens);

    Parser parser(&pc);
    parser.parse();

    if (module->errors.size > 0) {
        return module;
    }
    resolver.resolve(package);
    return module;
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
    auto rt_source = io::Buffer::from_string(runtime::source);
    auto module = process_source(package, &rt_source, "__chiroot/runtime.xc");
    resolver.context_init_builtins(module);
}

ast::Module *Analyzer::analyze_package_file(ast::Package *package, const string &file_name) {
    build_runtime();
    return process_file(package, file_name);
}

ast::Module *Analyzer::analyze_file(const string &entry_file_name) {
    return analyze_package_file(m_ctx.add_package(), entry_file_name);
}