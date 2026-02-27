#include "format.h"
#include "fmt/core.h"
#include "sema.h"
#include <algorithm>
#include <cstring>

using namespace cx;

// ---------------------------------------------------------------------------
// parse_format_spec — parses the mini-language after ':'
//   [[fill]align][sign][#][0][width][.precision][type]
// ---------------------------------------------------------------------------

FormatSpec parse_format_spec(const char *spec, int len) {
    FormatSpec fs;
    int i = 0;

    // Try to detect [fill]align.  Align char is one of '<', '>', '^'.
    // If the *second* char is an align char, the first char is fill.
    // If the *first* char is an align char (and there's no preceding fill), use default fill.
    if (len >= 2 && (spec[1] == '<' || spec[1] == '>' || spec[1] == '^')) {
        fs.fill = spec[0];
        fs.align = spec[1];
        i = 2;
    } else if (len >= 1 && (spec[0] == '<' || spec[0] == '>' || spec[0] == '^')) {
        fs.align = spec[0];
        i = 1;
    }

    // sign
    if (i < len && (spec[i] == '+' || spec[i] == '-')) {
        fs.sign = spec[i];
        i++;
    }

    // alt form '#'
    if (i < len && spec[i] == '#') {
        fs.alt_form = true;
        i++;
    }

    // zero pad '0'  (shorthand for fill='0', align='>')
    if (i < len && spec[i] == '0') {
        fs.zero_pad = true;
        if (fs.align == '\0') {
            fs.fill = '0';
            fs.align = '>';
        }
        i++;
    }

    // width (digits)
    while (i < len && spec[i] >= '0' && spec[i] <= '9') {
        fs.width = fs.width * 10 + (spec[i] - '0');
        i++;
    }

    // .precision
    if (i < len && spec[i] == '.') {
        i++;
        fs.precision = 0;
        while (i < len && spec[i] >= '0' && spec[i] <= '9') {
            fs.precision = fs.precision * 10 + (spec[i] - '0');
            i++;
        }
    }

    // type
    if (i < len) {
        char c = spec[i];
        if (c == 'x' || c == 'X' || c == 'b' || c == 'o' || c == 'e' || c == 'E') {
            fs.type = c;
        }
    }

    return fs;
}

// ---------------------------------------------------------------------------
// Integer helpers
// ---------------------------------------------------------------------------

// Extract a raw 64-bit integer (sign-extended or zero-extended) from CxAny.
static int64_t extract_signed_int(const CxAny &v) {
    auto &spec = v.type->data.int_;
    switch (spec.bit_count) {
    case 8:  return *(int8_t *)v.data;
    case 16: return *(int16_t *)v.data;
    case 32: return *(int32_t *)v.data;
    case 64: return *(int64_t *)v.data;
    default: return 0;
    }
}

static uint64_t extract_unsigned_int(const CxAny &v) {
    auto &spec = v.type->data.int_;
    switch (spec.bit_count) {
    case 8:  return *(uint8_t *)v.data;
    case 16: return *(uint16_t *)v.data;
    case 32: return *(uint32_t *)v.data;
    case 64: return *(uint64_t *)v.data;
    default: return 0;
    }
}

static std::string format_int_with_base(const CxAny &v, const FormatSpec &fs) {
    auto &int_spec = v.type->data.int_;
    bool is_unsigned = int_spec.is_unsigned;

    // For hex/bin/oct, treat the value as unsigned bit pattern
    uint64_t uval = extract_unsigned_int(v);
    int64_t sval = is_unsigned ? 0 : extract_signed_int(v);
    bool negative = !is_unsigned && sval < 0;

    std::string digits;
    std::string prefix;

    switch (fs.type) {
    case 'x': case 'X': {
        // For signed negative values, format as two's complement
        uint64_t val = negative ? (uint64_t)sval : uval;
        digits = fmt::format(fs.type == 'x' ? "{:x}" : "{:X}", val);
        if (fs.alt_form) prefix = fs.type == 'x' ? "0x" : "0X";
        break;
    }
    case 'b': {
        uint64_t val = negative ? (uint64_t)sval : uval;
        // Manual binary conversion
        if (val == 0) {
            digits = "0";
        } else {
            while (val > 0) {
                digits.push_back('0' + (val & 1));
                val >>= 1;
            }
            std::reverse(digits.begin(), digits.end());
        }
        if (fs.alt_form) prefix = "0b";
        break;
    }
    case 'o': {
        uint64_t val = negative ? (uint64_t)sval : uval;
        digits = fmt::format("{:o}", val);
        if (fs.alt_form) prefix = "0o";
        break;
    }
    default: {
        // Decimal — respect sign
        if (is_unsigned) {
            digits = fmt::format("{}", uval);
        } else {
            if (negative) {
                digits = fmt::format("{}", -sval);
            } else {
                digits = fmt::format("{}", sval);
            }
        }
        break;
    }
    }

    // Build result: [sign][prefix][digits]
    std::string result;
    if (!is_unsigned && fs.type == '\0') {
        // decimal: handle sign
        if (negative) {
            result = "-";
        } else if (fs.sign == '+') {
            result = "+";
        }
    } else if (fs.sign == '+' && !negative && fs.type == '\0') {
        result = "+";
    }
    result += prefix;
    result += digits;
    return result;
}

