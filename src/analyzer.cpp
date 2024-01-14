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

AnalyzerContext::AnalyzerContext(cx::Allocator *allocator)
    : resolve_ctx(new ResolveContext(allocator)) {}

Analyzer::Analyzer() : m_ctx(this) {}

ast::Module *Analyzer::process_source(ast::Package *package, io::Buffer *src,
                                      const string &file_name) {
    auto parts = string_split(file_name, ".");
    auto kind = ModuleKind::XC;
    if (!parts.empty()) {
        auto ext = parts[parts.size() - 1];
        if (ext == "h") {
            kind = ModuleKind::HEADER;
        }
    }

    auto module = package->modules.emplace();
    module->package = package;
    module->path = file_name;
    module->kind = kind;

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
    ParseContext pc;
    pc.resolver = &scope_resolver;
    pc.module = module;
    pc.tokens = &tokenization.tokens;
    pc.allocator = this;
    pc.debug_mode = m_debug_mode;
    pc.error_handler = [module](Error error) { module->errors.add(error); };

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

    auto package = add_package();
    package->kind = PackageKind::BUILTIN;
    auto rt_source = io::Buffer::from_string(runtime::source);
    auto module = process_source(package, &rt_source, "runtime.chi");
    resolver.context_init_builtins(module);
}

ast::Module *Analyzer::analyze_package_file(ast::Package *package, const string &file_name) {
    build_runtime();
    return process_file(package, file_name);
}

ast::Module *Analyzer::analyze_file(const string &entry_file_name) {
    return analyze_package_file(add_package(), entry_file_name);
}

Node *Analyzer::create_node(NodeType type) { return m_ast_nodes.emplace(new Node(type))->get(); }

ChiType *Analyzer::create_type(TypeKind kind) {
    return m_types.emplace(new ChiType(kind, m_types.size + 1))->get();
}