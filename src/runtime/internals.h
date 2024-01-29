//
// Created by haint on 11/25/18.
//

#pragma once

#include "sema.h"
#include <cxxabi.h>
#include <unwind.h>

using namespace cx;

#ifdef __cplusplus
extern "C" {
#endif

struct CxString {
    char *data;
    uint32_t size;
};

struct Data64ab {
    char a[8];
    char b[8];
};

struct CxAny {
    TypeInfo *type;

    // store 16 bytes of data
    char data[16];
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

void cx_print_any(CxAny *value);

void cx_print_number(uint64_t value);

void cx_array_construct(CxArray *dest);

void cx_array_reserve(CxArray *dest, uint32_t elem_size, uint32_t new_cap);

void *cx_array_add(CxArray *dest, uint32_t elem_size);

void cx_debug(CxString message);

void cx_debug_i(const char *prefix, int value);

void cx_panic(CxString message);

void *cx_refc_alloc(CxRefc *dest, uint32_t size);

void *cx_gc_alloc(uint32_t size, void (*dtor)(void *) = NULL);

void cx_runtime_start(void *stack);
void cx_runtime_stop();

_Unwind_Reason_Code cx_personality(int version, _Unwind_Action actions, uint64_t exceptionClass,
                                   struct _Unwind_Exception *exceptionObject,
                                   struct _Unwind_Context *context);

#ifdef __cplusplus
}
#endif