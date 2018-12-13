//
// Created by haint on 11/25/18.
//

#include <sstream>

#include "internals.h"

using namespace cx;

void cx_string_set_data(CxString* dest, const char* data) {
    if (data) {
        dest->size = (uint32_t) strlen(data);
        dest->data = (char*) realloc(dest->data, dest->size + 1);
        memcpy(dest->data, data, dest->size + 1);
    } else {
        dest->size = 0;
        dest->data = NULL;
    }
}

void cx_string_concat(CxString* dest, CxString s1, CxString s2) {
    dest->size = (uint32_t) (s1.size + s2.size);
    dest->data = (char*) malloc(dest->size + 1);
    memcpy(dest->data, s1.data, s1.size + 1);
    strncat(dest->data, s2.data, s2.size);
}

static std::string to_string(const CxAny& v) {
    switch (v.type) {
        case TypeKind::String: {
            auto s = (CxString*) &v.data;
            return fmt::format(s->data);
        }
        case TypeKind::Bool:
            return fmt::format("{}", *(bool*) &v.data);
        case TypeKind::Int:
        case TypeKind::Pointer:
            return fmt::format("{}", *(int64_t*) &v.data);

        default:
            return fmt::format("<{}>", PRINT_ENUM(v.type));
    }
}

static string format_cstr(CxString format, const CxArray& values) {
    int val_i = 0;
    int state = 0;
    std::stringstream ss;
    for (int i = 0; i < format.size; i++) {
        auto c = format.data[i];
        switch (c) {
            case '{':
                if (state == 1) {
                    ss.write("{", 1);
                    state = 0;
                } else {
                    state = 1;
                }
                break;
            case '}':
                if (state == 1) {
                    if (val_i < values.size) {
                        ss << to_string(((CxAny*) values.data)[val_i++]);
                    }
                    state = 0;
                } else if (state == 2) {
                    ss.write(&c, 1);
                    state = 0;
                } else {
                    state = 2;
                }
                break;
            default:
                state = 0;
                ss.write(&c, 1);
                break;
        }
    }
    return ss.str();
}

void cx_string_format(CxString* dest, CxString format, CxArray values) {
    auto str = format_cstr(format, values);
    dest->data = str.data();
    dest->size = (uint32_t) str.size();
}

void cx_printf(CxString format, CxArray values) {
    printf("%s", format_cstr(format, values).c_str());
}

void cx_array_construct(CxArray* dest) {
    dest->size = 0;
    dest->capacity = 0;
    dest->data = NULL;
}

void cx_array_reserve(CxArray* dest, uint32_t elem_size, uint32_t new_cap) {
    if (dest->capacity >= new_cap)
        return;

    size_t better_cap = dest->capacity;
    do {
        better_cap = better_cap * 5 / 2 + 8;
    } while (better_cap < new_cap);

    dest->data = realloc(dest->data, better_cap * elem_size);
    dest->capacity = (uint32_t) better_cap;
}

void* cx_array_add(CxArray* dest, uint32_t elem_size) {
    cx_array_reserve(dest, elem_size, ++dest->size);
    return ((char*) dest->data) + (dest->size - 1) * elem_size;
}

void cx_debug(const char* s) {
    fmt::print(s);
}