//
// Created by haint on 11/25/18.
//

#define CHI_RUNTIME_HAS_BACKTRACE 1
#include "fmt/core.h"
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <type_traits>
#include <algorithm>
#include <atomic>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <sstream>
#include <vector>
#include <string>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <dirent.h>
#include <uv.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif
#if __has_include(<ptrauth.h>)
#include <ptrauth.h>
#endif

#include "internals.h"
#include "format.h"
#include "sema.h"
#include "trace.h"
#include "util.h"

extern "C" {
#include "include/hashdict/hashdict.h"
#include "include/tgc/tgc.h"
}

using namespace cx;

#include <inttypes.h>

#define BOOST_JSON_STANDALONE
#include <boost/json/src.hpp>

struct StackTrace {
    string message;
    char trace[8096];
    string site_file;
    uint32_t site_line = 0;
    uint32_t site_col = 0;
    bool has_site = false;
    // Typed error support (throw/catch)
    void *error_type_info = nullptr;  // TypeInfo* from Error interface
    void *error_data_ptr = nullptr;   // data_ptr from Error interface (heap-allocated struct)
    void *error_vtable_ptr = nullptr; // vtable_ptr from Error interface
    uint32_t error_type_id = 0;       // ChiType::id for downcast matching
};

namespace {
#if CHI_DEBUG_ALLOCATOR_RUNTIME
std::atomic<bool> g_debug_allocator_enabled = false;
std::atomic<uint64_t> g_debug_live_bytes = 0;
std::atomic<uint64_t> g_debug_peak_live_bytes = 0;
std::atomic<uint64_t> g_debug_live_alloc_count = 0;
std::atomic<uint64_t> g_debug_peak_live_alloc_count = 0;
std::atomic<uint64_t> g_debug_alloc_count = 0;
std::atomic<uint64_t> g_debug_free_count = 0;

static uint64_t alloc_usable_size_or_zero(void *address) {
    if (!address) return 0;
#if defined(__APPLE__)
    return malloc_size(address);
#elif defined(__linux__)
    return malloc_usable_size(address);
#else
    return 0;
#endif
}

static void update_debug_peak(uint64_t live_bytes) {
    auto peak = g_debug_peak_live_bytes.load(std::memory_order_relaxed);
    while (live_bytes > peak &&
           !g_debug_peak_live_bytes.compare_exchange_weak(
               peak, live_bytes, std::memory_order_relaxed, std::memory_order_relaxed
           )) {
    }
}

static void update_debug_alloc_peak(uint64_t live_alloc_count) {
    auto peak = g_debug_peak_live_alloc_count.load(std::memory_order_relaxed);
    while (live_alloc_count > peak &&
           !g_debug_peak_live_alloc_count.compare_exchange_weak(
               peak, live_alloc_count, std::memory_order_relaxed, std::memory_order_relaxed
           )) {
    }
}
#endif
} // namespace

struct CxCapture {
    uint32_t ref_count;
    uint32_t payload_size;
    TypeInfo *type;
    void (*dtor)(void *);
    void *data;
};

static tgc_t gc;
static thread_local StackTrace st;
static int32_t program_argc = 0;
static char **program_argv = nullptr;

namespace {

void set_command_result_error(CxCommandResult *result, int32_t exit_code, const char *message) {
    if (!result) return;
    result->exit_code = exit_code;
    result->stdout_text = {};
    result->stderr_text = {};
    if (message) {
        cx_string_from_chars(message, (uint32_t)strlen(message), &result->stderr_text);
    }
}

void init_command_result(CxCommandResult *result) {
    if (!result) return;
    result->exit_code = -1;
    result->stdout_text = {};
    result->stderr_text = {};
}

int32_t decode_wait_status(int status) {
    if (status == -1) {
        return -1;
    }
#ifdef WIFEXITED
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
#endif
    return status;
}

int read_chunk_from_fd(int fd, string &output) {
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            output.append(buf, (size_t)n);
            return 1;
        }
        if (n == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

template <typename ExecFn>
void run_child_command(ExecFn exec_child, CxCommandResult *result) {
    init_command_result(result);

    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) == -1) {
        set_command_result_error(result, -1, strerror(errno));
        return;
    }
    if (pipe(stderr_pipe) == -1) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        set_command_result_error(result, -1, strerror(errno));
        return;
    }

    auto pid = fork();
    if (pid == -1) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        set_command_result_error(result, -1, strerror(errno));
        return;
    }

    if (pid == 0) {
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        exec_child();
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    struct StreamCapture {
        int fd;
        string text;
        bool open = true;
    };
    StreamCapture captures[2] = {{stdout_pipe[0]}, {stderr_pipe[0]}};

    while (captures[0].open || captures[1].open) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int max_fd = -1;
        for (auto &cap : captures) {
            if (cap.open) {
                FD_SET(cap.fd, &rfds);
                max_fd = std::max(max_fd, cap.fd);
            }
        }

        if (select(max_fd + 1, &rfds, nullptr, nullptr, nullptr) == -1) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (auto &cap : captures) {
            if (cap.open && FD_ISSET(cap.fd, &rfds)) {
                if (read_chunk_from_fd(cap.fd, cap.text) <= 0) {
                    cap.open = false;
                    close(cap.fd);
                }
            }
        }
    }

    for (auto &cap : captures) {
        if (cap.open) {
            close(cap.fd);
        }
    }

    auto &stdout_text = captures[0].text;
    auto &stderr_text = captures[1].text;

    int status = 0;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            status = -1;
            break;
        }
    }

    result->exit_code = decode_wait_status(status);
    cx_string_from_chars(stdout_text.data(), (uint32_t)stdout_text.size(), &result->stdout_text);
    cx_string_from_chars(stderr_text.data(), (uint32_t)stderr_text.size(), &result->stderr_text);
}

struct MainThreadUvTask {
    MainThreadUvTask *next = nullptr;
    virtual ~MainThreadUvTask() = default;
    virtual void run() = 0;
};

static uv_loop_t *main_uv_loop = nullptr;
static uv_async_t main_uv_async;
static uv_mutex_t main_uv_mutex;
static bool main_uv_mutex_ready = false;
static MainThreadUvTask *main_uv_task_head = nullptr;
static MainThreadUvTask *main_uv_task_tail = nullptr;
static uv_thread_t main_uv_thread_id;
static bool main_uv_thread_id_ready = false;
static bool main_uv_async_ready = false;
static bool main_uv_stopping = false;

struct SpawnedThread {
    uv_thread_t thread;
    uv_mutex_t mutex;
    bool done = false;
    void (*fn_ptr)(void *);
    void *captures;
    SpawnedThread *next = nullptr;
};

static uv_mutex_t spawned_thread_mutex;
static bool spawned_thread_mutex_ready = false;
static SpawnedThread *spawned_thread_head = nullptr;

static bool is_main_uv_thread() {
    if (!main_uv_thread_id_ready) {
        return true;
    }
    auto current = uv_thread_self();
    return uv_thread_equal(&main_uv_thread_id, &current) != 0;
}

static void run_pending_main_uv_tasks() {
    while (true) {
        MainThreadUvTask *task = nullptr;
        uv_mutex_lock(&main_uv_mutex);
        if (main_uv_task_head) {
            task = main_uv_task_head;
            main_uv_task_head = task->next;
            if (!main_uv_task_head) {
                main_uv_task_tail = nullptr;
            }
        }
        uv_mutex_unlock(&main_uv_mutex);
        if (!task) {
            break;
        }
        task->run();
    }
}

