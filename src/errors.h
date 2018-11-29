/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "util.h"

namespace cx {
    namespace errors {
        constexpr auto TOKEN_EXPECTED_GOT = "expected {}, got '{}'";
        constexpr auto TOKEN_UNEXPECTED = "unexpected '{}'";
        constexpr auto UNEXPECTED_EOF = "unexpected end-of-file";
        constexpr auto REDECLARED = "'{}' redeclared";
        constexpr auto UNDECLARED = "undeclared identifier '{}'";
        constexpr auto CANNOT_CONVERT = "cannot convert from {} to {}";
        constexpr auto CALL_WRONG_NUMBER_OF_ARGS = "wrong number of arguments for function call, expected {}, got {}";
        constexpr auto SUBTYPE_WRONG_NUMBER_OF_ARGS = "wrong number of type arguments for {}, expected {}, got {}";
        constexpr auto CANNOT_CALL_NON_FUNCTION = "cannot call non-function value";
        constexpr auto MEMBER_NOT_FOUND = "member '{}' not found for type {}";
        constexpr auto CONSTRUCT_CANNOT_INFER_TYPE = "cannot infer type for construct expression";
        constexpr auto CANNOT_SUBSCRIPT = "cannot perform array subscript on type {}";
        constexpr auto INVALID_OPERATOR = "invalid operator {} on type {}";
        constexpr auto STMT_NOT_WITHIN_LOOP = "{} statement not within a loop";
        constexpr auto TRAIT_FIELD_NOT_ALLOWED = "member field declaration is not allowed within a trait";
        constexpr auto INVALID_EMBED = "invalid embed, can only embed a trait or struct";
        constexpr auto METHOD_NOT_IMPLEMENTED = "trait method '{}' has not been implemented";
        constexpr auto MISSING_TYPE_ARGUMENTS = "type arguments are required for generic type '{}'";
        constexpr auto CANNOT_TAKE_ADDRESS_UNADDRESSABLE = "cannot take address of unaddressable value";
        constexpr auto VALUE_NOT_CONSTANT = "const value must be a compile-time constant expression";
    }
}