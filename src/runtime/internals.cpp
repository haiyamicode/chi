//
// Created by haint on 11/25/18.
//

#include "fmt/core.h"
#include <cstddef>
#include <cstdint>
#define CHI_RUNTIME_HAS_BACKTRACE 1
#include <csignal>
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

static tgc_t gc;
static thread_local StackTrace st;

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

void cx_string_delete(CxString *dest) {
    if (!dest->is_static) {
        free(dest->data);
        dest->data = NULL;
    }
}

void cx_string_copy(CxString *dest, CxString *src) {
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
        if (entry->symbol == IntrinsicSymbol::OpDisplay && entry->vtable_index >= 0) {
            return program_vtable[entry->vtable_index];
        }
    }
    return nullptr;
}

static std::string get_value_display(const CxAny &v) {
    switch ((TypeKind)v.type->kind) {
    case TypeKind::String: {
        auto s = (CxString *)&v.data;
        return string(s->data, s->size);
    }
    case TypeKind::Bool:
        return fmt::format("{}", *(bool *)&v.data);
    case TypeKind::Int:
        return istringf(v);
    case TypeKind::Pointer:
    case TypeKind::Reference:
        return fmt::format("{:#x}", *(intptr_t *)&v.data);
    case TypeKind::Optional: {
        auto has_value = *(bool *)&v.data;
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
            auto fn = (CxString(*)(void *))display_method;
            intptr_t p = v.inlined ? (intptr_t)v.data : *(intptr_t *)(v.data);
            auto a = (CxArray *)v.data;
            auto s = fn((void *)p);
            auto str = string(s.data, s.size);
            free(s.data);
            return str;
        }
        return fmt::format("<{}>", PRINT_ENUM((TypeKind)v.type->kind));
    }
    default:
        return fmt::format("<{}>", PRINT_ENUM(v.type->kind));
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

CxString cx_string_format(CxString *format, CxSlice *values) {
    CxString s;
    auto str = format_cstr(*format, *values);
    s.size = (uint32_t)str.size();
    s.data = (char *)malloc(str.size());
    s.is_static = false;
    memcpy(s.data, str.data(), s.size);
    return s;
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
    dest->size = 0;
    dest->capacity = 0;
    dest->data = NULL;
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

CxString cx_string_from_chars(const char *data, uint32_t size) {
    CxString s;
    s.size = size;
    s.data = (char *)malloc(size);
    s.is_static = false;
    memcpy(s.data, data, size);
    return s;
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

void *cx_gc_alloc(uint32_t size, void (*dtor)(void *)) {
    auto p = tgc_alloc(&gc, size);
    if (dtor) {
        tgc_set_dtor(&gc, p, dtor);
    }
    return p;
}

void *cx_malloc(uint32_t size, void *_ignored) { return malloc(size); }

void cx_free(void *address) { return free(address); }

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

static CxLambda *cx_lambda_clone(CxLambda *dest, CxLambda *callback) {
    *dest = *callback;
    if (callback->data) {
        dest->data = malloc(callback->size);
        memcpy(dest->data, callback->data, callback->size);
    }
    return dest;
}

static void cx_lambda_free(CxLambda *callback) {
    free(callback->data);
    delete callback;
}

void cx_timeout(uint64_t delay, CxLambda *callback) {
    auto cb = cx_lambda_clone(new CxLambda(), callback);
    uv_timer_t *timer = (uv_timer_t *)malloc(sizeof(uv_timer_t));
    uv_timer_init(uv_default_loop(), timer);
    timer->data = cb;

    uv_timer_start(
        timer,
        [](uv_timer_t *handle) {
            auto cb = (CxLambda *)handle->data;
            auto fn = (void (*)(void *))cb->ptr;
            fn(cb->data);
            uv_timer_stop(handle);
            cx_lambda_free(cb);
            free(handle);
        },
        delay, 0);
}

CxHash cx_hbytes(CxAny *v) {
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

void cx_call(CxLambda *fn) {
    auto fn_ptr = (void (*)(void *))fn->ptr;
    fn_ptr(fn->data);
}

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
        *(CxString *)result = cx_string_from_chars(str.data(), str.size());
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

CxString cx_file_read(CxString *path) {
    string s(path->data, path->size);
    auto buf = io::Buffer::from_file(s);
    auto str = buf.read_all();
    return cx_string_from_chars(str.data(), str.size());
}

void cx_json_value_copy(void *data, void *result) {
    auto value = (boost::json::value *)data;
    create_cx_json_result(value, result);
}