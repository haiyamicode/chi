#pragma once
#include <cstdint>
#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*cx_backtrace_pcinfo_callback_t)(void *data, uintptr_t pc, const char *filename,
                                              int lineno, const char *function);

bool init_backtrace(const char *filename);
void backtrace();
void set_bt_output_file(FILE *file);
bool cx_backtrace_pcinfo(uintptr_t pc, cx_backtrace_pcinfo_callback_t callback, void *data);

#ifdef __cplusplus
}
#endif
