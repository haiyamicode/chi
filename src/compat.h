/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <vector>
#include <fmt/format.h>

#include "include/optional.h"
#include "include/sso23.h"
#include "include/enum.h"

namespace cx {

    typedef int64_t int64;

    template<typename T>
    class array {
        std::vector<T> vec;
    public:
        T* push(T&& value) {
            vec.push_back(value);
            return &vec.back();
        }

        T& operator[](long i) {
            return vec.operator[](i);
        }
    };

    typedef sso23::basic_string<char> basic_string;

    class string : public basic_string {
    public:
        using basic_string::basic_string;

        bool is_empty() {
            return size() == 0;
        }

        char operator[](long i) {
            return data()[0];
        }
    };

    using nonstd::optional;
    using fmt::print;
    using fmt::format;

}
