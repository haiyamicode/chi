#include <assert.h>
#include <node/node_api.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

#define BOOST_NO_EXCEPTIONS
#include "../../analyzer.h"
#include "../../ast_printer.h"
#include "../../c_importer.h"
#include "../../package_config.h"
#include "../../include/boost/json/src.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ============================================================================
// UTF-8 / UTF-16 offset conversion
// ============================================================================

// Convert a UTF-16 code unit offset (as used by JS/LSP) to a UTF-8 byte offset.
static long utf16_to_byte_offset(const std::string &source, long utf16_offset) {
    long utf16_pos = 0;
    long byte_pos = 0;
    while (byte_pos < (long)source.size() && utf16_pos < utf16_offset) {
        auto c = (unsigned char)source[byte_pos];
        int seq_len;
        uint32_t codepoint;
        if (c < 0x80) {
            seq_len = 1;
            codepoint = c;
        } else if ((c & 0xE0) == 0xC0) {
            seq_len = 2;
            codepoint = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            seq_len = 3;
            codepoint = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            seq_len = 4;
            codepoint = c & 0x07;
        } else {
            byte_pos++;
            utf16_pos++;
            continue;
        }
        for (int i = 1; i < seq_len && byte_pos + i < (long)source.size(); i++) {
            codepoint = (codepoint << 6) | ((unsigned char)source[byte_pos + i] & 0x3F);
        }
        byte_pos += seq_len;
        utf16_pos += (codepoint >= 0x10000) ? 2 : 1;
    }
    return byte_pos;
}

// Convert a UTF-8 byte offset to a UTF-16 code unit offset.
static long byte_to_utf16_offset(const std::string &source, long byte_offset) {
    long utf16_pos = 0;
    long byte_pos = 0;
    while (byte_pos < byte_offset && byte_pos < (long)source.size()) {
        auto c = (unsigned char)source[byte_pos];
        int seq_len;
        uint32_t codepoint;
        if (c < 0x80) {
            seq_len = 1;
            codepoint = c;
        } else if ((c & 0xE0) == 0xC0) {
            seq_len = 2;
            codepoint = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            seq_len = 3;
            codepoint = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            seq_len = 4;
            codepoint = c & 0x07;
        } else {
            byte_pos++;
            utf16_pos++;
            continue;
        }
        for (int i = 1; i < seq_len && byte_pos + i < (long)source.size(); i++) {
            codepoint = (codepoint << 6) | ((unsigned char)source[byte_pos + i] & 0x3F);
        }
        byte_pos += seq_len;
        utf16_pos += (codepoint >= 0x10000) ? 2 : 1;
    }
    return utf16_pos;
}

// Convert a codepoint-based column to a UTF-16 column, given the source line.
static long col_to_utf16(const std::string &source, long line, long col) {
    // Find the start of the given line
    long line_start = 0;
    for (long l = 0; l < line && line_start < (long)source.size(); line_start++) {
        if (source[line_start] == '\n') l++;
    }
    // Walk codepoints and count UTF-16 code units
    long utf16_col = 0;
    long cp_count = 0;
    long pos = line_start;
    while (pos < (long)source.size() && cp_count < col) {
        auto c = (unsigned char)source[pos];
        int seq_len;
        uint32_t codepoint;
        if (c < 0x80) {
            seq_len = 1;
            codepoint = c;
        } else if ((c & 0xE0) == 0xC0) {
            seq_len = 2;
            codepoint = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            seq_len = 3;
            codepoint = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            seq_len = 4;
            codepoint = c & 0x07;
        } else {
            pos++;
            utf16_col++;
            cp_count++;
            continue;
        }
        for (int i = 1; i < seq_len && pos + i < (long)source.size(); i++) {
            codepoint = (codepoint << 6) | ((unsigned char)source[pos + i] & 0x3F);
        }
        pos += seq_len;
        utf16_col += (codepoint >= 0x10000) ? 2 : 1;
        cp_count++;
    }
    return utf16_col;
}

// ============================================================================
// Utilities
// ============================================================================

static void load_package_config(cx::ast::Package* package, const std::string& file_path) {
    if (!package) return;

    fs::path current = fs::path(file_path).parent_path();
    fs::path package_json_path;

    while (!current.empty()) {
        auto candidate = current / "package.jsonc";
        if (fs::exists(candidate)) {
            package_json_path = candidate;
            break;
        }
        auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }

    if (package_json_path.empty()) return;

    std::ifstream config_file(package_json_path);
    if (!config_file.is_open()) return;

    std::string config_content((std::istreambuf_iterator<char>(config_file)),
                               std::istreambuf_iterator<char>());
    config_file.close();

    std::error_code ec;
    boost::json::parse_options opts;
    opts.allow_comments = true;
    opts.allow_trailing_commas = true;
    auto config_json = boost::json::parse(config_content, ec, {}, opts);
    if (ec) return;

    package->config = new cx::PackageConfig(
        boost::json::value_to<cx::PackageConfig>(config_json));
}

