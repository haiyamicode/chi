//
// Created by haint on 11/25/18.
//

#include <sstream>

#include "internals.h"
#include "sema.h"

using namespace cx;

void cx_string_set_data(CxString *dest, const char *data) {
    if (data) {
        dest->size = (uint32_t)strlen(data);
        dest->data = (char *)realloc(dest->data, dest->size + 1);
        memcpy(dest->data, data, dest->size + 1);
    } else {
        free(dest->data);
        dest->size = 0;
        dest->data = NULL;
    }
}

void cx_string_concat(CxString *dest, CxString s1, CxString s2) {
    dest->size = (uint32_t)(s1.size + s2.size);
    dest->data = (char *)malloc(dest->size + 1);
    memcpy(dest->data, s1.data, s1.size + 1);
    strncat(dest->data, s2.data, s2.size);
}

static std::string istringf(const CxAny &v) {
    auto &spec = v.type->itype->data.int_;

#define _FORMAT_INT(T, v) fmt::format("{}", *(T *)&v.data)
    if (!spec.is_unsigned) {
        switch (spec.bit_count) {
        case 8:
            return _FORMAT_INT(int8_t, v);
        case 16:
            return _FORMAT_INT(int16_t, v);
        case 32:
            return _FORMAT_INT(int32_t, v);
        case 64:
            return _FORMAT_INT(int64_t, v);
        default:
            break;
        }
    }
    panic("unhandled");
    return "";
}

static std::string stringf(const CxAny &v) {
    switch (v.type->itype->kind) {
    case TypeKind::String: {
        auto s = (CxString *)&v.data;
        return fmt::format(s->data);
    }
    case TypeKind::Bool:
        return fmt::format("{}", *(bool *)&v.data);
    case TypeKind::Int:
        return istringf(v);
    case TypeKind::Pointer:
        return fmt::format("{:#x}", *(intptr_t *)&v.data);
    case TypeKind::Reference:
        CxAny a;
        a.type = v.type->data.ptr.elem;
        memcpy(&a.data, v.data.a, (size_t)a.type->size);
        return stringf(a);
    default:
        return fmt::format("<{}>", PRINT_ENUM(v.type->itype->kind));
    }
}

static string format_cstr(CxString format, const CxSlice &values) {
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
                    ss << stringf(((CxAny *)values.data)[val_i++]);
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

void cx_string_format(CxString *dest, CxString format, CxSlice values) {
    auto str = format_cstr(format, values);
    dest->data = str.data();
    dest->size = (uint32_t)str.size();
}

void cx_printf(CxString format, CxSlice values) {
    printf("%s", format_cstr(format, values).c_str());
}

void cx_array_construct(CxArray *dest) {
    dest->size = 0;
    dest->capacity = 0;
    dest->data = NULL;
    dest->flags = 0;
}

void cx_array_reserve(CxArray *dest, uint32_t elem_size, uint32_t new_cap) {
    if (dest->capacity >= new_cap)
        return;

    size_t better_cap = dest->capacity;
    do {
        better_cap = better_cap * 5 / 2 + 8;
    } while (better_cap < new_cap);

    dest->data = realloc(dest->data, better_cap * elem_size);
    dest->capacity = (uint32_t)better_cap;
}

void *cx_array_add(CxArray *dest, uint32_t elem_size) {
    cx_array_reserve(dest, elem_size, ++dest->size);
    return ((char *)dest->data) + (dest->size - 1) * elem_size;
}

void cx_debug(CxString message) { fmt::print(message.data); }

void cx_panic(const char *s) {
    fmt::print(s);
    exit(1);
}