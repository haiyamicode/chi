#pragma once

#ifdef __cplusplus
extern "C" {
#endif

bool init_backtrace(const char *filename);
void backtrace();

#ifdef __cplusplus
}
#endif