// ---------------------------------------------------------------------------
// Float helpers
// ---------------------------------------------------------------------------

static double extract_float(const CxAny &v) {
    auto &spec = v.type->data.float_;
    switch (spec.bit_count) {
    case 32: return (double)(*(float *)v.data);
    case 64: return *(double *)v.data;
    default: return 0.0;
    }
}

static std::string format_float_with_spec(const CxAny &v, const FormatSpec &fs) {
    double val = extract_float(v);
    std::string result;

    if (fs.type == 'e' || fs.type == 'E') {
        if (fs.precision >= 0) {
            result = fmt::format(fs.type == 'e' ? "{:.{}e}" : "{:.{}E}", val, fs.precision);
        } else {
            result = fmt::format(fs.type == 'e' ? "{:e}" : "{:E}", val);
        }
    } else if (fs.precision >= 0) {
        result = fmt::format("{:.{}f}", val, fs.precision);
    } else {
        result = fmt::format("{}", val);
    }

    // Prepend '+' if requested and value is non-negative
    if (fs.sign == '+' && val >= 0.0 && result[0] != '-') {
        result = "+" + result;
    }

    return result;
}

// ---------------------------------------------------------------------------
// apply_padding — width / fill / alignment
// ---------------------------------------------------------------------------

std::string apply_padding(const std::string &content, const FormatSpec &fs, bool is_numeric) {
    int content_len = (int)content.size();
    if (fs.width <= 0 || content_len >= fs.width) {
        return content;
    }

    int pad_total = fs.width - content_len;
    char fill = fs.fill;
    char align = fs.align;

    // Default alignment: right for numbers, left for strings
    if (align == '\0') {
        align = is_numeric ? '>' : '<';
    }

    // For zero-pad with sign/prefix: pad zeros after the sign/prefix
    if (fs.zero_pad && fill == '0' && is_numeric && !content.empty()) {
        // Find where digits start (after sign and prefix like 0x)
        size_t digits_start = 0;
        if (content[0] == '+' || content[0] == '-') digits_start = 1;
        if (digits_start < content.size() && content[digits_start] == '0' &&
            digits_start + 1 < content.size() &&
            (content[digits_start + 1] == 'x' || content[digits_start + 1] == 'X' ||
             content[digits_start + 1] == 'b' || content[digits_start + 1] == 'o')) {
            digits_start += 2;
        }
        std::string result = content.substr(0, digits_start);
        result.append(pad_total, '0');
        result += content.substr(digits_start);
        return result;
    }

    std::string result;
    switch (align) {
    case '<': // left-align
        result = content;
        result.append(pad_total, fill);
        break;
    case '>': // right-align
        result.append(pad_total, fill);
        result += content;
        break;
    case '^': { // center
        int left = pad_total / 2;
        int right = pad_total - left;
        result.append(left, fill);
        result += content;
        result.append(right, fill);
        break;
    }
    default:
        result = content;
        break;
    }
    return result;
}

// ---------------------------------------------------------------------------
// apply_format — main entry point
// ---------------------------------------------------------------------------

std::string apply_format(const CxAny &value, const FormatSpec &fs) {
    auto kind = (TypeKind)value.type->kind;
    bool is_numeric = false;
    std::string content;

    if (kind == TypeKind::Int) {
        is_numeric = true;
        content = format_int_with_base(value, fs);
    } else if (kind == TypeKind::Float) {
        is_numeric = true;
        content = format_float_with_spec(value, fs);
    } else {
        // For non-numeric types, use the default display and just apply padding
        content = get_value_display(value);
        // Sign doesn't apply to non-numeric types
    }

    return apply_padding(content, fs, is_numeric);
}