static void crash_handler(int sig) {
    fprintf(stderr, "\n=== CRASH: signal %d ===\n", sig);
    void *bt[64];
    int n = backtrace(bt, 64);
    backtrace_symbols_fd(bt, n, 2);
    fprintf(stderr, "=== END CRASH ===\n");
    _exit(1);
}

__attribute__((constructor)) static void install_crash_handler() {
    signal(SIGSEGV, crash_handler);
    signal(SIGBUS, crash_handler);
    signal(SIGABRT, crash_handler);
}

// Parse the single string argument from a napi call
static bool parse_napi_arg(napi_env env, napi_callback_info info, std::string &out) {
    size_t argc = 1;
    napi_value args[1];
    if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok) return false;
    size_t len;
    if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) != napi_ok) return false;
    out.resize(len);
    if (napi_get_value_string_utf8(env, args[0], out.data(), len + 1, &len) != napi_ok) return false;
    return true;
}

// Serialize a JSON value and return as napi string
static napi_value napi_json_result(napi_env env, const boost::json::value &json) {
    auto str = boost::json::serialize(json);
    napi_value result;
    if (napi_create_string_utf8(env, str.c_str(), str.size(), &result) != napi_ok)
        return nullptr;
    return result;
}

// Set up analyzer from common JSON fields (chiRoot, file)
static void init_analyzer(cx::Analyzer &analyzer, const boost::json::object &input,
                          std::string &input_file) {
    if (input.contains("chiRoot")) {
        analyzer.get_context()->root_path = input.at("chiRoot").as_string().c_str();
    }
    input_file = input.at("file").as_string().c_str();
}

// Process source through the full compiler pipeline (runtime + resolve + sema)
static cx::ast::Module* process_source(cx::Analyzer &analyzer, const std::string &input_file,
                                       cx::io::Buffer *src) {
    auto rt_path = analyzer.get_context()->get_stdlib_path("runtime.xs");
    bool is_runtime = false;
    std::error_code ec;
    if (fs::exists(input_file) && fs::exists(rt_path)) {
        auto canon_input = fs::canonical(input_file, ec);
        if (!ec) {
            auto canon_rt = fs::canonical(rt_path, ec);
            if (!ec) {
                is_runtime = canon_input == canon_rt;
            }
        }
    }
    if (is_runtime) {
        return analyzer.build_runtime_from_source(src);
    }

    analyzer.build_runtime();
    auto pkg = analyzer.get_context()->add_package(".");
    load_package_config(pkg, input_file);

    if (pkg->config && pkg->config->c_interop.has_value() &&
        !pkg->config->c_interop->native_modules.empty()) {
        auto* ctx = analyzer.get_context();
        for (const auto& [mod_name, mod_config] : pkg->config->c_interop->native_modules) {
            cx::CImporter importer;
            cx::CImportConfig import_config;
            import_config.symbols = mod_config.symbols;
            import_config.include_paths = mod_config.include_paths;

            for (const auto& header : mod_config.includes) {
                importer.import_header_by_name(header, import_config);
            }

            auto* virtual_module = cx::create_native_module(
                ctx, mod_name,
                importer.get_functions(),
                importer.get_enum_constants(),
                importer.get_macros(),
                importer.get_structs(),
                importer.get_typedefs()
            );

            auto mod_resolver = ctx->create_resolver();
            mod_resolver.resolve(virtual_module);
            ctx->module_map[mod_name] = virtual_module;
        }
    }

    return analyzer.process_source(pkg, src, input_file);
}

static boost::json::array collect_errors(cx::ast::Module *module, const std::string &source = "") {
    boost::json::array errors;
    if (!module) return errors;
    for (auto &error : module->errors) {
        boost::json::object e;
        e["message"] = error.message;
        if (!source.empty()) {
            e["offset"] = byte_to_utf16_offset(source, error.pos.offset);
        } else {
            e["offset"] = error.pos.offset;
        }
        e["range"] = error.range;
        errors.push_back(e);
    }
    return errors;
}

// ============================================================================
// Symbol info helpers
// ============================================================================

static std::string get_symbol_kind(cx::ast::Node *node) {
    if (node->type == cx::ast::NodeType::ImportSymbol && node->data.import_symbol.resolved_decl) {
        node = node->data.import_symbol.resolved_decl;
    }
    switch (node->type) {
    case cx::ast::NodeType::FnDef:
        return node->data.fn_def.fn_kind == cx::ast::FnKind::Method ? "method" : "function";
    case cx::ast::NodeType::StructDecl:
        return "struct";
    case cx::ast::NodeType::VarDecl:
        return node->data.var_decl.kind == cx::ast::VarKind::Constant ? "constant" : "variable";
    case cx::ast::NodeType::Primitive:
        return "interface";
    default:
        return "interface";
    }
}

