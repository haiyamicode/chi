/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <fmt/ostream.h>
#include <list>

#include "internals.h"
#include "resolver.h"

namespace cx {
namespace codegen {
struct CompilationContext;
typedef void *unknown_t;

struct VarLabel {
    unknown_t label;
    ast::Node *var = nullptr;
};

struct LoopLabels {
    unknown_t start;
    unknown_t end;
};

typedef std::list<VarLabel> VarLabels;

struct Function {
    string qualified_name;
    ast::Node *node;
    CompilationContext *ctx;

    unknown_t is_returning;
    unknown_t return_value;
    std::list<VarLabels> return_labels; // state value
    std::list<LoopLabels> loop_labels;
    ChiTypeSubtype *container_subtype;

    Function(unknown_t signature, CompilationContext *_ctx, ast::Node *_node);
    virtual ~Function() {}

    virtual void build();

    const char *get_jit_name() const { return qualified_name.c_str(); }

    unknown_t get_null_constant();

    unknown_t *get_return_label() { return &return_labels.back().back().label; }
    VarLabels *push_return_scope() { return &return_labels.emplace_back(); }
    void pop_return_scope() { return_labels.pop_back(); }

    LoopLabels *push_loop() { return &loop_labels.emplace_back(); }
    void pop_loop() { loop_labels.pop_back(); }
    LoopLabels *get_loop() { return &loop_labels.back(); }
};

struct StructField {
    unknown_t type;
    long offset;
};

struct DotValue {
    ChiType *ctn_type;
    unknown_t ctn_address;
    optional<StructField> field;
    optional<ast::Node *> method;
    optional<unknown_t> vtable_fn;
};

struct Array {
    ChiType *array_type;
    ChiType *elem_type;
    unknown_t ptr;
    unknown_t size;
    unknown_t data;
    unknown_t elem;
    unknown_t elem_size;
};

struct ValueRef {
    unknown_t address;
    unknown_t type;
    Function *fn_ref = nullptr;
    ValueRef(const unknown_t &_address, unknown_t _type) : address(_address), type(_type) {}
    ValueRef() {}
};

struct WrappedType {
    unknown_t type;
    unknown_t elem;
    unknown_t elem_size;

    unknown_t get_opt_flag_offset() { return elem_size; }
};

struct CompilationSettings {
    bool enable_jit_dump = false;
};

struct StructData {
    Function *constructor;
    Function *destructor;
};

struct Struct {
    ChiType *type;
    ChiTypeStruct *spec;
    StructData *data;
};

struct ImplInfo {
    TypeInfo *type;
    void **vtable;
    int32_t vtable_size;
};

typedef array<void *> JumpTable;

struct CompilationContext {
    unknown_t jit_ctx;
    CompilationSettings settings;
    Resolver resolver;

    array<box<Function>> functions;
    array<box<JumpTable>> jump_tables;
    array<box<ImplInfo>> impls;
    array<unknown_t> types;
    array<box<string>> strings;

    map<ast::Node *, unknown_t> value_table;
    map<ChiType *, box<StructData>> struct_table;
    map<TypeId, unknown_t> type_table;
    map<TypeId, box<TypeInfo>> info_table;
    map<ast::Node *, Function *> function_table;

    CompilationContext(ResolveContext *rctx) : resolver(rctx) {}
    ~CompilationContext();

    Function *add_fn(ast::Node *node, Function *fn);
};

enum ArrayOp { Destroy, Copy };
enum ArrayFlag { None, Static };

class Compiler {
    CompilationContext *m_ctx;
    Function *m_fn = nullptr;

    unknown_t &get_unknown_t() { return m_ctx->jit_ctx; }

    unknown_t add_type(unknown_t type) { return *m_ctx->types.add(type); }
    const char *add_string(const string &str) {
        return m_ctx->strings.emplace(new string(str))->get()->c_str();
    }

    Resolver *get_resolver() { return &m_ctx->resolver; }

    SystemTypes *get_system_types() { return get_resolver()->get_system_types(); }

    ChiType *eval_type(ChiType *type);
    ChiType *get_chitype(ast::Node *node);

    unknown_t _compile_type(ChiType *type);

    unknown_t convert_int_type(ChiType *type);

    WrappedType compile_wrapped_type(ChiType *type);

    inline unknown_t compile_type_of(ast::Node *node);

    Array compile_array_ref(Function *fn, ast::Node *expr);

