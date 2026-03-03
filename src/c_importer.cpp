#include "c_importer.h"
#include "context.h"
#include "ast.h"
#include "resolver.h"
#include <iostream>
#include <sstream>

namespace cx {

CImporter::CImporter()
#ifdef HAVE_LIBCLANG
    : index_(nullptr), tu_(nullptr)
#endif
{
#ifdef HAVE_LIBCLANG
    index_ = clang_createIndex(0, 0);
#endif
}

CImporter::~CImporter() {
#ifdef HAVE_LIBCLANG
    if (tu_) {
        clang_disposeTranslationUnit(tu_);
    }
    if (index_) {
        clang_disposeIndex(index_);
    }
#endif
}

bool CImporter::import_header(const std::string& header_path, const CImportConfig& config) {
#ifndef HAVE_LIBCLANG
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

    // Parse the header with detailed preprocessing to capture macros
    tu_ = clang_parseTranslationUnit(
        index_,
        header_path.c_str(),
        args.data(), args.size(),
        nullptr, 0,
        CXTranslationUnit_DetailedPreprocessingRecord
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

bool CImporter::import_header_by_name(const std::string& header_name, const CImportConfig& config) {
#ifndef HAVE_LIBCLANG
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

    // Create wrapper content in memory (no disk I/O!)
    std::string wrapper_content;
    bool is_system = header_name.find('/') == std::string::npos;
    if (is_system) {
        wrapper_content = "#include <" + header_name + ">\n";
    } else {
        wrapper_content = "#include \"" + header_name + "\"\n";
    }

    // Use unsaved file for in-memory wrapper
    CXUnsavedFile unsaved_file;
    unsaved_file.Filename = "virtual_wrapper.h";
    unsaved_file.Contents = wrapper_content.c_str();
    unsaved_file.Length = wrapper_content.size();

    // Parse the virtual wrapper with detailed preprocessing to capture macros
    tu_ = clang_parseTranslationUnit(
        index_,
        "virtual_wrapper.h",  // Virtual filename
        args.data(), args.size(),
        &unsaved_file, 1,  // Provide wrapper in memory!
        CXTranslationUnit_DetailedPreprocessingRecord
    );

    if (!tu_) {
        error_ = "Failed to parse header: " + header_name;
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

#ifdef HAVE_LIBCLANG
void CImporter::extract_symbols_from_tu(CXTranslationUnit tu, const std::vector<std::string>& symbol_patterns) {
    // Clear previous extractions
    functions_.clear();
    enum_constants_.clear();
    macros_.clear();
    structs_.clear();
    typedefs_.clear();

    // Set up symbol filter
    symbol_filter_.clear();
    for (const auto& pattern : symbol_patterns) {
        symbol_filter_.insert(pattern);
    }

    // Visit AST and extract matching symbols
    CXCursor cursor = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(cursor, visitor_callback, this);
}

std::string CImporter::to_string(CXString cx_str) {
    std::string result = clang_getCString(cx_str);
    clang_disposeString(cx_str);
    return result;
}

// Check if a name matches a pattern (supports * wildcard)
static bool matches_pattern(const std::string& name, const std::string& pattern) {
    size_t name_idx = 0;
    size_t pattern_idx = 0;
    size_t star_idx = std::string::npos;
    size_t match_idx = 0;

    while (name_idx < name.length()) {
        if (pattern_idx < pattern.length() && pattern[pattern_idx] == '*') {
            // Remember the position of * and the current match position
            star_idx = pattern_idx;
            match_idx = name_idx;
            pattern_idx++;
        } else if (pattern_idx < pattern.length() &&
                   (pattern[pattern_idx] == name[name_idx] || pattern[pattern_idx] == '?')) {
            // Characters match or pattern has ?
            name_idx++;
            pattern_idx++;
        } else if (star_idx != std::string::npos) {
            // No match, but we have a * - backtrack
            pattern_idx = star_idx + 1;
            match_idx++;
            name_idx = match_idx;
        } else {
            // No match and no * to fall back on
            return false;
        }
    }

    // Skip any trailing * in pattern
    while (pattern_idx < pattern.length() && pattern[pattern_idx] == '*') {
        pattern_idx++;
    }

    return pattern_idx == pattern.length();
}

CXChildVisitResult CImporter::visitor_callback(CXCursor cursor, CXCursor parent, CXClientData client_data) {
    auto* importer = static_cast<CImporter*>(client_data);
    CXCursorKind kind = clang_getCursorKind(cursor);
    std::string name = to_string(clang_getCursorSpelling(cursor));

    // Check filter (empty filter means accept all)
    bool passes_filter = importer->symbol_filter_.empty();
    if (!passes_filter) {
        // Check if name matches any pattern in the filter
        for (const auto& pattern : importer->symbol_filter_) {
            if (matches_pattern(name, pattern)) {
                passes_filter = true;
                break;
            }
        }
    }

    switch (kind) {
    case CXCursor_FunctionDecl:
        if (passes_filter) {
            importer->process_function(cursor);
        }
        break;

    case CXCursor_EnumConstantDecl:
        if (passes_filter) {
            importer->process_enum_constant(cursor);
        }
        break;

    case CXCursor_MacroDefinition:
        if (passes_filter) {
            importer->process_macro(cursor);
        }
        break;

    case CXCursor_StructDecl:
        if (passes_filter && !name.empty()) {  // Skip anonymous structs
            importer->process_struct(cursor);
        }
        return CXChildVisit_Continue;  // Don't recurse into struct members here

    case CXCursor_TypedefDecl:
        if (passes_filter) {
            importer->process_typedef(cursor);
        }
        break;

    default:
        break;
    }

    return CXChildVisit_Recurse;
}

void CImporter::process_function(CXCursor cursor) {
    CFunction func;
    func.name = to_string(clang_getCursorSpelling(cursor));

    CXType func_type = clang_getCursorType(cursor);
    CXType return_type = clang_getResultType(func_type);
    // Resolve typedefs to get the actual underlying type
    CXType canonical_return = clang_getCanonicalType(return_type);
    func.return_type = to_string(clang_getTypeSpelling(canonical_return));

    // Check if function is variadic
    func.is_variadic = clang_isFunctionTypeVariadic(func_type) != 0;

    // Extract parameters
    int num_args = clang_Cursor_getNumArguments(cursor);
    for (int i = 0; i < num_args; i++) {
        CXCursor arg = clang_Cursor_getArgument(cursor, i);
        CXType arg_type = clang_getCursorType(arg);
        // Resolve typedefs to get the actual underlying type
        CXType canonical_arg = clang_getCanonicalType(arg_type);
        std::string arg_type_str = to_string(clang_getTypeSpelling(canonical_arg));
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

void CImporter::process_macro(CXCursor cursor) {
    CMacro macro;
    macro.name = to_string(clang_getCursorSpelling(cursor));

    // Get macro tokens to extract the value
    CXSourceRange range = clang_getCursorExtent(cursor);
    CXToken* tokens = nullptr;
    unsigned num_tokens = 0;
    clang_tokenize(tu_, range, &tokens, &num_tokens);

    // Skip the macro name token and extract the value
    std::string value;
    for (unsigned i = 1; i < num_tokens; i++) {
        std::string token_str = to_string(clang_getTokenSpelling(tu_, tokens[i]));
        if (!value.empty()) value += " ";
        value += token_str;
    }

    clang_disposeTokens(tu_, tokens, num_tokens);

    // Only keep macros that have simple numeric/string values
    if (!value.empty()) {
        macro.value = value;
        macros_.push_back(macro);
    }
}

void CImporter::process_struct(CXCursor cursor) {
    CStruct struct_decl;
    struct_decl.name = to_string(clang_getCursorSpelling(cursor));

    // Visit struct fields
    clang_visitChildren(cursor, [](CXCursor c, CXCursor parent, CXClientData client_data) -> CXChildVisitResult {
        auto* struct_ptr = static_cast<CStruct*>(client_data);

        if (clang_getCursorKind(c) == CXCursor_FieldDecl) {
            CStructField field;
            field.name = to_string(clang_getCursorSpelling(c));
            // Resolve typedefs to get the actual underlying type
            CXType field_type = clang_getCursorType(c);
            CXType canonical_type = clang_getCanonicalType(field_type);
            field.type = to_string(clang_getTypeSpelling(canonical_type));
            struct_ptr->fields.push_back(field);
        }

        return CXChildVisit_Continue;
    }, &struct_decl);

    structs_.push_back(struct_decl);
}

void CImporter::process_typedef(CXCursor cursor) {
    CTypedef typedef_decl;
    typedef_decl.name = to_string(clang_getCursorSpelling(cursor));

    CXType underlying = clang_getTypedefDeclUnderlyingType(cursor);
    // Resolve nested typedefs to get the actual underlying type
    CXType canonical = clang_getCanonicalType(underlying);
    typedef_decl.underlying_type = to_string(clang_getTypeSpelling(canonical));

    typedefs_.push_back(typedef_decl);
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
    while ((pos = type_str.find("restrict")) != std::string::npos) {
        type_str.erase(pos, 8);
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
    if (type_str == "unsigned long" || type_str == "uint64_t" || type_str == "unsigned long long" || type_str == "size_t") {
        return resolve_ctx->system_types.uint64;
    }
    if (type_str == "char" || type_str == "signed char") {
        return resolve_ctx->system_types.byte_;
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
    const std::vector<CEnumConstant>& enum_constants,
    const std::vector<CMacro>& macros,
    const std::vector<CStruct>& structs,
    const std::vector<CTypedef>& typedefs
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
    module->kind = ast::ModuleKind::XS;  // Manual memory
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
    module->root->data.root.top_level_decls.add(extern_decl);

    // Add all symbols to the module (reuse add_symbols_to_module logic)
    add_symbols_to_module(ctx, module, functions, enum_constants, macros, structs, typedefs);

    return module;
}

// Get or create a module-scoped virtual "C" module
ast::Module* get_or_create_c_module(
    CompilationContext* ctx,
    const std::string& module_key
) {
    // Check if module already exists
    auto existing = ctx->module_map.get(module_key);
    if (existing) {
        return *existing;
    }

    // Create a dummy token for position info
    auto* dummy_token = ctx->create_token();
    dummy_token->type = TokenType::KW_IMPORT;
    dummy_token->str = "<virtual>";
    dummy_token->pos = Pos{0, 0};

    // Create new virtual "C" module
    auto* module = new ast::Module();
    module->name = "C";
    module->id_path = module_key;  // Use namespaced key as id_path
    module->kind = ast::ModuleKind::XS;  // Manual memory
    module->path = "<virtual:" + module_key + ">";
    module->filename = "<virtual:" + module_key + ">";

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

    return module;
}

// Add extracted C symbols to an existing module
void add_symbols_to_module(
    CompilationContext* ctx,
    ast::Module* module,
    const std::vector<CFunction>& functions,
    const std::vector<CEnumConstant>& enum_constants,
    const std::vector<CMacro>& macros,
    const std::vector<CStruct>& structs,
    const std::vector<CTypedef>& typedefs
) {
    // Create a dummy token for position info
    auto* dummy_token = ctx->create_token();
    dummy_token->type = TokenType::KW_IMPORT;
    dummy_token->str = "<virtual>";
    dummy_token->pos = Pos{0, 0};

    // Find or create the extern "C" block in the module
    ast::Node* extern_decl = nullptr;
    for (auto* decl : module->root->data.root.top_level_decls) {
        if (decl->type == ast::NodeType::ExternDecl) {
            extern_decl = decl;
            break;
        }
    }

    if (!extern_decl) {
        // Create extern "C" block to wrap function declarations
        extern_decl = ctx->create_node(ast::NodeType::ExternDecl);
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

        module->root->data.root.top_level_decls.add(extern_decl);
    }

    // Create extern function declarations (same logic as create_native_module)
    for (const auto& func : functions) {
            // Create function name token
            auto* name_token = ctx->create_token();
            name_token->type = TokenType::IDEN;
            name_token->str = func.name;

            // Create FnProto node
            auto* fn_proto = ctx->create_node(ast::NodeType::FnProto);
            fn_proto->data.fn_proto.params = {};
            fn_proto->data.fn_proto.return_type = nullptr;
            fn_proto->data.fn_proto.is_vararg = func.is_variadic;

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
            fn_type->data.fn.is_variadic = func.is_variadic;

            // Create FnDef node
            auto* fn_def = ctx->create_node(ast::NodeType::FnDef);
            fn_def->name = func.name;
            // Extern C functions use just the function name (no module prefix) for C linkage
            fn_def->global_id = func.name;
            fn_def->module = module;
            fn_def->token = name_token;
            fn_def->start_token = name_token;
            fn_def->end_token = name_token;
            fn_def->data.fn_def.fn_proto = fn_proto;
            fn_def->data.fn_def.body = nullptr;  // No body for extern
            fn_def->data.fn_def.decl_spec = ctx->create_decl_spec();
            fn_def->data.fn_def.decl_spec->flags = ast::DECL_EXTERN;
            fn_def->resolved_type = fn_type;
            fn_proto->data.fn_proto.fn_def_node = fn_def;

            // Check if function already exists in scope (from previous import/export)
            auto existing = module->scope->find_one(func.name, false);
            if (!existing) {
                // Add to extern block members
                extern_decl->data.extern_decl.members.add(fn_def);

                // Add to exports, scope, and import_scope
                module->exports.add(fn_def);
                module->scope->put(func.name, fn_def);
                module->import_scope->put(func.name, fn_def);
            }
            // else: function already added from a previous import/export, skip
    }

    // Create const declarations for enum constants
    for (const auto& constant : enum_constants) {
        auto* const_node = ctx->create_node(ast::NodeType::VarDecl);
        const_node->name = constant.name;
        const_node->module = module;
        const_node->resolved_type = ctx->resolve_ctx.system_types.int32;

        auto* value_token = ctx->create_token();
        value_token->type = TokenType::INT;
        value_token->val.i = constant.value;

        auto* value_node = ctx->create_node(ast::NodeType::LiteralExpr);
        value_node->token = value_token;
        value_node->module = module;
        value_node->resolved_type = ctx->resolve_ctx.system_types.int32;

        const_node->data.var_decl.expr = value_node;
        const_node->data.var_decl.decl_spec = ctx->create_decl_spec();
        const_node->data.var_decl.kind = ast::VarKind::Constant;

        // Check if already exists before adding
        if (!module->scope->find_one(constant.name, false)) {
            module->root->data.root.top_level_decls.add(const_node);
            module->exports.add(const_node);
            module->scope->put(constant.name, const_node);
            module->import_scope->put(constant.name, const_node);
        }
    }

    // Create const declarations for macro constants
    for (const auto& macro : macros) {
        std::string value = macro.value;

        // Detect and strip integer suffixes
        bool is_unsigned = false;
        bool is_long = false;
        bool is_long_long = false;

        if (value.size() >= 3) {
            std::string suffix = value.substr(value.size() - 3);
            std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);
            if (suffix == "ull") {
                is_unsigned = true;
                is_long_long = true;
                value.erase(value.size() - 3);
            }
        }
        if (!is_long_long && value.size() >= 2) {
            std::string suffix = value.substr(value.size() - 2);
            std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);
            if (suffix == "ul" || suffix == "lu") {
                is_unsigned = true;
                is_long = true;
                value.erase(value.size() - 2);
            } else if (suffix == "ll") {
                is_long_long = true;
                value.erase(value.size() - 2);
            }
        }
        if (!is_unsigned && !is_long && !is_long_long && !value.empty()) {
            char last = value.back();
            if (last == 'u' || last == 'U') {
                is_unsigned = true;
                value.pop_back();
            } else if (last == 'l' || last == 'L') {
                is_long = true;
                value.pop_back();
            }
        }

        char* endptr;
        long long int_value = std::strtoll(value.c_str(), &endptr, 0);

        if (*endptr == '\0' || *endptr == ' ') {
            auto* const_node = ctx->create_node(ast::NodeType::VarDecl);
            const_node->name = macro.name;
            const_node->module = module;

            ChiType* type;
            if (is_long_long) {
                type = is_unsigned ? ctx->resolve_ctx.system_types.uint64
                                   : ctx->resolve_ctx.system_types.int64;
            } else if (is_long) {
                type = is_unsigned ? ctx->resolve_ctx.system_types.uint64
                                   : ctx->resolve_ctx.system_types.int64;
            } else {
                type = is_unsigned ? ctx->resolve_ctx.system_types.uint32
                                   : ctx->resolve_ctx.system_types.int32;
            }
            const_node->resolved_type = type;

            auto* value_token = ctx->create_token();
            value_token->type = TokenType::INT;
            value_token->val.i = int_value;

            auto* value_node = ctx->create_node(ast::NodeType::LiteralExpr);
            value_node->token = value_token;
            value_node->module = module;
            value_node->resolved_type = type;

            const_node->data.var_decl.expr = value_node;
            const_node->data.var_decl.decl_spec = ctx->create_decl_spec();
            const_node->data.var_decl.kind = ast::VarKind::Constant;

            // Check if already exists before adding
            if (!module->scope->find_one(macro.name, false)) {
                module->root->data.root.top_level_decls.add(const_node);
                module->exports.add(const_node);
                module->scope->put(macro.name, const_node);
                module->import_scope->put(macro.name, const_node);
            }
        }
    }

    // Create struct declarations
    for (const auto& c_struct : structs) {
        // Check if already exists before adding
        if (module->scope->find_one(c_struct.name, false)) {
            continue;
        }

        auto* struct_decl = ctx->create_node(ast::NodeType::StructDecl);
        struct_decl->name = c_struct.name;
        struct_decl->module = module;
        struct_decl->data.struct_decl.kind = ContainerKind::Struct;
        struct_decl->data.struct_decl.decl_spec = ctx->create_decl_spec();
        struct_decl->data.struct_decl.decl_spec->flags = ast::DECL_EXTERN;

        for (const auto& field : c_struct.fields) {
            auto* field_node = ctx->create_node(ast::NodeType::VarDecl);
            field_node->name = field.name;
            field_node->module = module;
            field_node->parent = struct_decl;

            auto* field_token = ctx->create_token();
            field_token->type = TokenType::IDEN;
            field_token->str = field.name;
            field_node->token = field_token;

            field_node->resolved_type = parse_c_type(ctx, field.type);
            field_node->data.var_decl.is_field = true;

            struct_decl->data.struct_decl.members.add(field_node);
        }

        module->root->data.root.top_level_decls.add(struct_decl);
        module->exports.add(struct_decl);
        module->scope->put(c_struct.name, struct_decl);
        module->import_scope->put(c_struct.name, struct_decl);
    }

    // Create typedef declarations
    for (const auto& c_typedef : typedefs) {
        // Check if already exists before adding
        if (module->scope->find_one(c_typedef.name, false)) {
            continue;
        }

        auto* typedef_node = ctx->create_node(ast::NodeType::TypedefDecl);
        typedef_node->name = c_typedef.name;
        typedef_node->module = module;

        auto* typedef_token = ctx->create_token();
        typedef_token->type = TokenType::IDEN;
        typedef_token->str = c_typedef.name;
        typedef_node->data.typedef_decl.identifier = typedef_token;

        typedef_node->resolved_type = parse_c_type(ctx, c_typedef.underlying_type);

        module->root->data.root.top_level_decls.add(typedef_node);
        module->exports.add(typedef_node);
        module->scope->put(c_typedef.name, typedef_node);
        module->import_scope->put(c_typedef.name, typedef_node);
    }
}

// High-level function to import C header symbols lazily
// Re-parses header when needed (libclang caches internally) but only extracts requested symbols
ast::Module* import_c_header_as_module(
    CompilationContext* ctx,
    const std::string& header_name,
    const std::vector<std::string>& symbol_patterns,
    const std::vector<std::string>& include_directories,
    bool* out_newly_created
) {
#ifndef HAVE_LIBCLANG
    if (out_newly_created) *out_newly_created = false;
    return nullptr;
#else
    bool is_new_module = false;

    // Check if module already exists in this context
    auto* existing = ctx->module_map.get(header_name);
    if (!existing) {
        is_new_module = true;
        auto* module = create_native_module(ctx, header_name, {}, {}, {}, {}, {});
        ctx->module_map[header_name] = module;
    }

    if (out_newly_created) *out_newly_created = is_new_module;

    auto* module = *ctx->module_map.get(header_name);

    // Determine which NEW symbols need to be extracted
    auto* extracted = ctx->header_extracted_symbols.get(header_name);
    std::vector<std::string> patterns_to_extract;
    for (const auto& pattern : symbol_patterns) {
        if (!extracted || !extracted->has_key(pattern)) {
            patterns_to_extract.push_back(pattern);
            ctx->header_extracted_symbols[header_name][pattern] = true;
        }
    }

    if (!patterns_to_extract.empty()) {
        // Re-parse header and extract only the new symbols
        // libclang caches parsed system headers internally, so this is fast
        CImporter importer;
        CImportConfig config;
        config.symbols = patterns_to_extract;
        config.include_paths = include_directories;

        if (importer.import_header_by_name(header_name, config)) {
            add_symbols_to_module(
                ctx, module,
                importer.get_functions(),
                importer.get_enum_constants(),
                importer.get_macros(),
                importer.get_structs(),
                importer.get_typedefs()
            );
        }
    }

    return module;
#endif
}

} // namespace cx