static void main_uv_async_cb(uv_async_t *) {
    run_pending_main_uv_tasks();
}

static void init_main_uv_dispatcher() {
    if (main_uv_async_ready) {
        return;
    }

    main_uv_loop = uv_default_loop();
    if (!main_uv_mutex_ready) {
        uv_mutex_init(&main_uv_mutex);
        main_uv_mutex_ready = true;
    }
    if (!spawned_thread_mutex_ready) {
        uv_mutex_init(&spawned_thread_mutex);
        spawned_thread_mutex_ready = true;
    }
    main_uv_thread_id = uv_thread_self();
    main_uv_thread_id_ready = true;
    auto r = uv_async_init(main_uv_loop, &main_uv_async, main_uv_async_cb);
    if (r < 0) {
        panic("failed to initialize main-thread libuv dispatcher: {}", uv_strerror(r));
    }
    uv_unref((uv_handle_t *)&main_uv_async);
    main_uv_async_ready = true;
    main_uv_stopping = false;
}

static void enqueue_main_uv_task(MainThreadUvTask *task) {
    uv_mutex_lock(&main_uv_mutex);
    if (!main_uv_async_ready || main_uv_stopping) {
        uv_mutex_unlock(&main_uv_mutex);
        panic("main-thread libuv dispatcher is not available");
    }
    task->next = nullptr;
    if (main_uv_task_tail) {
        main_uv_task_tail->next = task;
    } else {
        main_uv_task_head = task;
    }
    main_uv_task_tail = task;
    uv_mutex_unlock(&main_uv_mutex);

    auto r = uv_async_send(&main_uv_async);
    if (r < 0) {
        panic("failed to wake main-thread libuv dispatcher: {}", uv_strerror(r));
    }
}

static bool is_spawned_thread_done(SpawnedThread *thread) {
    uv_mutex_lock(&thread->mutex);
    bool done = thread->done;
    uv_mutex_unlock(&thread->mutex);
    return done;
}

static SpawnedThread *take_finished_spawned_thread() {
    uv_mutex_lock(&spawned_thread_mutex);
    SpawnedThread *prev = nullptr;
    auto *cur = spawned_thread_head;
    while (cur) {
        if (is_spawned_thread_done(cur)) {
            if (prev) {
                prev->next = cur->next;
            } else {
                spawned_thread_head = cur->next;
            }
            cur->next = nullptr;
            uv_mutex_unlock(&spawned_thread_mutex);
            return cur;
        }
        prev = cur;
        cur = cur->next;
    }
    uv_mutex_unlock(&spawned_thread_mutex);
    return nullptr;
}

static bool has_spawned_threads() {
    uv_mutex_lock(&spawned_thread_mutex);
    bool has_threads = spawned_thread_head != nullptr;
    uv_mutex_unlock(&spawned_thread_mutex);
    return has_threads;
}

static void join_finished_spawned_threads() {
    while (auto *thread = take_finished_spawned_thread()) {
        uv_thread_join(&thread->thread);
        uv_mutex_destroy(&thread->mutex);
        delete thread;
    }
}

static void pump_main_uv_until_threads_done(uv_loop_t *loop) {
    while (has_spawned_threads()) {
        run_pending_main_uv_tasks();
        uv_run(loop, UV_RUN_NOWAIT);
        join_finished_spawned_threads();
        if (has_spawned_threads()) {
            usleep(1000);
        }
    }
}

static void spawned_thread_entry(void *arg) {
    auto *thread = (SpawnedThread *)arg;
    void *payload = thread->captures ? cx_capture_get_data(thread->captures) : nullptr;
    thread->fn_ptr(payload);
    if (thread->captures) {
        cx_capture_release(thread->captures);
    }
    uv_mutex_lock(&thread->mutex);
    thread->done = true;
    uv_mutex_unlock(&thread->mutex);
}

template <typename Fn>
auto run_on_main_uv_thread_sync(Fn &&fn) -> decltype(fn()) {
    using ReturnType = decltype(fn());

    if (is_main_uv_thread()) {
        return fn();
    }

    if constexpr (std::is_void_v<ReturnType>) {
        struct Task final : MainThreadUvTask {
            std::decay_t<Fn> fn;
            uv_mutex_t mutex;
            uv_cond_t cond;
            bool done = false;

            explicit Task(Fn &&cb) : fn(std::forward<Fn>(cb)) {
                uv_mutex_init(&mutex);
                uv_cond_init(&cond);
            }

            ~Task() override {
                uv_cond_destroy(&cond);
                uv_mutex_destroy(&mutex);
            }

            void run() override {
                fn();
                uv_mutex_lock(&mutex);
                done = true;
                uv_cond_signal(&cond);
                uv_mutex_unlock(&mutex);
            }
        };

        Task task(std::forward<Fn>(fn));
        enqueue_main_uv_task(&task);

        uv_mutex_lock(&task.mutex);
        while (!task.done) {
            uv_cond_wait(&task.cond, &task.mutex);
        }
        uv_mutex_unlock(&task.mutex);
        return;
    } else {
        struct Task final : MainThreadUvTask {
            std::decay_t<Fn> fn;
            uv_mutex_t mutex;
            uv_cond_t cond;
            ReturnType result{};
            bool done = false;

            explicit Task(Fn &&cb) : fn(std::forward<Fn>(cb)) {
                uv_mutex_init(&mutex);
                uv_cond_init(&cond);
            }

            ~Task() override {
                uv_cond_destroy(&cond);
                uv_mutex_destroy(&mutex);
            }

            void run() override {
                result = fn();
                uv_mutex_lock(&mutex);
                done = true;
                uv_cond_signal(&cond);
                uv_mutex_unlock(&mutex);
            }
        };

        Task task(std::forward<Fn>(fn));
        enqueue_main_uv_task(&task);

        uv_mutex_lock(&task.mutex);
        while (!task.done) {
            uv_cond_wait(&task.cond, &task.mutex);
        }
        uv_mutex_unlock(&task.mutex);
        return task.result;
    }
}

template <typename Fn>
void run_on_main_uv_thread_async(Fn &&fn) {
    if (is_main_uv_thread()) {
        fn();
        return;
    }

    struct Task final : MainThreadUvTask {
        std::decay_t<Fn> fn;

        explicit Task(Fn &&cb) : fn(std::forward<Fn>(cb)) {}

        void run() override {
            auto self = this;
            fn();
            delete self;
        }
    };

    enqueue_main_uv_task(new Task(std::forward<Fn>(fn)));
}

} // namespace

static void *get_any_data(const CxAny *v) {
    if (v->inlined) {
        return (void *)v->storage.inline_data;
    }
    return v->storage.heap_ptr;
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

#define _FORMAT_INT(T, v) fmt::format("{}", (T)(*(T *)(&v.storage.inline_data)))
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

#define _FORMAT_FLOAT(T, v) fmt::format("{}", (T)(*(T *)(&v.storage.inline_data)))
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
    if (type->meta_table_len == 0 || !type->meta_table) {
        return nullptr;
    }

    auto table = type->meta_table;
    for (int i = 0; i < type->meta_table_len; i++) {
        auto entry = &table[i];
        if (entry->symbol == IntrinsicSymbol::Display && entry->vtable_index >= 0) {
            return program_vtable[entry->vtable_index];
        }
    }
    return nullptr;
}

