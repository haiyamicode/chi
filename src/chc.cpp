/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "analyzer.h"
#include "builder.h"
#include "util.h"

using namespace cx;

int main(int argc, char *argv[]) {
    // backward::SignalHandling sh;
    Builder bld;
    string file_name;
    array<int> z;
    string flag;
    bld.build_mode = BuildMode::Executable;
    bool fuzz_mode = false;

    int state = 0;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg[0] == '-') {
            flag = arg.substr(1);
            if (flag == "d") {
                bld.debug_mode = true;
            } else if (flag == "o") {
                state = 1;
            } else if (flag == "a") {
                bld.build_mode = BuildMode::AST;
            } else if (flag == "w") {
                state = 2;
            } else if (flag == "fuzz") {
                fuzz_mode = true;
            } else {
                print("unknown flag: %c\n", flag);
                return 1;
            }
        } else {
            if (state == 1) {
                bld.output_file_name = arg;
                state = 0;
            } else if (state == 2) {
                bld.working_dir = arg;
                state = 0;
            } else {
                file_name = arg;
            }
        }
    }
    if (file_name.empty()) {
        print("usage: chi [-d | -s | -e] source_file\n");
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
