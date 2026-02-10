#pragma once

#ifdef __clang__
#include <clang-c/Index.h>
#endif

#include <string>
#include <vector>
#include <set>
#include <map>

// Forward declarations
namespace cx {
    namespace ast {
        struct Module;
    }
    struct CompilationContext;

struct CFunction {
    std::string name;
    std::string return_type;
    std::vector<std::pair<std::string, std::string>> params;  // (type, name)
    bool is_variadic = false;
};

struct CEnumConstant {
    std::string name;
    int64_t value;
};

struct CMacro {
    std::string name;
    std::string value;  // String representation of the macro value
};

struct CStructField {
    std::string name;
    std::string type;
};

struct CStruct {
    std::string name;
    std::vector<CStructField> fields;
};

struct CTypedef {
    std::string name;
    std::string underlying_type;
};

struct CImportConfig {
    std::vector<std::string> includes;
    std::vector<std::string> symbols;  // Filter: only extract these symbols (supports patterns like "str*")
    std::vector<std::string> include_paths;
};

// Cache for C header modules - tracks what symbols have been extracted
// We re-parse headers when needed (libclang caches internally) but only extract requested symbols
struct CHeaderCache {
    cx::ast::Module* module = nullptr;
    std::set<std::string> extracted_symbols;  // Track what we've extracted already
};

class CImporter {
public:
    CImporter();
    ~CImporter();

    // Import C header and extract symbols matching the filter
    bool import_header(const std::string& header_path, const CImportConfig& config);

    // Get extracted functions
    const std::vector<CFunction>& get_functions() const { return functions_; }

    // Get extracted enum constants
    const std::vector<CEnumConstant>& get_enum_constants() const { return enum_constants_; }

    // Get extracted macros
    const std::vector<CMacro>& get_macros() const { return macros_; }

    // Get extracted structs
    const std::vector<CStruct>& get_structs() const { return structs_; }

    // Get extracted typedefs
    const std::vector<CTypedef>& get_typedefs() const { return typedefs_; }

    // Get error message if import failed
    const std::string& get_error() const { return error_; }

    // Import a header by name (e.g., "string.h") using in-memory wrapper
    // This avoids creating temporary files on disk
    bool import_header_by_name(const std::string& header_name, const CImportConfig& config);

#ifdef __clang__
    // Extract symbols from an already-parsed translation unit
    // Used for lazy/incremental symbol extraction
    void extract_symbols_from_tu(CXTranslationUnit tu, const std::vector<std::string>& symbol_patterns);
#endif

private:
#ifdef __clang__
    CXIndex index_;
    CXTranslationUnit tu_;
#endif

    std::vector<CFunction> functions_;
    std::vector<CEnumConstant> enum_constants_;
    std::vector<CMacro> macros_;
    std::vector<CStruct> structs_;
    std::vector<CTypedef> typedefs_;
    std::set<std::string> symbol_filter_;
    std::string error_;

#ifdef __clang__
    // Helper to convert CXString to std::string
    static std::string to_string(CXString cx_str);

    // Visitor callback for AST traversal
    static CXChildVisitResult visitor_callback(CXCursor cursor, CXCursor parent, CXClientData client_data);

    // Process a function declaration
    void process_function(CXCursor cursor);

    // Process an enum constant declaration
    void process_enum_constant(CXCursor cursor);

    // Process a macro definition
    void process_macro(CXCursor cursor);

    // Process a struct declaration
    void process_struct(CXCursor cursor);

    // Process a typedef declaration
    void process_typedef(CXCursor cursor);
#endif
};

// Create a virtual Chi module from extracted C symbols
cx::ast::Module* create_native_module(
    cx::CompilationContext* ctx,
    const std::string& module_name,
    const std::vector<CFunction>& functions,
    const std::vector<CEnumConstant>& enum_constants,
    const std::vector<CMacro>& macros,
    const std::vector<CStruct>& structs,
    const std::vector<CTypedef>& typedefs
);

// Get or create a module-scoped virtual "C" module
// If module_key already exists in module_map, returns existing module
// Otherwise creates a new empty virtual module
cx::ast::Module* get_or_create_c_module(
    cx::CompilationContext* ctx,
    const std::string& module_key
);

// Add extracted C symbols to an existing module
void add_symbols_to_module(
    cx::CompilationContext* ctx,
    cx::ast::Module* module,
    const std::vector<CFunction>& functions,
    const std::vector<CEnumConstant>& enum_constants,
    const std::vector<CMacro>& macros,
    const std::vector<CStruct>& structs,
    const std::vector<CTypedef>& typedefs
);

// High-level function to import C header symbols and add to virtual module
// Uses lazy extraction: only extracts symbols matching the patterns
// Caches parsed CXTranslationUnit for incremental extraction
// Returns nullptr on failure
// If out_newly_created is not null, sets it to true if module was newly created, false if cached
cx::ast::Module* import_c_header_as_module(
    cx::CompilationContext* ctx,
    const std::string& header_name,
    const std::vector<std::string>& symbol_patterns,  // Patterns like "strlen", "str*"
    const std::vector<std::string>& include_directories,
    bool* out_newly_created = nullptr
);

} // namespace cx
