/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#define CHI_RUNTIME_HAS_BACKTRACE 1
#include "analyzer.h"
#include "ast_printer.h"
#include "builder.h"
#include "util.h"

using namespace cx;

MAKE_ENUM(FlagType, String, Bool);
MAKE_ENUM(FlagId, CompileEntry, CompilePackage, Debug, Output, Ast, Format, WorkingDir, Analyzer,
          Help);
MAKE_ENUM(InputMode, File, Package)
MAKE_ENUM(ProcessingMode, Build, Analyzer, Format)

struct Flag {
    FlagId id;
    string short_name = "";
    string name = "";
    FlagType type = FlagType::Bool;
    string value = "";
};

array<Flag> flags = {};
map<string, Flag> flag_map_short = {};
map<string, Flag> flag_map_long = {};

void init_flags() {
    flags.add({FlagId::CompileEntry, "c", "compile", FlagType::String});
    flags.add({FlagId::CompilePackage, "p", "package", FlagType::String});
    flags.add({FlagId::Debug, "d", "debug", FlagType::Bool});
    flags.add({FlagId::Output, "o", "output", FlagType::String});
    flags.add({FlagId::Ast, "a", "ast", FlagType::Bool});
    flags.add({FlagId::Format, "f", "format", FlagType::Bool});
    flags.add({FlagId::WorkingDir, "w", "working-dir", FlagType::String});
    flags.add({FlagId::Analyzer, "analyzer", "analyzer", FlagType::Bool});
    flags.add({FlagId::Help, "h", "help", FlagType::Bool});

    for (auto &f : flags) {
        if (!f.short_name.empty()) {
            flag_map_short[f.short_name] = f;
        }
        flag_map_long[f.name] = f;
    }
}

int main(int argc, char *argv[]) {
    init_backtrace(NULL);
    init_flags();

    Builder bld;
    string input_file;
    InputMode input_mode = InputMode::File;
    array<int> z;
    string flag_name;
    bld.build_mode = BuildMode::Executable;
    bld.working_dir = "build";
    ProcessingMode processing_mode = ProcessingMode::Build;
    Flag *flag = nullptr;

    auto print_help = [&]() {
        print("usage: chi [flags]\n");
        print("flags:\n");
        print("  -c --compile <file>: compile from entry source file\n");
        print("  -p --package <dir>: compile from a package directory\n");
        print("  -d --debug: debug mode\n");
        print("  -o --output: output file name\n");
        print("  -a --ast: build ast\n");
        print("  -f --format: format source code\n");
        print("  -w --working-dir <dir>: working directory\n");
        print("  --analyzer: analyzer mode\n");
        print("  -h --help: help\n");
    };

    auto on_flag = [&]() {
        switch (flag->id) {
        case FlagId::CompileEntry:
            input_file = flag->value;
            input_mode = InputMode::File;
            break;
        case FlagId::CompilePackage:
            input_file = flag->value;
            input_mode = InputMode::Package;
            break;
        case FlagId::Debug:
            bld.debug_mode = true;
            break;
        case FlagId::Output:
            bld.output_file_name = flag->value;
            break;
        case FlagId::Ast:
            bld.build_mode = BuildMode::AST;
            break;
        case FlagId::Format:
            processing_mode = ProcessingMode::Format;
            break;
        case FlagId::WorkingDir:
            bld.working_dir = flag->value;
            break;
        case FlagId::Analyzer:
            processing_mode = ProcessingMode::Analyzer;
            break;
        case FlagId::Help:
            print_help();
            exit(0);
        default:
            break;
        };
    };

    int state = 0;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (state == 1) {
            assert(flag);
            flag->value = arg;
            on_flag();
            state = 0;
            continue;
        }
        if (arg[0] == '-') {
            if (arg[1] == '-') {
                flag_name = arg.substr(2);
                flag = flag_map_long.get(flag_name);
            } else {
                flag_name = arg.substr(1);
                flag = flag_map_short.get(flag_name);
            }
            if (!flag) {
                fmt::print("unknown flag: {}\n", flag_name);
                return 1;
            }
            switch (flag->type) {
            case FlagType::Bool:
                flag->value = "true";
                on_flag();
                break;
            case FlagType::String:
                state = 1;
                break;
            default:
                break;
            }
        } else {
            fmt::print("unknown argument: {}\n", arg);
            return 1;
        }
    }

    if (input_file.empty()) {
        print_help();
        return 0;
    }

    if (processing_mode == ProcessingMode::Analyzer) {
        Analyzer analyzer;
        analyzer.build_runtime();
        auto pkg = analyzer.add_package(".");
        auto module = analyzer.process_file(pkg, input_file);

        // Print collected errors for analyzer testing
        if (module && module->errors.len > 0) {
            for (auto &error : module->errors) {
                print("{}:{}:{}: error: {}\n", module->full_path(), error.pos.line_number(),
                      error.pos.col_number(), error.message);
            }
            return 1; // Return error code to indicate parsing issues
        }

        return 0;
    }

    if (processing_mode == ProcessingMode::Format) {
        Analyzer analyzer;
        auto pkg = analyzer.add_package(".");
        auto module = analyzer.format_file(pkg, input_file);

        if (module && module->root) {
            cx::AstPrinter printer(module->root, &module->comments);
            printer.print_ast();
        }
        return 0;
    }
    bool format_mode = false;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "-f" || arg == "--format") {
            format_mode = true;
            break;
        }
    }

    if (format_mode) {
        Analyzer analyzer;
        analyzer.build_runtime();
        auto pkg = analyzer.add_package(".");
        auto module = analyzer.process_file(pkg, input_file);

        if (module && module->root) {
            cx::AstPrinter printer(module->root, &module->comments);
            printer.print_ast();
        }
        return 0;
    }
    if (bld.build_mode == BuildMode::Executable) {
        if (bld.output_file_name.empty()) {
            print("error: output file name is not specified\n");
            return 1;
        }
    }

    switch (input_mode) {
    case InputMode::File:
        bld.build_program(input_file);
        break;
    case InputMode::Package:
        bld.build_package(input_file);
        break;
    default:
        assert(false);
    }

    return 0;
}
