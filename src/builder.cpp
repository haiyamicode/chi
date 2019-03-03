/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include <jit/jit-dump.h>
#include <Zydis/DecoderTypes.h>

#include "builder.h"
#include "parser.h"
#include "ast_printer.h"
#include "jit.h"

using namespace cx;

BuildContext::BuildContext(cx::Allocator* allocator) :
        resolve_ctx(new ResolveContext(allocator)),
        jit_ctx(new jit::CompilationContext(resolve_ctx.get())) {
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
            jitc.add_fn_node(node);
        }
    }
}

void Builder::process_file(ast::Package* package, const string& file_name) {
    auto src = io::Buffer::from_file(file_name);
    auto parts = string_split(file_name, ".");
    auto kind = ModuleKind::CX;
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
    Lexer lexer(&src, &tokenization);
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

    Parser parser(&pc);
    parser.parse();
//    print_ast(pc.module->root);
    resolver.resolve(package);

    auto jitc = m_ctx.create_compiler();
    jitc.compile_internals();
    jitc.compile_module(module);
    auto entry_fn = jitc.get_context()->function_table[package->entry_fn];

    switch (m_build_mode) {
        case BuildMode::Run: {
            auto& main_fn = entry_fn;
            main_fn->apply(NULL, NULL);
            break;
        }
        case BuildMode::Executable: {
            break;
        }
    }
}


void Builder::build_program(const string& entry_file_name) {
    process_file(add_package(), entry_file_name);
}

Node* Builder::create_node(NodeType type) {
    return m_ast_nodes.emplace(new Node(type))->get();
}

ChiType* Builder::create_type(TypeKind kind) {
    return m_types.emplace(new ChiType(kind, m_types.size + 1))->get();
}

void Builder::set_build_mode(BuildMode value) {
    m_build_mode = value;
}

string Builder::get_tmp_file_path(const string& filename) {
#if JIT_WIN32_PLATFORM
    char *tmp_dir = getenv("TMP");
    if(!tmp_dir)
    {
        tmp_dir = getenv("TEMP");
        if(!tmp_dir)
        {
            tmp_dir = "c:/tmp";
        }
    }
    return fmt::format("{}/{}", tmp_dir, filename);
#else
    return fmt::format("/tmp/{}", filename);
#endif
}