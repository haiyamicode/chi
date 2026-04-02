# Reads INPUT_FILE and writes OUTPUT_FILE as a C++ header with a constexpr string_view.
file(READ "${INPUT_FILE}" CONTENT)

# Escape backslashes and quotes for a C++ raw string literal — R"json(...)json"
# Raw string delimiters handle everything except the delimiter itself,
# which won't appear in valid JSON.
file(WRITE "${OUTPUT_FILE}"
"// Auto-generated from ${INPUT_FILE} — do not edit.
#pragma once
#include <string_view>

constexpr std::string_view PACKAGE_SCHEMA_JSON = R\"json(
${CONTENT})json\";
")
