/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#define CHI_RUNTIME_HAS_BACKTRACE 1
#include "analyzer.h"
#include "builder.h"
#include "util.h"

using namespace cx;

MAKE_ENUM(FlagType, String, Bool);
MAKE_ENUM(FlagId, Debug, Output, Ast, WorkingDir, Fuzz, Help);

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
    flags.add({FlagId::Debug, "d", "debug", FlagType::Bool});
    flags.add({FlagId::Output, "o", "output", FlagType::String});
    flags.add({FlagId::Ast, "a", "ast", FlagType::Bool});
    flags.add({FlagId::WorkingDir, "w", "working-dir", FlagType::String});
    flags.add({FlagId::Fuzz, "fuzz", "fuzz", FlagType::Bool});
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
    string file_name;
    array<int> z;
    string flag_name;
    bld.build_mode = BuildMode::Executable;
    bld.working_dir = "build";
    bool fuzz_mode = false;
    Flag *flag = nullptr;

    auto print_help = [&]() {
        print("usage: chi [flags] source_file\n");
        print("flags:\n");
        print("  -d --debug: debug mode\n");
        print("  -o --output: output file name\n");
        print("  -a --ast: build ast\n");
        print("  -w --working-dir: working dir\n");
        print("  --fuzz: fuzz mode\n");
        print("  -h --help: help\n");
    };

    auto on_flag = [&]() {
        switch (flag->id) {
        case FlagId::Debug:
            bld.debug_mode = true;
            break;
        case FlagId::Output:
            bld.output_file_name = flag->value;
            break;
        case FlagId::Ast:
            bld.build_mode = BuildMode::AST;
            break;
        case FlagId::WorkingDir:
            bld.working_dir = flag->value;
            break;
        case FlagId::Fuzz:
            fuzz_mode = true;
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
            if (!file_name.empty()) {
                print_help();
                return 1;
            }
            file_name = arg;
        }
    }

    if (file_name.empty()) {
        print_help();
        return 0;
    }

    if (fuzz_mode) {
        auto N_TIMES = std::getenv("TIMES") ? std::atoi(std::getenv("TIMES")) : 1000;
        print("runnning compilation {} times on {}...\n", N_TIMES, file_name);
        for (int i = 0; i < N_TIMES; i++) {
            Analyzer analyzer;
            analyzer.build_runtime();
            auto pkg = analyzer.add_package();
            analyzer.process_file(pkg, file_name);
        }
    } else {
        if (bld.build_mode == BuildMode::Executable) {
            if (bld.output_file_name.empty()) {
                print("error: output file name is not specified\n");
                return 1;
            }
        }
        bld.build_program(file_name);
    }

    return 0;
}