static std::string format_fn_signature(cx::ast::Node *decl, cx::Resolver &resolver) {
    auto type = decl->resolved_type;
    if (!type || type->kind != cx::TypeKind::Fn) {
        return type ? resolver.format_type_display(type) : "unknown";
    }
    auto &fn = type->data.fn;
    auto *proto = decl->data.fn_def.fn_proto;
    auto &proto_params = proto->data.fn_proto.params;

    std::stringstream ss;
    ss << "func(";
    for (int i = 0; i < fn.params.len; i++) {
        auto *param_type = fn.params[i];
        if (fn.is_variadic && i == fn.params.len - 1) {
            ss << "...";
        }
        if (i < proto_params.len && !proto_params[i]->name.empty()) {
            ss << proto_params[i]->name << ": ";
        }
        if (fn.is_variadic && i == fn.params.len - 1) {
            if (auto *elem_type = fn.get_variadic_elem_type()) {
                ss << resolver.format_type_display(elem_type);
            } else {
                ss << resolver.format_type_display(param_type);
            }
        } else {
            ss << resolver.format_type_display(param_type);
        }
        if (i < fn.params.len - 1) {
            ss << ", ";
        }
    }
    ss << ")";
    if (fn.return_type && fn.return_type->kind != cx::TypeKind::Void) {
        ss << " " << resolver.format_type_display(fn.return_type);
    }
    return ss.str();
}

static std::string get_symbol_info(cx::ast::Node *decl, cx::Resolver &resolver) {
    if (decl->type == cx::ast::NodeType::ImportSymbol && decl->data.import_symbol.resolved_decl) {
        decl = decl->data.import_symbol.resolved_decl;
    }
    // Handle 'this' keyword
    if (decl->type == cx::ast::NodeType::Identifier &&
        decl->data.identifier.kind == cx::ast::IdentifierKind::This) {
        auto type = decl->resolved_type;
        return fmt::format("(variable) this: {}",
                           type ? resolver.format_type_display(type) : "unknown");
    }
    auto kind = get_symbol_kind(decl);
    auto name = decl->name.size() ? decl->name : "<anonymous>";
    if (decl->type == cx::ast::NodeType::FnDef) {
        return fmt::format("({}) {}: {}", kind, name, format_fn_signature(decl, resolver));
    }
    auto type = decl->resolved_type;
    return fmt::format("({}) {}: {}", kind, name,
                       type ? resolver.format_type_display(type) : "unknown");
}

// ============================================================================
// Scan operation helpers
// ============================================================================

static cx::ast::Node *get_fn_decl_from_call(cx::ast::Node *call_node) {
    auto fn_ref = call_node->data.fn_call_expr.fn_ref_expr;
    if (!fn_ref) return nullptr;
    if (fn_ref->type == cx::ast::NodeType::Identifier && fn_ref->data.identifier.decl) {
        return fn_ref->data.identifier.decl;
    }
    if (fn_ref->type == cx::ast::NodeType::DotExpr && fn_ref->data.dot_expr.resolved_decl) {
        return fn_ref->data.dot_expr.resolved_decl;
    }
    return nullptr;
}

static boost::json::object build_signature_help(cx::ScanResult &result, cx::Resolver &resolver) {
    boost::json::object sig_help;
    auto call_node = result.fn_call;
    if (!call_node) return sig_help;

    auto fn_decl = get_fn_decl_from_call(call_node);
    if (!fn_decl || fn_decl->type != cx::ast::NodeType::FnDef) return sig_help;

    auto type = fn_decl->resolved_type;
    if (!type || type->kind != cx::TypeKind::Fn) return sig_help;

    auto &fn = type->data.fn;
    auto *proto = fn_decl->data.fn_def.fn_proto;
    auto &proto_params = proto->data.fn_proto.params;

    std::string label = fn_decl->name + "(";
    boost::json::array params_json;

    for (int i = 0; i < fn.params.len; i++) {
        auto *param_type = fn.params[i];
        auto param_start = label.size();
        if (fn.is_variadic && i == fn.params.len - 1) {
            label += "...";
        }
        if (i < proto_params.len && !proto_params[i]->name.empty()) {
            label += proto_params[i]->name + ": ";
        }
        if (fn.is_variadic && i == fn.params.len - 1) {
            if (auto *elem_type = fn.get_variadic_elem_type()) {
                label += resolver.format_type_display(elem_type);
            } else {
                label += resolver.format_type_display(param_type);
            }
        } else {
            label += resolver.format_type_display(param_type);
        }
        auto param_end = label.size();

        boost::json::object param;
        param["label"] = boost::json::array{(int64_t)param_start, (int64_t)param_end};
        params_json.push_back(param);

        if (i < fn.params.len - 1) {
            label += ", ";
        }
    }
    label += ")";
    if (fn.return_type && fn.return_type->kind != cx::TypeKind::Void) {
        label += " " + resolver.format_type_display(fn.return_type);
    }

    boost::json::object sig;
    sig["label"] = label;
    sig["parameters"] = params_json;

    sig_help["signatures"] = boost::json::array{sig};
    sig_help["activeSignature"] = 0;
    sig_help["activeParameter"] = result.active_param;
    return sig_help;
}

