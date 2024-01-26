//
// Created by haint on 11/25/18.
//

#include <sstream>

#include "internals.h"
#include "sema.h"

extern "C" {
#include "include/tgc/tgc.h"
}

using namespace cx;

static tgc_t gc;

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
    auto typedata = (TypeInfoData *)v.type->data;
    auto &spec = typedata->int_;

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
    switch ((TypeKind)v.type->kind) {
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
    default:
        return fmt::format("<{}>", PRINT_ENUM(v.type->kind));
    }
}

void cx_print_any(CxAny *value) { fmt::print(stringf(*value)); }

void cx_print_number(uint64_t value) { fmt::print("{}\n", value); }

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

void cx_print(CxString str) {
    string s(str.data, str.size);
    printf("%s", s.c_str());
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

void cx_debug_i(const char *prefix, int value) { fmt::print("{}: {}\n", prefix, value); }

void cx_panic(const char *s) {
    fmt::print(s);
    exit(1);
}

void *cx_refc_alloc(CxRefc *dest, uint32_t size) {
    dest->data = malloc(size);
    dest->refcnt = (int32_t *)malloc(sizeof(int32_t));
    *dest->refcnt = 1;
    return dest->data;
}

void *cx_gc_alloc(uint32_t size, void (*dtor)(void *)) {
    return malloc(size);
    // auto p = tgc_alloc(&gc, size);
    // if (dtor) {
    //     tgc_set_dtor(&gc, p, dtor);
    // }
    // return p;
}

void cx_runtime_start(void *stack) { tgc_start(&gc, stack); }

void cx_runtime_stop() { tgc_stop(&gc); }