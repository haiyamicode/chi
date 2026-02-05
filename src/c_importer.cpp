#include "c_importer.h"
#include "context.h"
#include "ast.h"
#include "resolver.h"
#include <iostream>
#include <sstream>

namespace cx {

CImporter::CImporter()
#ifdef __clang__
    : index_(nullptr), tu_(nullptr)
#endif
{
#ifdef __clang__
    index_ = clang_createIndex(0, 0);
#endif
}

CImporter::~CImporter() {
#ifdef __clang__
    if (tu_) {
        clang_disposeTranslationUnit(tu_);
    }
    if (index_) {
        clang_disposeIndex(index_);
    }
#endif
}

bool CImporter::import_header(const std::string& header_path, const CImportConfig& config) {
#ifndef __clang__
    error_ = "C interop not available - libclang not found";
    return false;
#else
    // Store symbol filter
    symbol_filter_.clear();
    for (const auto& sym : config.symbols) {
        symbol_filter_.insert(sym);
    }

    // Build compiler arguments
    std::vector<const char*> args;
    for (const auto& path : config.include_paths) {
        args.push_back("-I");
        args.push_back(path.c_str());
    }
    // Default include path
    args.push_back("-I/usr/include");

    // Parse the header
    tu_ = clang_parseTranslationUnit(
        index_,
        header_path.c_str(),
        args.data(), args.size(),
        nullptr, 0,
        CXTranslationUnit_None
    );

    if (!tu_) {
        error_ = "Failed to parse header: " + header_path;
        return false;
    }

    // Check for parse errors
    unsigned num_diag = clang_getNumDiagnostics(tu_);
    if (num_diag > 0) {
        error_ = "Parse errors in header:\n";
        for (unsigned i = 0; i < num_diag; i++) {
            CXDiagnostic diag = clang_getDiagnostic(tu_, i);
            error_ += to_string(clang_formatDiagnostic(diag, CXDiagnostic_DisplaySourceLocation));
            error_ += "\n";
            clang_disposeDiagnostic(diag);
        }
    }

    // Visit AST and extract symbols
    CXCursor cursor = clang_getTranslationUnitCursor(tu_);
    clang_visitChildren(cursor, visitor_callback, this);

    return true;
#endif
}

#ifdef __clang__
std::string CImporter::to_string(CXString cx_str) {
    std::string result = clang_getCString(cx_str);
    clang_disposeString(cx_str);
    return result;
}

CXChildVisitResult CImporter::visitor_callback(CXCursor cursor, CXCursor parent, CXClientData client_data) {
    auto* importer = static_cast<CImporter*>(client_data);
    CXCursorKind kind = clang_getCursorKind(cursor);

    if (kind == CXCursor_FunctionDecl) {
        std::string name = to_string(clang_getCursorSpelling(cursor));

        // Check if this symbol is in our filter
        if (importer->symbol_filter_.empty() || importer->symbol_filter_.count(name)) {
            importer->process_function(cursor);
        }
    }
    else if (kind == CXCursor_EnumConstantDecl) {
        std::string name = to_string(clang_getCursorSpelling(cursor));

        // Check if this symbol is in our filter
        if (importer->symbol_filter_.empty() || importer->symbol_filter_.count(name)) {
            importer->process_enum_constant(cursor);
        }
    }

    return CXChildVisit_Continue;
}

void CImporter::process_function(CXCursor cursor) {
    CFunction func;
    func.name = to_string(clang_getCursorSpelling(cursor));

    CXType func_type = clang_getCursorType(cursor);
    CXType return_type = clang_getResultType(func_type);
    func.return_type = to_string(clang_getTypeSpelling(return_type));

    // Extract parameters
    int num_args = clang_Cursor_getNumArguments(cursor);
    for (int i = 0; i < num_args; i++) {
        CXCursor arg = clang_Cursor_getArgument(cursor, i);
        CXType arg_type = clang_getCursorType(arg);
        std::string arg_type_str = to_string(clang_getTypeSpelling(arg_type));
        std::string arg_name = to_string(clang_getCursorSpelling(arg));

        func.params.push_back({arg_type_str, arg_name});
    }

    functions_.push_back(func);
}

void CImporter::process_enum_constant(CXCursor cursor) {
    CEnumConstant constant;
    constant.name = to_string(clang_getCursorSpelling(cursor));
    constant.value = clang_getEnumConstantDeclValue(cursor);
    enum_constants_.push_back(constant);
}
#endif

// Helper to parse C type string and convert to Chi type (verbatim mapping)
static ChiType* parse_c_type(CompilationContext* ctx, const std::string& c_type_str) {
    auto* resolve_ctx = &ctx->resolve_ctx;
    std::string type_str = c_type_str;

    // Strip const, volatile, restrict (we don't care)
    size_t pos;
    while ((pos = type_str.find("const ")) != std::string::npos) {
        type_str.erase(pos, 6);
    }
    while ((pos = type_str.find("volatile ")) != std::string::npos) {
        type_str.erase(pos, 9);
    }
    while ((pos = type_str.find("restrict ")) != std::string::npos) {
        type_str.erase(pos, 9);
    }

    // Trim whitespace
    type_str.erase(0, type_str.find_first_not_of(" \t"));
    type_str.erase(type_str.find_last_not_of(" \t") + 1);

    // Handle pointer types: convert "char *" to "*char"
    if (type_str.find('*') != std::string::npos) {
        // Extract base type (everything before *)
        size_t star_pos = type_str.find('*');
        std::string base_type = type_str.substr(0, star_pos);
        base_type.erase(base_type.find_last_not_of(" \t") + 1);

        // Count pointer levels
        int ptr_count = 0;
        for (char c : type_str) {
            if (c == '*') ptr_count++;
        }

        // Recursively handle base type
        ChiType* base_chi_type = parse_c_type(ctx, base_type);

        // Wrap in pointer types
        ChiType* result = base_chi_type;
        for (int i = 0; i < ptr_count; i++) {
            ChiType* ptr_type = ctx->create_type(TypeKind::Pointer);
            ptr_type->data.pointer.elem = result;
            result = ptr_type;
        }
        return result;
    }

    // Map primitive types
    if (type_str == "int" || type_str == "int32_t" || type_str == "signed int") {
        return resolve_ctx->system_types.int32;
    }
    if (type_str == "unsigned int" || type_str == "uint32_t") {
        return resolve_ctx->system_types.uint32;
    }
    if (type_str == "long" || type_str == "int64_t" || type_str == "long long") {
        return resolve_ctx->system_types.int64;
    }
    if (type_str == "char" || type_str == "signed char") {
        return resolve_ctx->system_types.char_;
    }
    if (type_str == "unsigned char" || type_str == "uint8_t") {
        return resolve_ctx->system_types.uint8;
    }
    if (type_str == "void") {
        return resolve_ctx->system_types.void_;
    }
    if (type_str == "float") {
        return resolve_ctx->system_types.float_;
    }
    if (type_str == "double") {
        return resolve_ctx->system_types.float64;
    }

    // Unknown type - treat as void pointer
    return resolve_ctx->system_types.void_ptr;
}

// Create a virtual Chi module from extracted C symbols
ast::Module* create_native_module(
    CompilationContext* ctx,
    const std::string& module_name,
    const std::vector<CFunction>& functions,
    const std::vector<CEnumConstant>& enum_constants
) {
    // Create a dummy token for position info
    auto* dummy_token = ctx->create_token();
    dummy_token->type = TokenType::KW_IMPORT;
    dummy_token->str = "<virtual>";
    dummy_token->pos = Pos{0, 0};

    // Create module
    auto* module = new ast::Module();
    module->name = module_name;
    module->id_path = module_name;
    module->kind = ast::ModuleKind::XC;  // Manual memory
    module->path = "<virtual:" + module_name + ">";
    module->filename = "<virtual:" + module_name + ">";

    // Create a virtual package for this module
    auto* package = ctx->add_package("<virtual>");
    module->package = package;

    // Create root node
    module->root = ctx->create_node(ast::NodeType::Root);
    module->root->data.root.top_level_decls = {};
    module->root->module = module;
    module->root->token = dummy_token;
    module->root->start_token = dummy_token;
    module->root->end_token = dummy_token;

    // Create module scope
    module->scope = ctx->create_scope(nullptr);
    module->import_scope = ctx->create_scope(module->scope);

    // Create extern "C" block to wrap all function declarations
    auto* extern_decl = ctx->create_node(ast::NodeType::ExternDecl);
    extern_decl->module = module;
    extern_decl->token = dummy_token;
    extern_decl->start_token = dummy_token;
    extern_decl->end_token = dummy_token;

    // Create token for extern language type ("C")
    auto* extern_type_token = ctx->create_token();
    extern_type_token->type = TokenType::STRING;
    extern_type_token->str = "C";
    extern_decl->data.extern_decl.type = extern_type_token;
    extern_decl->data.extern_decl.members = {};

    // Create extern function declarations
    for (const auto& func : functions) {
            // Create function name token
            auto* name_token = ctx->create_token();
            name_token->type = TokenType::IDEN;
            name_token->str = func.name;

            // Create FnProto node
            auto* fn_proto = ctx->create_node(ast::NodeType::FnProto);
            fn_proto->data.fn_proto.params = {};
            fn_proto->data.fn_proto.return_type = nullptr;
            fn_proto->data.fn_proto.is_vararg = false;  // TODO: handle variadic

            // Parse return type
            ChiType* return_type = parse_c_type(ctx, func.return_type);

            // Create parameters
            for (const auto& [param_type_str, param_name] : func.params) {
                auto* param = ctx->create_node(ast::NodeType::ParamDecl);

                // Create param name token
                auto* param_token = ctx->create_token();
                param_token->type = TokenType::IDEN;
                param_token->str = param_name.empty() ? "_" : param_name;
                param->name = param_token->str;
                param->token = param_token;

                // Parse param type
                ChiType* param_type = parse_c_type(ctx, param_type_str);
                param->resolved_type = param_type;

                fn_proto->data.fn_proto.params.add(param);
            }

            // Create function type
            auto* fn_type = ctx->create_type(TypeKind::Fn);
            fn_type->data.fn.return_type = return_type;
            fn_type->data.fn.params = {};
            for (auto* param : fn_proto->data.fn_proto.params) {
                fn_type->data.fn.params.add(param->resolved_type);
            }
            fn_type->data.fn.is_extern = true;
            fn_type->data.fn.is_variadic = false;

            // Create FnDef node
            auto* fn_def = ctx->create_node(ast::NodeType::FnDef);
            fn_def->name = func.name;
            fn_def->global_id = func.name;  // Use C linkage - no mangling
            fn_def->module = module;
            fn_def->token = name_token;
            fn_def->start_token = name_token;
            fn_def->end_token = name_token;
            fn_def->data.fn_def.fn_proto = fn_proto;
            fn_def->data.fn_def.body = nullptr;  // No body for extern
            fn_def->data.fn_def.decl_spec = ctx->create_decl_spec();
            fn_def->data.fn_def.decl_spec->flags = ast::DECL_EXTERN;  // Extern, public by default
            fn_def->resolved_type = fn_type;
            fn_proto->data.fn_proto.fn_def_node = fn_def;

            // Add to extern block members (not top-level directly)
            extern_decl->data.extern_decl.members.add(fn_def);

            // Add to exports and scope
            module->exports.add(fn_def);
            module->scope->put(func.name, fn_def);
    }

    // Add extern block to module as top-level declaration
    module->root->data.root.top_level_decls.add(extern_decl);

    // Create const declarations for enum constants
    for (const auto& constant : enum_constants) {
        auto* const_node = ctx->create_node(ast::NodeType::VarDecl);
        const_node->name = constant.name;
        const_node->module = module;
        const_node->resolved_type = ctx->resolve_ctx.system_types.int32;

        // Create const value token
        auto* value_token = ctx->create_token();
        value_token->type = TokenType::INT;
        value_token->val.i = constant.value;

        // Create literal expression
        auto* value_node = ctx->create_node(ast::NodeType::LiteralExpr);
        value_node->token = value_token;
        value_node->module = module;
        value_node->resolved_type = ctx->resolve_ctx.system_types.int32;

        const_node->data.var_decl.expr = value_node;
        const_node->data.var_decl.decl_spec = ctx->create_decl_spec();
        const_node->data.var_decl.is_const = true;

        // Add to module and scope
        module->root->data.root.top_level_decls.add(const_node);
        module->exports.add(const_node);
        module->scope->put(constant.name, const_node);
    }

    return module;
}

} // namespace cx
