#include <assert.h>
#include <node/node_api.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

#define BOOST_JSON_STANDALONE
#define BOOST_NO_EXCEPTIONS
#include "../../analyzer.h"
#include "../../ast_printer.h"
#include <boost/json/src.hpp>

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

static std::string get_symbol_kind(cx::ast::Node *node) {
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
        return type ? resolver.format_type(type, true) : "unknown";
    }
    auto &fn = type->data.fn;
    auto *proto = decl->data.fn_def.fn_proto;
    auto &proto_params = proto->data.fn_proto.params;

    std::stringstream ss;
    ss << "func(";
    for (int i = 0; i < fn.params.len; i++) {
        if (fn.is_variadic && i == fn.params.len - 1) {
            ss << "...";
        }
        if (i < proto_params.len && !proto_params[i]->name.empty()) {
            ss << proto_params[i]->name << ": ";
        }
        ss << resolver.format_type(fn.params[i], true);
        if (i < fn.params.len - 1) {
            ss << ", ";
        }
    }
    ss << ")";
    if (fn.return_type && fn.return_type->kind != cx::TypeKind::Void) {
        ss << " " << resolver.format_type(fn.return_type, true);
    }
    return ss.str();
}

static std::string get_symbol_info(cx::ast::Node *decl, cx::Resolver &resolver) {
    auto kind = get_symbol_kind(decl);
    auto name = decl->name.size() ? decl->name : "<anonymous>";
    if (decl->type == cx::ast::NodeType::FnDef) {
        return fmt::format("({}) {}: {}", kind, name, format_fn_signature(decl, resolver));
    }
    auto type = decl->resolved_type;
    return fmt::format("({}) {}: {}", kind, name,
                       type ? resolver.format_type(type, true) : "unknown");
}

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

    // Build the full signature label and track parameter label offsets
    std::string label = fn_decl->name + "(";
    boost::json::array params_json;

    for (int i = 0; i < fn.params.len; i++) {
        auto param_start = label.size();
        if (fn.is_variadic && i == fn.params.len - 1) {
            label += "...";
        }
        if (i < proto_params.len && !proto_params[i]->name.empty()) {
            label += proto_params[i]->name + ": ";
        }
        label += resolver.format_type(fn.params[i], true);
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
        label += " " + resolver.format_type(fn.return_type, true);
    }

    boost::json::object sig;
    sig["label"] = label;
    sig["parameters"] = params_json;

    sig_help["signatures"] = boost::json::array{sig};
    sig_help["activeSignature"] = 0;
    sig_help["activeParameter"] = result.active_param;
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

    // Handle enum type completions (e.g. JsonKind.)
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
            completion["detail"] = resolver.format_type(member->resolved_type, true);
        }
        completions.push_back(completion);
    }
    return completions;
}

