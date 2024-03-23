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
    uint8_t is_static;
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
};

typedef CxArray CxSlice;

struct CxRefc {
    void *data;
    int32_t *refcnt;
};

struct CxLambda {
    void *ptr;
    uint32_t size; // size of captured data
    void *data;    // captured data, lives on the heap
};

struct CxPromise {
    CxLambda callback;
};

struct CxHash {
    char *data;
    uint32_t size;
};

void cx_string_set_data(CxString *dest, const char *data);

void cx_string_concat(CxString *dest, CxString s1, CxString s2);

void cx_string_delete(CxString *dest);

CxString cx_string_format(CxString *format, CxSlice *values);

CxString cx_string_from_chars(const char *data, uint32_t size);

void cx_printf(CxString *format, CxSlice *values);

void cx_print(CxString str);

void cx_print_any(CxAny *value);

void cx_print_number(uint64_t value);

void cx_array_new(CxArray *dest);

void cx_array_delete(CxArray *dest);

void cx_array_reserve(CxArray *dest, uint32_t elem_size, uint32_t new_cap);

void *cx_array_add(CxArray *dest, uint32_t elem_size);

void cx_array_write_str(CxArray *dest, CxString *str);

void cx_print_string(CxString *message);

void cx_debug_i(const char *prefix, int value);

void cx_panic(CxString *message);

void *cx_refc_alloc(CxRefc *dest, uint32_t size);

void *cx_gc_alloc(uint32_t size, void (*dtor)(void *) = NULL);
void *cx_malloc(uint32_t size, void *ignored = NULL);
void cx_free(void *address);

void cx_runtime_start(void *stack);
void cx_runtime_stop();

_Unwind_Reason_Code cx_personality(int version, _Unwind_Action actions, uint64_t exceptionClass,
                                   struct _Unwind_Exception *exceptionObject,
                                   struct _Unwind_Context *context);

void cx_timeout(uint64_t delay, CxLambda *callback);
void cx_call(CxLambda *callback);

CxHash cx_hbytes(CxAny *value);
void *cx_map_new();
void cx_map_delete(void *data);
void *cx_map_find(void *data, CxHash *key);
void cx_map_add(void *data, CxHash *key, void *value);
void cx_map_remove(void *data, CxHash *key);

#ifdef __cplusplus
}
#endif