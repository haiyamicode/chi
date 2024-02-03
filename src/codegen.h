/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <list>

#include "llvm.h"
#include "resolver.h"
#include "runtime/internals.h"

namespace cx {
struct CompilationContext;

namespace codegen {
struct CodegenContext;
typedef void *unknown_t;
typedef llvm::BasicBlock label_t;

struct VarLabel {
    label_t *label = nullptr;
    ast::Node *var = nullptr;
};

typedef std::list<VarLabel> VarLabels;

struct LoopLabels {
    label_t *start = nullptr;
    label_t *end = nullptr;
};

struct BlockScope {
    VarLabels vars = {};
    bool returned = false;
};

struct Function {
    string qualified_name = "";
    ast::Node *node = nullptr;
    CodegenContext *ctx = nullptr;
    llvm::Function *llvm_fn = nullptr;

    llvm::Value *is_returning = nullptr;
    llvm::Value *return_value = nullptr;
    label_t *return_label = nullptr;
    ChiTypeSubtype *container_subtype = nullptr;
    ChiType *fn_type = nullptr;

    std::list<BlockScope> block_scopes;
    std::list<BlockScope *> scope_stack;
    std::list<LoopLabels> loop_labels;
    bool has_try = false;

    Function(CodegenContext *ctx, llvm::Function *llvm_fn, ast::Node *node);
    ~Function() {}

    const char *get_llvm_name() const { return qualified_name.c_str(); }

    unknown_t get_null_constant();
    BlockScope *push_scope() {
        auto scope = &block_scopes.emplace_back();
        scope_stack.push_back(scope);
        return scope;
    }
    void pop_scope() { scope_stack.pop_back(); }
    BlockScope *get_scope() { return &block_scopes.back(); }

    LoopLabels *push_loop() { return &loop_labels.emplace_back(); }
    void pop_loop() { loop_labels.pop_back(); }
    LoopLabels *get_loop() { return &loop_labels.back(); }

    label_t *new_label(const string &name = "", label_t *insert_before = nullptr);
    void use_label(label_t *bb);

    void insn_noop();
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
    optional<unknown_t> vtable_4n;
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

struct WrappedType {
    unknown_t type;
    unknown_t elem;
    unknown_t elem_size;

    unknown_t get_opt_flag_offset() { return elem_size; }
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

struct CompilationSettings {
    string output_obj_to_file = "";
    string output_ir_to_file = "";
    uint32_t lang_flags = LANG_FLAG_NONE;
};

struct InvokeInfo {
    label_t *normal = nullptr;
    label_t *landing = nullptr;
};

struct CodegenContext {
    CompilationSettings settings;
    CompilationContext *compilation_ctx = nullptr;
    Resolver resolver;

    array<box<Function>> functions;
    array<box<JumpTable>> jump_tables;
    array<box<ImplInfo>> impls;
    array<llvm::Type *> types;
    array<box<string>> strings;
    array<Function *> pending_fns;

    map<ast::Node *, llvm::Value *> value_table = {};
    map<ChiType *, box<StructData>> struct_table = {};
    map<TypeId, llvm::Type *> type_table = {};
    map<TypeId, box<TypeInfo>> info_table = {};
    map<ast::Node *, Function *> function_table = {};
    map<string, Function *> system_functions = {};
    map<ChiType *, llvm::Value *> typeinfo_table = {};

    // llvm
    box<llvm::LLVMContext> llvm_ctx;
    box<llvm::Module> llvm_module;
    box<llvm::IRBuilder<>> llvm_builder;
    box<llvm::DIBuilder> dbg_builder;
    llvm::DICompileUnit *dbg_cu = nullptr;
    array<llvm::DIScope *> dbg_scopes = {};

    CodegenContext(CompilationContext *compilation_ctx);
    ~CodegenContext();

    void init_llvm();

    Function *add_fn(ast::Node *node, Function *fn);
};

enum ArrayOp { Destroy, Copy };
enum ArrayFlag { None, Static };

struct RefValue {
    llvm::Value *address = nullptr;
    llvm::Value *value = nullptr;
};

class Compiler {
    CodegenContext *m_ctx;
    Function *m_fn = nullptr;