static boost::json::object build_construct_signature_help(cx::ScanResult &result,
                                                          cx::Resolver &resolver) {
    boost::json::object sig_help;
    auto *node = result.construct_expr;
    if (!node)
        return sig_help;

    auto *resolved = node->resolved_type;
    if (!resolved)
        return sig_help;
    auto *struct_ = resolver.resolve_struct_type(resolved);
    if (!struct_)
        return sig_help;
    auto *constructor = struct_->get_constructor();
    if (!constructor || !constructor->resolved_type ||
        constructor->resolved_type->kind != cx::TypeKind::Fn)
        return sig_help;

    auto &fn = constructor->resolved_type->data.fn;
    auto *ctor_node = constructor->node;
    if (!ctor_node || ctor_node->type != cx::ast::NodeType::FnDef)
        return sig_help;
    auto *proto = ctor_node->data.fn_def.fn_proto;
    auto &proto_params = proto->data.fn_proto.params;

    // Build signature label
    std::string type_name = resolver.format_type_display(resolved);
    std::string label = type_name + "{";
    boost::json::array params_json;

    for (int i = 0; i < fn.params.len; i++) {
        auto param_start = label.size();
        if (i < proto_params.len && !proto_params[i]->name.empty()) {
            label += proto_params[i]->name + ": ";
        }
        label += resolver.format_type_display(fn.params[i]);
        auto param_end = label.size();

        boost::json::object param;
        param["label"] = boost::json::array{(int64_t)param_start, (int64_t)param_end};
        params_json.push_back(param);

        if (i < fn.params.len - 1) {
            label += ", ";
        }
    }
    label += "}";

    // Compute active parameter from cursor position vs construct items
    auto &data = node->data.construct_expr;
    int active_param = 0;
    for (int i = 0; i < data.items.len; i++) {
        auto item = data.items[i];
        auto item_start = item->start_token ? item->start_token : item->token;
        if (item_start && result.pos.offset >= item_start->pos.offset) {
            active_param = i;
        }
    }
    // Past all items (trailing comma) → next param
    if (data.items.len > 0) {
        auto last = data.items[data.items.len - 1];
        auto last_end = last->end_token ? last->end_token : last->token;
        if (last_end &&
            result.pos.offset > last_end->pos.offset + (long)last_end->to_string().size()) {
            active_param = data.items.len;
        }
    }

    boost::json::object sig;
    sig["label"] = label;
    sig["parameters"] = params_json;

    sig_help["signatures"] = boost::json::array{sig};
    sig_help["activeSignature"] = 0;
    sig_help["activeParameter"] = active_param;
    return sig_help;
}

static boost::json::array complete_dot(cx::ScanResult &result, cx::Resolver &resolver) {
    boost::json::array completions = {};
    assert(result.is_dot && result.dot_expr);
    auto struct_expr = result.dot_expr->data.dot_expr.expr;
    if (!struct_expr) {
        return completions;
    }
    auto expr_type = struct_expr->resolved_type;
    if (!expr_type) {
        return completions;
    }

    bool is_static = expr_type->kind == cx::TypeKind::TypeSymbol;

    if (is_static) {
        auto underlying = expr_type->data.type_symbol.underlying_type;
        if (underlying && underlying->kind == cx::TypeKind::Enum) {
            for (auto variant : underlying->data.enum_.variants) {
                boost::json::object completion;
                completion["label"] = variant->name;
                completion["kind"] = "Enum";
                completions.push_back(completion);
            }
            return completions;
        }
    }

    auto struct_ = resolver.resolve_struct_type(expr_type);
    if (!struct_) {
        return completions;
    }

    auto &members = is_static ? struct_->static_members : struct_->members;
    for (auto member : members) {
        auto name = member->get_name();
        if (!is_static && (name == "new" || name == "delete")) continue;
        boost::json::object completion;
        completion["label"] = member->get_name();
        completion["kind"] = member->is_method() ? "Method" : "Field";
        if (member->resolved_type) {
            completion["detail"] = resolver.format_type_display(member->resolved_type);
        }
        completions.push_back(completion);
    }
    return completions;
}

