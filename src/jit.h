/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <jit/jit-plus.h>

#include "resolver.h"

namespace cx {
    namespace jit {
        class Compiler;

        struct Function : public jit_function {
            Compiler* compiler;
            ast::Node* node;

            Function(jit_context& context, jit_type_t signature, Compiler* compiler, ast::Node* node);

            virtual void build();
        };

        struct DotField {
            jit_type_t value_type;
            long offset;
            ChiStructField* field;
            jit_value struct_ptr;
        };

        struct CompileSettings {
            bool enable_asm_print = false;
        };

        struct CompileContext {
            map<ast::Node*, box<Function>> functions;
            map<ast::Node*, jit_value> values;
            map<ChiType*, jit_type_t> types;
            jit_context jit_ctx;
            CompileSettings settings;
        };

        class Compiler {
            Resolver m_resolver;
            CompileContext* m_ctx;

            jit_context& get_jit_context() { return m_ctx->jit_ctx; }

            ChiType* node_get_type(ast::Node* node) { return m_resolver.node_get_type(node); }

            jit_type_t _to_jit_type(ChiType* type);

            jit_type_t to_jit_type(ChiType* type);

            inline jit_type_t build_jit_type(ast::Node* node);

            DotField compile_dot_expr(jit::Function* fn, ast::Node* node);

            jit::Function* get_fn(ast::Node* node);

            void add_value(ast::Node* node, jit_value value) { m_ctx->values[node] = value; }

        public:
            Compiler(CompileContext* compile_ctx, ResolveContext* resolve_ctx);

            CompileContext* get_context() { return m_ctx; }

            jit_value create_string_const(jit::Function* fn, const string& str);

            void compile_stmt(jit::Function* fn, ast::Node* stmt);

            void compile_fn_body(jit::Function* fn);

            void compile_fn(ast::Node* node);

            void compile(ast::Module* module);

            jit_value compile_expr_value(jit::Function* fn, ast::Node* expr);

            void compile_block(jit::Function* fn, ast::Node* node);

            void compile_struct(ast::Node* node);
        };
    }
}