static std::string encode_utf8(uint32_t codepoint) {
    std::string result;
    if (codepoint <= 0x7F) {
        result.push_back((char)codepoint);
    } else if (codepoint <= 0x7FF) {
        result.push_back((char)(0xC0 | (codepoint >> 6)));
        result.push_back((char)(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        result.push_back((char)(0xE0 | (codepoint >> 12)));
        result.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
        result.push_back((char)(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
        result.push_back((char)(0xF0 | (codepoint >> 18)));
        result.push_back((char)(0x80 | ((codepoint >> 12) & 0x3F)));
        result.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
        result.push_back((char)(0x80 | (codepoint & 0x3F)));
    }
    return result;
}

static std::string get_type_name(TypeInfo *type) {
    if (!type || type->name_len == 0) {
        return "";
    }
    return std::string(type->name, type->name_len);
}

static bool is_field_visible(const TypeFieldEntry &field) {
    return field.visibility == (int32_t)Visibility::Public ||
           field.visibility == (int32_t)Visibility::Protected;
}

static std::string get_value_display_from_ptr(TypeInfo *type, const void *data_p);

static std::string get_struct_display(TypeInfo *type, const void *data_p) {
    auto type_name = get_type_name(type);
    if (type_name.empty()) {
        type_name = "struct";
    }

    std::stringstream ss;
    ss << type_name << "{";
    bool first = true;
    for (int i = 0; i < type->field_table_len; i++) {
        auto &field = type->field_table[i];
        if (!field.type || !is_field_visible(field)) {
            continue;
        }

        auto field_ptr = (const uint8_t *)data_p + field.offset;

        if (!first) {
            ss << ", ";
        }
        first = false;
        ss.write(field.name, field.name_len);
        ss << ": " << get_value_display_from_ptr(field.type, field_ptr);
    }
    ss << "}";
    return ss.str();
}

static std::string get_indexed_display(TypeInfo *type, const void *data_p, const char *open,
                                       const char *close) {
    std::stringstream ss;
    ss << open;
    bool first = true;
    for (int i = 0; i < type->field_table_len; i++) {
        auto &field = type->field_table[i];
        if (!field.type) {
            continue;
        }

        auto field_ptr = (const uint8_t *)data_p + field.offset;
        if (!first) {
            ss << ", ";
        }
        first = false;
        ss << get_value_display_from_ptr(field.type, field_ptr);
    }
    ss << close;
    return ss.str();
}

static std::string get_value_display_from_ptr(TypeInfo *type, const void *data_p) {
    switch ((TypeKind)type->kind) {
    case TypeKind::Null:
        return "null";
    case TypeKind::Unit:
        return "()";
    case TypeKind::Any: {
        auto any = (const CxAny *)data_p;
        if (!any->type) {
            return "null";
        }
        return get_value_display_from_ptr(any->type, get_any_data(any));
    }
    case TypeKind::String: {
        auto s = (const CxString *)data_p;
        return string(s->data, s->size);
    }
    case TypeKind::Bool:
        return fmt::format("{}", *(const bool *)data_p);
    case TypeKind::Byte: {
        uint8_t char_value = *(const uint8_t *)data_p;
        // Only display as character if it's printable, otherwise show as number
        if (char_value >= 32 && char_value <= 126) {
            return fmt::format("{}", (char)char_value);
        }
        return fmt::format("{}", char_value);
    }
    case TypeKind::Rune: {
        uint32_t rune_value = *(const uint32_t *)data_p;
        return encode_utf8(rune_value);
    }
    case TypeKind::Int: {
        auto &spec = type->data.int_;
        if (spec.is_unsigned) {
            switch (spec.bit_count) {
            case 8:
                return fmt::format("{}", *(const uint8_t *)data_p);
            case 16:
                return fmt::format("{}", *(const uint16_t *)data_p);
            case 32:
                return fmt::format("{}", *(const uint32_t *)data_p);
            case 64:
                return fmt::format("{}", *(const uint64_t *)data_p);
            default:
                break;
            }
        } else {
            switch (spec.bit_count) {
            case 8:
                return fmt::format("{}", *(const int8_t *)data_p);
            case 16:
                return fmt::format("{}", *(const int16_t *)data_p);
            case 32:
                return fmt::format("{}", *(const int32_t *)data_p);
            case 64:
                return fmt::format("{}", *(const int64_t *)data_p);
            default:
                break;
            }
        }
        return "0";
    }
    case TypeKind::Float: {
        auto &spec = type->data.float_;
        switch (spec.bit_count) {
        case 32:
            return fmt::format("{}", *(const float *)data_p);
        case 64:
            return fmt::format("{}", *(const double *)data_p);
        default:
            return "0";
        }
    }
    case TypeKind::Fn:
        return "<function>";
    case TypeKind::FnLambda:
        return "<function_lambda>";
    case TypeKind::Pointer:
    case TypeKind::Reference:
    case TypeKind::MutRef:
    case TypeKind::MoveRef: {
        auto ptr = *(void *const *)data_p;
        auto elem_ti = type->data.pointer.elem;
        if (elem_ti && ptr) {
            return get_value_display_from_ptr(elem_ti, ptr);
        }
        return fmt::format("{:#x}", (intptr_t)ptr);
    }
    case TypeKind::Optional: {
        auto has_value = *(const bool *)data_p;
        if (has_value) {
            auto elem_ti = type->data.pointer.elem;
            if (elem_ti) {
                // Value offset = optional size - elem size
                auto value_offset = type->size - elem_ti->size;
                auto value_p = (const uint8_t *)data_p + value_offset;
                return get_value_display_from_ptr(elem_ti, value_p);
            }
            return "<optional>";
        } else {
            return "null";
        }
    }
    case TypeKind::FixedArray:
        if (type->field_table && type->field_table_len >= 0) {
            return get_indexed_display(type, data_p, "[", "]");
        }
        return "[]";
    case TypeKind::Tuple:
        if (type->field_table && type->field_table_len >= 0) {
            return get_indexed_display(type, data_p, "(", ")");
        }
        return "()";
    case cx::TypeKind::Array:
    case cx::TypeKind::Span:
    case cx::TypeKind::Struct: {
        auto display_method = get_typemeta_display_method(type);
        if (display_method) {
            auto fn = (void (*)(void *a, void *b))display_method;
            CxString s;
            fn(&s, const_cast<void *>(data_p));
            auto str = string(s.data, s.size);
            cx_string_delete(&s);
            return str;
        }
        if (type->kind == (int32_t)TypeKind::Struct && type->field_table &&
            type->field_table_len >= 0) {
            return get_struct_display(type, data_p);
        }
        return fmt::format("<TypeKind:{}>", PRINT_ENUM((TypeKind)type->kind));
    }
    case cx::TypeKind::EnumValue: {
        auto display_method = get_typemeta_display_method(type);
        if (display_method) {
            auto fn = (void (*)(void *a, void *b))display_method;
            CxString s;
            fn(&s, const_cast<void *>(data_p));
            auto str = string(s.data, s.size);
            cx_string_delete(&s);
            return str;
        }
        auto ev = (const CxEnumValue *)data_p;
        string display_name(ev->display_name->data, ev->display_name->size);
        return fmt::format("{}", display_name);
    }
    default:
        return fmt::format("<TypeKind:{}>", PRINT_ENUM(type->kind));
    }
}

std::string get_value_display(const CxAny &v) {
    return get_value_display_from_ptr(v.type, get_any_data(&v));
}

void cx_print_any(CxAny *value) { fmt::print("{}", get_value_display(*value)); }

void cx_print_number(uint64_t value) { fmt::print("{}\n", value); }

static string format_cstr(CxString &format, const CxSlice &values) {
    int val_i = 0;
    std::stringstream ss;
    char spec_buf[64];

    for (int i = 0; i < (int)format.size; i++) {
        auto c = format.data[i];
        if (c == '{') {
            // Check for '{{' escape
            if (i + 1 < (int)format.size && format.data[i + 1] == '{') {
                ss.write("{", 1);
                i++;
                continue;
            }
            // Collect everything until '}'
            int spec_len = 0;
            i++;
            while (i < (int)format.size && format.data[i] != '}') {
                if (spec_len < (int)sizeof(spec_buf) - 1) {
                    spec_buf[spec_len++] = format.data[i];
                }
                i++;
            }
            // i now points at '}' (or past end)
            if (val_i < values.size) {
                auto &val = ((CxAny *)values.data)[val_i++];
                if (spec_len > 0 && spec_buf[0] == ':') {
                    auto fs = parse_format_spec(spec_buf + 1, spec_len - 1);
                    ss << apply_format(val, fs);
                } else {
                    ss << get_value_display(val);
                }
            }
        } else if (c == '}') {
            // Check for '}}' escape
            if (i + 1 < (int)format.size && format.data[i + 1] == '}') {
                ss.write("}", 1);
                i++;
            }
        } else {
            ss.write(&c, 1);
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
    fmt::print("{}", s);
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

extern "C" void cx_typeinfo_destroy(TypeInfo *type, void *data) {
    if (!type || !data || !type->destructor) {
        return;
    }
    auto dtor = (void (*)(void *))type->destructor;
    dtor(data);
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

void cx_array_append(CxArray *dest, CxArray *src, uint32_t elem_size) {
    auto new_size = dest->size + src->size;
    cx_array_reserve(dest, elem_size, new_size);
    memcpy(((char *)dest->data) + dest->size * elem_size, src->data, src->size * elem_size);
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

static void capture_unhandled_state(CxString *message) {
    if (message && message->data) {
        st.message = {message->data, message->size};
    } else {
        st.message.clear();
    }
    memset(st.trace, 0, sizeof(st.trace));
    auto bt_output_file = fmemopen(st.trace, sizeof(st.trace), "w");
    set_bt_output_file(bt_output_file);
    backtrace();
    fclose(bt_output_file);
}

static void print_unhandled_state() {
    auto message = st.message.empty() ? string("unhandled throw") : st.message;
    print("panic: {}\n", message);
    if (st.has_site) {
        print("at {}:{}:{}\n", st.site_file, st.site_line, st.site_col);
    }
    print(st.trace);
}

static string get_error_message(void *data_ptr, void *vtable_ptr) {
    if (!data_ptr || !vtable_ptr) {
        return "";
    }
    auto vtable = (void **)vtable_ptr;
    auto method_ptr = vtable[3];
    if (!method_ptr) {
        return "";
    }
    auto fn = (void (*)(CxString *, void *))method_ptr;
    CxString s = {};
    fn(&s, data_ptr);
    auto message = string(s.data, s.size);
    cx_string_delete(&s);
    return message;
}

void cx_set_panic_location(CxString *file, uint32_t line, uint32_t col) {
    if (!file || !file->data) {
        return;
    }
    st.site_file = string(file->data, file->size);
    st.site_line = line;
    st.site_col = col;
    st.has_site = true;
}

void cx_clear_panic_location() {
    st.site_file.clear();
    st.site_line = 0;
    st.site_col = 0;
    st.has_site = false;
}

void cx_throw(void *type_info, void *data_ptr, void *vtable_ptr, uint32_t type_id) {
    st.error_type_info = type_info;
    st.error_data_ptr = data_ptr;
    st.error_vtable_ptr = vtable_ptr;
    st.error_type_id = type_id;
    CxString message = {};
    auto rendered = get_error_message(data_ptr, vtable_ptr);
    message.data = (char *)rendered.data();
    message.size = (uint32_t)rendered.size();
    capture_unhandled_state(&message);
    throw data_ptr; // non-null, distinguishes from panic's NULL
}

void *cx_get_error_type_info() { return st.error_type_info; }

void *cx_get_error_data() { return st.error_data_ptr; }

void *cx_get_error_vtable() { return st.error_vtable_ptr; }

uint32_t cx_get_error_type_id() { return st.error_type_id; }

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
    __atomic_fetch_add(&capture->ref_count, 1u, __ATOMIC_RELAXED);
}

void cx_capture_release(void *capture_ptr) {
    if (!capture_ptr) return;
    auto capture = (CxCapture *)capture_ptr;
    auto old = __atomic_fetch_sub(&capture->ref_count, 1u, __ATOMIC_ACQ_REL);
    if (old == 1) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
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

void *cx_gc_realloc(void *address, uint32_t size, void *_ignored) {
    return tgc_realloc(&gc, address, size);
}

void cx_gc_free(void *address) { tgc_free(&gc, address); }

#if CHI_DEBUG_ALLOCATOR_RUNTIME
void cx_debug_allocator_set_enabled(bool enabled) {
    g_debug_allocator_enabled.store(enabled, std::memory_order_relaxed);
}

bool cx_debug_allocator_is_enabled() {
    return g_debug_allocator_enabled.load(std::memory_order_relaxed);
}

void cx_debug_allocator_reset() {
    g_debug_live_bytes.store(0, std::memory_order_relaxed);
    g_debug_peak_live_bytes.store(0, std::memory_order_relaxed);
    g_debug_live_alloc_count.store(0, std::memory_order_relaxed);
    g_debug_peak_live_alloc_count.store(0, std::memory_order_relaxed);
    g_debug_alloc_count.store(0, std::memory_order_relaxed);
    g_debug_free_count.store(0, std::memory_order_relaxed);
}

void *cx_malloc(uint32_t size, void *_ignored) {
    if (!g_debug_allocator_enabled.load(std::memory_order_relaxed)) {
        return malloc(size);
    }
    auto address = malloc(size);
    if (!address) {
        return nullptr;
    }
    auto actual = alloc_usable_size_or_zero(address);
    g_debug_alloc_count.fetch_add(1, std::memory_order_relaxed);
    auto live_alloc_count =
        g_debug_live_alloc_count.fetch_add(1, std::memory_order_relaxed) + 1;
    auto live = g_debug_live_bytes.fetch_add(actual, std::memory_order_relaxed) + actual;
    update_debug_peak(live);
    update_debug_alloc_peak(live_alloc_count);
    return address;
}

void *cx_realloc(void *address, uint32_t size, void *_ignored) {
    if (!g_debug_allocator_enabled.load(std::memory_order_relaxed)) {
        return realloc(address, size);
    }
    auto old_size = alloc_usable_size_or_zero(address);
    bool had_old = address != nullptr;
    auto new_address = realloc(address, size);
    if (had_old && !new_address && size != 0) {
        return nullptr;
    }
    auto new_size = alloc_usable_size_or_zero(new_address);
    if (!had_old && new_address) {
        g_debug_alloc_count.fetch_add(1, std::memory_order_relaxed);
        auto live_alloc_count =
            g_debug_live_alloc_count.fetch_add(1, std::memory_order_relaxed) + 1;
        auto live = g_debug_live_bytes.fetch_add(new_size, std::memory_order_relaxed) + new_size;
        update_debug_peak(live);
        update_debug_alloc_peak(live_alloc_count);
        return new_address;
    }
    if (had_old && !new_address) {
        g_debug_free_count.fetch_add(1, std::memory_order_relaxed);
        g_debug_live_alloc_count.fetch_sub(1, std::memory_order_relaxed);
        g_debug_live_bytes.fetch_sub(old_size, std::memory_order_relaxed);
        return new_address;
    }
    auto live =
        g_debug_live_bytes.fetch_sub(old_size, std::memory_order_relaxed) - old_size + new_size;
    g_debug_live_bytes.store(live, std::memory_order_relaxed);
    update_debug_peak(live);
    return new_address;
}

void cx_free(void *address) {
    if (!g_debug_allocator_enabled.load(std::memory_order_relaxed)) {
        free(address);
        return;
    }
    if (!address) {
        return;
    }
    auto actual = alloc_usable_size_or_zero(address);
    g_debug_free_count.fetch_add(1, std::memory_order_relaxed);
    g_debug_live_alloc_count.fetch_sub(1, std::memory_order_relaxed);
    g_debug_live_bytes.fetch_sub(actual, std::memory_order_relaxed);
    free(address);
}

uint64_t cx_debug_live_bytes() { return g_debug_live_bytes.load(std::memory_order_relaxed); }
uint64_t cx_debug_peak_live_bytes() {
    return g_debug_peak_live_bytes.load(std::memory_order_relaxed);
}
uint64_t cx_debug_live_alloc_count() {
    return g_debug_live_alloc_count.load(std::memory_order_relaxed);
}
uint64_t cx_debug_peak_live_alloc_count() {
    return g_debug_peak_live_alloc_count.load(std::memory_order_relaxed);
}
uint64_t cx_debug_alloc_count() { return g_debug_alloc_count.load(std::memory_order_relaxed); }
uint64_t cx_debug_free_count() { return g_debug_free_count.load(std::memory_order_relaxed); }
#else
void cx_debug_allocator_set_enabled(bool enabled) { (void)enabled; }
bool cx_debug_allocator_is_enabled() { return false; }
void cx_debug_allocator_reset() {}
void *cx_malloc(uint32_t size, void *_ignored) { return malloc(size); }
void *cx_realloc(void *address, uint32_t size, void *_ignored) { return realloc(address, size); }
void cx_free(void *address) { free(address); }
uint64_t cx_debug_live_bytes() { return 0; }
uint64_t cx_debug_peak_live_bytes() { return 0; }
uint64_t cx_debug_live_alloc_count() { return 0; }
uint64_t cx_debug_peak_live_alloc_count() { return 0; }
uint64_t cx_debug_alloc_count() { return 0; }
uint64_t cx_debug_free_count() { return 0; }
#endif

void cx_debug(void *ptr) {
    auto p = (CxAny *)ptr;
    auto data_p = get_any_data(p);
    auto v_p = (CxEnumValue *)data_p;
    fmt::print("{}\n", data_p);
    // to be implemented
}

void cx_memset(void *dest, uint8_t value, uint32_t size) { memset(dest, value, size); }

void signal_handler(int signal_num) {
    print_unhandled_state();
    exit(1);
}

static void terminate_handler() {
    auto current = std::current_exception();
    if (current) {
        try {
            std::rethrow_exception(current);
        } catch (void *ptr) {
            if (ptr == nullptr || !st.message.empty()) {
                print_unhandled_state();
                exit(1);
            }
        } catch (...) {
        }
    }
    abort();
}

static bool is_probable_null_fault(uintptr_t address) {
    return address < 4096;
}

#ifdef _WIN32
static LONG WINAPI fault_exception_filter(EXCEPTION_POINTERS *ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const char *message = "panic: segmentation fault\n";
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2 &&
        is_probable_null_fault((uintptr_t)ep->ExceptionRecord->ExceptionInformation[1])) {
        message = "panic: null pointer dereference\n";
    }
    write_fault_message(message);
    write_fault_backtrace(ep->ContextRecord);
    _exit(1);
    return EXCEPTION_CONTINUE_SEARCH; // unreachable
}
#else
static void fault_signal_handler(int signal_num, siginfo_t *info, void *ucontext) {
    const char *message = "panic: segmentation fault\n";
    if (info && is_probable_null_fault((uintptr_t)info->si_addr)) {
        message = "panic: null pointer dereference\n";
    } else if (signal_num == SIGBUS) {
        message = "panic: bus error\n";
    }
    write_fault_message(message);
    write_fault_backtrace(ucontext);
    _exit(1);
}
#endif

static const char *get_executable_path() {
    auto debug_file = getenv("DEBUG_FILE");
    if (debug_file && debug_file[0]) return debug_file;
#ifdef _WIN32
    static char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) return path;
#elif defined(__APPLE__)
    static char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) return path;
#elif defined(__linux__)
    static char path[4096];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0) { path[len] = '\0'; return path; }
#endif
    return nullptr;
}

void cx_runtime_start(void *stack) {
    init_backtrace(get_executable_path());
    tgc_start(&gc, stack);
    init_main_uv_dispatcher();

    auto trace_mode = getenv("CHI_BACKTRACE");
    bool enable_trace = true;
    if (trace_mode && string(trace_mode) == "none") {
        enable_trace = false;
    }
    if (enable_trace) {
        signal(SIGABRT, signal_handler);
        std::set_terminate(terminate_handler);

#ifdef _WIN32
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        SetUnhandledExceptionFilter(fault_exception_filter);
#else
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = fault_signal_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
#endif
    }
}

void cx_set_program_args(int32_t argc, char **argv) {
    program_argc = argc;
    program_argv = argv;
}

void cx_runtime_stop() {
    // Run any cross-thread submissions that arrived before the loop starts.
    run_pending_main_uv_tasks();

    uv_loop_t *loop = main_uv_loop ? main_uv_loop : uv_default_loop();
    pump_main_uv_until_threads_done(loop);
    run_pending_main_uv_tasks();
    uv_run(loop, UV_RUN_DEFAULT);
    run_pending_main_uv_tasks();

    if (main_uv_async_ready) {
        main_uv_stopping = true;
        uv_close((uv_handle_t *)&main_uv_async, nullptr);
        uv_run(loop, UV_RUN_DEFAULT);
        main_uv_async_ready = false;
        main_uv_loop = nullptr;
        main_uv_thread_id_ready = false;
    }

    if (main_uv_mutex_ready) {
        uv_mutex_destroy(&main_uv_mutex);
        main_uv_mutex_ready = false;
    }
    if (spawned_thread_mutex_ready) {
        uv_mutex_destroy(&spawned_thread_mutex);
        spawned_thread_mutex_ready = false;
    }

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

void cx_thread_spawn(void *callback_ptr) {
    auto lambda = (CxLambda *)callback_ptr;
    auto *thread = new SpawnedThread();
    uv_mutex_init(&thread->mutex);
    thread->fn_ptr = (void (*)(void *))lambda->fn_ptr;
    thread->captures = lambda->captures;
    if (thread->captures) {
        cx_capture_retain(thread->captures);
    }

    auto r = uv_thread_create(&thread->thread, spawned_thread_entry, thread);
    if (r < 0) {
        if (thread->captures) {
            cx_capture_release(thread->captures);
        }
        uv_mutex_destroy(&thread->mutex);
        delete thread;
        panic("uv_thread_create failed: {}", uv_strerror(r));
    }

    uv_mutex_lock(&spawned_thread_mutex);
    thread->next = spawned_thread_head;
    spawned_thread_head = thread;
    uv_mutex_unlock(&spawned_thread_mutex);
}

bool cx_is_main_thread() {
    return is_main_uv_thread();
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

    run_on_main_uv_thread_async([delay, td]() {
        auto timer = new uv_timer_t();
        timer->data = td;

        auto init_r = uv_timer_init(main_uv_loop, timer);
        if (init_r < 0) {
            if (td->captures)
                cx_capture_release(td->captures);
            delete td;
            delete timer;
            panic("uv_timer_init failed: {}", uv_strerror(init_r));
        }

        auto start_r = uv_timer_start(timer, timeout_cb, delay, 0);
        if (start_r < 0) {
            if (td->captures)
                cx_capture_release(td->captures);
            delete td;
            uv_close((uv_handle_t *)timer, [](uv_handle_t *h) { delete (uv_timer_t *)h; });
            panic("uv_timer_start failed: {}", uv_strerror(start_r));
        }
    });
}

static inline uint32_t meiyan_hash(const char *key, int count) {
    typedef uint32_t *P;
    uint32_t h = 0x811c9dc5;
    while (count >= 8) {
        h = (h ^ ((((*(P)key) << 5) | ((*(P)key) >> 27)) ^ *(P)(key + 4))) * 0xad3e7;
        count -= 8;
        key += 8;
    }
#define meiyan_tmp h = (h ^ *(uint16_t *)key) * 0xad3e7; key += 2;
    if (count & 4) { meiyan_tmp meiyan_tmp }
    if (count & 2) { meiyan_tmp }
    if (count & 1) { h = (h ^ *key) * 0xad3e7; }
#undef meiyan_tmp
    return h ^ (h >> 16);
}

uint64_t cx_meiyan(const void *key, int count) {
    return (uint64_t)meiyan_hash((const char *)key, count);
}

bool cx_string_eq(CxString *a, CxString *b) {
    if (a->size != b->size) return false;
    return memcmp(a->data, b->data, a->size) == 0;
}

// DEPRECATED: Promise is now a Chi-native struct in runtime.xs
// void cx_promise_init(CxPromise *promise) { ... }
// void cx_promise_resolve(CxPromise *promise, void *value) { ... }
// void cx_promise_reject(CxPromise *promise, void *error) { ... }
// void cx_promise_then(CxPromise *promise, CxLambda *on_resolve, CxLambda *on_reject) { ... }

static void create_cx_json_result(boost::json::value *data, void *result) {
    auto result_p = (CxJsonValue *)result;
    result_p->data = new boost::json::value(*data);
    result_p->kind = (uint32_t)data->kind();
}

static void fill_cx_json_parse_error(CxString *input, size_t offset, const string &detail,
                                     CxJsonParseError *error) {
    if (!error) {
        return;
    }

    error->has_location = true;
    error->offset = (uint32_t)offset;
    error->line = 1;
    error->column = 1;

    auto limit = std::min(offset, (size_t)input->size);
    for (size_t i = 0; i < limit; i++) {
        if (input->data[i] == '\n') {
            error->line++;
            error->column = 1;
        } else {
            error->column++;
        }
    }

    cx_string_from_chars(detail.data(), (uint32_t)detail.size(), &error->detail);
}

bool cx_parse_json(CxString *str, bool allow_jsonc, void *result, CxJsonParseError *error) {
    string s(str->data, str->size);
    std::error_code ec;
    boost::json::parse_options opts;
    opts.allow_comments = allow_jsonc;
    opts.allow_trailing_commas = allow_jsonc;

    boost::json::stream_parser parser({}, opts);
    auto consumed = parser.write(s.data(), s.size(), ec);
    if (ec) {
        fill_cx_json_parse_error(str, consumed, ec.message(), error);
        return false;
    }

    if (!parser.done()) {
        parser.finish(ec);
        if (ec) {
            fill_cx_json_parse_error(str, s.size(), ec.message(), error);
            return false;
        }
    }

    auto value = parser.release();
    create_cx_json_result(&value, result);
    return true;
}

void cx_json_value_delete(void *data) { delete (boost::json::value *)data; }
void cx_json_value_get(void *data, CxString *key, void *result) {
    auto value = (boost::json::value *)data;
    string k(key->data, key->size);
    auto &obj = value->as_object();
    auto it = obj.find(k);
    if (it == obj.end()) {
        // Return null JSON value
        auto result_p = (CxJsonValue *)result;
        auto null_val = new boost::json::value(nullptr);
        result_p->data = null_val;
        result_p->kind = (uint32_t)null_val->kind();
        return;
    }
    auto &found = it->value();
    create_cx_json_result(&found, result);
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

void cx_json_value_stringify(void *data, CxString *result) {
    auto value = (boost::json::value *)data;
    auto text = boost::json::serialize(*value);
    cx_string_from_chars(text.data(), (uint32_t)text.size(), result);
}

// SharedData refcounting helpers for type-erased pointers
// All SharedData<T> instances have ref_count as the first field at offset 0

// std/math wrappers
extern "C" {
double __cx_sqrt(double x) { return sqrt(x); }
double __cx_pow(double base, double exp) { return pow(base, exp); }
double __cx_log(double x) { return log(x); }
double __cx_log2(double x) { return log2(x); }
double __cx_log10(double x) { return log10(x); }
double __cx_exp(double x) { return exp(x); }
double __cx_sin(double x) { return sin(x); }
double __cx_cos(double x) { return cos(x); }
double __cx_tan(double x) { return tan(x); }
double __cx_atan2(double y, double x) { return atan2(y, x); }
double __cx_floor(double x) { return floor(x); }
double __cx_ceil(double x) { return ceil(x); }
double __cx_round(double x) { return round(x); }
double __cx_fabs(double x) { return fabs(x); }
double __cx_fmod(double x, double y) { return fmod(x, y); }

// Random number generation (xoshiro256**)
// Thread-local state
static thread_local uint64_t xoshiro_state[4];
static thread_local bool xoshiro_initialized = false;

// SplitMix64 for seeding
static uint64_t splitmix64(uint64_t &x) {
    x += 0x9e3779b97f4a7c15;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

static void xoshiro_init(uint64_t seed) {
    uint64_t sm = seed;
    xoshiro_state[0] = splitmix64(sm);
    xoshiro_state[1] = splitmix64(sm);
    xoshiro_state[2] = splitmix64(sm);
    xoshiro_state[3] = splitmix64(sm);
    xoshiro_initialized = true;
}

static inline uint64_t rotl(const uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static uint64_t xoshiro_next(void) {
    const uint64_t result = rotl(xoshiro_state[1] * 5, 7) * 9;
    const uint64_t t = xoshiro_state[1] << 17;
    xoshiro_state[2] ^= xoshiro_state[0];
    xoshiro_state[3] ^= xoshiro_state[1];
    xoshiro_state[1] ^= xoshiro_state[2];
    xoshiro_state[0] ^= xoshiro_state[3];
    xoshiro_state[2] ^= t;
    xoshiro_state[3] = rotl(xoshiro_state[3], 45);
    return result;
}

// Get high-quality seed from OS
static uint64_t get_seed(void) {
#if defined(__APPLE__)
    uint64_t seed;
    arc4random_buf(&seed, sizeof(seed));
    return seed;
#elif defined(__linux__)
    uint64_t seed;
    if (getentropy(&seed, sizeof(seed)) == 0) {
        return seed;
    }
    // Fallback: mix time and pid
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec) ^ (getpid() * 0x9e3779b97f4a7c15ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec) ^ (getpid() * 0x9e3779b97f4a7c15ULL);
#endif
}

double __cx_random(void) {
    if (!xoshiro_initialized) {
        xoshiro_init(get_seed());
    }
    // Convert to [0.0, 1.0)
    return (xoshiro_next() >> 11) * (1.0 / (1ULL << 53));
}

void __cx_random_seed(uint64_t seed) {
    xoshiro_init(seed);
}

// std/conv helpers
// Returns 1 on success (result written to *out), 0 on failure
int32_t __cx_parse_int(const char *str, int64_t *out) {
    if (!str || !*str) return 0;
    char *end;
    errno = 0;
    long long val = strtoll(str, &end, 10);
    if (errno != 0 || *end != '\0' || end == str) return 0;
    *out = (int64_t)val;
    return 1;
}

uint32_t __cx_strlen(const char *s) {
    if (!s) return 0;
    return (uint32_t)strlen(s);
}

// std/os helpers
const char *__cx_default_chi_home() {
#ifdef _WIN32
    auto local = getenv("LOCALAPPDATA");
    if (local) {
        static std::string path = (fs::path(local) / "chi").string();
        return path.c_str();
    }
#else
    auto home = getenv("HOME");
    if (home) {
        static std::string path = (fs::path(home) / ".chi").string();
        return path.c_str();
    }
#endif
    return nullptr;
}

const char *__cx_getenv(const char *key) {
    return getenv(key);
}

void __cx_setenv(const char *key, const char *value) {
    setenv(key, value, 1);
}

void __cx_exit(int32_t code) {
    exit(code);
}

char *__cx_getcwd(void) {
    static char buf[4096];
    if (getcwd(buf, sizeof(buf))) return buf;
    return (char *)"";
}

void __cx_system(const char *command, CxCommandResult *result) {
    run_child_command(
        [command]() {
            execl("/bin/sh", "sh", "-c", command, (char *)nullptr);
            perror("sh");
        },
        result
    );
}

void __cx_command(void *args_ptr, CxCommandResult *result) {
    init_command_result(result);
    auto *args = (CxSlice *)args_ptr;
    if (!args || args->size == 0) {
        set_command_result_error(result, -1, "empty command");
        return;
    }

    auto *items = (CxString *)args->data;
    std::vector<char *> argv;
    argv.reserve(args->size + 1);
    for (uint32_t i = 0; i < args->size; i++) {
        argv.push_back(cx_string_to_cstring(&items[i]));
    }
    argv.push_back(nullptr);

    run_child_command(
        [&argv]() {
            execvp(argv[0], argv.data());
            perror(argv[0]);
        },
        result
    );

    for (auto *arg : argv) {
        if (arg) {
            free(arg);
        }
    }
}

int32_t __cx_argc(void) { return program_argc; }

const char *__cx_argv(int32_t index) {
    if (index < 0 || index >= program_argc || !program_argv) {
        return nullptr;
    }
    return program_argv[index];
}

int32_t __cx_parse_float(const char *str, double *out) {
    if (!str || !*str) return 0;
    char *end;
    errno = 0;
    double val = strtod(str, &end);
    if (errno != 0 || *end != '\0' || end == str) return 0;
    *out = val;
    return 1;
}

// std/time helpers
uint64_t __cx_time_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

uint64_t __cx_time_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// std/fs helpers — minimal C primitives
// Maps a libuv error code to a FileError kind value.
// The enum order must match the Chi-side ErrorKind enum in std/fs.
int32_t __cx_fs_error_kind(int32_t uv_err) {
    switch (uv_err) {
    case UV_ENOENT:    return 1; // NotFound
    case UV_EACCES:    return 2; // PermissionDenied
    case UV_EEXIST:    return 3; // AlreadyExists
    case UV_ENOTDIR:   return 4; // NotADirectory
    case UV_EISDIR:    return 5; // IsADirectory
    case UV_ENOTEMPTY: return 6; // DirectoryNotEmpty
    case UV_ENOSPC:    return 7; // NoSpace
    case UV_EROFS:     return 8; // ReadOnlyFs
    case UV_EBUSY:     return 9; // Busy
    case UV_EPERM:     return 2; // PermissionDenied (same as EACCES)
    default:           return 0; // Unknown
    }
}

int32_t __cx_fs_flags(int32_t which) {
    switch (which) {
    case 0: return O_RDONLY;
    case 1: return O_WRONLY;
    case 2: return O_RDWR;
    case 3: return O_CREAT;
    case 4: return O_TRUNC;
    case 5: return O_APPEND;
    default: return 0;
    }
}

int32_t __cx_path_separator(void) {
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

int32_t __cx_fs_open(const char *path, int32_t flags, int32_t mode) {
    return run_on_main_uv_thread_sync([=]() -> int32_t {
        uv_fs_t req;
        int32_t fd = uv_fs_open(main_uv_loop, &req, path, flags, mode, NULL);
        uv_fs_req_cleanup(&req);
        return fd;
    });
}

int32_t __cx_fs_read(int32_t fd, void *buf, uint32_t size) {
    return run_on_main_uv_thread_sync([=]() -> int32_t {
        uv_fs_t req;
        uv_buf_t uvbuf = uv_buf_init((char *)buf, size);
        int32_t n = uv_fs_read(main_uv_loop, &req, fd, &uvbuf, 1, -1, NULL);
        uv_fs_req_cleanup(&req);
        return n;
    });
}

struct FsReadAsyncData {
    uv_fs_t req;
    uv_buf_t buf;
    void (*resolve_fn)(void *, uint32_t);
    void *resolve_captures;
    void (*reject_fn)(void *, int32_t);
    void *reject_captures;
};

static void fs_read_async_cleanup(FsReadAsyncData *data) {
    if (data->resolve_captures)
        cx_capture_release(data->resolve_captures);
    if (data->reject_captures)
        cx_capture_release(data->reject_captures);
    uv_fs_req_cleanup(&data->req);
    delete data;
}

static void fs_read_async_cb(uv_fs_t *req) {
    auto data = (FsReadAsyncData *)req->data;
    auto result = req->result;
    if (result >= 0) {
        void *payload = data->resolve_captures ? cx_capture_get_data(data->resolve_captures) : nullptr;
        data->resolve_fn(payload, (uint32_t)result);
    } else {
        void *payload = data->reject_captures ? cx_capture_get_data(data->reject_captures) : nullptr;
        data->reject_fn(payload, (int32_t)result);
    }
    fs_read_async_cleanup(data);
}

void __cx_fs_read_async(int32_t fd, void *buf, uint32_t size, void *resolve_lambda_ptr,
                        void *reject_lambda_ptr) {
    auto resolve_lambda = (CxLambda *)resolve_lambda_ptr;
    auto reject_lambda = (CxLambda *)reject_lambda_ptr;

    auto data = new FsReadAsyncData();
    data->buf = uv_buf_init((char *)buf, size);
    data->resolve_fn = (void (*)(void *, uint32_t))resolve_lambda->fn_ptr;
    data->resolve_captures = resolve_lambda->captures;
    data->reject_fn = (void (*)(void *, int32_t))reject_lambda->fn_ptr;
    data->reject_captures = reject_lambda->captures;
    if (data->resolve_captures)
        cx_capture_retain(data->resolve_captures);
    if (data->reject_captures)
        cx_capture_retain(data->reject_captures);
    data->req.data = data;

    run_on_main_uv_thread_async([data, fd]() {
        auto r = uv_fs_read(main_uv_loop, &data->req, fd, &data->buf, 1, -1, fs_read_async_cb);
        if (r < 0) {
            void *payload =
                data->reject_captures ? cx_capture_get_data(data->reject_captures) : nullptr;
            data->reject_fn(payload, r);
            fs_read_async_cleanup(data);
        }
    });
}

int32_t __cx_fs_write(int32_t fd, const void *data, uint32_t size) {
    return run_on_main_uv_thread_sync([=]() -> int32_t {
        uv_fs_t req;
        uv_buf_t uvbuf = uv_buf_init((char *)data, size);
        int32_t n = uv_fs_write(main_uv_loop, &req, fd, &uvbuf, 1, -1, NULL);
        uv_fs_req_cleanup(&req);
        return n;
    });
}

struct FsWriteAsyncData {
    uv_fs_t req;
    uv_buf_t buf;
    char *data;
    void (*resolve_fn)(void *);
    void *resolve_captures;
    void (*reject_fn)(void *, int32_t);
    void *reject_captures;
};

static void fs_write_async_cleanup(FsWriteAsyncData *data) {
    if (data->resolve_captures)
        cx_capture_release(data->resolve_captures);
    if (data->reject_captures)
        cx_capture_release(data->reject_captures);
    free(data->data);
    uv_fs_req_cleanup(&data->req);
    delete data;
}

static void fs_write_async_cb(uv_fs_t *req) {
    auto data = (FsWriteAsyncData *)req->data;
    auto result = req->result;
    if (result >= 0) {
        void *payload = data->resolve_captures ? cx_capture_get_data(data->resolve_captures) : nullptr;
        data->resolve_fn(payload);
    } else {
        void *payload = data->reject_captures ? cx_capture_get_data(data->reject_captures) : nullptr;
        data->reject_fn(payload, (int32_t)result);
    }
    fs_write_async_cleanup(data);
}

void __cx_fs_write_async(int32_t fd, const void *src_data, uint32_t size, void *resolve_lambda_ptr,
                         void *reject_lambda_ptr) {
    auto resolve_lambda = (CxLambda *)resolve_lambda_ptr;
    auto reject_lambda = (CxLambda *)reject_lambda_ptr;

    auto data = new FsWriteAsyncData();
    data->data = (char *)malloc(size);
    if (size > 0) {
        memcpy(data->data, src_data, size);
    }
    data->buf = uv_buf_init(data->data, size);
    data->resolve_fn = (void (*)(void *))resolve_lambda->fn_ptr;
    data->resolve_captures = resolve_lambda->captures;
    data->reject_fn = (void (*)(void *, int32_t))reject_lambda->fn_ptr;
    data->reject_captures = reject_lambda->captures;
    if (data->resolve_captures)
        cx_capture_retain(data->resolve_captures);
    if (data->reject_captures)
        cx_capture_retain(data->reject_captures);
    data->req.data = data;

    run_on_main_uv_thread_async([data, fd]() {
        auto r = uv_fs_write(main_uv_loop, &data->req, fd, &data->buf, 1, -1, fs_write_async_cb);
        if (r < 0) {
            void *payload =
                data->reject_captures ? cx_capture_get_data(data->reject_captures) : nullptr;
            data->reject_fn(payload, r);
            fs_write_async_cleanup(data);
        }
    });
}

int32_t __cx_fs_close(int32_t fd) {
    return run_on_main_uv_thread_sync([=]() -> int32_t {
        uv_fs_t req;
        int32_t r = uv_fs_close(main_uv_loop, &req, fd, NULL);
        uv_fs_req_cleanup(&req);
        return r;
    });
}

int32_t __cx_file_exists(const char *path) {
    return run_on_main_uv_thread_sync([=]() -> int32_t {
        uv_fs_t req;
        int32_t r = uv_fs_stat(main_uv_loop, &req, path, NULL);
        uv_fs_req_cleanup(&req);
        return r == 0 ? 1 : 0;
    });
}

int32_t __cx_is_dir(const char *path) {
    return run_on_main_uv_thread_sync([=]() -> int32_t {
        uv_fs_t req;
        int32_t r = uv_fs_stat(main_uv_loop, &req, path, NULL);
        if (r < 0) {
            uv_fs_req_cleanup(&req);
            return 0;
        }
        auto is_dir = S_ISDIR(req.statbuf.st_mode) ? 1 : 0;
        uv_fs_req_cleanup(&req);
        return is_dir;
    });
}

int32_t __cx_file_remove(const char *path) {
    return run_on_main_uv_thread_sync([=]() -> int32_t {
        uv_fs_t req;
        int32_t r = uv_fs_unlink(main_uv_loop, &req, path, NULL);
        uv_fs_req_cleanup(&req);
        return r;
    });
}

int32_t __cx_dir_remove(const char *path) {
    return run_on_main_uv_thread_sync([=]() -> int32_t {
        uv_fs_t req;
        int32_t r = uv_fs_rmdir(main_uv_loop, &req, path, NULL);
        uv_fs_req_cleanup(&req);
        return r;
    });
}

int32_t __cx_copy_file(const char *src, const char *dest) {
    return run_on_main_uv_thread_sync([=]() -> int32_t {
        uv_fs_t req;
        int32_t r = uv_fs_copyfile(main_uv_loop, &req, src, dest, 0, NULL);
        uv_fs_req_cleanup(&req);
        return r;
    });
}

int32_t __cx_mkdir(const char *path) {
    return run_on_main_uv_thread_sync([=]() -> int32_t {
        uv_fs_t req;
        int32_t r = uv_fs_mkdir(main_uv_loop, &req, path, 0755, NULL);
        uv_fs_req_cleanup(&req);
        return r;
    });
}

int32_t __cx_get_errno() {
    return errno;
}

void __cx_uv_strerror(int32_t errnum, CxString *result) {
    run_on_main_uv_thread_sync([=]() {
        auto msg = uv_strerror(errnum);
        cx_string_from_chars(msg, strlen(msg), result);
    });
}

int32_t __cx_list_dir(const char *path, CxArray *result) {
    return run_on_main_uv_thread_sync([=]() -> int32_t {
        uv_fs_t req;
        int32_t n = uv_fs_scandir(main_uv_loop, &req, path, 0, NULL);
        if (n < 0) {
            uv_fs_req_cleanup(&req);
            return n;
        }
        uv_dirent_t entry;
        while (uv_fs_scandir_next(&req, &entry) != UV_EOF) {
            CxString s;
            cx_string_from_chars(entry.name, strlen(entry.name), &s);
            CxString *slot = (CxString *)cx_array_add(result, sizeof(CxString));
            *slot = s;
        }
        uv_fs_req_cleanup(&req);
        return 0;
    });
}

int32_t __cx_glob(const char *base, const char *pattern, CxArray *result) {
    return run_on_main_uv_thread_sync([=]() -> int32_t {
        try {
            fs::path base_path(base);
            if (!fs::exists(base_path)) {
                return UV_ENOENT;
            }
            if (!fs::is_directory(base_path)) {
                return UV_ENOTDIR;
            }

            auto matched = glob_files(base_path, pattern);
            for (auto &match : matched) {
                auto relative = fs::relative(fs::path(match), base_path);
                auto relative_string = relative.string();
                CxString s;
                cx_string_from_chars(relative_string.c_str(), relative_string.size(), &s);
                CxString *slot = (CxString *)cx_array_add(result, sizeof(CxString));
                *slot = s;
            }
            return 0;
        } catch (const fs::filesystem_error &err) {
            return uv_translate_sys_error(err.code().value());
        }
    });
}

void __cx_platform_tags(CxArray *result) {
    auto tags = get_active_platform_tags();
    for (auto &tag : tags) {
        CxString s;
        cx_string_from_chars(tag.c_str(), (uint32_t)tag.size(), &s);
        CxString *slot = (CxString *)cx_array_add(result, sizeof(CxString));
        *slot = s;
    }
}
}
