/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "sema.h"
#include "resolver.h"
#include "parser.h"
#include "jit.h"

namespace cx {
    struct BuildContext {
        box<ResolveContext> resolve_ctx;
        box<jit::CompileContext> jit_ctx;

        BuildContext(Allocator* allocator);

        Resolver create_resolver() { return {resolve_ctx.get()}; }

        jit::Compiler create_compiler();
    };

    class Builder : Allocator {
        bool m_debug_mode = false;
        bool m_assembly_mode = false;
        array<ast::Package> m_packages;
        array<box<ast::Node>> m_ast_nodes;
        array<box<ChiType>> m_types;
        BuildContext m_ctx;

    public:
        Builder();

        ast::Package* add_package() { return m_packages.emplace(); }

        Node* create_node(NodeType type);

        ChiType* create_type(TypeId type_id);

        void set_debug_mode(bool value) { m_debug_mode = value; }

        void set_assembly_mode(bool value) { m_assembly_mode = value; }

        void process_file(ast::Package* package, const string& file_name);

        void build_program(const string& entry_file_name);
    };

}