static boost::json::array complete_construct(cx::ScanResult &result, cx::Resolver &resolver,
                                              cx::ScopeResolver &scope_resolver,
                                              const std::string &source) {
    boost::json::array completions = {};
    auto *node = result.construct_expr;
    if (!node)
        return completions;

    auto &data = node->data.construct_expr;

    // Resolve the struct type from the construct expression
    auto *resolved = node->resolved_type;
    if (!resolved)
        return completions;
    auto *struct_ = resolver.resolve_struct_type(resolved);
    if (!struct_)
        return completions;

    // Collect already-provided field names
    std::set<std::string> provided;
    for (auto fi : data.field_inits) {
        provided.insert(fi->data.field_init_expr.field->str);
    }

    // Check if cursor is right after ':' for shorthand
    bool is_colon = false;
    if (result.pos.offset > 0 && result.pos.offset <= source.size()) {
        is_colon = source[result.pos.offset - 1] == ':';
    }

    // For shorthand ':' completions, collect local symbols in scope
    std::set<std::string> local_symbols;
    if (is_colon && result.scope) {
        for (auto symbol : scope_resolver.get_all_symbols(result.scope)) {
            local_symbols.insert(symbol->name);
        }
    }

    for (auto member : struct_->members) {
        if (member->is_method())
            continue;
        auto name = member->get_name();
        if (provided.count(name))
            continue;

        if (is_colon) {
            // Only suggest shorthand if a matching local symbol exists
            if (!local_symbols.count(name))
                continue;
            boost::json::object completion;
            completion["label"] = name;
            completion["kind"] = "Field";
            if (member->resolved_type) {
                completion["detail"] = resolver.format_type_display(member->resolved_type);
            }
            completion["insertText"] = name;
            completions.push_back(completion);
        } else {
            boost::json::object completion;
            completion["label"] = name;
            completion["kind"] = "Field";
            if (member->resolved_type) {
                completion["detail"] = resolver.format_type_display(member->resolved_type);
            }
            completion["insertText"] = name + ": ";
            completions.push_back(completion);
        }
    }
    return completions;
}

static boost::json::object build_definition(cx::ScanResult &result) {
    boost::json::object def_link;
    if (!result.decl) return def_link;

    auto start_tok = result.decl->start_token ? result.decl->start_token : result.decl->token;
    if (!start_tok || !result.decl->module) return def_link;

    auto start_pos = start_tok->pos;
    auto end_pos = result.decl->end_token ? result.decl->end_token->pos : start_pos.add_line(1);

    auto start = boost::json::object({{"character", start_pos.col - 1}, {"line", start_pos.line}});
    auto end = boost::json::object({{"character", end_pos.col}, {"line", end_pos.line}});
    auto range = boost::json::object({{"start", start}, {"end", end}});

    def_link["targetUri"] = fmt::format("file://{}", result.decl->module->full_path());
    def_link["targetSelectionRange"] = range;
    def_link["targetRange"] = range;
    return def_link;
}

// ============================================================================
// Semantic tokens
// ============================================================================

enum SemanticTokenType {
    ST_Namespace = 0, ST_Type = 1, ST_Function = 2, ST_Method = 3,
    ST_Variable = 4, ST_Parameter = 5, ST_Property = 6, ST_EnumMember = 7,
    ST_Keyword = 8, ST_Number = 9, ST_String = 10, ST_Operator = 11, ST_Decorator = 12,
    ST_Modifier = 13,
};
enum SemanticTokenModifier {
    SM_Declaration = 1 << 0, SM_Readonly = 1 << 1,
    SM_Static = 1 << 2, SM_Async = 1 << 3,
};

