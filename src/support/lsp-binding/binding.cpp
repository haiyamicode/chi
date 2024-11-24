#include <assert.h>
#include <node/node_api.h>

#define BOOST_JSON_STANDALONE
#define BOOST_NO_EXCEPTIONS
#include "../../analyzer.h"
#include <boost/json/src.hpp>

static std::string get_symbol_kind(cx::ast::Node *node) {
    switch (node->type) {
    case cx::ast::NodeType::FnDef:
        return node->data.fn_def.fn_kind == cx::ast::FnKind::InstanceMethod ? "method" : "function";
    case cx::ast::NodeType::StructDecl:
        return "struct";
    case cx::ast::NodeType::VarDecl:
        return node->data.var_decl.is_const ? "constant" : "variable";
    case cx::ast::NodeType::Primitive:
        return "interface";
    default:
        return "interface";
    }
}

static std::string get_symbol_info(cx::ast::Node *decl, cx::Resolver &resolver) {
    auto type = decl->resolved_type;
    auto kind = get_symbol_kind(decl);
    return fmt::format("({}) {}: {}", kind, decl->name.size() ? decl->name : "<anonymous>",
                       type ? resolver.to_string(type) : "unknown");
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

    auto struct_ = resolver.resolve_struct_type(expr_type);
    if (!struct_) {
        return completions;
    }

    for (auto member : struct_->members) {
        boost::json::object completion;
        completion["label"] = member->get_name();
        completion["kind"] = "Field";
        completion["detail"] = resolver.to_string(member->resolved_type);
        completions.push_back(completion);
    }
    return completions;
}

static napi_value Method(napi_env env, napi_callback_info info) {
    napi_status status;
    size_t argc = 1;
    napi_value args[1];
    status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    assert(status == napi_ok);

    char buf[1021024];
    size_t len;
    status = napi_get_value_string_utf8(env, args[0], buf, 1021024, &len);
    assert(status == napi_ok);
    std::string_view input_text(buf, len);
    auto input = boost::json::parse(input_text).as_object();

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
    // auto src = cx::io::Buffer::from_file(input_file);

    // process source code
    analyzer.build_runtime();
    auto pkg = analyzer.get_context()->add_package();
    auto module = analyzer.process_source(pkg, &src, input_file);

    // collect errors
    auto &errors = module->errors;
    boost::json::array errors_json = boost::json::array();
    for (auto &error : errors) {
        boost::json::object error_json;
        error_json["message"] = error.message;
        error_json["offset"] = error.pos.offset;
        error_json["range"] = error.range;
        errors_json.push_back(error_json);
    }

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
                            completion["detail"] = resolver.to_string(symbol->resolved_type);
                        }
                        completion["data"] = index;
                        completions.push_back(completion);
                    }
                }
            }
            result_object["completions"] = completions;
        } else if (operation == "info") {
            if (result.decl) {
                result_object["info"] = get_symbol_info(result.decl, resolver);
            }
        } else if (operation == "definition") {
            if (result.decl) {
                boost::json::object def_link = {};
                def_link["targetUri"] = fmt::format("file://{}", result.decl->module->full_path());
                auto start_tok =
                    result.decl->start_token ? result.decl->start_token : result.decl->token;
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
    assert(status == napi_ok);
    return result;
}

static napi_value Method(napi_env env, napi_callback_info info);

#define DECLARE_NAPI_METHOD(name, func) {name, 0, func, 0, 0, 0, napi_default, 0}

static napi_value Init(napi_env env, napi_value exports) {
    napi_status status;
    napi_property_descriptor desc = DECLARE_NAPI_METHOD("analyze", Method);
    status = napi_define_properties(env, exports, 1, &desc);
    assert(status == napi_ok);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
