/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include <filesystem>

#include "analyzer.h"
#include "ast_printer.h"
#include "builder.h"
#include "parser.h"
#include "util.h"
#define BOOST_JSON_STANDALONE
#include "boost/json.hpp"

using namespace cx;
namespace fs = std::filesystem;

codegen::Compiler Builder::create_codegen_compiler() { return {m_codegen_ctx.get()}; }

Builder::Builder() : m_ctx(), m_codegen_ctx(nullptr) {
    m_codegen_ctx.reset(new codegen::CodegenContext(&m_ctx));
}

ast::Module *Builder::process_source(ast::Package *package, io::Buffer *src,
                                     const string &file_name) {
    return m_ctx.process_source(package, src, file_name);
}

ast::Module *Builder::process_file(ast::Package *package, const string &file_name) {
    auto src = io::Buffer::from_file(file_name);
    return process_source(package, &src, file_name);
}

ast::Module *Builder::build_runtime() {
    auto resolver = m_ctx.create_resolver();
    resolver.context_init_primitives();

    auto rt_file_path = m_ctx.init_rt_stdlib();
    auto rt_source = io::Buffer::from_file(rt_file_path);
    auto module = process_source(m_ctx.rt_package, &rt_source, rt_file_path);
    resolver.context_init_builtins(module);
    return module;
}

void Builder::build_single_file(const string &file_name) {
    auto runtime_module = build_runtime();

    auto package = add_package(".");
    package->name = "__main";

    auto module = process_file(package, file_name);
    if (m_ctx.flags & FLAG_PRINT_AST) {
        return;
    }

    auto compiler = create_codegen_compiler();
    auto settings = compiler.get_settings();
    settings->output_obj_to_file = get_tmp_file_path("main.o");
    settings->output_ir_to_file = get_tmp_file_path("main.ll");
    settings->lang_flags = module->get_lang_flags();

    compiler.compile_module(runtime_module);
    compiler.compile_module(module);
    compiler.emit_output();

    // produce executable
    auto cmd =
        fmt::format("c++ {} -g -o {} -lchrt", settings->output_obj_to_file, output_file_name);

    if (debug_mode) {
        print("running: {}\n", cmd);
    }
    auto result = system(cmd.c_str());
    if (result != 0) {
        print("error: failed to run command: {}\n", cmd);
    }

#if __APPLE__
    cmd = fmt::format("dsymutil {} -o {}.dSYM", output_file_name, output_file_name);
    if (debug_mode) {
        print("running: {}\n", cmd);
    }
    result = system(cmd.c_str());
    if (result != 0) {
        print("error: failed to run command: {}\n", cmd);
    }
#endif
}

void Builder::build_program(const string &entry_file_name) {
    uint32_t flags = FLAG_EXIT_ON_ERROR;
    if (build_mode == BuildMode::AST) {
        flags |= FLAG_PRINT_AST;
    }
    m_ctx.flags = flags;

    if (!working_dir.empty()) {
        if (!fs::exists(working_dir)) {
            fs::create_directories(working_dir);
        }
    }

    build_single_file(entry_file_name);
}

