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

Builder::Builder() {
    m_allocated.reserve(1024);
}

void Builder::compile(ast::Module* file) {

}

void Builder::process_file(ast::Package* package, string file_name) {
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
    pc.alloc = std::bind(&Builder::alloc_mem, this, std::placeholders::_1);

    Parser parser(&pc);
    parser.parse();
    print_ast(pc.module->root);
}

void Builder::build_program(string entry_file_name) {
    process_file(add_package(), entry_file_name);
}

void* Builder::alloc_mem(size_t size) {
    return m_allocated.add(malloc(size));
}