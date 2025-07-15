/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <list>

#include "ast.h"
#include "llvm.h"
#include "resolver.h"
#include "runtime/internals.h"

namespace cx {
struct CompilationContext;

namespace codegen {
struct CodegenContext;
typedef void *unknown_t;
typedef llvm::BasicBlock label_t;

struct LoopLabels {
    label_t *start = nullptr;
    label_t *end = nullptr;
};

enum class ParameterKind {
    SRet,   // Struct return parameter
    Bind,   // Bind parameter (this/lambda capture)
    Regular // Regular user parameter
};

struct ParameterInfo {
    ParameterKind kind;
    int llvm_index;       // Position in LLVM function args
    int user_param_index; // Index in original function parameters (-1 for generated)
    std::string name;     // Parameter name

    ParameterInfo(ParameterKind k, int llvm_idx, int user_idx = -1, std::string n = "")
        : kind(k), llvm_index(llvm_idx), user_param_index(user_idx), name(n) {}
};

struct BlockScope {
    bool branched = false;
};

struct Function {
    string qualified_name = "";
    ast::Node *node = nullptr;
    CodegenContext *ctx = nullptr;
    llvm::Function *llvm_fn = nullptr;

    llvm::Value *is_returning = nullptr;
    llvm::Value *return_value = nullptr;
    label_t *cleanup_landing_label = nullptr;
    label_t *return_label = nullptr;
    ChiType *container_type = nullptr;
    ChiTypeSubtype *container_subtype = nullptr;
    ChiType *fn_type = nullptr;
    ChiType *specialized_subtype = nullptr; // For specialized generic functions
    label_t *next_end_label = nullptr;
    array<llvm::Value *> vararg_pointers = {};

    std::list<BlockScope> block_scopes;
    std::list<BlockScope *> scope_stack;
    std::list<LoopLabels> loop_labels;
    bool has_cleanup_invoke = false;

    // Parameter information
    std::vector<ParameterInfo> parameter_info;
    llvm::Value *bind_ptr = nullptr;

    Function(CodegenContext *ctx, llvm::Function *llvm_fn, ast::Node *node);
    ~Function() {}

    string get_llvm_name() const { return qualified_name; }
    bool use_sret() { return get_sret_param() != nullptr; }
    llvm::Value *get_this_arg() {
        auto bind_param = get_bind_param();
        return bind_param ? llvm_fn->getArg(bind_param->llvm_index) : nullptr;
    }

    // New parameter system helper methods
    ParameterInfo *get_sret_param() {
        for (auto &param : parameter_info) {
            if (param.kind == ParameterKind::SRet)
                return &param;
        }
        return nullptr;
    }

    ParameterInfo *get_bind_param() {
        for (auto &param : parameter_info) {
            if (param.kind == ParameterKind::Bind)
                return &param;
        }
        return nullptr;
    }

    ParameterInfo *get_param_at_llvm_index(int llvm_index) {
        for (auto &param : parameter_info) {
            if (param.llvm_index == llvm_index)
                return &param;
        }
        return nullptr;
    }

    int get_user_param_llvm_offset() {
        int offset = 0;
        for (const auto &param : parameter_info) {
            if (param.kind != ParameterKind::Regular)
                offset++;
        }
        return offset;
    }

    unknown_t get_null_constant();
    BlockScope *push_scope() {
        auto scope = &block_scopes.emplace_back();
        scope_stack.push_back(scope);
        return scope;
    }
    void pop_scope() { scope_stack.pop_back(); }
    BlockScope *get_scope() { return scope_stack.back(); }

    LoopLabels *push_loop() { return &loop_labels.emplace_back(); }
    void pop_loop() { loop_labels.pop_back(); }
    LoopLabels *get_loop() { return &loop_labels.back(); }

    label_t *new_label(const string &name = "", label_t *insert_before = nullptr);
    void use_label(label_t *bb);