void Builder::build_package(const string &package_dir) {
    uint32_t flags = FLAG_EXIT_ON_ERROR;
    if (build_mode == BuildMode::AST) {
        flags |= FLAG_PRINT_AST;
    }
    m_ctx.flags = flags;

    if (!working_dir.empty()) {
        if (!fs::exists(working_dir)) {
            fs::create_directories(working_dir);
        }
    }

    // Read and parse package.jsonc
    auto package_config_path = fs::path(package_dir) / "package.jsonc";
    if (!fs::exists(package_config_path)) {
        print("error: package.jsonc not found in {}\n", package_dir);
        exit(1);
    }

    auto config_content = io::Buffer::from_file(package_config_path.string());
    auto config_str = config_content.read_all();
    boost::json::value config;
    std::error_code ec;
    config = boost::json::parse(config_str, ec);

    if (ec) {
        print("error: failed to parse package.jsonc: {}\n", ec.message());
        exit(1);
    }

    // Extract entry file
    string entry_file;
    if (config.as_object().if_contains("entry_file")) {
        entry_file = config.at("entry_file").as_string().c_str();
    } else {
        print("error: entry_file not specified in package.jsonc\n");
        exit(1);
    }

    auto entry_file_path = fs::path(package_dir) / entry_file;
    if (!fs::exists(entry_file_path)) {
        print("error: entry file {} not found\n", entry_file_path.string());
        exit(1);
    }

    // Build the Chi code
    auto runtime_module = build_runtime();
    auto package = add_package(".");
    package->name = "__main";

    auto module = process_file(package, entry_file_path.string());
    if (m_ctx.flags & FLAG_PRINT_AST) {
        return;
    }

    auto compiler = create_codegen_compiler();
    auto settings = compiler.get_settings();
    settings->output_obj_to_file = get_tmp_file_path("main.o");
    settings->output_ir_to_file = get_tmp_file_path("main.ll");
    settings->lang_flags = module->get_lang_flags();

    compiler.compile_module(runtime_module);
    compiler.compile_module(module);
    compiler.emit_output();

    // Check for C compiler configuration
    array<string> c_object_files;
    if (config.as_object().if_contains("c_interop")) {
        auto c_config = config.at("c_interop").as_object();

        if (c_config.if_contains("enabled") && c_config.at("enabled").as_bool()) {
            // Get include directories
            array<string> include_dirs;
            if (c_config.if_contains("include_directories")) {
                for (auto &dir : c_config.at("include_directories").as_array()) {
                    auto include_path = fs::path(package_dir) / dir.as_string().c_str();
                    include_dirs.add(include_path.string());
                }
            }

            // Get source files (with glob pattern support)
            array<string> source_files;
            if (c_config.if_contains("source_files")) {
                for (auto &pattern : c_config.at("source_files").as_array()) {
                    string pattern_str = pattern.as_string().c_str();

                    // Use glob pattern matching
                    auto matched_files = glob_files(fs::path(package_dir), pattern_str);
                    for (auto &file : matched_files) {
                        source_files.add(file);
                    }
                }
            }

            // Compile C source files
            for (auto &src_file : source_files) {
                auto obj_file = get_tmp_file_path(fs::path(src_file).stem().string() + ".o");

                string include_flags = "";
                for (auto &inc_dir : include_dirs) {
                    include_flags += fmt::format("-I{} ", inc_dir);
                }

                auto cmd = fmt::format("gcc -c {} {} -o {}", include_flags, src_file, obj_file);

                if (debug_mode) {
                    print("running: {}\n", cmd);
                }

                auto result = system(cmd.c_str());
                if (result != 0) {
                    print("error: failed to compile C source file: {}\n", src_file);
                    exit(1);
                }

                c_object_files.add(obj_file);
            }
        }
    }

    // Get external libraries to link
    array<string> link_libraries;
    if (config.as_object().if_contains("c_interop")) {
        auto c_config = config.at("c_interop").as_object();
        if (c_config.if_contains("link_libraries")) {
            for (auto &lib : c_config.at("link_libraries").as_array()) {
                link_libraries.add(lib.as_string().c_str());
            }
        }
    }

    // Link everything together
    string obj_files = settings->output_obj_to_file;
    for (auto &c_obj : c_object_files) {
        obj_files += " " + c_obj;
    }

    // Add external library flags
    string library_flags = "";
    for (auto &lib : link_libraries) {
        library_flags += fmt::format("-l{} ", lib);
    }

    auto cmd = fmt::format("c++ {} -g -o {} -lchrt {}", obj_files, output_file_name, library_flags);

    if (debug_mode) {
        print("running: {}\n", cmd);
    }
    auto result = system(cmd.c_str());
    if (result != 0) {
        print("error: failed to link executable: {}\n", cmd);
        exit(1);
    }

#if __APPLE__
    cmd = fmt::format("dsymutil {} -o {}.dSYM", output_file_name, output_file_name);
    if (debug_mode) {
        print("running: {}\n", cmd);
    }
    result = system(cmd.c_str());
    if (result != 0) {
        print("error: failed to run dsymutil: {}\n", cmd);
    }
#endif
}

string Builder::get_tmp_file_path(const string &filename) {
    auto dir = !working_dir.empty() ? fs::path(working_dir) : fs::temp_directory_path();
    return dir / filename;
}