/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include "fmt/format.h"

namespace cx {
    template<typename... Args>
    static inline void panic(const char* format, const Args& ...args) {
        fmt::print(format, args...);
        fmt::print("\n");
        abort();
    }

    template<typename T>
    static inline T* reallocate_nonzero(T* old, size_t old_count, size_t new_count) {
        T* ptr = reinterpret_cast<T*>(realloc(old, new_count * sizeof(T)));
        if (!ptr)
            panic("allocation failed");
        return ptr;
    }

    static inline void unreachable() {
        panic("unreachable");
    }
}