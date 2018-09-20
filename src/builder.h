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

namespace cx {
    class Builder : Allocator {
        bool m_debug_mode = false;
        bool m_assembly_mode = false;
        Resolver m_resolver;
        array<ast::Package> m_packages;
        array<box<ast::Node>> m_ast_nodes;
        array<box<ChiType>> m_types;

    public:
        Builder();

        ast::Package* add_package() { return m_packages.emplace(); }

        Node* create_node(NodeType type);

        ChiType* create_type(TypeId type_id);

        void set_debug_mode(bool value) { m_debug_mode = value; }

        void set_assembly_mode(bool value) { m_assembly_mode = value; }

        void compile(ast::Module* file);

        void process_file(ast::Package* package, const string& file_name);

        void build_program(const string& entry_file_name);
    };

}