    llvm::Type *add_type(llvm::Type *type) { return *m_ctx->types.add(type); }

    const char *add_string(const string &str) {
        return m_ctx->strings.emplace(new string(str))->get()->c_str();
    }

    Resolver *get_resolver() { return &m_ctx->resolver; }
    SystemTypes *get_system_types() { return get_resolver()->get_system_types(); }

    ChiType *eval_type(ChiType *type);
    ChiType *get_chitype(ast::Node *node);

    llvm::Type *_compile_type(ChiType *type);
    llvm::DISubroutineType *compile_di_fn_type(Function *fn);
    llvm::DIType *compile_di_type(ChiType *type);

    unknown_t convert_int_type(ChiType *type);

    WrappedType compile_wrapped_type(ChiType *type);

    inline llvm::Type *compile_type_of(ast::Node *node);

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

    llvm::Value *compile_string_literal(Function *fn, const string &str);

    Function *get_fn(ast::Node *node);

    Function *add_internal_method_fn(ChiType *type, ast::Node *method);

    Function *add_fn(llvm::Function *llvm_fn, ast::Node *node, ChiType *fn_type = nullptr);

    JumpTable *add_jump_table(long *table_index);

    ImplInfo *get_impl_info(TraitImpl *impl);

    void init_method_fn(Function *fn, const string &fn_name, ChiType *struct_type,
                        ChiTypeSubtype *subtype);

    void add_value(ast::Node *node, llvm::Value *value) { m_ctx->value_table[node] = value; }

    llvm::Value *&get_value(ast::Node *node) { return m_ctx->value_table.at(node); }

    llvm::Value *compile_expr(Function *fn, ast::Node *expr);

    RefValue compile_expr_ref(Function *fn, ast::Node *expr);

    llvm::Value *compile_fn_call(Function *fn, ast::Node *fn_call, InvokeInfo *invoke = nullptr);

    llvm::Value *compile_arithmetic_op(Function *fn, ChiType *value_type, TokenType op_type,
                                       llvm::Value *op1, llvm::Value *op2);

    llvm::Value *compile_assignment_value(Function *fn, ast::Node *expr, ast::Node *dest);

    llvm::Value *compile_assignment_to_type(Function *fn, ast::Node *expr, ChiType *dest_type);

    llvm::Value *compile_lambda_alloc(Function *fn, ChiType *lambda_type, llvm::Value *fn_ptr);

    llvm::Value *compile_conversion(Function *fn, llvm::Value *value, ChiType *from_type,
                                    ChiType *to_type);

    llvm::Value *compile_value_copy(Function *fn, const unknown_t &value, ChiType *type);

    llvm::Value *compile_constant_value(Function *fn, const ConstantValue &value, ChiType *type);

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

    llvm::Value *compile_alloc(Function *fn, ast::Node *decl);

    void compile_stmt(Function *fn, ast::Node *stmt);

    void compile_block(Function *fn, ast::Node *parent, ast::Node *block);

    void compile_struct(ast::Node *node);

    void compile_extern(ast::Node *node);

    Function *compile_fn_proto(ast::Node *node, ast::Node *fn, string name = "");
    Function *compile_fn_def(ast::Node *node, Function *fn = nullptr);

    Struct get_struct(ChiType *struct_type);

    void compile_cprintf(Function *fn, const char *s);
    void compile_debug_i(Function *fn, const char *prefix, const unknown_t &v);
    Function *get_system_fn(const string &name);
    llvm::Value *compile_type_info(ChiType *type);

  public:
    Compiler(CodegenContext *ctx);

    CodegenContext *get_context() { return m_ctx; }
    CompilationSettings *get_settings() { return &m_ctx->settings; }
    bool is_managed() { return has_lang_flag(get_settings()->lang_flags, LANG_FLAG_MANAGED); }

    TypeInfo *get_type_info(ChiType *type);

    void compile_module(ast::Module *module);

    llvm::Type *compile_type(ChiType *type);

    Function *compile_end_fn();

    void emit_dbg_location(ast::Node *node);
    void emit_output();
};
} // namespace codegen
} // namespace cx