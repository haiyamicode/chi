#include "context.h"
#include "ast_printer.h"
#include "parser.h"
#ifndef CHI_NO_RUNTIME
#include "runtime/internals.h"
#endif
#include "util.h"
#include <filesystem>
#include <functional>

using namespace cx;

bool cx::is_relative_path(const string &path) { return path.size() && path[0] == '.'; }

CompilationContext::CompilationContext() : resolve_ctx(this) {
    auto rootenv = std::getenv("CHI_ROOT");
    if (rootenv) {
        root_path = rootenv;
    } else {
        auto home_env = std::getenv("CHI_HOME");
        if (home_env) {
            root_path = home_env;
        }
#ifndef CHI_NO_RUNTIME
        else {
            auto default_home = __cx_default_chi_home();
            if (default_home) {
                root_path = default_home;
            }
        }
#endif
    }
    init_platform_tags();
}

void CompilationContext::init_platform_tags() {
    for (auto tag : get_known_platform_tags()) {
        platform_tags[tag] = false;
    }
    for (auto tag : get_active_platform_tags()) {
        platform_tags[tag] = true;
    }
}

ast::Module *CompilationContext::module_from_path(ast::Package *package, const string &path,
                                                  bool import) {
    auto fs_path = fs::path(path);
    auto extension = fs_path.extension().string();
    auto rel_path = fs::relative(fs_path, package->src_path).string();
    auto module_path = rel_path.substr(0, rel_path.size() - extension.size());
    auto module = package->add_module();
    module->package = package;
    auto abs_path = fs::weakly_canonical(fs_path);
    module->path = abs_path.parent_path().string();
    module->id_path = string_join(string_split(module_path, "/"), ".");
    module->filename = abs_path.string();
    module->name = fs_path.stem().string();
    module->kind = ast::Module::kind_from_extension(extension);
    return module;
}

optional<ModulePathInfo> CompilationContext::find_module_path(const string &path,
                                                              const string &base_path) {
    auto fs_path = fs::path(path);
    bool is_relative = false;
    string package_id_path = "";

    // relative import
    if (is_relative_path(path)) {
        fs_path = fs::path(base_path) / path;
        package_id_path = ".";
        is_relative = true;
    } else {
        auto [package_path, module_path] = parse_import_path(path);
        auto package_p = package_map.get(package_path);
        if (!package_p) {
            // Auto-discover installed packages from {root_path}/src/
            if (!root_path.empty()) {
                auto candidate = fs::path(root_path) / "src" / package_path;
                if (fs::is_directory(candidate)) {
                    auto package = add_package(package_path);
                    package->src_path = candidate.string();
                    package_p = &package_map[package_path];
                }
            }
            if (!package_p) {
                return std::nullopt;
            }
        }
        // Non-std packages only allow package-level imports (via _index.xs)
        if (package_path != "std" && module_path.size()) {
            return std::nullopt;
        }
        auto package = *package_p;
        fs_path = fs::path(package->src_path) / module_path;
        package_id_path = package->id_path;
    }

    return find_module_at_path(fs_path.string(), package_id_path);
}

std::pair<string, string> CompilationContext::parse_import_path(const string &path) {
    auto segments = string_split(path, "/");
    if (!segments.size()) {
        return {"", ""};
    }

    auto first_segment = segments[0];
    if (first_segment == "std") {
        return {"std", string_join(segments.slice(1), "/")};
    }

    auto package_path = string_join(segments.slice(0, 2), "/");
    auto module_path = string_join(segments.slice(2), ".");
    return {package_path, module_path};
}

optional<ModulePathInfo> CompilationContext::find_module_at_path(const string &path,
                                                                 const string &package_path) {
    auto fs_path = fs::path(path);

    // directory import
    if (fs::is_directory(fs_path)) {
        string index_path = "";
        for (auto &ext : file_extensions) {
            auto file = fs_path / ("_index." + ext);
            if (fs::exists(file)) {
                index_path = file.string();
                break;
            }
        }
        return {{fs_path.string(), true, index_path, package_path}};
    }

    for (auto &ext : file_extensions) {
        auto file = fs_path;
        file.replace_extension(ext);
        if (fs::exists(file)) {
            return {{file.string(), false, file.string(), package_path}};
        }
    }

    return std::nullopt;
}

string CompilationContext::init_rt_stdlib() {
    // initialize std package
    auto std = add_package("std");
    std->kind = PackageKind::STDLIB;
    std->src_path = get_stdlib_path("std");
    stdlib_package = std;

    // initialize runtime package
    auto rt = add_package("");
    rt->kind = PackageKind::BUILTIN;
    auto rt_path = get_stdlib_path("runtime.xs");
    rt->src_path = fs::path(rt_path).parent_path().string();
    rt_package = rt;
    return rt_path;
}

ast::Module *CompilationContext::process_source(ast::Package *package, io::Buffer *src,
                                                const string &path) {
    auto module = module_from_path(package, path);
    bool exitOnError = flags & FLAG_EXIT_ON_ERROR;

    auto saved_error_handler = resolve_ctx.error_handler;

    optional<ErrorHandler> error_handler = std::nullopt;
    if (!exitOnError) {
        error_handler = [module](Error error) {
            if (module->errors.size() > MAX_ERRORS) {
                return;
            }
            module->errors.add(error);
        };
    }
    resolve_ctx.error_handler = error_handler;

    if (module_map.has_key(module->global_id())) {
        if (exitOnError) {
            print("{}:{}:{}: error: module {} already exists\n", module->display_path(), 1, 1,
                  module->global_id());
            exit(1);
        } else {
            module->errors.add({
                fmt::format("module {} already exists", module->global_id()),
                {1, 1},
            });
            return module;
        }
    }

    Tokenization tokenization;
    Lexer lexer(src, &tokenization);
    lexer.tokenize();
    module->comments = std::move(tokenization.comments);
    if (tokenization.error) {
        if (exitOnError) {
            print("{}:{}:{}: error: {}\n", module->display_path(), tokenization.error_pos.line_number(),
                  tokenization.error_pos.col_number(), *tokenization.error);
            exit(1);
        } else {
            module->errors.add({*tokenization.error, tokenization.error_pos});
            return module;
        }
    }

    auto resolver = create_resolver();
    ScopeResolver scope_resolver(&resolver);
    module->scope = scope_resolver.get_scope();
    module->import_scope = create_scope(module->scope);

    // parse the source
    ParseContext pc;
    pc.resolver = &scope_resolver;
    pc.module = module;
    pc.allocator = this;
    pc.add_token_results(tokenization.tokens);
    pc.error_handler = error_handler;
    Parser parser(&pc);
    parser.parse();

    if (FLAG_SAVE_TOKENS) {
        module->tokens = pc.tokens;
    }

    if (module->broken) {
        return module;
    }

    if (flags & FLAG_PRINT_AST) {
        if (module->errors.size()) {
            return module;
        }
        print_ast(module->root);
    }

    if (module->broken) {
        return module;
    }

    resolver.resolve(module);

    resolve_ctx.error_handler = saved_error_handler;
    return module;
}
