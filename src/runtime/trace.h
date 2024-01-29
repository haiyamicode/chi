#pragma once
#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

bool init_backtrace(const char *filename);
void backtrace();
void set_bt_output_file(FILE *file);

#ifdef __cplusplus
}
#endif