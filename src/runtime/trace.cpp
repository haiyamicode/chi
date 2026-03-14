#include <fmt/format.h>
///

#include <backtrace.h>
#include <cstdlib>
#include <cxxabi.h>
#include <filesystem>

#include "runtime/trace.h"

extern "C" {

void *__bt_state = nullptr;
FILE *bt_output_file = nullptr;

static bool str_contains(const char *s, const char *needle) {
    return s && needle && strstr(s, needle) != nullptr;
}

static std::string normalize_path(const char *path) {
    if (!path || !path[0]) return {};
    namespace fs = std::filesystem;
    try {
        return fs::weakly_canonical(fs::path(path)).lexically_normal().string();
    } catch (...) {
        try {
            return fs::path(path).lexically_normal().string();
        } catch (...) {
            return std::string(path);
        }
    }
}

static bool is_internal_runtime_cpp(const char *filename) {
    static const std::string trace_cpp_path = normalize_path(__FILE__);
    static const std::string internals_cpp_path = []() {
        namespace fs = std::filesystem;
        auto trace_path = normalize_path(__FILE__);
        if (trace_path.empty()) return std::string();
        auto p = fs::path(trace_path).parent_path() / "internals.cpp";
        return normalize_path(p.string().c_str());
    }();

    auto normalized = normalize_path(filename);
    return !normalized.empty() &&
           (normalized == trace_cpp_path || normalized == internals_cpp_path);
}

static bool is_noisy_runtime_frame(const char *filename) {
    if (!filename) return false;
    return str_contains(filename, "libc++") || str_contains(filename, "/c++/v1/") ||
           str_contains(filename, "libunwind") || is_internal_runtime_cpp(filename);
}

int bt_callback(void *, uintptr_t, const char *filename, int lineno, const char *function) {
    if (!bt_output_file || !filename || !function || is_noisy_runtime_frame(filename)) {
        return 0;
    }
    fprintf(bt_output_file, "%s:%d in function %s\n", filename, lineno, function);
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
    __bt_state = backtrace_create_state(filename, 0, bt_error_callback_create, (void *)&status);
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

void set_bt_output_file(FILE *file) { bt_output_file = file; }

bool cx_backtrace_pcinfo(uintptr_t pc, cx_backtrace_pcinfo_callback_t callback, void *data) {
    if (!__bt_state || !callback) {
        return false;
    }
    return backtrace_pcinfo((backtrace_state *)__bt_state, pc, callback, bt_error_callback, data) == 0;
}

} // extern "C"
