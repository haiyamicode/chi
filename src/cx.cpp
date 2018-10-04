/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "builder.h"

using namespace cx;

int main(int argc, char* argv[]) {
    Builder bld;
    string file_name;
    array<int> z;
    
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg[0] == '-') {
            auto flag = arg[1];
            if (flag == 'd') {
                bld.set_debug_mode(true);
            } else if (flag == 's') {
                bld.set_assembly_mode(true);
            }
        } else {
            file_name = arg;
        }
    }
    if (file_name.empty()) {
        print("usage: cx [-d | -s] source_file\n");
        return 0;
    }
    bld.build_program(file_name);
    return 0;
}
