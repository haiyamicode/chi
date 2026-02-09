//
// Created by haint on 11/25/18.
//

#define CHI_RUNTIME_HAS_BACKTRACE 1
#include "fmt/core.h"
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <uv.h>

#include "internals.h"
#include "sema.h"

extern "C" {
#include "include/hashdict/hashdict.h"
#include "include/tgc/tgc.h"
}

using namespace cx;

#include <inttypes.h>
#include <libunwind.h>

#define BOOST_JSON_STANDALONE
#include <boost/json/src.hpp>

struct StackTrace {
    string message;
    char trace[8096];
};

struct CxCapture {
    uint32_t ref_count;
    uint32_t payload_size;
    TypeInfo *type;
    void (*dtor)(void *);
    void *data;
};

static tgc_t gc;
static thread_local StackTrace st;

static void *get_any_data(const CxAny *v) {
    auto p = v->inlined ? (intptr_t)v->data : *(intptr_t *)(v->data);
    return (void *)p;
}

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
    dest->is_static = false;
}

void cx_string_concat(CxString *dest, CxString s1, CxString s2) {
    dest->size = (uint32_t)(s1.size + s2.size);
    dest->data = (char *)malloc(dest->size + 1);
    memcpy(dest->data, s1.data, s1.size + 1);
    strncat(dest->data, s2.data, s2.size);
    dest->is_static = false;
}

void cx_string_delete(CxString *dest) {
    if (!dest->is_static) {
        free(dest->data);
        dest->data = NULL;
    }
}

void cx_string_copy(CxString *dest, CxString *src) {
    dest->is_static = src->is_static;

    if (src->is_static) {
        dest->data = src->data;
        dest->size = src->size;
        return;
    }

    string s(src->data, src->size);
    dest->data = (char *)malloc(s.size());
    memcpy(dest->data, s.data(), s.size());
    dest->size = s.size();
}

char *cx_string_to_cstring(CxString *str) {
    // Allocate buffer with space for null terminator
    char *result = (char *)malloc(str->size + 1);
    memcpy(result, str->data, str->size);
    result[str->size] = '\0';  // Add null terminator
    return result;
}

void cx_string_concat(CxString *dest, CxString *s1, CxString *s2) {
    dest->is_static = 0;
    dest->size = s1->size + s2->size;
    dest->data = (char *)malloc(dest->size);
    memcpy(dest->data, s1->data, s1->size);
    memcpy(dest->data + s1->size, s2->data, s2->size);
}

char *cx_cstring_copy(char *src) {
    if (src == nullptr) return nullptr;
    size_t len = strlen(src);
    char *copy = (char *)malloc(len + 1);
    strcpy(copy, src);
    return copy;
}

static std::string istringf(const CxAny &v) {
    auto typedata = &v.type->data;
    auto &spec = typedata->int_;

#define _FORMAT_INT(T, v) fmt::format("{}", (T)(*(T *)(&v.data)))
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
    } else {
        switch (spec.bit_count) {
        case 8:
            return _FORMAT_INT(uint8_t, v);
        case 16:
            return _FORMAT_INT(uint16_t, v);
        case 32:
            return _FORMAT_INT(uint32_t, v);
        case 64:
            return _FORMAT_INT(uint64_t, v);
        default:
            break;
        }
    }
    panic("unhandled");
    return "";
}

static std::string fstringf(const CxAny &v) {
    auto typedata = &v.type->data;
    auto &spec = typedata->float_;

#define _FORMAT_FLOAT(T, v) fmt::format("{}", (T)(*(T *)(&v.data)))
    switch (spec.bit_count) {
    case 32:
        return _FORMAT_FLOAT(float, v);
    case 64:
        return _FORMAT_FLOAT(double, v);
    default:
        break;
    }

    panic("unhandled");
    return "";
}

static void **program_vtable;

void cx_set_program_vtable(void *ptr) { program_vtable = (void **)ptr; }

static void *get_typemeta_display_method(TypeInfo *type) {
    if (type->meta_table_len == 0) {
        return nullptr;
    }

    auto offset = (uintptr_t)(&type->meta_table) - (uintptr_t)(&type->kind);
    auto table = (TypeMetaEntry *)((uintptr_t)type + offset);
    for (int i = 0; i < type->meta_table_len; i++) {
        auto entry = &table[i];
        if (entry->symbol == IntrinsicSymbol::Display && entry->vtable_index >= 0) {
            return program_vtable[entry->vtable_index];
        }
    }
    return nullptr;
}

