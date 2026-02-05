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

    // Get extracted macros
    const std::vector<CMacro>& get_macros() const { return macros_; }

    // Get extracted structs
    const std::vector<CStruct>& get_structs() const { return structs_; }

    // Get extracted typedefs
    const std::vector<CTypedef>& get_typedefs() const { return typedefs_; }

    // Get error message if import failed
    const std::string& get_error() const { return error_; }

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

} // namespace cx
