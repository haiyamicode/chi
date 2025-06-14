/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */
#include "ast.h"

using namespace cx;
using namespace cx::ast;

string Module::global_id() const {
    if (package->id_path.empty()) {
        return id_path;
    }
    if (package->id_path == ".") {
        return package->id_path + id_path;
    }
    return package->id_path + "." + id_path;
}