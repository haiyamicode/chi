#include <fmt/format.h>
///

#include <backtrace.h>
#include <cstdlib>
#include <cxxabi.h>

#include "runtime/trace.h"

extern "C" {

void *__bt_state = nullptr;

int bt_callback(void *, uintptr_t, const char *filename, int lineno, const char *function) {
    /// demangle function name
    const char *func_name = function;
    int status;
    // char *demangled = abi::__cxa_demangle(function, nullptr, nullptr, &status);
    // if (status == 0) {
    //     func_name = demangled;
    // }

    if (filename && func_name) {
        printf("%s:%d in function %s\n", filename, lineno, func_name);
    }
    return 0;
}

void bt_error_callback(void *, const char *msg, int errnum) {
    printf("Error %d occurred when getting the stacktrace: %s", errnum, msg);
}

void bt_error_callback_create(void *data, const char *msg, int errnum) {
    printf("Error %d occurred when initializing the stacktrace: %s", errnum, msg);
    bool *status = (bool *)data;
    *status = false;
}

bool init_backtrace(const char *filename) {
    bool status = true;
    __bt_state = backtrace_create_state(filename, 0, bt_error_callback_create, (void *)status);
    return status;
}

void print_backtrace() {
    if (!__bt_state) {
        printf("Make sure init_backtrace() is called before calling print_stack_trace()\n");
        abort();
    }
    backtrace_full((backtrace_state *)__bt_state, 0, bt_callback, bt_error_callback, nullptr);
}

void backtrace() { print_backtrace(); }

} // extern "C"