static boost::json::array generate_semantic_tokens(cx::ast::Module *module) {
    using namespace cx::ast;
    boost::json::array result;
    if (!module) return result;

    for (auto tok : module->tokens) {
        if (tok->pos.line < 0) continue;

        // Emit keyword tokens directly
        if (tok->type >= cx::TokenType::KW_BREAK && tok->type < cx::TokenType::BOOL) {
            auto len = tok->str.size();
            if (len == 0) continue;
            // Storage modifiers get a distinct token type (blue in Dark+)
            auto st = ST_Keyword;
            if (tok->type == cx::TokenType::KW_MUT ||
                tok->type == cx::TokenType::KW_MUTEX ||
                tok->type == cx::TokenType::KW_STATIC ||
                tok->type == cx::TokenType::KW_PRIVATE ||
                tok->type == cx::TokenType::KW_PROTECTED ||
                tok->type == cx::TokenType::KW_UNSAFE) {
                st = ST_Modifier;
            }
            result.push_back(tok->pos.line);
            result.push_back(tok->pos.col - 1);
            result.push_back((int64_t)len);
            result.push_back(st);
            result.push_back(0);
            continue;
        }

        if (tok->type != cx::TokenType::IDEN && tok->type != cx::TokenType::KW_THIS_TYPE)
            continue;

        auto node = tok->node ? tok->node : tok->semantic_node;
        if (!node) continue;

        int token_type = -1;
        int modifiers = 0;

        if (node->type == NodeType::ImportSymbol && node->data.import_symbol.resolved_decl) {
            node = node->data.import_symbol.resolved_decl;
        }

        switch (node->type) {
        case NodeType::Identifier: {
            auto kind = node->data.identifier.kind;
            if (kind == IdentifierKind::TypeName || kind == IdentifierKind::ThisType) {
                token_type = ST_Type;
            } else if (kind == IdentifierKind::This) {
                continue;
            } else {
                auto decl = node->data.identifier.decl;
                if (!decl) {
                    token_type = ST_Variable;
                } else {
                    switch (decl->type) {
                    case NodeType::ParamDecl:
                        token_type = ST_Parameter;
                        break;
                    case NodeType::VarDecl:
                        token_type = decl->data.var_decl.is_field ? ST_Property : ST_Variable;
                        if (decl->data.var_decl.kind != VarKind::Mutable)
                            modifiers |= SM_Readonly;
                        break;
                    case NodeType::FnDef: {
                        auto fk = decl->data.fn_def.fn_kind;
                        token_type = (fk == FnKind::Method || fk == FnKind::Constructor ||
                                      fk == FnKind::Destructor)
                                         ? ST_Method : ST_Function;
                        break;
                    }
                    case NodeType::EnumVariant:
                        token_type = ST_EnumMember;
                        break;
                    case NodeType::StructDecl:
                    case NodeType::EnumDecl:
                    case NodeType::TypedefDecl:
                    case NodeType::Primitive:
                        token_type = ST_Type;
                        break;
                    default:
                        token_type = ST_Variable;
                        break;
                    }
                }
            }
            break;
        }
        case NodeType::FnDef: {
            auto fk = node->data.fn_def.fn_kind;
            token_type = (fk == FnKind::Method || fk == FnKind::Constructor ||
                          fk == FnKind::Destructor)
                             ? ST_Method : ST_Function;
            modifiers |= SM_Declaration;
            auto spec = node->data.fn_def.decl_spec;
            if (spec) {
                if (spec->is_static()) modifiers |= SM_Static;
                if (spec->is_async()) modifiers |= SM_Async;
            }
            break;
        }
        case NodeType::VarDecl:
            token_type = node->data.var_decl.is_field ? ST_Property : ST_Variable;
            modifiers |= SM_Declaration;
            if (node->data.var_decl.kind != VarKind::Mutable)
                modifiers |= SM_Readonly;
            break;
        case NodeType::ParamDecl:
            token_type = ST_Parameter;
            modifiers |= SM_Declaration;
            break;
        case NodeType::StructDecl:
            token_type = ST_Type;
            modifiers |= SM_Declaration;
            break;
        case NodeType::EnumDecl:
            token_type = ST_Type;
            modifiers |= SM_Declaration;
            break;
        case NodeType::EnumVariant:
            token_type = ST_EnumMember;
            modifiers |= SM_Declaration;
            break;
        case NodeType::TypedefDecl:
            token_type = ST_Type;
            modifiers |= SM_Declaration;
            break;
        case NodeType::ImportDecl:
        case NodeType::ImportSymbol:
            token_type = ST_Namespace;
            break;
        case NodeType::DotExpr: {
            auto resolved = node->data.dot_expr.resolved_decl;
            if (resolved && resolved->type == NodeType::FnDef)
                token_type = ST_Method;
            else
                token_type = ST_Property;
            break;
        }
        case NodeType::FieldInitExpr:
            token_type = ST_Property;
            break;
        case NodeType::ConstructExpr:
            token_type = ST_Type;
            break;
        case NodeType::BindIdentifier:
            token_type = ST_Variable;
            modifiers |= SM_Declaration;
            break;
        case NodeType::TypeParam:
            token_type = ST_Type;
            modifiers |= SM_Declaration;
            break;
        default:
            continue;
        }

        if (token_type < 0) continue;

        auto len = cx::utf8_length(tok->str);
        if (len == 0) len = cx::utf8_length(tok->to_string());
        if (len == 0) continue;

        result.push_back(tok->pos.line);
        result.push_back(tok->pos.col - 1);
        result.push_back((int64_t)len);
        result.push_back(token_type);
        result.push_back(modifiers);
    }

    return result;
}

// ============================================================================
// Auto-import: suggest symbols from unimported stdlib modules
// ============================================================================

// Load a stdlib module by absolute path (cached).
static cx::ast::Module *load_stdlib_module(cx::CompilationContext *ctx,
                                           const std::string &abs_path) {
    auto cached = ctx->source_modules.get(abs_path);
    if (cached)
        return *cached;
    auto pkg = ctx->get_or_create_package("std");
    auto src = cx::io::Buffer::from_file(abs_path);
    auto *mod = ctx->process_source(pkg, &src, abs_path);
    cx::Resolver mod_resolver = ctx->create_resolver();
    mod_resolver.resolve(mod);
    ctx->source_modules[abs_path] = mod;
    return mod;
}

static bool is_type_export(cx::ast::Node *node) {
    if (!node) return false;
    auto type = node->type;
    return type == cx::ast::NodeType::StructDecl ||
           type == cx::ast::NodeType::EnumDecl ||
           type == cx::ast::NodeType::Primitive;  // interfaces
}

