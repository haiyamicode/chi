/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <jit/jit-plus.h>
#include <list>
#include <fmt/ostream.h>

#include "resolver.h"

namespace cx {
    namespace jit {
        struct CompileContext;

        struct VarLabel {
            jit_label label;
            ast::Node* var = nullptr;
        };

        struct LoopLabels {
            jit_label start;
            jit_label end;
        };

        typedef std::list<VarLabel> VarLabels;

        struct Function : public jit_function {
            string qualified_name;
            ast::Node* node;
            CompileContext* ctx;

            jit_value is_returning;
            jit_value return_value;
            std::list<VarLabels> return_labels; // state value
            std::list<LoopLabels> loop_labels;
            ChiTypeSubtype* container_subtype;

            Function(jit_type_t signature, CompileContext* _ctx, ast::Node* _node);

            virtual void build();

            void set_qualified_name(const string& container_name, const string& name);

            const char* get_jit_name() { return qualified_name.c_str(); }

            jit_value insn_call(Function* fn_ref, jit_value_t* args, long num_args);
            jit_value get_null_constant();

            jit_label* get_return_label() { return &return_labels.back().back().label; }
            VarLabels* push_return_scope() { return &return_labels.emplace_back(); }
            void pop_return_scope() { return_labels.pop_back(); }

            LoopLabels* push_loop() { return &loop_labels.emplace_back(); }
            void pop_loop() { loop_labels.pop_back(); }
            LoopLabels* get_loop() { return &loop_labels.back(); }
        };

        struct StructField {
            jit_type_t type;
            long offset;
        };

        struct DotValue {
            ChiType* ctn_type;
            jit_value ctn_value;
            optional<StructField> field;
            optional<ast::Node*> method;
            optional<jit_value> vtable_fn;
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
            ValueRef() {}
        };

        struct CompileSettings {
            bool enable_asm_print = false;
        };

        struct StructData {
            box<Function> constructor;
            box<Function> destructor;
        };

        struct Struct {
            ChiType* type;
            ChiTypeStruct* spec;
            StructData* data;
        };

        struct StringInternal {
            char* data;
            uint32_t size;
        };

        struct AnyInternal {
            ChiType* type;
            struct {
                void* a;
                void* b;
            } data;
            friend std::ostream&operator<<(std::ostream&os, const AnyInternal &v);
        };

        struct CompileContext {
            map<ast::Node*, box<Function>> functions;
            map<ast::Node*, jit_value> values;
            map<ChiType*, box<StructData>> structs;
            map<ChiType*, jit_type_t> types;
            jit_context jit_ctx;
            CompileSettings settings;
            Resolver resolver;
            CompileContext(ResolveContext* rctx): resolver(rctx) {}
        };

        class Compiler {
            CompileContext* m_ctx;
            Function* m_fn = nullptr;

            jit_context& get_jit_context() { return m_ctx->jit_ctx; }

            Resolver* get_resolver() { return &m_ctx->resolver; }

            ChiType* eval_type(ChiType* type);
            ChiType* get_chitype(ast::Node* node);

            jit_type_t _compile_type(ChiType* type);

            jit_type_t to_jit_int_type(ChiType* type);

            jit_type_t compile_type(ChiType* type);

            inline jit_type_t compile_type_of(ast::Node* node);

            Array compile_array_ref(Function* fn, ast::Node* expr);

            void compile_field_destroy(Function* fn, jit_value& arr, jit_nuint offset);

            jit_value compile_array_add(Function* fn, ast::Node* expr, ast::Node* value_arg);

            bool should_destroy(ast::Node* node);

            void compile_destruction(Function* fn, jit_value& address, ast::Node* node);

            DotValue compile_dot_expr(Function* fn, ast::Node* expr);

            void compile_array_construction(Function* fn, const jit_value& dest, ast::Node* expr);

            void compile_construction(Function* fn, jit_value_t dest, ChiType* type, ast::Node* expr);

            jit_value compile_string_alloc(Function* fn, const jit_value& data);

            void compile_string_construction(Function* fn, const jit_value& dest, optional<jit_value> data);

            Function* get_fn(ast::Node* node);

            Function* new_fn(jit_type_t signature, ast::Node* node);

            void fn_method(Function* fn, const string& name, ChiType* struct_type, ChiTypeSubtype* subtype);

            void add_value(ast::Node* node, const jit_value& value) { m_ctx->values[node] = value; }

            jit_value compile_simple_value(Function* fn, ast::Node* expr);

            jit_value compile_assignment_value(Function* fn, ast::Node* value, ast::Node* dest);

            jit_value compile_assignment_to_type(Function* fn, ast::Node* value, ChiType* dest_type);

            jit_value compile_conversion(Function* fn, const jit_value& value, ChiType* from_type, ChiType* to_type);

            ValueRef compile_value_ref(Function *fn, ast::Node *expr);

            void build_jump_table(TraitImpl* impl);

            jit_value compile_mem_alloc(Function* fn, const jit_value& size_value);

            ChiType* ptr_type_deref(ChiType* ptr_type) { return ptr_type->data.pointer.elem; }

            jit_value compile_string_concat(Function* fn, const jit_value& s1, const jit_value& s2);

        public:
            Compiler(CompileContext* ctx, Function* fn = nullptr);

            CompileContext* get_context() { return m_ctx; }

            jit_value create_string_const(Function* fn, const string& str);

            void compile_stmt(Function* fn, ast::Node* stmt);

            void compile_fn_body(Function* fn);

            Function* compile_fn(ast::Node* node);

            void compile(ast::Module* module);

            void compile_block(Function* fn, ast::Node* parent, ast::Node* block);

            void compile_struct(ast::Node* node);

            void _compile_struct(ast::Node* node, ChiType* struct_type);

            StructData* get_struct_data(ChiType* struct_type);

            Struct get_struct(ChiType* struct_type);
        };
    }
}