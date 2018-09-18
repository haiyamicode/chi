/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "compat.h"

namespace cx {
    namespace errors {
        constexpr auto TOKEN_UNEXPECTED_GOT = "expected '{}', got '{}'";
        constexpr auto TOKEN_UNEXPECTED = "unexpected '{}'";
        constexpr auto REDECLARED = "'{}' redeclared";
    }
}