#include "context.h"
#include "ast_printer.h"
#include "parser.h"

using namespace cx;

ast::Module *CompilationContext::module_from_path(ast::Package *package, const string &path,
                                                  const string &base_path, bool import) {
    auto fs_path = fs::path(path);
    auto module = package->add_module();
    module->package = package;
    module->path = fs_path.parent_path().string();
    module->filename = fs::absolute(fs_path).string();
    module->name = fs_path.stem().string();
    module->kind = ast::Module::kind_from_extension(fs_path.extension().string());
    return module;
}

string CompilationContext::find_module_path(const string &path, const string &base_path) {
    auto fs_path = fs::path(path);
    if (base_path.size() && path.size() && path[0] == '.') {
        fs_path = fs::path(base_path) / path.substr(2);
    }
    for (auto &ext : file_extensions) {
        auto file = fs_path;
        file.replace_extension(ext);
        if (fs::exists(file)) {
            return file.string();
        }
    }
    return "";
}

ast::Module *CompilationContext::process_source(ast::Package *package, io::Buffer *src,
                                                const string &path) {
    auto module = module_from_path(package, path);
    bool exitOnError = flags & FLAG_EXIT_ON_ERROR;
    optional<ErrorHandler> error_handler = std::nullopt;
    if (!exitOnError) {
        error_handler = [module](Error error) { module->errors.add(error); };
    }
    resolve_ctx.error_handler = error_handler;

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

    // parse the source
    ParseContext pc;
    pc.resolver = &scope_resolver;
    pc.module = module;
    pc.allocator = this;
    pc.add_token_results(tokenization.tokens);
    pc.error_handler = error_handler;
    Parser parser(&pc);
    parser.parse();
    if (module->errors.size) {
        return module;
    }

    if (flags & FLAG_PRINT_AST) {
        print_ast(module->root);
    }

    if (module->errors.size) {
        return module;
    }

    resolver.resolve(module);
    return module;
}