// Suggest types (struct/enum/interface) from unimported stdlib modules.
// These use selective import: import {File} from "std/fs";
static void collect_auto_import_completions(
    boost::json::array &completions,
    cx::Analyzer &analyzer,
    cx::ast::Module *module,
    cx::Resolver &resolver,
    const std::set<std::string> &in_scope_names,
    long &index) {

    auto *ctx = analyzer.get_context();
    auto stdlib_dir = ctx->get_stdlib_path("std");

    if (!fs::exists(stdlib_dir) || !fs::is_directory(stdlib_dir))
        return;

    // Collect already-imported module paths
    std::set<std::string> imported_paths;
    for (auto *imp : module->imports) {
        imported_paths.insert(imp->full_path());
    }

    for (auto &entry : fs::directory_iterator(stdlib_dir)) {
        if (!entry.is_regular_file())
            continue;
        auto ext = entry.path().extension().string();
        if (ext != ".xs" && ext != ".x")
            continue;

        auto abs_path = fs::absolute(entry.path()).string();
        if (imported_paths.count(abs_path))
            continue;

        auto mod_name = "std/" + entry.path().stem().string();
        auto *stdlib_mod = load_stdlib_module(ctx, abs_path);
        if (!stdlib_mod || stdlib_mod->broken)
            continue;

        for (auto *exp : stdlib_mod->exports) {
            if (!exp || exp->name.empty())
                continue;
            if (!is_type_export(exp))
                continue;
            if (in_scope_names.count(exp->name))
                continue;

            boost::json::object completion;
            completion["label"] = exp->name;
            completion["kind"] = get_symbol_kind(exp);
            if (exp->resolved_type) {
                completion["detail"] = resolver.format_type_display(exp->resolved_type);
            }
            completion["autoImport"] = mod_name;
            completion["autoImportStyle"] = "symbol";
            completion["data"] = ++index;
            completions.push_back(completion);
        }
    }
}

// Try to complete `mod_name.` as a dot-access into an unimported stdlib module.
// Returns the module's exports as dot completions, tagged with autoImport.
static boost::json::array complete_auto_import_dot(
    cx::Analyzer &analyzer,
    cx::ast::Module *module,
    cx::Resolver &resolver,
    const std::string &mod_name) {

    boost::json::array completions;
    auto *ctx = analyzer.get_context();
    auto stdlib_dir = ctx->get_stdlib_path("std");
    auto mod_path = (fs::path(stdlib_dir) / (mod_name + ".xs")).string();

    if (!fs::exists(mod_path))
        return completions;

    auto abs_path = fs::absolute(mod_path).string();
    auto *stdlib_mod = load_stdlib_module(ctx, abs_path);
    if (!stdlib_mod || stdlib_mod->broken)
        return completions;

    auto import_path = "std/" + mod_name;
    for (auto *exp : stdlib_mod->exports) {
        if (!exp || exp->name.empty())
            continue;
        boost::json::object completion;
        completion["label"] = exp->name;
        completion["kind"] = get_symbol_kind(exp);
        if (exp->resolved_type) {
            completion["detail"] = resolver.format_type_display(exp->resolved_type);
        }
        completion["autoImport"] = import_path;
        completion["autoImportStyle"] = "module";
        completions.push_back(completion);
    }
    return completions;
}

// ============================================================================
// N-API methods
// ============================================================================

// analyze: process source, return diagnostics
// Input:  {"file": "...", "source": "...", "chiRoot": "..."}
// Output: {"errors": [...]}
static napi_value AnalyzeMethod(napi_env env, napi_callback_info info) {
    std::string input_buf;
    if (!parse_napi_arg(env, info, input_buf)) return nullptr;
    auto input = boost::json::parse(input_buf).as_object();

    cx::Analyzer analyzer;
    std::string input_file;
    init_analyzer(analyzer, input, input_file);
    auto source_str = std::string(input["source"].as_string().c_str());
    auto src = cx::io::Buffer::from_string(source_str.c_str());

    auto module = process_source(analyzer, input_file, &src);

    return napi_json_result(env, boost::json::object{{"errors", collect_errors(module, source_str)}});
}

