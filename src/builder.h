/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "types.h"
#include "package.h"

namespace cx {
    struct Builder {
        bool m_debug_mode = false;
        bool m_assembly_mode = false;

        Package* add_package();


        array<Package> m_packages;

        Builder();

        void compile(Module* file);

        void process_file(Package* pkg, string file_name);

        void build_program(string entry_file_name);
    };

}
