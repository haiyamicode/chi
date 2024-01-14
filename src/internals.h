//
// Created by haint on 11/25/18.
//

#pragma once

#include "sema.h"

using namespace cx;

#ifdef __cplusplus
extern "C" {
#endif

struct CxString {
    char *data;
    uint32_t size;
};

struct CxAny {
    TypeInfo *type;
    struct {
        void *a;
        void *b;
    } data;
};

struct CxArray {
    void *data;
    uint32_t size;
    uint32_t capacity;
    uint8_t flags;
};

struct CxRefc {
    void *data;
    int32_t *refcnt;
};

typedef CxArray CxSlice;

void cx_string_set_data(CxString *dest, const char *data);

void cx_string_concat(CxString *dest, CxString s1, CxString s2);

void cx_string_format(CxString *dest, CxString format, CxSlice values);

void cx_printf(CxString format, CxSlice values);

void cx_print(CxString str);

void cx_array_construct(CxArray *dest);

void cx_array_reserve(CxArray *dest, uint32_t elem_size, uint32_t new_cap);

void *cx_array_add(CxArray *dest, uint32_t elem_size);

void cx_debug(CxString message);

void cx_debug_i(const char *prefix, int value);

void cx_panic(const char *s);

void *cx_refc_alloc(CxRefc *dest, uint32_t size);

void *cx_gc_alloc(uint32_t size, void (*dtor)(void *) = NULL);

#ifdef __cplusplus
}
#endif