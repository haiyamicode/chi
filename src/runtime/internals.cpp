//
// Created by haint on 11/25/18.
//

#define CHI_RUNTIME_HAS_BACKTRACE 1
#include <csignal>
#include <sstream>
#include <uv.h>

#include "internals.h"
#include "sema.h"

extern "C" {
#include "include/tgc/tgc.h"
}

using namespace cx;

#include <inttypes.h>
#include <libunwind.h>

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
    case TypeKind::Reference:
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

void cx_array_new(CxArray *dest) {
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

void cx_panic(CxString message) {
    st.message = {message.data, message.size};
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

void cx_call(CxLambda *fn) {
    auto fn_ptr = (void (*)(void *))fn->ptr;
    fn_ptr(fn->data);
}