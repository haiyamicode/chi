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

// Mirrors the Chi __CxLambda struct layout: { fn_ptr, length, captures }
struct CxLambda {
    void *fn_ptr;
    uint32_t length;
    void *captures; // CxCapture pointer (or null)
};

// Type-erased refcounted capture allocation (header + payload). The runtime owns
// refcounting and destruction so lambdas can be safely type-erased.
CHI_RT_EXPORT void *cx_capture_new(uint32_t payload_size, TypeInfo *type, void *dtor);
CHI_RT_EXPORT void cx_capture_retain(void *capture_ptr);
CHI_RT_EXPORT void cx_capture_release(void *capture_ptr);
CHI_RT_EXPORT void *cx_capture_get_type(void *capture_ptr);
CHI_RT_EXPORT void *cx_capture_get_data(void *capture_ptr);


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

CHI_RT_EXPORT char *cx_string_to_cstring(CxString *str);

CHI_RT_EXPORT void cx_string_concat(CxString *dest, CxString *s1, CxString *s2);

CHI_RT_EXPORT char *cx_cstring_copy(char *src);

CHI_RT_EXPORT void cx_printf(CxString *format, CxSlice *values);

CHI_RT_EXPORT void cx_print(CxString str);

CHI_RT_EXPORT void cx_print_any(CxAny *value);

CHI_RT_EXPORT void cx_print_number(uint64_t value);

CHI_RT_EXPORT void cx_array_new(CxArray *dest);

CHI_RT_EXPORT void cx_array_delete(CxArray *dest);

CHI_RT_EXPORT void cx_array_reserve(CxArray *dest, uint32_t elem_size, uint32_t new_cap);

CHI_RT_EXPORT void *cx_array_add(CxArray *dest, uint32_t elem_size);

CHI_RT_EXPORT void cx_array_write_str(CxArray *dest, CxString *str);

CHI_RT_EXPORT void cx_array_append(CxArray *dest, CxArray *src, uint32_t elem_size);

CHI_RT_EXPORT void cx_print_string(CxString *message);

CHI_RT_EXPORT void cx_debug_i(const char *prefix, int value);

CHI_RT_EXPORT void cx_panic(CxString *message);
CHI_RT_EXPORT void cx_set_panic_location(CxString *file, uint32_t line, uint32_t col);
CHI_RT_EXPORT void cx_clear_panic_location();

CHI_RT_EXPORT void cx_throw(void *type_info, void *data_ptr, void *vtable_ptr, uint32_t type_id);
CHI_RT_EXPORT void *cx_get_error_type_info();
CHI_RT_EXPORT void *cx_get_error_data();
CHI_RT_EXPORT void *cx_get_error_vtable();
CHI_RT_EXPORT uint32_t cx_get_error_type_id();

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

CHI_RT_EXPORT void cx_timeout(uint64_t delay, void *callback);

CHI_RT_EXPORT uint64_t cx_meiyan(const void *key, int count);
CHI_RT_EXPORT bool cx_string_eq(CxString *a, CxString *b);


CHI_RT_EXPORT void cx_parse_json(CxString *str, void *result);
CHI_RT_EXPORT void cx_json_value_delete(void *data);
CHI_RT_EXPORT void cx_json_value_get(void *data, CxString *key, void *result);
CHI_RT_EXPORT void cx_json_value_convert(void *data, uint32_t kind, void *result);
CHI_RT_EXPORT void cx_json_array_index(void *data, uint32_t index, void *result);
CHI_RT_EXPORT uint32_t cx_json_array_length(void *data);
CHI_RT_EXPORT void cx_json_value_copy(void *data, void *result);

CHI_RT_EXPORT void cx_file_read(CxString *path, CxString *result);

CHI_RT_EXPORT uint64_t __cx_time_now(void);
CHI_RT_EXPORT uint64_t __cx_time_monotonic(void);

CHI_RT_EXPORT int32_t __cx_fs_error_kind(int32_t uv_err);
CHI_RT_EXPORT int32_t __cx_fs_flags(int32_t which);
CHI_RT_EXPORT int32_t __cx_fs_open(const char *path, int32_t flags, int32_t mode);
CHI_RT_EXPORT int32_t __cx_fs_read(int32_t fd, void *buf, uint32_t size);
CHI_RT_EXPORT int32_t __cx_fs_write(int32_t fd, const void *data, uint32_t size);
CHI_RT_EXPORT int32_t __cx_fs_close(int32_t fd);
CHI_RT_EXPORT int32_t __cx_file_exists(const char *path);
CHI_RT_EXPORT int32_t __cx_file_remove(const char *path);
CHI_RT_EXPORT int32_t __cx_mkdir(const char *path);
CHI_RT_EXPORT int32_t __cx_get_errno();
CHI_RT_EXPORT void __cx_uv_strerror(int32_t errnum, CxString *result);
CHI_RT_EXPORT int32_t __cx_list_dir(const char *path, CxArray *result);

#ifdef __cplusplus
}
#endif
