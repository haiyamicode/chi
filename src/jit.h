/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <jit/jit-plus.h>
#include <list>

#include "resolver.h"

namespace cx {
    namespace jit {
        struct CompileContext;

        struct VarLabel {
            jit_label label;
            ast::Node* var = nullptr;
        };

        typedef std::list<VarLabel> VarLabels;

        struct Function : public jit_function {
            string qualified_name;
            ast::Node* node;
            CompileContext* ctx;

            jit_value is_returning;
            jit_value return_value;
            std::list<VarLabels> return_labels; // state value

            Function(jit_type_t signature, CompileContext* _ctx, ast::Node* _node);

            virtual void build();

            void set_qualified_name(ast::Node* container, const string& name);

            const char* get_jit_name() { return qualified_name.c_str(); }

            jit_value insn_call(Function* fn_ref, jit_value_t* args, long num_args);

//            jit_label* get_return_label(ast::Node* var) { return var ? return_labels.get(var) : &end; }
//            jit_label* get_return_label() { return get_return_label(current_var); }
            jit_label* get_return_label() { return &return_labels.back().back().label; }
            VarLabels* push_return_scope() { return &return_labels.emplace_back(); }
            void pop_return_scope() { return_labels.pop_back(); }
        };

        struct StructField {
            jit_type_t type;
            long offset;
        };

        struct DotValue {
            jit_value container;
            optional<StructField> field;
            DotValue(const jit_value& _container): container(_container) { }
        };

        struct Array {
            jit_value ptr;
            jit_value size;
            jit_value data;
            jit_type_t elem_type;
            jit_nint elem_size;
        };

        struct ValueRef {
            jit_value address;
            jit_type_t type;
            Function* fn_ref = nullptr;
            ValueRef(const jit_value& _address, jit_type_t _type): address(_address), type(_type) {}
        };

        struct CompileSettings {
            bool enable_asm_print = false;
        };

        struct DefaultMethods {
            box<Function> constructor;
            box<Function> destructor;
        };

        struct CompileContext {
            map<ast::Node*, box<Function>> functions;
            map<ast::Node*, DefaultMethods> defaults;
            map<ast::Node*, jit_value> values;
            map<ChiType*, jit_type_t> types;
            jit_context jit_ctx;
            CompileSettings settings;
            Resolver resolver;
            CompileContext(ResolveContext* rctx): resolver(rctx) {}
        };

        class Compiler {
            CompileContext* m_ctx;

            jit_context& get_jit_context() { return m_ctx->jit_ctx; }

            ChiType* node_get_type(ast::Node* node) { return node->resolved_type; }

            jit_type_t _to_jit_type(ChiType* type);

            jit_type_t to_jit_type(ChiType* type);

            inline jit_type_t build_jit_type(ast::Node* node);

            Array compile_array_ref(Function* fn, ast::Node* expr);

            void compile_array_destroy(Function *fn, jit_value &arr);

            jit_value compile_array_add(Function* fn, ast::Node* expr, ast::Node* value_arg);

            bool should_destroy(ast::Node* node);

            void compile_var_destroy(Function* fn, ast::Node* var, jit_value& address);

            DotValue compile_dot_expr(Function* fn, ast::Node* expr);

            void compile_construction(Function* fn, jit_value_t dest, ChiType* struct_type, ast::Node* expr);

            Function* get_fn(ast::Node* node);
            Function* new_fn(jit_type_t signature, ast::Node* node);

            void add_value(ast::Node* node, const jit_value& value) { m_ctx->values[node] = value; }

            jit_value compile_simple_value(Function* fn, ast::Node* expr);

            ValueRef compile_value_ref(Function *fn, ast::Node *expr);

        public:
            Compiler(CompileContext* ctx);

            CompileContext* get_context() { return m_ctx; }

            jit_value create_string_const(Function* fn, const string& str);

            void compile_stmt(Function* fn, ast::Node* stmt);

            void compile_fn_body(Function* fn);

            void compile_fn(ast::Node* node);

            void compile(ast::Module* module);

            void compile_block(Function* fn, ast::Node* parent, ast::Node* block);

            void compile_struct(ast::Node* node);
        };
    }
}