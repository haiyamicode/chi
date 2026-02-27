#pragma once

#include "internals.h"
#include <string>

// Defined in internals.cpp — default display for any value
std::string get_value_display(const CxAny &v);

struct FormatSpec {
    char fill = ' ';
    char align = '\0'; // '<' left, '>' right, '^' center, '\0' default
    char sign = '-';   // '+' always, '-' negative only
    bool alt_form = false;
    bool zero_pad = false;
    int width = 0;
    int precision = -1; // -1 = unspecified
    char type = '\0';   // 'x','X','b','o','e','E' or '\0' for default
};

FormatSpec parse_format_spec(const char *spec, int len);
std::string apply_format(const CxAny &value, const FormatSpec &spec);
std::string apply_padding(const std::string &content, const FormatSpec &spec, bool is_numeric);