// scan: process source, run cursor scan, return operation-specific result
// Input:  {"file": "...", "source": "...", "chiRoot": "...", "offset": N, "operation": "..."}
// Output: {"completions": [...]} | {"definition": {...}} | {"info": "..."} | {"signatureHelp": {...}}
static napi_value ScanMethod(napi_env env, napi_callback_info info) {
    std::string input_buf;
    if (!parse_napi_arg(env, info, input_buf)) return nullptr;
    auto input = boost::json::parse(input_buf).as_object();

    cx::Analyzer analyzer;
    std::string input_file;
    init_analyzer(analyzer, input, input_file);
    auto src = cx::io::Buffer::from_string(input["source"].as_string().c_str());

    auto module = process_source(analyzer, input_file, &src);

    auto source_str = std::string(input["source"].as_string().c_str());
    auto utf16_offset = input["offset"].as_int64();
    auto offset = utf16_to_byte_offset(source_str, utf16_offset);
    auto operation = input["operation"].as_string();
    auto pos = cx::Pos::from_offset(offset);
    auto result = analyzer.scan(module, pos);

    auto resolver = analyzer.get_resolver();
    cx::ScopeResolver scope_resolver(&resolver);
    boost::json::object output;

    if (operation == "completion") {
        boost::json::array completions;
        if (result.construct_expr) {
            completions = complete_construct(result, resolver, scope_resolver, source_str);
        } else if (result.scope) {
            if (result.is_dot) {
                completions = complete_dot(result, resolver);
                // If dot completions are empty (unresolved), check for stdlib module name
                if (completions.empty() && result.dot_expr) {
                    auto *expr = result.dot_expr->data.dot_expr.expr;
                    if (expr && expr->token && expr->token->type == cx::TokenType::IDEN) {
                        completions = complete_auto_import_dot(
                            analyzer, module, resolver, expr->token->str);
                    }
                }
            } else {
                long index = 0;
                std::set<std::string> in_scope_names;
                for (auto symbol : scope_resolver.get_all_symbols(result.scope)) {
                    ++index;
                    in_scope_names.insert(symbol->name);
                    boost::json::object completion;
                    completion["label"] = symbol->name;
                    completion["kind"] = get_symbol_kind(symbol);
                    if (symbol->resolved_type) {
                        completion["detail"] = resolver.format_type_display(symbol->resolved_type);
                    }
                    completion["data"] = index;
                    completions.push_back(completion);
                }
                // Add auto-import suggestions from unimported stdlib modules
                collect_auto_import_completions(
                    completions, analyzer, module, resolver, in_scope_names, index);
            }
        }
        output["completions"] = completions;
    } else if (operation == "signatureHelp") {
        if (result.fn_call) {
            output["signatureHelp"] = build_signature_help(result, resolver);
        } else if (result.construct_expr) {
            auto sh = build_construct_signature_help(result, resolver);
            if (!sh.empty())
                output["signatureHelp"] = sh;
        }
    } else if (operation == "info") {
        if (result.decl) {
            output["info"] = get_symbol_info(result.decl, resolver);
        }
    } else if (operation == "definition") {
        auto def = build_definition(result);
        if (!def.empty()) {
            output["definition"] = def;
        }
    }

    return napi_json_result(env, output);
}

// format: lightweight parse + format (no runtime needed)
// Input:  {"file": "...", "source": "...", "chiRoot": "..."}
// Output: {"errors": [...], "formatted": "..."}
static napi_value FormatMethod(napi_env env, napi_callback_info info) {
    std::string input_buf;
    if (!parse_napi_arg(env, info, input_buf)) return nullptr;
    auto input = boost::json::parse(input_buf).as_object();

    cx::Analyzer analyzer;
    std::string input_file;
    init_analyzer(analyzer, input, input_file);
    auto source_str = std::string(input["source"].as_string().c_str());
    auto src = cx::io::Buffer::from_string(source_str.c_str());

    auto pkg = analyzer.get_context()->add_package(".");
    load_package_config(pkg, input_file);
    auto module = analyzer.format_source(pkg, &src, input_file);

    boost::json::object output;
    output["errors"] = collect_errors(module, source_str);
    if (module && module->errors.len == 0 && module->root) {
        cx::AstPrinter printer(module->root, &module->comments);
        output["formatted"] = printer.format_to_string();
    }

    return napi_json_result(env, output);
}

// semanticTokens: process source, generate semantic token data
// Input:  {"file": "...", "source": "...", "chiRoot": "..."}
// Output: [line, col, len, type, mod, ...]
static napi_value SemanticTokensMethod(napi_env env, napi_callback_info info) {
    std::string input_buf;
    if (!parse_napi_arg(env, info, input_buf)) return nullptr;
    auto input = boost::json::parse(input_buf).as_object();

    cx::Analyzer analyzer;
    std::string input_file;
    init_analyzer(analyzer, input, input_file);
    auto src = cx::io::Buffer::from_string(input["source"].as_string().c_str());

    auto module = process_source(analyzer, input_file, &src);

    return napi_json_result(env, generate_semantic_tokens(module));
}

// ============================================================================
// Module registration
// ============================================================================

#define DECLARE_NAPI_METHOD(name, func) \
    { name, 0, func, 0, 0, 0, napi_default, 0 }

static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor descs[] = {
        DECLARE_NAPI_METHOD("analyze", AnalyzeMethod),
        DECLARE_NAPI_METHOD("scan", ScanMethod),
        DECLARE_NAPI_METHOD("format", FormatMethod),
        DECLARE_NAPI_METHOD("semanticTokens", SemanticTokensMethod),
    };
    napi_status status = napi_define_properties(env, exports, 4, descs);
    assert(status == napi_ok);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
