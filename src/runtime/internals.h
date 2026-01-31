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

#include "generated/rt_export.h"

struct CxString {
    char *data;
    uint32_t size;
    uint32_t is_static;
};

struct Data64ab {
    char a[8];
    char b[8];
};

struct CxAny {
    TypeInfo *type;

    bool inlined = 0;
    // store 23 bytes of data
    char data[23];
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

// DEPRECATED: Promise is now a Chi-native struct in runtime.xc
// enum CxPromiseState : uint32_t {
//     CX_PROMISE_PENDING = 0,
//     CX_PROMISE_RESOLVED = 1,
//     CX_PROMISE_REJECTED = 2
// };

// struct CxPromise {
//     CxPromiseState state;
//     void *value;
//     void *error;
//     CxLambda on_resolve;
//     CxLambda on_reject;
// };

struct CxHash {
    char *data;
    uint32_t size;
};

struct CxJsonValue {
    void *data;
    uint32_t kind;
};

struct CxEnumValue {
    int32_t value;
    CxString *display_name;
};

CHI_RT_EXPORT void cx_debug(void *ptr);

CHI_RT_EXPORT void cx_set_program_vtable(void *ptr);

CHI_RT_EXPORT void cx_string_copy(CxString *dest, CxString *src);

CHI_RT_EXPORT void cx_string_delete(CxString *dest);

CHI_RT_EXPORT void cx_string_format(CxString *format, CxSlice *values, CxString *str);

CHI_RT_EXPORT void cx_string_from_chars(const char *data, uint32_t size, CxString *str);

CHI_RT_EXPORT void cx_printf(CxString *format, CxSlice *values);

CHI_RT_EXPORT void cx_print(CxString str);

CHI_RT_EXPORT void cx_print_any(CxAny *value);

CHI_RT_EXPORT void cx_print_number(uint64_t value);

CHI_RT_EXPORT void cx_array_new(CxArray *dest);

CHI_RT_EXPORT void cx_array_delete(CxArray *dest);

CHI_RT_EXPORT void cx_array_reserve(CxArray *dest, uint32_t elem_size, uint32_t new_cap);

CHI_RT_EXPORT void *cx_array_add(CxArray *dest, uint32_t elem_size);

CHI_RT_EXPORT void cx_array_write_str(CxArray *dest, CxString *str);

CHI_RT_EXPORT void cx_print_string(CxString *message);

CHI_RT_EXPORT void cx_debug_i(const char *prefix, int value);

CHI_RT_EXPORT void cx_panic(CxString *message);

CHI_RT_EXPORT void *cx_refc_alloc(CxRefc *dest, uint32_t size);

CHI_RT_EXPORT void *cx_gc_alloc(uint32_t size, void (*dtor)(void *) = NULL);
CHI_RT_EXPORT void *cx_malloc(uint32_t size, void *ignored = NULL);
CHI_RT_EXPORT void cx_free(void *address);
CHI_RT_EXPORT void cx_memset(void *dest, uint8_t value, uint32_t size);

CHI_RT_EXPORT void cx_runtime_start(void *stack);
CHI_RT_EXPORT void cx_runtime_stop();

_Unwind_Reason_Code cx_personality(int version, _Unwind_Action actions, uint64_t exceptionClass,
                                   struct _Unwind_Exception *exceptionObject,
                                   struct _Unwind_Context *context);

CHI_RT_EXPORT void cx_timeout(uint64_t delay, CxLambda *callback);
CHI_RT_EXPORT void cx_call(CxLambda *callback);
CHI_RT_EXPORT void cx_call_with_value(CxLambda *callback, void *value);

// DEPRECATED: Promise is now a Chi-native struct in runtime.xc
// CHI_RT_EXPORT void cx_promise_init(CxPromise *promise);
// CHI_RT_EXPORT void cx_promise_resolve(CxPromise *promise, void *value);
// CHI_RT_EXPORT void cx_promise_reject(CxPromise *promise, void *error);
// CHI_RT_EXPORT void cx_promise_then(CxPromise *promise, CxLambda *on_resolve, CxLambda *on_reject);

CHI_RT_EXPORT void cx_hbytes(CxAny *value, CxHash *result);
CHI_RT_EXPORT void *cx_map_new();
CHI_RT_EXPORT void cx_map_delete(void *data);
CHI_RT_EXPORT void *cx_map_find(void *data, CxHash *key);
CHI_RT_EXPORT void cx_map_add(void *data, CxHash *key, void *value);
CHI_RT_EXPORT void cx_map_remove(void *data, CxHash *key);

CHI_RT_EXPORT void cx_parse_json(CxString *str, void *result);
CHI_RT_EXPORT void cx_json_value_delete(void *data);
CHI_RT_EXPORT void cx_json_value_get(void *data, char *key, void *result);
CHI_RT_EXPORT void cx_json_value_convert(void *data, uint32_t kind, void *result);
CHI_RT_EXPORT void cx_json_array_index(void *data, uint32_t index, void *result);
CHI_RT_EXPORT uint32_t cx_json_array_length(void *data);
CHI_RT_EXPORT void cx_json_value_copy(void *data, void *result);

CHI_RT_EXPORT void cx_file_read(CxString *path, CxString *result);

// SharedData refcounting helpers for type-erased pointers
CHI_RT_EXPORT void cx_shared_retain(void *ptr);
CHI_RT_EXPORT void cx_shared_release(void *ptr);
CHI_RT_EXPORT void *cx_shared_get_value_ptr(void *shared_data_ptr);

#ifdef __cplusplus
}
#endif