    void compile_field_mem_free(Function *fn, unknown_t &arr, unknown_t offset);

    unknown_t compile_array_add(Function *fn, const unknown_t &dest, unknown_t elem_size,
                                const unknown_t &value);

    unknown_t compile_array_loop(Function *fn, unknown_t &address, ChiType *type, ArrayOp op);

    bool should_destroy(ast::Node *node);

    bool should_destroy_for_type(ChiType *type);

    void compile_destruction_for_type(Function *fn, unknown_t &address, ChiType *type);

    void compile_destruction(Function *fn, unknown_t &address, ast::Node *node);

    DotValue compile_dot_expr(Function *fn, ast::Node *expr);

    void compile_array_construction(Function *fn, const unknown_t &dest);

    void compile_array_reserve(Function *fn, const unknown_t &dest, unknown_t elem_size,
                               const unknown_t &size);

    void compile_construction(Function *fn, unknown_t dest, ChiType *type, ast::Node *expr);

    unknown_t compile_string_alloc(Function *fn, const unknown_t &data);

    void compile_string_construction(Function *fn, const unknown_t &dest, optional<unknown_t> data);

    unknown_t compile_string_literal(Function *fn, string str);

    Function *get_fn(ast::Node *node);

    Function *add_internal_method_fn(ChiType *type, ast::Node *method);

    Function *new_fn(unknown_t signature, ast::Node *node);

    Function *add_fn(unknown_t signature, ast::Node *node);

    JumpTable *add_jump_table(long *table_index);

    ImplInfo *get_impl_info(TraitImpl *impl);

    void init_method_fn(Function *fn, const string &fn_name, ChiType *struct_type,
                        ChiTypeSubtype *subtype);

    void add_value(ast::Node *node, const unknown_t &value) { m_ctx->value_table[node] = value; }

    unknown_t &get_value(ast::Node *node) { return m_ctx->value_table.at(node); }

    unknown_t compile_simple_value(Function *fn, ast::Node *expr);

    unknown_t compile_fn_call(Function *fn, ast::Node *fn_call);

    unknown_t compile_arithmetic_op(Function *fn, ChiType *value_type, TokenType op_type,
                                    const unknown_t &op1, const unknown_t &op2);

    unknown_t compile_assignment_value(Function *fn, ast::Node *expr, ast::Node *dest);

    unknown_t compile_assignment_to_type(Function *fn, ast::Node *expr, ChiType *dest_type);

    unknown_t compile_conversion(Function *fn, const unknown_t &value, ChiType *from_type,
                                 ChiType *to_type);

    unknown_t compile_value_copy(Function *fn, const unknown_t &value, ChiType *type);

    ValueRef compile_value_ref(Function *fn, ast::Node *expr);

    unknown_t compile_constant_value(Function *fn, const ConstantValue &value, ChiType *type);

    unknown_t compile_mem_alloc(Function *fn, const unknown_t &size_value);
    void compile_mem_free(Function *fn, const unknown_t &ptr);

    unknown_t compile_refc_construction(Function *fn, const unknown_t &dest,
                                        const unknown_t &size_value);
    void compile_refc_incref(Function *fn, unknown_t &address);
    void compile_refc_decref(Function *fn, unknown_t &address, ChiType *elem_type);

    unknown_t compile_string_concat(Function *fn, const unknown_t &s1, const unknown_t &s2);

    void compile_panic(Function *fn, const string &message);

    void _compile_struct(ast::Node *node, ChiType *struct_type);

    StructData *get_struct_data(ChiType *struct_type);

    unknown_t create_string_constant(Function *fn, const char *str);

    void compile_stmt(Function *fn, ast::Node *stmt);

    void compile_block(Function *fn, ast::Node *parent, ast::Node *block);

    void compile_struct(ast::Node *node);

    Struct get_struct(ChiType *struct_type);

    void compile_cprintf(Function *fn, const char *s);
    void compile_debug_i(Function *fn, const char *prefix, const unknown_t &v);

  public:
    Compiler(CompilationContext *ctx);

    CompilationContext *get_context() { return m_ctx; }

    TypeInfo *get_type_info(ChiType *type);

    void compile_module(ast::Module *module);

    Function *add_fn_node(ast::Node *node);

    void compile_fn_body(Function *fn);

    unknown_t compile_type(ChiType *type);

    void compile_internals();

    Function *compile_end_fn();
};
} // namespace codegen
} // namespace cx