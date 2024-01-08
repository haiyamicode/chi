/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include <filesystem>

#include "ast_printer.h"
#include "builder.h"
#include "parser.h"
#include "runtime.h"
#include "util.h"

using namespace cx;
namespace fs = std::filesystem;

BuildContext::BuildContext(cx::Allocator *allocator)
    : resolve_ctx(new ResolveContext(allocator)),
      codegen_ctx(new codegen::CompilationContext(resolve_ctx.get())) {}

codegen::Compiler BuildContext::create_compiler() { return {codegen_ctx.get()}; }

Builder::Builder() : m_ctx(this) {}

ast::Module *Builder::process_source(ast::Package *package, io::Buffer *src,
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
        print("{}:{}:{}: error: {}\n", module->path, tokenization.error_pos.line + 1,
              tokenization.error_pos.col + 1, *tokenization.error);
        exit(0);
    }

    auto resolver = m_ctx.create_resolver();
    ScopeResolver scope_resolver(&resolver);
    ParseContext pc;
    pc.resolver = &scope_resolver;
    pc.module = module;
    pc.tokens = &tokenization.tokens;
    pc.allocator = this;
    pc.debug_mode = m_debug_mode;

    Parser parser(&pc);
    parser.parse();

    if (m_build_mode == BuildMode::AST) {
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
    package->kind = PackageKind::BUILTIN;
    auto rt_source = io::Buffer::from_string(runtime::source);
    auto module = process_source(package, &rt_source, "runtime.chi");
    auto compiler = m_ctx.create_compiler();
    compiler.compile_module(module);
    resolver.context_init_builtins(module);
}

void Builder::build_single_file(ast::Package *package, const string &file_name) {
    build_runtime();
    auto module = process_file(package, file_name);
    auto compiler = m_ctx.create_compiler();
    compiler.compile_module(module);
    auto &settings = compiler.get_context()->settings;
    settings.output_obj_to_file = get_tmp_file_path("main.o");
    settings.output_ir_to_file = get_tmp_file_path("main.ir");
    compiler.emit_output();

    switch (m_build_mode) {
    case BuildMode::Run: {
        break;
    }
    default:
        break;
    }
}

void Builder::build_program(const string &entry_file_name) {
    if (!m_working_dir.empty()) {
        if (!fs::exists(m_working_dir)) {
            fs::create_directories(m_working_dir);
        }
    }
    build_single_file(add_package(), entry_file_name);
}

Node *Builder::create_node(NodeType type) { return m_ast_nodes.emplace(new Node(type))->get(); }

ChiType *Builder::create_type(TypeKind kind) {
    return m_types.emplace(new ChiType(kind, m_types.size + 1))->get();
}

void Builder::set_build_mode(BuildMode value) { m_build_mode = value; }

string Builder::get_tmp_file_path(const string &filename) {
    auto dir = !m_working_dir.empty() ? fs::path(m_working_dir) : fs::temp_directory_path();
    return dir / filename;
}