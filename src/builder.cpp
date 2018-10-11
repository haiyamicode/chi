/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include <jit/jit-dump.h>

#include "builder.h"
#include "parser.h"
#include "ast_printer.h"
#include "jit.h"

using namespace cx;

BuildContext::BuildContext(cx::Allocator* allocator) :
        resolve_ctx(new ResolveContext(allocator)),
        jit_ctx(new jit::CompileContext(resolve_ctx.get())) {
}

jit::Compiler BuildContext::create_compiler() {
    return {jit_ctx.get()};
}

Builder::Builder() : m_ctx(this) {
    auto resolver = m_ctx.create_resolver();
    resolver.context_init_builtins();
    auto jitc = m_ctx.create_compiler();
    for (auto node: resolver.get_context()->builtins) {
        if (node->type == NodeType::FnDef) {
            jitc.compile_fn(node);
        }
    }
}

void Builder::process_file(ast::Package* package, const string& file_name) {
    auto src = io::Buffer::from_file(file_name);

    auto module = package->modules.emplace();
    module->package = package;
    module->path = file_name;

    Tokenization tokenization;
    Lexer lexer(&src, &tokenization);
    lexer.tokenize();

    auto resolver = m_ctx.create_resolver();
    ScopeResolver scope_resolver(&resolver);
    ParseContext pc;
    pc.resolver = &scope_resolver;
    pc.module = module;
    pc.tokens = &tokenization.tokens;
    pc.allocator = this;

    Parser parser(&pc);
    parser.parse();
//    print_ast(pc.module->root);

    resolver.resolve(package);
    auto jitc = m_ctx.create_compiler();
    jitc.compile(module);
    auto& main_fn = jitc.get_context()->functions[package->entry_fn];
    main_fn->apply(NULL, NULL);
}

void Builder::build_program(const string& entry_file_name) {
    process_file(add_package(), entry_file_name);
}

Node* Builder::create_node(NodeType type) {
    return m_ast_nodes.emplace(new Node(type))->get();
}

ChiType* Builder::create_type(TypeId type) {
    return m_types.emplace(new ChiType(type))->get();
}