    void insn_noop();
    llvm::AllocaInst *entry_alloca(llvm::Type *ty, const string &name = "");

    ast::FnDef *get_def() {
        assert(node && node->type == ast::NodeType::FnDef);
        return &node->data.fn_def;
    }
};

struct CompilationSettings {
    string output_obj_to_file = "";
    string output_ir_to_file = "";
    uint32_t lang_flags = LANG_FLAG_NONE;
};

struct InvokeInfo {
    label_t *normal = nullptr;
    label_t *landing = nullptr;
    llvm::Value *sret = nullptr;
    llvm::Type *sret_type = nullptr;
};

struct CodegenContext {
    CodegenContext(const CodegenContext &) = delete;
    CodegenContext &operator=(const CodegenContext &) = delete;

    CompilationSettings settings;
    CompilationContext *compilation_ctx = nullptr;
    Resolver resolver;

    array<box<Function>> functions = {};
    array<llvm::Type *> types = {};
    array<box<string>> strings = {};
    array<Function *> pending_fns = {};
    std::vector<llvm::Constant *> reflection_vtable = {};

    map<ast::Node *, llvm::Value *> var_table = {};
    map<TypeId, llvm::Type *> type_table = {};
    map<TypeId, box<TypeInfo>> info_table = {};
    map<string, Function *> function_table = {};
    map<string, Function *> system_functions = {};
    map<string, ast::Node *> generic_functions =
        {}; // Maps function global_id to AST node for instantiation
    map<ChiType *, llvm::Value *> typeinfo_table = {};
    map<string, llvm::Type *> anon_type_table = {};
    map<InterfaceImpl *, llvm::Value *> impl_table = {};
    map<ChiEnumVariant *, llvm::Value *> enum_variant_table = {};
    map<string, llvm::DICompileUnit *> module_cu_table = {};

    // llvm
    box<llvm::LLVMContext> llvm_ctx = {};
    box<llvm::Module> llvm_module = {};
    box<llvm::IRBuilder<>> llvm_builder = {};
    box<llvm::DIBuilder> dbg_builder = {};
    llvm::DICompileUnit *dbg_cu = nullptr;
    array<llvm::DIScope *> dbg_scopes = {};

    CodegenContext(CompilationContext *compilation_ctx);
    ~CodegenContext();

    void init_llvm();

    Function *add_fn(ast::Node *node, Function *fn);

    llvm::StructType *get_caught_result_type();
};

enum ArrayOp { Destroy, Copy };
enum ArrayFlag { None, Static };

struct RefValue {
    llvm::Value *address = nullptr;
    llvm::Value *value = nullptr;

    static RefValue from_value(llvm::Value *value) { return {nullptr, value}; }
    static RefValue from_address(llvm::Value *address) { return {address, nullptr}; }
};

struct CompiledVtable {
    int32_t offset = 0;
    InterfaceImpl *impl = nullptr;
};

class Compiler {
    CodegenContext *m_ctx = nullptr;
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

    inline llvm::Type *compile_type_of(ast::Node *node);

    void compile_destruction(Function *fn, llvm::Value *address, ast::Node *node);

    void compile_construction(Function *fn, llvm::Value *dest, ChiType *type, ast::Node *expr);

    llvm::Value *compile_string_literal(const string &str);

    Function *get_fn(ast::Node *node);

    Function *add_fn(llvm::Function *llvm_fn, ast::Node *node, ChiType *fn_type = nullptr);

    void add_var(ast::Node *node, llvm::Value *value) { m_ctx->var_table[node] = value; }

    llvm::Value *&get_var(ast::Node *node) { return m_ctx->var_table.at(node); }

    llvm::Value *compile_comparator(Function *fn, ast::Node *expr, ChiType *type = nullptr);

    llvm::Value *compile_expr(Function *fn, ast::Node *expr);

    llvm::Type *get_llvm_ptr_type();

