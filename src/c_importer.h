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
};

struct CEnumConstant {
    std::string name;
    int64_t value;
};

struct CImportConfig {
    std::vector<std::string> includes;
    std::vector<std::string> symbols;  // Filter: only extract these symbols
    std::vector<std::string> include_paths;
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

    // Get error message if import failed
    const std::string& get_error() const { return error_; }

private:
#ifdef __clang__
    CXIndex index_;
    CXTranslationUnit tu_;
#endif

    std::vector<CFunction> functions_;
    std::vector<CEnumConstant> enum_constants_;
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
#endif
};

// Create a virtual Chi module from extracted C symbols
cx::ast::Module* create_native_module(
    cx::CompilationContext* ctx,
    const std::string& module_name,
    const std::vector<CFunction>& functions,
    const std::vector<CEnumConstant>& enum_constants
);

} // namespace cx