static napi_value Method(napi_env env, napi_callback_info info) {
    napi_status status;
    size_t argc = 1;
    napi_value args[1];
    status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (status != napi_ok) return nullptr;

    // get input string length, then allocate on heap (avoid large stack buffers)
    size_t len;
    status = napi_get_value_string_utf8(env, args[0], nullptr, 0, &len);
    if (status != napi_ok) return nullptr;
    std::string input_buf(len, '\0');
    status = napi_get_value_string_utf8(env, args[0], input_buf.data(), len + 1, &len);
    if (status != napi_ok) return nullptr;

    auto input = boost::json::parse(input_buf).as_object();

    // initialize analyzer
    cx::Analyzer analyzer;
    auto resolver = analyzer.get_resolver();
    cx::ScopeResolver scope_resolver(&resolver);

    if (input.contains("chiRoot")) {
        std::string chi_root_path = input["chiRoot"].as_string().c_str();
        analyzer.get_context()->root_path = chi_root_path;
    }

    std::string input_file = input["file"].as_string().c_str();
    auto src = cx::io::Buffer::from_string(input["source"].as_string().c_str());

    // handle format operation early (lightweight path, no runtime needed)
    if (input.contains("scan")) {
        auto scan_input = input["scan"].as_object();
        auto operation = scan_input["operation"].as_string();
        if (operation == "format") {
            auto pkg = analyzer.get_context()->add_package(".");
            auto module = analyzer.format_source(pkg, &src, input_file);

            boost::json::array errors_json;
            if (module) {
                for (auto &error : module->errors) {
                    boost::json::object error_json;
                    error_json["message"] = error.message;
                    error_json["offset"] = error.pos.offset;
                    error_json["range"] = error.range;
                    errors_json.push_back(error_json);
                }
            }

            boost::json::object result_object;
            if (module && module->errors.len == 0 && module->root) {
                cx::AstPrinter printer(module->root, &module->comments);
                result_object["formatted"] = printer.format_to_string();
            }

            boost::json::object result_json = {
                {"errors", errors_json},
                {"scanResult", result_object},
            };
            auto str = boost::json::serialize(result_json);
            napi_value result;
            status = napi_create_string_utf8(env, str.c_str(), str.size(), &result);
            if (status != napi_ok) return nullptr;
            return result;
        }
    }

    // process source code
    auto rt_path = analyzer.get_context()->get_stdlib_path("runtime.xc");
    bool is_runtime_file = (input_file == rt_path);
    cx::ast::Module *module;
    if (is_runtime_file) {
        // Process the editor content as the runtime to avoid double-processing
        module = analyzer.build_runtime_from_source(&src);
    } else {
        analyzer.build_runtime();
        auto pkg = analyzer.get_context()->add_package(".");
        module = analyzer.process_source(pkg, &src, input_file);
    }

    // collect errors from user module and runtime module
    boost::json::array errors_json = boost::json::array();
    auto collect_errors = [&](cx::ast::Module *mod) {
        if (!mod) return;
        for (auto &error : mod->errors) {
            boost::json::object error_json;
            error_json["message"] = error.message;
            error_json["offset"] = error.pos.offset;
            error_json["range"] = error.range;
            errors_json.push_back(error_json);
        }
    };
    collect_errors(module);

    // scan
    std::optional<boost::json::object> scan_result = std::nullopt;
    if (input.contains("scan")) {
        auto scan_input = input["scan"].as_object();
        auto offset = scan_input["offset"].as_int64();
        auto operation = scan_input["operation"].as_string();
        auto pos = cx::Pos::from_offset(offset);
        auto result = analyzer.scan(module, pos);

        auto result_object = boost::json::object();
        if (operation == "completion") {
            // return completion results
            auto completions = boost::json::array();
            if (result.scope) {
                if (result.is_dot) {
                    completions = complete_dot(result, resolver);
                } else {
                    long index = 0;
                    for (auto symbol : scope_resolver.get_all_symbols(result.scope)) {
                        ++index;
                        boost::json::object completion;
                        completion["label"] = symbol->name;
                        completion["kind"] = get_symbol_kind(symbol);
                        if (symbol->resolved_type) {
                            completion["detail"] = resolver.format_type(symbol->resolved_type, true);
                        }
                        completion["data"] = index;
                        completions.push_back(completion);
                    }
                }
            }
            result_object["completions"] = completions;
        } else if (operation == "signatureHelp") {
            if (result.fn_call) {
                result_object["signatureHelp"] = build_signature_help(result, resolver);
            }
        } else if (operation == "info") {
            if (result.decl) {
                result_object["info"] = get_symbol_info(result.decl, resolver);
            }
        } else if (operation == "definition") {
            auto start_tok = result.decl
                ? (result.decl->start_token ? result.decl->start_token : result.decl->token)
                : nullptr;
            if (result.decl && result.decl->module && start_tok) {
                boost::json::object def_link = {};
                def_link["targetUri"] = fmt::format("file://{}", result.decl->module->full_path());
                auto start_pos = start_tok->pos;
                auto end_pos =
                    result.decl->end_token ? result.decl->end_token->pos : start_pos.add_line(1);
                auto start = boost::json::object({
                    {"character", start_pos.col - 1},
                    {"line", start_pos.line},
                });
                auto end = boost::json::object({
                    {"character", end_pos.col},
                    {"line", end_pos.line},
                });
                auto range = boost::json::object({
                    {"start", start},
                    {"end", end},
                });
                def_link["targetSelectionRange"] = range;
                def_link["targetRange"] = range;
                result_object["definition"] = def_link;
            }
        }
        scan_result = {result_object};
    }

    // return final result
    napi_value result;
    boost::json::object result_json = {
        {"errors", errors_json},
    };
    if (scan_result) {
        result_json["scanResult"] = *scan_result;
    }
    auto str = boost::json::serialize(result_json);
    status = napi_create_string_utf8(env, str.c_str(), str.size(), &result);
    if (status != napi_ok) return nullptr;
    return result;
}

static napi_value Method(napi_env env, napi_callback_info info);

#define DECLARE_NAPI_METHOD(name, func)                                                            \
    { name, 0, func, 0, 0, 0, napi_default, 0 }

static napi_value Init(napi_env env, napi_value exports) {
    napi_status status;
    napi_property_descriptor desc = DECLARE_NAPI_METHOD("analyze", Method);
    status = napi_define_properties(env, exports, 1, &desc);
    assert(status == napi_ok);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