static std::string get_value_display(const CxAny &v) {
    switch ((TypeKind)v.type->kind) {
    case TypeKind::String: {
        auto data_p = get_any_data(&v);
        auto s = (CxString *)data_p;
        return string(s->data, s->size);
    }
    case TypeKind::Bool:
        return fmt::format("{}", *(bool *)&v.data);
    case TypeKind::Char: {
        uint8_t char_value = *(uint8_t *)&v.data;
        // Only display as character if it's printable, otherwise show as number
        if (char_value >= 32 && char_value <= 126) {
            return fmt::format("'{}'", (char)char_value);
        }
        return fmt::format("{}", char_value);
    }
    case TypeKind::Int: {
        return istringf(v);
    }
    case TypeKind::Float:
        return fstringf(v);
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef: {
        auto ptr = *(void **)&v.data;
        auto elem_ti = v.type->data.pointer.elem;
        if (elem_ti && ptr) {
            CxAny inner;
            inner.type = elem_ti;
            inner.inlined = true;
            auto elem_size = elem_ti->size;
            if (elem_size <= (int32_t)sizeof(inner.data)) {
                memcpy(inner.data, ptr, elem_size);
            } else {
                inner.inlined = false;
                memcpy(inner.data, &ptr, sizeof(ptr));
            }
            return get_value_display(inner);
        }
        return fmt::format("{:#x}", *(intptr_t *)&v.data);
    }
    case TypeKind::Optional: {
        auto data_p = get_any_data(&v);
        auto has_value = *(bool *)data_p;
        if (has_value) {
            return "<optional>";
        } else {
            return "null";
        }
        break;
    }
    case cx::TypeKind::Array:
    case cx::TypeKind::Struct: {
        auto display_method = get_typemeta_display_method(v.type);
        if (display_method) {
            auto fn = (void (*)(void *a, void *b))display_method;
            auto p = get_any_data(&v);
            auto a = (CxArray *)v.data;
            CxString s;
            fn(&s, (void *)p);
            auto str = string(s.data, s.size);
            cx_string_delete(&s);
            return str;
        }
        return fmt::format("<TypeKind:{}>", PRINT_ENUM((TypeKind)v.type->kind));
    }
    case cx::TypeKind::EnumValue: {
        auto data_p = get_any_data(&v);
        auto ev = (CxEnumValue *)data_p;
        string display_name(ev->display_name->data, ev->display_name->size);
        return fmt::format("{}", display_name);
    }
    default:
        return fmt::format("<TypeKind:{}>", PRINT_ENUM(v.type->kind));
    }
}

void cx_print_any(CxAny *value) { fmt::print(get_value_display(*value)); }

void cx_print_number(uint64_t value) { fmt::print("{}\n", value); }

