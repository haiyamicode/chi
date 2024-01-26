#include "context.h"

using namespace cx;
namespace fs = std::filesystem;

ast::Module *CompilationContext::module_from_path(ast::Package *package, const string &path) {
    auto module = package->add_module();
    auto fs_path = fs::path(path);
    module->package = package;
    module->path = path;
    module->name = fs_path.filename().string();
    module->kind = ast::Module::kind_from_extension(fs_path.extension().string());
    return module;
}