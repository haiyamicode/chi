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
constexpr auto CANNOT_CONVERT_IMPLICIT = "conversion from {} to {} requires an explicit cast";
constexpr auto CALL_WRONG_NUMBER_OF_ARGS =
    "wrong number of arguments for function call, expected {}, got {}";
constexpr auto SUBTYPE_WRONG_NUMBER_OF_ARGS =
    "wrong number of type arguments for {}, expected {}, got {}";
constexpr auto GENERIC_TYPE_INFERENCE_FAILED =
    "failed to infer type parameters for generic function call";
constexpr auto CANNOT_CALL_NON_FUNCTION = "cannot call non-function value";
constexpr auto MEMBER_NOT_FOUND = "member '{}' not found for type {}";
constexpr auto PRIVATE_MEMBER_NOT_ACCESSIBLE =
    "private member '{}' is not accessible from this scope";
constexpr auto PROTECTED_MEMBER_NOT_WRITABLE = "protected member '{}' is not writable in this "
                                               "scope";
constexpr auto CONSTRUCT_CANNOT_INFER_TYPE = "cannot infer type for construct expression";
constexpr auto CANNOT_SUBSCRIPT = "cannot perform array subscript on type {}";
constexpr auto CANNOT_WRITE_IMMUTABLE_SPAN =
    "cannot write to immutable span {}; use []mut {} for a mutable span";
constexpr auto INVALID_OPERATOR = "invalid operator '{}' on type {}";
constexpr auto CANNOT_MODIFY_IMMUTABLE_REFERENCE = "immutable reference {} cannot be modified";
constexpr auto STMT_NOT_WITHIN_LOOP = "{} statement not within a loop";
constexpr auto INVALID_EMBED = "invalid embed, can only embed an interface or struct";
constexpr auto CANNOT_EMBED_INTO = "cannot embed '{}' into '{}'";
constexpr auto METHOD_NOT_IMPLEMENTED = "interface method '{}' has not been implemented";
constexpr auto MISSING_TYPE_ARGUMENTS = "type arguments are required for generic type '{}'";
constexpr auto CANNOT_GET_POINTER_UNADDRESSABLE = "cannot get pointer of unaddressable value";
constexpr auto CANNOT_GET_REFERENCE_UNADDRESSABLE = "cannot take reference of unaddressable value";
constexpr auto INVALID_THIS_IN_STATIC =
    "'this' cannot be used in static methods; use 'This' for the type instead";
constexpr auto VALUE_NOT_CONSTANT = "const value must be a compile-time constant expression";
constexpr auto NON_INTERFACE_IMPL_TYPE =
    "cannot use non-interface type '{}' in struct implement list";
constexpr auto VARIADIC_NOT_FINAL = "cannot use ... with non-final parameter '{}'";
constexpr auto TRY_NOT_CALL = "try must be used with a function call expression";
constexpr auto CATCH_NOT_ERROR = "catch type '{}' must implement the Error interface";
constexpr auto THROW_NOT_ERROR = "throw expression type '{}' must implement the Error interface";
constexpr auto THROW_NOT_REFERENCE = "throw expression must be a reference type";
constexpr auto MODULE_NOT_FOUND = "module '{}' not found";
constexpr auto MODULE_INDEX_NOT_FOUND = "module '{}' has no index file (_index.xs or _index.x)";
constexpr auto INVALID_ATTRIBUTE_TERM = "invalid attribute term '{}'";
constexpr auto IMPLEMENT_NOT_MATCH = "member '{}' does not match definition from interface of {}";
constexpr auto FOR_EXPR_NOT_ITERABLE = "for expression must be an iterable, got {}";
constexpr auto CANNOT_INDEX = "cannot perform index operation on type {}";
constexpr auto SYMBOL_NOT_FOUND_MODULE = "symbol '{}' not found in module '{}'";
constexpr auto DUPLICATE_EXPORT_SYMBOL = "duplicate export symbol '{}' in module '{}'";
constexpr auto VARIABLE_USED_BEFORE_INITIALIZED = "variable '{}' used before initialized";
constexpr auto UNINITIALIZED_FIELD = "field '{}' of type '{}' has not been initialized";
constexpr auto INVALID_SWITCH_TYPE = "switch type {} must be an enum or integer";
constexpr auto INVALID_VARIABLE_TYPE = "cannot declare variable of type {}";
constexpr auto ASSIGNMENT_TO_CONST = "assignment to const value '{}'";
constexpr auto SWITCH_EXPR_MUST_HAVE_ELSE = "non-exhaustive switch expression must have an else clause";
constexpr auto EXPORT_DECL_MUST_HAVE_SYMBOLS = "export declaration must have symbol or alias";
constexpr auto CANNOT_MODIFY_CONST = "cannot modify immutable constant '{}'";
constexpr auto INVALID_MUT_TYPE = "Mut can only be used for reference type, got '{}'";
constexpr auto ASYNC_MUST_RETURN_PROMISE = "async function must return Promise<T>";
constexpr auto MUTATING_METHOD_ON_IMMUTABLE_REFERENCE =
    "cannot access mutating method '{}' on immutable reference {}";
constexpr auto INVALID_THIS = "'this' keyword can only be used in a method scope";
constexpr auto BARE_INTERFACE_TYPE =
    "interface type '{}' cannot be used directly; use '&{}' instead";
constexpr auto MOVE_REF_IN_STRUCT_FIELD =
    "'&move' references cannot be used as struct fields; use Box<T> instead";
constexpr auto UNSAFE_CALL_IN_SAFE_MODE =
    "call to unsafe function '{}' is not allowed in safe mode";
constexpr auto DESTRUCTOR_WITHOUT_COPY =
    "struct '{}' defines 'func delete()' but does not implement 'ops.Copy'; "
    "types with custom destructors must define copy semantics";
constexpr auto TYPE_NOT_COPYABLE =
    "type '{}' cannot be copied (implements ops.NoCopy)";
constexpr auto TRAIT_METHOD_NOT_CALLABLE =
    "trait method '{}' on generic type {} must be called, not used as a value";

constexpr auto CHAR_USE_BYTE =
    "use 'byte' instead of 'char'; for a unicode character, use 'rune'";
constexpr auto GENERIC_DEPTH_EXCEEDED =
    "generic type '{}' exceeds maximum nesting depth ({}); this likely indicates infinite type expansion";

} // namespace errors

// Compiler limits
constexpr int MAX_GENERIC_DEPTH = 16;

} // namespace cx