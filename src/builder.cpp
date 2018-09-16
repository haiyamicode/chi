/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "builder.h"

using namespace cx;

Builder::Builder() = default;

Package* Builder::add_package() {
    return m_packages.push({});
}

void Builder::compile(Module* file) {

}

void Builder::process_file(Package* pkg, string file_name) {

}

void Builder::build_program(string entry_file_name) {

}

