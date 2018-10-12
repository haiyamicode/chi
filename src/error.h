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
        constexpr auto TOKEN_UNEXPECTED_GOT = "expected {}, got '{}'";
        constexpr auto TOKEN_UNEXPECTED = "unexpected '{}'";
        constexpr auto UNEXPECTED_EOF = "unexpected end-of-file";
        constexpr auto REDECLARED = "'{}' redeclared";
        constexpr auto UNDECLARED = "undeclared identifier '{}'";
        constexpr auto CANNOT_CONVERT = "cannot convert from {} to {}";
        constexpr auto CALL_WRONG_NUMBER_OF_ARGS = "wrong number of arguments for function call, expected {}, got {}";
        constexpr auto SUBTYPE_WRONG_NUMBER_OF_ARGS = "wrong number of type arguments for {}, expected {}, got {}";
        constexpr auto CANNOT_CALL_NON_FUNCTION = "cannot call non-function value";
        constexpr auto MEMBER_NOT_FOUND = "member '{}' not found for type {}";
        constexpr auto COMPLIT_CANNOT_INFER_TYPE = "cannot infer type for composite literal";
        constexpr auto CANNOT_SUBSCRIPT = "cannot perform array subscript on type {}";
    }
}