static string format_cstr(CxString &format, const CxSlice &values) {
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
                    ss << get_value_display(((CxAny *)values.data)[val_i++]);
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

void cx_string_format(CxString *format, CxSlice *values, CxString *str) {
    auto &s = *str;
    auto cstr = format_cstr(*format, *values);
    s.size = (uint32_t)cstr.size();
    s.data = (char *)malloc(cstr.size());
    s.is_static = false;
    memcpy(s.data, cstr.data(), s.size);
}

void cx_printf(CxString *format, CxSlice *values) {
    auto str = format_cstr(*format, *values);
    fmt::print("{}", str);
}

void cx_print(CxString str) {
    string s(str.data, str.size);
    fmt::print(s);
}

void cx_array_new(CxArray *dest) {
    memset(dest, 0, sizeof(CxArray));
    dest->size = 0;
    dest->capacity = 0;
    dest->data = 0;
}

void cx_array_delete(CxArray *dest) {
    if (dest->data) {
        free(dest->data);
    }
    dest->size = 0;
    dest->capacity = 0;
    dest->data = NULL;
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

void cx_array_write_str(CxArray *dest, CxString *str) {
    auto new_size = dest->size + str->size;
    cx_array_reserve(dest, sizeof(char), new_size);
    memcpy(((char *)dest->data) + dest->size, str->data, str->size);
    dest->size = (uint32_t)new_size;
}

void cx_string_from_chars(const char *data, uint32_t size, CxString *str) {
    auto &s = *str;
    s.size = size;
    s.data = (char *)malloc(size);
    s.is_static = false;
    memcpy(s.data, data, size);
}

void cx_print_string(CxString *message) {
    string s(message->data, message->size);
    fmt::print(s);
}

void cx_debug_i(const char *prefix, int value) { fmt::print("{}: {}\n", prefix, value); }

void cx_panic(CxString *message) {
    st.message = {message->data, message->size};
    auto bt_output_file = fmemopen(st.trace, sizeof(st.trace), "w");
    set_bt_output_file(bt_output_file);
    backtrace();
    fclose(bt_output_file);
    throw (void *)NULL;
}

void *cx_refc_alloc(CxRefc *dest, uint32_t size) {
    dest->data = malloc(size);
    dest->refcnt = (int32_t *)malloc(sizeof(int32_t));
    *dest->refcnt = 1;
    return dest->data;
}

extern "C" {
void *cx_capture_new(uint32_t payload_size, TypeInfo *type, void *dtor) {
    if (payload_size == 0) {
        return nullptr;
    }

    auto capture = (CxCapture *)malloc(sizeof(CxCapture));
    if (!capture) {
        return nullptr;
    }

    capture->ref_count = 1;
    capture->payload_size = payload_size;
    capture->type = type;
    capture->dtor = (void (*)(void *))dtor;

    // Allocate and zero-init payload data
    capture->data = malloc(payload_size);
    if (!capture->data) {
        free(capture);
        return nullptr;
    }
    memset(capture->data, 0, payload_size);

    return capture;
}

void cx_capture_retain(void *capture_ptr) {
    if (!capture_ptr) return;
    auto capture = (CxCapture *)capture_ptr;
    capture->ref_count++;
}

void cx_capture_release(void *capture_ptr) {
    if (!capture_ptr) return;
    auto capture = (CxCapture *)capture_ptr;
    capture->ref_count--;
    if (capture->ref_count == 0) {
        if (capture->dtor) {
            capture->dtor(capture->data);
        }
        free(capture->data);
        free(capture);
    }
}

void *cx_capture_get_type(void *capture_ptr) {
    if (!capture_ptr) return nullptr;
    auto capture = (CxCapture *)capture_ptr;
    return capture->type;
}

void *cx_capture_get_data(void *capture_ptr) {
    if (!capture_ptr) return nullptr;
    auto capture = (CxCapture *)capture_ptr;
    return capture->data;
}
}

void *cx_gc_alloc(uint32_t size, void (*dtor)(void *)) {
    auto p = tgc_alloc(&gc, size);
    if (dtor) {
        tgc_set_dtor(&gc, p, dtor);
    }
    return p;
}

void *cx_malloc(uint32_t size, void *_ignored) { return malloc(size); }

void cx_free(void *address) { return free(address); }

void cx_debug(void *ptr) {
    auto p = (CxAny *)ptr;
    auto data_p = get_any_data(p);
    auto v_p = (CxEnumValue *)data_p;
    fmt::print("{}\n", data_p);
    // to be implemented
}

void cx_memset(void *dest, uint8_t value, uint32_t size) { memset(dest, value, size); }

void signal_handler(int signal_num) {
    print("panic: {}\n", st.message);
    print(st.trace);
    exit(1);
}

void cx_runtime_start(void *stack) {
    init_backtrace(getenv("DEBUG_FILE"));
    tgc_start(&gc, stack);

    auto trace_mode = getenv("CHI_BACKTRACE");
    bool enable_trace = true;
    if (trace_mode && string(trace_mode) == "none") {
        enable_trace = false;
    }
    if (enable_trace) {
        signal(SIGABRT, signal_handler);
    }
}

void cx_runtime_stop() {
    // run event loop
    uv_loop_t *loop = uv_default_loop();
    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);

    tgc_stop(&gc);
}

extern "C" {
extern _Unwind_Reason_Code __gxx_personality_v0(...);
}

_Unwind_Reason_Code cx_personality(int version, _Unwind_Action actions, uint64_t exceptionClass,
                                   struct _Unwind_Exception *exceptionObject,
                                   struct _Unwind_Context *context) {
    return __gxx_personality_v0(version, actions, exceptionClass, exceptionObject, context);
}

struct TimeoutData {
    void (*fn_ptr)(void *);
    void *captures; // CxCapture pointer (retained)
};

static void timeout_cb(uv_timer_t *handle) {
    auto data = (TimeoutData *)handle->data;
    void *payload = data->captures ? cx_capture_get_data(data->captures) : nullptr;
    data->fn_ptr(payload);
    if (data->captures)
        cx_capture_release(data->captures);
    delete data;
    uv_close((uv_handle_t *)handle, [](uv_handle_t *h) { delete (uv_timer_t *)h; });
}

void cx_timeout(uint64_t delay, void *lambda_ptr) {
    auto lambda = (CxLambda *)lambda_ptr;
    auto fn_ptr = lambda->fn_ptr;
    auto captures = lambda->captures;

    auto td = new TimeoutData();
    td->fn_ptr = (void (*)(void *))fn_ptr;
    td->captures = captures;
    if (captures)
        cx_capture_retain(captures);

    auto timer = new uv_timer_t();
    timer->data = td;
    uv_timer_init(uv_default_loop(), timer);
    uv_timer_start(timer, timeout_cb, delay, 0);
}

static CxHash get_hbytes(CxAny *v) {
    switch ((TypeKind)v->type->kind) {
    case TypeKind::String: {
        auto s = (CxString *)v->data;
        return {s->data, s->size};
    }
    default:
        return {v->data, (uint32_t)v->type->size};
    }
    return {v->data, (uint32_t)v->type->size};
}

void cx_hbytes(CxAny *v, CxHash *result) { *result = get_hbytes(v); }

// DEPRECATED: Promise is now a Chi-native struct in runtime.xc
// void cx_promise_init(CxPromise *promise) { ... }
// void cx_promise_resolve(CxPromise *promise, void *value) { ... }
// void cx_promise_reject(CxPromise *promise, void *error) { ... }
// void cx_promise_then(CxPromise *promise, CxLambda *on_resolve, CxLambda *on_reject) { ... }

void *cx_map_new() { return dic_new(0); }

void cx_map_delete(void *data) {
    auto dict = (dictionary *)data;
    dic_forEach(
        dict,
        [](void *key, int count, void **value, void *user) -> int {
            free(*value);
            return true;
        },
        NULL);
    dic_delete(dict);
}

void *cx_map_find(void *data, CxHash *key) {
    auto dict = (dictionary *)data;
    auto found = dic_find(dict, key->data, key->size);
    if (!found) {
        return nullptr;
    }
    return *dict->value;
}

void cx_map_add(void *data, CxHash *key, void *value) {
    auto dict = (dictionary *)data;
    dic_add(dict, key->data, key->size);
    *dict->value = value;
}

void cx_map_remove(void *data, CxHash *key) {}

static void create_cx_json_result(boost::json::value *data, void *result) {
    auto result_p = (CxJsonValue *)result;
    result_p->data = new boost::json::value(*data);
    result_p->kind = (uint32_t)data->kind();
}

void cx_parse_json(CxString *str, void *result) {
    string s(str->data, str->size);
    auto value = boost::json::parse(s);
    create_cx_json_result(&value, result);
}

void cx_json_value_delete(void *data) { delete (boost::json::value *)data; }
void cx_json_value_get(void *data, char *key, void *result) {
    auto value = (boost::json::value *)data;
    auto ptr = value->at(key);
    create_cx_json_result(&ptr, result);
}

void cx_json_value_convert(void *data, uint32_t kind, void *result) {
    auto value = (boost::json::value *)data;
    switch ((boost::json::kind)kind) {
    case boost::json::kind::string: {
        auto str = value->as_string();
        CxString s;
        cx_string_from_chars(str.data(), str.size(), &s);
        *(CxString *)result = s;
        break;
    }
    case boost::json::kind::int64: {
        auto i = value->as_int64();
        *(int64_t *)result = i;
        break;
    }
    case boost::json::kind::uint64: {
        auto i = value->as_uint64();
        *(uint64_t *)result = i;
        break;
    }
    case boost::json::kind::double_: {
        auto d = value->as_double();
        *(double *)result = d;
        break;
    }
    case boost::json::kind::null: {
        *(bool *)result = true;
        break;
    }
    case boost::json::kind::bool_: {
        auto b = value->as_bool();
        *(bool *)result = b;
        break;
    }
    default:
        *(bool *)result = false;
        break;
    }
}

void cx_json_array_index(void *data, uint32_t index, void *result) {
    auto value = (boost::json::value *)data;
    auto item = value->at(index);
    create_cx_json_result(&item, result);
}

uint32_t cx_json_array_length(void *data) {
    auto value = (boost::json::value *)data;
    return value->as_array().size();
}

void cx_file_read(CxString *path, CxString *result) {
    string s(path->data, path->size);
    auto buf = io::Buffer::from_file(s);
    auto str = buf.read_all();
    return cx_string_from_chars(str.data(), str.size(), result);
}

void cx_json_value_copy(void *data, void *result) {
    auto value = (boost::json::value *)data;
    create_cx_json_result(value, result);
}

// SharedData refcounting helpers for type-erased pointers
// All SharedData<T> instances have ref_count as the first field at offset 0
