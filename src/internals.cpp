//
// Created by haint on 11/25/18.
//

#include <sstream>

#include "internals.h"

using namespace cx;
using namespace cx::internals;

void cx::internals::string_set_data(String* dest, const char* data) {
    if (data) {
        dest->size = (uint32_t) strlen(data);
        dest->data = (char*) realloc(dest->data, dest->size + 1);
        memcpy(dest->data, data, dest->size + 1);
    } else {
        dest->size = 0;
        dest->data = NULL;
    }
}

void cx::internals::string_concat(String* dest, String s1, String s2) {
    dest->size = (uint32_t) (s1.size + s2.size);
    dest->data = (char*) malloc(dest->size + 1);
    memcpy(dest->data, s1.data, s1.size + 1);
    strncat(dest->data, s2.data, s2.size);
}

static std::string to_string(const Any& v) {
    switch (v.type->id) {
        case TypeId::String: {
            auto s = (String*) &v.data;
            return fmt::format(s->data);
        }
        case TypeId::Int:
        case TypeId::Bool:
        case TypeId::Pointer:
            return fmt::format("{}", *(int64_t*) &v.data);

        default:
            return fmt::format("<{}>", PRINT_ENUM(v.type->id));
    }
}

static string format_cstr(String format, const Array<Any>& values) {
    int val_i = 0;
    int state = 0;
    std::stringstream ss;
    for (int i = 0; i < format.size; i++) {
        auto c = format.data[i];
        switch (c) {
            case '{':
                if (state == 1) {
                    ss.write("{", 1);
                    state = 0;
                } else {
                    state = 1;
                }
                break;
            case '}':
                if (state == 1) {
                    if (val_i < values.size) {
                        ss << to_string(values.data[val_i++]);
                    }
                    state = 0;
                } else if (state == 2) {
                    ss.write(&c, 1);
                    state = 0;
                } else {
                    state = 2;
                }
                break;
            default:
                state = 0;
                ss.write(&c, 1);
                break;
        }
    }
    return ss.str();
}

void cx::internals::string_format(String* dest, String format, Array<Any> values) {
    auto str = format_cstr(format, values);
    dest->data = str.data();
    dest->size = (uint32_t) str.size();
}

void cx::internals::printf(String format, Array<Any> values) {
    ::printf("%s", format_cstr(format, values).c_str());
}

void cx::internals::array_construct(GenericArray* dest) {
    dest->size = 0;
    dest->capacity = 0;
    dest->data = NULL;
}

void cx::internals::array_reserve(GenericArray* dest, uint32_t elem_size, uint32_t new_cap) {
    if (dest->capacity >= new_cap)
        return;

    size_t better_cap = dest->capacity;
    do {
        better_cap = better_cap * 5 / 2 + 8;
    } while (better_cap < new_cap);

    dest->data = realloc(dest->data, better_cap * elem_size);
    dest->capacity = (uint32_t) better_cap;
}

void* cx::internals::array_add(GenericArray* dest, uint32_t elem_size) {
    array_reserve(dest, elem_size, ++dest->size);
    return ((char*) dest->data) + (dest->size - 1) * elem_size;
}
