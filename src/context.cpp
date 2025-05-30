#include "context.h"
#include "ast_printer.h"
#include "parser.h"
#include "util.h"
#include <filesystem>

using namespace cx;

ast::Module *CompilationContext::module_from_path(ast::Package *package, const string &path,
                                                  bool import) {
    auto fs_path = fs::path(path);
    auto extension = fs_path.extension().string();
    auto rel_path = fs::relative(fs_path, package->path).string();
    auto module_path = rel_path.substr(0, rel_path.size() - extension.size());
    auto module = package->add_module();
    module->package = package;
    module->path = fs_path.parent_path().string();
    module->id_path = string_join(string_split(module_path, "/"), ".");
    module->filename = fs::absolute(fs_path).string();
    module->name = fs_path.stem().string();
    module->kind = ast::Module::kind_from_extension(extension);
    return module;
}

optional<ModulePathInfo> CompilationContext::find_module_path(const string &path,
                                                              const string &base_path) {
    auto fs_path = fs::path(path);
    if (base_path.size() && path.size() && path[0] == '.') {
        fs_path = fs::path(base_path) / path.substr(2);
    }

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
        return {{fs_path.string(), true, index_path}};
    }

    for (auto &ext : file_extensions) {
        auto file = fs_path;
        file.replace_extension(ext);
        if (fs::exists(file)) {
            return {{file.string(), false, file.string()}};
        }
    }

    return std::nullopt;
}

ast::Module *CompilationContext::process_source(ast::Package *package, io::Buffer *src,
                                                const string &path) {
    auto module = module_from_path(package, path);
    bool exitOnError = flags & FLAG_EXIT_ON_ERROR;

    optional<ErrorHandler> error_handler = std::nullopt;
    if (!exitOnError) {
        error_handler = [module](Error error) {
            if (module->errors.len > MAX_ERRORS) {
                module->broken = true;
                return;
            }
            module->errors.add(error);
        };
    }
    resolve_ctx.error_handler = error_handler;

    if (module_map.has_key(module->global_id())) {
        if (exitOnError) {
            print("{}:{}:{}: error: module {} already exists\n", module->path, 1, 1,
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
    if (tokenization.error) {
        if (exitOnError) {
            print("{}:{}:{}: error: {}\n", module->path, tokenization.error_pos.line_number(),
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
        if (module->errors.len) {
            return module;
        }
        print_ast(module->root);
    }

    if (module->broken) {
        return module;
    }

    resolver.resolve(module);
    return module;
}