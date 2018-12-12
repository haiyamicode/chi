//
// Created by haint on 9/17/18.
//

#pragma once

#include <cstdio>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#define MAKE_ENUM(name, ...)                                                   \
    enum class name: int32_t { __VA_ARGS__, __COUNT };                         \
    inline std::ostream &operator<<(std::ostream &os, name value) {            \
        std::string enum_name = #name;                                          \
        std::string str = #__VA_ARGS__;                                        \
        int len = str.length();                                                \
        std::vector<std::string> strings;                                      \
        std::ostringstream temp;                                               \
        for (int i = 0; i < len; i++) {                                        \
            if (isspace(str[i]))                                               \
                continue;                                                      \
            else if (str[i] == ',') {                                          \
                strings.push_back(temp.str());                                 \
                temp.str(std::string());                                       \
            } else                                                             \
                temp << str[i];                                                \
        }                                                                      \
        strings.push_back(temp.str());                                         \
        os << strings[static_cast<int>(value)];                                \
        return os;                                                             \
    }

template<typename E>
std::string PRINT_ENUM(E val) {
    std::stringstream ss;
    ss << val;
    return ss.str();
}