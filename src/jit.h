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
        struct CompilationContext;

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
            optional<string> asm_name;
            ast::Node* node;
            CompilationContext* ctx;

            jit_value is_returning;
            jit_value return_value;
            std::list<VarLabels> return_labels; // state value
            std::list<LoopLabels> loop_labels;
            ChiTypeSubtype* container_subtype;

            Function(jit_type_t signature, CompilationContext* _ctx, ast::Node* _node);

            virtual void build();

            const char* get_jit_name() const { return qualified_name.c_str(); }

            jit_value get_null_constant();

            jit_label* get_return_label() { return &return_labels.back().back().label; }
            VarLabels* push_return_scope() { return &return_labels.emplace_back(); }
            void pop_return_scope() { return_labels.pop_back(); }

            LoopLabels* push_loop() { return &loop_labels.emplace_back(); }
            void pop_loop() { loop_labels.pop_back(); }
            LoopLabels* get_loop() { return &loop_labels.back(); }

            jit_value insn_call_native(const char *name, void *native_func, jit_type_t signature,
                                       jit_value_t *args, unsigned int num_args, int flags=0);

            jit_value insn_call(Function* fn_ref, jit_value_t* args, long num_args);
        };

        struct StructField {
            jit_type_t type;
            long offset;
        };

        struct DotValue {
            ChiType* ctn_type;
            jit_value ctn_address;
            optional<StructField> field;
            optional<ast::Node*> method;
            optional<jit_value> vtable_fn;
        };

        struct Array {
            ChiType* array_type;
            ChiType* elem_type;
            jit_value ptr;
            jit_value size;
            jit_value data;
            jit_type_t elem;
            jit_nuint elem_size;
        };

        struct ValueRef {
            jit_value address;
            jit_type_t type;
            Function* fn_ref = nullptr;
            ValueRef(const jit_value& _address, jit_type_t _type): address(_address), type(_type) {}
            ValueRef() {}
        };

        struct WrappedType {
            jit_type_t type;
            jit_type_t elem;
            jit_nuint elem_size;

            jit_nuint get_opt_flag_offset() { return elem_size; }
        };

        struct CompilationSettings {
            bool enable_jit_dump = false;
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

        struct ImplInfo {
            TypeInfo* type;
            void** vtable;
            int32_t vtable_size;
        };

        typedef array<void*> JumpTable;

        struct CompilationContext {
            jit_context jit_ctx;
            CompilationSettings settings;
            Resolver resolver;

            array<box<Function>> functions;
            array<box<JumpTable>> jump_tables;
            array<box<ImplInfo>> impls;

            map<ast::Node*, jit_value> value_table;
            map<ChiType*, box<StructData>> struct_table;
            map<TypeId, jit_type_t> type_table;
            map<TypeId, box<TypeInfo>> info_table;
            map<ast::Node*, Function*> function_table;

            CompilationContext(ResolveContext* rctx): resolver(rctx) {}

            Function* add_fn(ast::Node* node, Function* fn);
        };

        enum ArrayOp { Destroy, Copy };

        class Compiler {
            CompilationContext* m_ctx;
            Function* m_fn = nullptr;

            jit_context& get_jit_context() { return m_ctx->jit_ctx; }

            Resolver* get_resolver() { return &m_ctx->resolver; }

            SystemTypes* get_system_types() { return get_resolver()->get_system_types(); }

            ChiType* eval_type(ChiType* type);
            ChiType* get_chitype(ast::Node* node);

            jit_type_t _compile_type(ChiType* type);

            jit_type_t to_jit_int_type(ChiType* type);

            WrappedType compile_wrapped_type(ChiType* type);

            inline jit_type_t compile_type_of(ast::Node* node);

            Array compile_array_ref(Function* fn, ast::Node* expr);

            void compile_field_mem_free(Function* fn, jit_value& arr, jit_nuint offset);

            jit_value compile_array_add(Function* fn, const jit_value& dest, jit_uint elem_size, const jit_value& value);

            jit_value compile_array_loop(Function* fn, jit_value& address, ChiType* type, ArrayOp op);

            bool should_destroy(ast::Node* node);

            bool should_destroy_for_type(ChiType* type);

            void compile_destruction_for_type(Function* fn, jit_value& address, ChiType* type);

            void compile_destruction(Function* fn, jit_value& address, ast::Node* node);

            DotValue compile_dot_expr(Function* fn, ast::Node* expr);

            void compile_array_construction(Function* fn, const jit_value& dest);

            void compile_array_reserve(Function* fn, const jit_value& dest, jit_uint elem_size, const jit_value& size);

            void compile_construction(Function* fn, jit_value_t dest, ChiType* type, ast::Node* expr);

            jit_value compile_string_alloc(Function* fn, const jit_value& data);

            void compile_string_construction(Function* fn, const jit_value& dest, optional<jit_value> data);

            Function* get_fn(ast::Node* node);

            Function* add_internal_method_fn(ChiType* type, ast::Node* method);

            Function* new_fn(jit_type_t signature, ast::Node* node);

            Function* add_fn(jit_type_t signature, ast::Node* node);

            JumpTable* add_jump_table(long* table_index);

            ImplInfo* get_impl_info(TraitImpl* impl);

            void init_method_fn(Function* fn, const string& fn_name, ChiType* struct_type, ChiTypeSubtype* subtype);

            void add_value(ast::Node* node, const jit_value& value) { m_ctx->value_table[node] = value; }

            jit_value compile_simple_value(Function* fn, ast::Node* expr);

            jit_value compile_arithmetic_op(Function* fn, ChiType* value_type, TokenType op_type, const jit_value& op1, const jit_value& op2);

            jit_value compile_assignment_value(Function* fn, ast::Node* expr, ast::Node* dest);

            jit_value compile_assignment_to_type(Function* fn, ast::Node* expr, ChiType* dest_type);

            jit_value compile_conversion(Function* fn, const jit_value& value, ChiType* from_type, ChiType* to_type);

            jit_value compile_value_copy(Function* fn, const jit_value& value, ChiType* type);

            ValueRef compile_value_ref(Function *fn, ast::Node *expr);

            jit_value compile_constant_value(Function *fn, const ConstantValue& value, ChiType* type);

            jit_value compile_mem_alloc(Function* fn, const jit_value& size_value);

            jit_value compile_string_concat(Function* fn, const jit_value& s1, const jit_value& s2);

            void compile_panic(Function* fn, const string& message);

            void _compile_struct(ast::Node* node, ChiType* struct_type);

            StructData* get_struct_data(ChiType* struct_type);

            jit_value create_string_constant(Function* fn, const string& str);

            void compile_stmt(Function* fn, ast::Node* stmt);

            void compile_block(Function* fn, ast::Node* parent, ast::Node* block);

            void compile_struct(ast::Node* node);

            Struct get_struct(ChiType* struct_type);

        public:
            Compiler(CompilationContext* ctx, Function* fn = nullptr);

            CompilationContext* get_context() { return m_ctx; }

            TypeInfo* get_type_info(ChiType* type);

            void compile_internals();

            void compile_module(ast::Module* module);

            Function* add_fn_node(ast::Node* node);

            void compile_fn_body(Function* fn);

            jit_type_t compile_type(ChiType* type);
        };
    }
}