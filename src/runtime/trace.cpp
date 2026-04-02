#include <fmt/format.h>
///

#include <backtrace.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <io.h>
#define STDERR_FD 2
#else
#include <cxxabi.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#define STDERR_FD STDERR_FILENO
#ifdef __APPLE__
#include <sys/ucontext.h>
#endif
#if __has_include(<ptrauth.h>)
#include <ptrauth.h>
#endif
#endif

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

// --- Signal-safe fault backtrace ---

void write_fault_message(const char *msg) {
    if (!msg) return;
    size_t len = 0;
    while (msg[len] != '\0') {
        len++;
    }
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    while (len > 0) {
        DWORD written = 0;
        if (!WriteFile(h, msg, (DWORD)len, &written, nullptr) || written == 0) break;
        msg += written;
        len -= (size_t)written;
    }
#else
    while (len > 0) {
        auto written = write(STDERR_FILENO, msg, len);
        if (written <= 0) break;
        msg += written;
        len -= (size_t)written;
    }
#endif
}

static void write_fault_hex_compact(uintptr_t value) {
    char buf[2 + sizeof(uintptr_t) * 2 + 1];
    static const char *digits = "0123456789abcdef";
    int pos = 0;
    buf[pos++] = '0';
    buf[pos++] = 'x';
    bool started = false;
    for (size_t i = 0; i < sizeof(uintptr_t) * 2; i++) {
        auto shift = (sizeof(uintptr_t) * 2 - 1 - i) * 4;
        auto digit = (unsigned)((value >> shift) & 0xF);
        if (digit != 0 || started || i == sizeof(uintptr_t) * 2 - 1) {
            buf[pos++] = digits[digit];
            started = true;
        }
    }
    buf[pos] = '\0';
    write_fault_message(buf);
}

static void write_fault_uint(unsigned int value) {
    char buf[16];
    int i = 0;
    if (value == 0) {
        write_fault_message("0");
        return;
    }
    while (value > 0 && i < (int)sizeof(buf) - 1) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    // Reverse in-place and null-terminate
    for (int l = 0, r = i - 1; l < r; l++, r--) {
        char tmp = buf[l]; buf[l] = buf[r]; buf[r] = tmp;
    }
    buf[i] = '\0';
    write_fault_message(buf);
}

static uintptr_t normalize_fault_ip(uintptr_t ip) {
#if __has_include(<ptrauth.h>)
    ip = (uintptr_t)ptrauth_strip((void *)ip, ptrauth_key_return_address);
#endif
    return ip;
}

struct FaultBtContext {
    uintptr_t ip;
    bool printed;
};

static int fault_bt_callback(void *data, uintptr_t, const char *filename, int lineno,
                             const char *function) {
    auto ctx = (FaultBtContext *)data;
    if (!filename && !function) {
        return 0;
    }

    write_fault_message("  ");
    if (filename) {
        write_fault_message(filename);
    } else {
        write_fault_message("<unknown>");
    }
    if (lineno > 0) {
        write_fault_message(":");
        write_fault_uint((unsigned int)lineno);
    }
    if (function) {
        write_fault_message(" in function ");
        write_fault_message(function);
    }
    write_fault_message("\n");
    ctx->printed = true;
    return 0;
}

// Returns true if this is a user-code frame worth printing, false to stop.
static bool write_fault_frame(uintptr_t pc) {
    auto normalized = normalize_fault_ip(pc);
    FaultBtContext bt_ctx{normalized, false};
    cx_backtrace_pcinfo(normalized, fault_bt_callback, &bt_ctx);
    if (bt_ctx.printed) return true;

#ifdef _WIN32
    // Fallback: try DbgHelp SymFromAddr for symbol name
    HANDLE process = GetCurrentProcess();
    alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    auto sym = (SYMBOL_INFO *)sym_buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, (DWORD64)pc, &displacement, sym) && sym->Name[0]) {
        if (strstr(sym->Name, "mainCRTStartup") ||
            strstr(sym->Name, "__scrt") ||
            strstr(sym->Name, "BaseThreadInitThunk") ||
            strstr(sym->Name, "RtlUserThreadStart")) {
            return false;
        }
        write_fault_message("  ");
        write_fault_message(sym->Name);
        write_fault_message(" + ");
        write_fault_hex_compact((uintptr_t)displacement);
        write_fault_message("\n");
        return true;
    }
#else
    // Fallback: try dladdr for symbol name
    Dl_info dli;
    if (dladdr((void *)pc, &dli) && dli.dli_sname) {
        // Stop at C runtime entry points
        if (strstr(dli.dli_sname, "start") ||
            strstr(dli.dli_sname, "__libc") ||
            strstr(dli.dli_sname, "_rt0")) {
            return false;
        }
        write_fault_message("  ");
        write_fault_message(dli.dli_sname);
        write_fault_message(" + ");
        write_fault_hex_compact(pc - (uintptr_t)dli.dli_saddr);
        write_fault_message("\n");
        return true;
    }
#endif
    return false;
}

void write_fault_backtrace(void *ucontext) {
    write_fault_message("backtrace:\n");

    uintptr_t pc = 0;
    uintptr_t fp = 0;

#if defined(_WIN32)
    if (ucontext) {
        auto ctx = (CONTEXT *)ucontext;
#if defined(_M_X64) || defined(__x86_64__)
        pc = (uintptr_t)ctx->Rip;
        fp = (uintptr_t)ctx->Rbp;
#elif defined(_M_ARM64) || defined(__aarch64__)
        pc = (uintptr_t)ctx->Pc;
        fp = (uintptr_t)ctx->Fp;
#elif defined(_M_IX86)
        pc = (uintptr_t)ctx->Eip;
        fp = (uintptr_t)ctx->Ebp;
#endif
    }
#elif defined(__APPLE__) && defined(__aarch64__)
    if (ucontext) {
        auto uc = (ucontext_t *)ucontext;
        pc = uc->uc_mcontext->__ss.__pc;
        fp = uc->uc_mcontext->__ss.__fp;
    }
#elif defined(__APPLE__) && defined(__x86_64__)
    if (ucontext) {
        auto uc = (ucontext_t *)ucontext;
        pc = uc->uc_mcontext->__ss.__rip;
        fp = uc->uc_mcontext->__ss.__rbp;
    }
#elif defined(__linux__) && defined(__aarch64__)
    if (ucontext) {
        auto uc = (ucontext_t *)ucontext;
        pc = uc->uc_mcontext.pc;
        fp = uc->uc_mcontext.regs[29];
    }
#elif defined(__linux__) && defined(__x86_64__)
    if (ucontext) {
        auto uc = (ucontext_t *)ucontext;
        pc = uc->uc_mcontext.gregs[REG_RIP];
        fp = uc->uc_mcontext.gregs[REG_RBP];
    }
#endif

    if (!pc) {
        write_fault_message("  <unavailable>\n");
        return;
    }

    // Print the faulting frame
    write_fault_frame(pc);

    // Walk the frame pointer chain
    int depth = 0;
    while (fp && depth < 64) {
        auto frame = (uintptr_t *)fp;
        uintptr_t next_fp = frame[0];
        uintptr_t ret_addr = frame[1];
        if (!ret_addr) break;

        ret_addr = normalize_fault_ip(ret_addr);
        if (!write_fault_frame(ret_addr)) break;

        if (next_fp <= fp) break; // stack grows down; FP must increase
        fp = next_fp;
        depth++;
    }
}

} // extern "C"
