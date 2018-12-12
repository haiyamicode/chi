/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <Zydis/Zydis.h>

#include "sema.h"
#include "resolver.h"
#include "parser.h"
#include "jit.h"

namespace cx {
    struct AotCompilation {
        map<int64_t, string> symbol_names;
    };

    struct AotFnInput {
        int32_t fid;
        array<ZyanU8>* instructions;
    };

    struct BuildContext {
        box<ResolveContext> resolve_ctx;
        box<jit::CompileContext> jit_ctx;

        BuildContext(Allocator* allocator);

        Resolver create_resolver() { return {resolve_ctx.get()}; }

        jit::Compiler create_compiler();
    };

    enum class BuildMode {
        Run,
        Executable
    };

    class Builder : Allocator {
        bool m_debug_mode = false;
        BuildMode m_build_mode = BuildMode::Run;
        string m_output_file_name;
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

        void set_assembly_mode(bool value) { m_ctx.jit_ctx->settings.enable_asm_print = value; }

        void set_build_mode(BuildMode value);

        void set_output_file_name(const string& value) { m_output_file_name = value; }

        void process_file(ast::Package* package, const string& file_name);

        void build_program(const string& entry_file_name);

        void generate_fn_asm(AotCompilation* ctx, AotFnInput* input, FILE* stream);

        string get_tmp_file_path(const string& filename);

        void build_binary(jit::Compiler* compiler);
    };

}