    void compile_copy(Function *fn, llvm::Value *value, llvm::Value *dest, ChiType *type,
                      ast::Node *expr = nullptr);
    void compile_copy_with_ref(Function *fn, RefValue src, llvm::Value *dest, ChiType *type,
                               ast::Node *expr = nullptr);

    llvm::Value *compile_dot_access(Function *fn, llvm::Value *ptr, ChiType *type,
                                    ChiStructMember *member);

    llvm::Value *compile_dot_ptr(Function *fn, ast::Node *expr);

    RefValue compile_expr_ref(Function *fn, ast::Node *expr);

    RefValue compile_iden_ref(Function *fn, ast::Node *iden);

    std::vector<llvm::Value *> compile_fn_args(Function *fn, Function *callee,
                                               array<ast::Node *> args, ast::Node *fn_call);
    llvm::Value *compile_fn_call(Function *fn, ast::Node *fn_call, InvokeInfo *invoke = nullptr);

    llvm::Value *create_fn_call_invoke(llvm::FunctionCallee callee, std::vector<llvm::Value *> args,
                                       llvm::Type *sret_type = nullptr,
                                       InvokeInfo *invoke = nullptr);

    llvm::Value *compile_assignment_value(Function *fn, ast::Node *expr, ast::Node *dest);

    llvm::Value *compile_assignment_to_type(Function *fn, ast::Node *expr, ChiType *dest_type);

    llvm::Value *compile_lambda_alloc(Function *fn, ChiType *lambda_type, llvm::Value *fn_ptr,
                                      NodeList *captures);

    llvm::Value *compile_number_conversion(Function *fn, llvm::Value *value, ChiType *from_type,
                                           ChiType *to_type);

    llvm::Value *compile_conversion(Function *fn, llvm::Value *value, ChiType *from_type,
                                    ChiType *to_type);

    llvm::Value *compile_constant_value(Function *fn, const ConstantValue &value, ChiType *type,
                                        llvm::Type *llvm_type = nullptr);

    void _compile_struct(ast::Node *node, ChiType *struct_type);

    llvm::Value *compile_alloc(Function *fn, ast::Node *decl, bool is_new = false,
                               ChiType *type = nullptr);

    void compile_stmt(Function *fn, ast::Node *stmt);

    llvm::Value *compile_block(Function *fn, ast::Node *parent, ast::Node *block,
                               label_t *end_label, llvm::Value *var = nullptr);

    void compile_struct(ast::Node *node);

    void compile_enum(ast::Node *node);

    void compile_extern(ast::Node *node);

    llvm::Value *compile_reflection_vtable();

    Function *compile_fn_proto(ast::Node *node, ast::Node *fn, string name = "");
    Function *compile_fn_proto_specialized(ast::Node *node, ast::Node *fn, ChiType *subtype);
    Function *compile_fn_def(ast::Node *node, Function *fn = nullptr);

    Function *get_system_fn(const string &name);
    Function *get_specialized_fn(ast::Node *generic_fn_decl, ChiType *specialized_subtype);
    
    // Helper function to get resolved function type from a specialized subtype
    ChiType *get_specialized_fn_type(ChiType *specialized_subtype);

    void compile_struct_vtables(ChiType *type);
    llvm::Value *compile_type_info(ChiType *type);

    llvm::TypeSize llvm_type_size(llvm::Type *type);

  public:
    Compiler(CodegenContext *ctx);

    CodegenContext *get_context() { return m_ctx; }
    CompilationSettings *get_settings() { return &m_ctx->settings; }
    bool is_managed() { return has_lang_flag(get_settings()->lang_flags, LANG_FLAG_MANAGED); }

    TypeInfo *get_type_info(ChiType *type);

    llvm::DICompileUnit *get_module_cu(ast::Module *module);
    void compile_module(ast::Module *module);

    llvm::Type *compile_type(ChiType *type);

    Function *compile_end_fn();

    void emit_dbg_location(ast::Node *node);
    void emit_output();
};
} // namespace codegen
} // namespace cx