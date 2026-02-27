/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <functional>
#include <list>
#include <optional>
#include <set>
#include <utility>

#include "ast.h"
#include "enum.h"
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
    label_t *continue_target = nullptr;  // where continue jumps (post-increment for for/range loops)
    size_t active_blocks_depth = 0;  // active_blocks.size() when loop was entered
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
    ChiType *type = nullptr;

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
    map<ChiType *, ChiType *> *type_env = nullptr; // TypeEnv from GenericResolver (placeholder → concrete)
    label_t *next_end_label = nullptr;
    array<llvm::Value *> vararg_pointers = {};

    std::list<BlockScope> block_scopes;
    std::list<BlockScope *> scope_stack;
    std::list<LoopLabels> loop_labels;
    std::vector<ast::Block *> active_blocks;  // block cleanup stack (inner to outer)
    bool has_cleanup_invoke = false;

    // Parameter information
    std::vector<ParameterInfo> parameter_info;
    llvm::Value *bind_ptr = nullptr;

    // Error ownership for catch blocks (cleaned up at function return for diverge paths)
    struct ErrorOwner {
        llvm::Value *ptr_var;    // alloca holding error data pointer (null if already freed)
        ChiType *concrete_type;  // concrete error type for destruction (null for catch-all)
    };
    std::vector<ErrorOwner> error_owner_vars;

    Function(CodegenContext *ctx, llvm::Function *llvm_fn, ast::Node *node);
    ~Function() {}

    string get_llvm_name() const {
        // Check if this is the entry point main function
        if (node && node->module && node->module->package &&
            node->module->package->entry_fn == node) {
            return "main";
        }

        // Check if this is an extern function (C linkage)
        if (node && node->type == ast::NodeType::FnDef) {
            auto& fn_def = node->data.fn_def;
            if (fn_def.decl_spec && (fn_def.decl_spec->flags & ast::DECL_EXTERN)) {
                // Extern functions use their original name (C linkage)
                return qualified_name;
            }
        }

        // Chi functions get prefixed with __chi_<module_id>_
        if (node && node->module) {
            return "__chi_" + node->module->id_path + "_" + qualified_name;
        }

        return "__chi_" + qualified_name;
    }
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

    LoopLabels *push_loop() {
        auto *loop = &loop_labels.emplace_back();
        loop->active_blocks_depth = active_blocks.size();
        return loop;
    }
    void pop_loop() { loop_labels.pop_back(); }
    LoopLabels *get_loop() { return &loop_labels.back(); }

    label_t *new_label(const string &name = "", label_t *insert_before = nullptr);
    void use_label(label_t *bb);

    void insn_noop();
    llvm::AllocaInst *entry_alloca(llvm::Type *ty, const string &name = "");

    ast::FnDef *get_def() {
        switch (node->type) {
        case ast::NodeType::GeneratedFn:
            return &node->data.generated_fn.original_fn->data.fn_def;
        case ast::NodeType::FnDef:
            return &node->data.fn_def;
        default:
            panic("invalid node type for function: {}", PRINT_ENUM(node->type));
            return nullptr;
        }
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

// Async/await support structures
struct AsyncSegment {
    std::vector<ast::Node *> stmts;      // statements in this segment
    ast::Node *await_expr = nullptr;     // the await that ends this segment (null for final)
    ast::Node *await_var_decl = nullptr; // the var decl containing the await (if any)
    ChiType *await_value_type = nullptr; // type of the awaited value
    std::set<ast::Node *> vars_to_capture; // variables needed in later segments
};

struct AsyncContext {
    Function *parent_fn = nullptr;
    std::vector<AsyncSegment> segments;
    std::vector<llvm::Function *> continuations;       // LLVM functions for continuations
    std::vector<Function *> continuation_fn_objects;   // Function objects for codegen
    llvm::Value *result_promise_ptr = nullptr; // Pointer to the result Promise
    ChiType *promise_type = nullptr;           // The Promise<T> return type
    llvm::StructType *promise_struct_type = nullptr;
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
    map<string, llvm::Type *> type_table = {};
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
    map<ChiType *, Function *> destructor_table = {};  // Generated __delete functions
    map<ChiType *, Function *> copier_table = {};      // Generated __copy functions
    map<ChiType *, Function *> constructor_table = {}; // Generated __new functions

    // Module-level let vars that need runtime initialization
    // Collected during compile_module, initialized at start of main
    array<std::pair<ast::Node *, llvm::GlobalVariable *>> pending_global_inits = {};

    // Tracing: track what codegen actually compiles (for comparison with GenericResolver)
    std::set<string> compiled_generic_fns = {};    // function global_ids compiled
    std::set<string> compiled_generic_structs = {}; // struct global_ids compiled
    std::set<ast::Module*> compiled_modules = {};  // modules already compiled

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
    ChiType *m_fn_eval_subtype = nullptr;

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

    void compile_block_cleanup(Function *fn, ast::Block *block);
    void compile_destruction(Function *fn, llvm::Value *address, ast::Node *node);
    void compile_destruction_for_type(Function *fn, llvm::Value *address, ChiType *type);
    void compile_heap_free(Function *fn, llvm::Value *ptr, ChiType *elem_type);
    void compile_interface_destruction(Function *fn, llvm::Value *iface_address, ChiType *iface_ref_type);
    void call_vtable_destructor(Function *fn, llvm::Value *vtable_ptr, llvm::Value *data_ptr);
    void call_vtable_copier(Function *fn, llvm::Value *vtable_ptr, llvm::Value *dest_data,
                            llvm::Value *src_data);
    llvm::Value *find_interface_vtable(Function *fn, ChiType *iface_type);
    llvm::Value *load_typesize_from_vtable(llvm::Value *vtable_ptr);
    llvm::ConstantPointerNull *get_null_ptr();
    Function *generate_destructor(ChiType *type, ChiType *container_type = nullptr);
    Function *generate_copier(ChiType *type);
    Function *generate_any_destructor(ChiType *type);
    Function *generate_any_copier(ChiType *type);
    Function *generate_destructor_optional(ChiType *type, ChiType *resolved_type);
    Function *generate_destructor_enum(ChiType *type, ChiType *resolved_type);
    Function *generate_copier_enum(ChiType *type);
    Function *generate_copier_fixed_array(ChiType *type);
    Function *generate_destructor_result(ChiType *type, ChiType *resolved_type);
    Function *generate_destructor_continuation(llvm::StructType *capture_struct_type,
                                               ChiType *promise_type,
                                               const std::vector<ast::Node *> &captured_vars);
    Function *generate_constructor(ChiType *struct_type, ChiType *container_type = nullptr);

    void compile_construction(Function *fn, llvm::Value *dest, ChiType *type, ast::Node *expr);

    llvm::Value *compile_string_literal(const string &str);

    llvm::Value *compile_c_string_literal(const string &str);

    Function *get_fn(ast::Node *node);

    Function *add_fn(llvm::Function *llvm_fn, ast::Node *node, ChiType *fn_type = nullptr);

    void add_var(ast::Node *node, llvm::Value *value) { m_ctx->var_table[node] = value; }

    llvm::Value *&get_var(ast::Node *node) {
        if (!m_ctx->var_table.has_key(node)) {
            panic("Variable '{}' not found in var_table (node type: {}, resolved_type: {})",
                  node->name, (int)node->type,
                  node->resolved_type ? "set" : "null");
        }
        return m_ctx->var_table.at(node);
    }

    llvm::Value *compile_comparator(Function *fn, ast::Node *expr, ChiType *type = nullptr);

    llvm::Value *compile_expr(Function *fn, ast::Node *expr);

    llvm::Type *get_llvm_ptr_type();

    void compile_copy(Function *fn, llvm::Value *value, llvm::Value *dest, ChiType *type,
                      ast::Node *expr = nullptr);
    void compile_store_or_copy(Function *fn, llvm::Value *value, llvm::Value *dest, ChiType *type,
                               ast::Node *expr, bool destruct_old = false);
    void compile_copy_with_ref(Function *fn, RefValue src, llvm::Value *dest, ChiType *type,
                               ast::Node *expr = nullptr, bool destruct_old = false);

    void compile_destructure_fields(Function *fn, array<ast::Node *> &fields,
                                    llvm::Value *source_ptr, ChiType *source_type);

    void compile_array_destructure(Function *fn, ast::DestructureDecl &data,
                                   llvm::Value *source_ptr, ChiType *source_type);

    llvm::Value *compile_optional_branch(
        Function *fn, ast::Node *opt_expr, llvm::Type *result_type_l, const char *label,
        std::function<llvm::Value *(llvm::Value *unwrapped_ptr)> on_has_value,
        std::function<llvm::Value *()> on_null);

    llvm::Value *compile_dot_access(Function *fn, llvm::Value *ptr, ChiType *type,
                                    ChiStructMember *member);

    llvm::Value *compile_dot_ptr(Function *fn, ast::Node *expr);

    RefValue compile_expr_ref(Function *fn, ast::Node *expr);

    RefValue compile_iden_ref(Function *fn, ast::Node *iden);

    std::vector<llvm::Value *> compile_fn_args(
        Function *fn, Function *callee, array<ast::Node *> args, ast::Node *fn_call,
        std::vector<std::pair<llvm::Value *, ast::Node *>> *out_temporaries = nullptr);
    llvm::Value *compile_fn_call(Function *fn, ast::Node *fn_call, InvokeInfo *invoke = nullptr,
                                 llvm::Value *sret_dest = nullptr);

    llvm::Value *create_fn_call_invoke(llvm::FunctionCallee callee, std::vector<llvm::Value *> args,
                                       llvm::Type *sret_type = nullptr,
                                       InvokeInfo *invoke = nullptr,
                                       llvm::Value *sret_dest = nullptr);

    llvm::Value *compile_assignment_value(Function *fn, ast::Node *expr, ast::Node *dest);

    llvm::Value *compile_assignment_to_type(Function *fn, ast::Node *expr, ChiType *dest_type);
    void compile_assignment_to_ptr(Function *fn, ast::Node *expr, llvm::Value *dest,
                                   ChiType *dest_type);

    llvm::Value *compile_lambda_alloc(Function *fn, ChiType *lambda_type, llvm::Value *fn_ptr,
                                      array<ast::FnCapture> *captures);

    // __CxLambda construction helpers (shared by lambda alloc, method-to-lambda, async continuations)
    struct CxLambdaInit {
        llvm::Value *alloca_ptr;     // stack-allocated __CxLambda
        llvm::Type *struct_type_l;   // LLVM type of __CxLambda
    };
    CxLambdaInit compile_cxlambda_init(Function *fn, llvm::Value *fn_ptr, uint32_t capture_size);

    struct CxCaptureInfo {
        llvm::Value *capture_ptr;       // CxCapture pointer (for set_captures_ptr)
        llvm::Value *payload_data_ptr;  // raw payload pointer (for storing capture data)
    };
    CxCaptureInfo compile_cxcapture_create(uint32_t payload_size, llvm::Value *type_info,
                                           llvm::Value *dtor);

    void compile_cxlambda_set_captures(llvm::Value *lambda_alloca, llvm::Value *capture_ptr);

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
    void compile_concrete_enum(ChiTypeEnum *enum_data);

    void compile_extern(ast::Node *node);

    llvm::Value *compile_reflection_vtable();

    Function *compile_fn_proto(ast::Node *node, ast::Node *fn, string name = "");
    Function *compile_fn_def(ast::Node *node, Function *fn = nullptr);

    Function *get_system_fn(const string &name);
    Function *get_specialized_fn(ast::Node *generic_fn_decl, ChiType *specialized_subtype);

    llvm::Value *generate_method_proxy_function(Function *fn, ChiStructMember *method_member,
                                                ChiType *lambda_type);
    llvm::Value *generate_lambda_proxy_function(Function *fn, llvm::Value *original_fn_ptr,
                                                ChiType *lambda_type, NodeList *captures);

    // Variant lookup helpers
    std::optional<TypeId> resolve_variant_type_id(Function *fn, ChiType *type);
    ast::Node *get_variant_member_node(ChiStructMember *member, std::optional<TypeId> variant_type_id);

    // Async/await codegen
    std::vector<AsyncSegment> collect_async_segments(ast::Node *body);
    void collect_vars_used_in_node(ast::Node *node, std::set<ast::Node *> &vars);
    std::pair<llvm::StructType *, std::vector<ast::Node *>> get_continuation_capture_info(
        AsyncContext &ctx, int segment_index);
    llvm::Function *create_continuation_fn_decl(AsyncContext &ctx, int segment_index);
    void generate_async_continuation_body(AsyncContext &ctx, int segment_index);
    llvm::Value *build_continuation_lambda(Function *fn, AsyncContext &ctx, int segment_index,
                                           map<ast::Node *, llvm::Value *> &local_vars,
                                           llvm::Value *result_promise_ptr);
    void emit_promise_chain(Function *fn, AsyncContext &ctx, ast::Node *await_expr,
                            int next_segment_index, map<ast::Node *, llvm::Value *> &local_vars,
                            llvm::Value *result_promise_ptr);
    void compile_async_fn_body(Function *fn);

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
    void dump_generics_comparison();
};
} // namespace codegen
} // namespace cx