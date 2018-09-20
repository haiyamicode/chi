/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "builder.h"
#include "parser.h"
#include "ast_printer.h"

using namespace cx;

Builder::Builder() : m_resolver(this) {}

void Builder::compile(ast::Module* file) {

}

void Builder::process_file(ast::Package* package, const string& file_name) {
    auto src = io::Buffer::from_file(file_name);

    auto module = package->modules.emplace();
    module->package = package;
    module->path = file_name;

    Tokenization tokenization;
    Lexer lexer(&src, &tokenization);
    lexer.tokenize();

    ModuleResolver mr(&m_resolver);
    ParseContext pc;
    pc.resolver = &mr;
    pc.module = module;
    pc.tokens = &tokenization.tokens;
    pc.allocator = this;

    Parser parser(&pc);
    parser.parse();
    print_ast(pc.module->root);

    m_resolver.resolve(package);
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
