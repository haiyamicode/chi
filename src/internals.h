//
// Created by haint on 11/25/18.
//

#pragma once

#include "sema.h"

namespace cx {
    namespace internals {
        struct String {
            char* data;
            uint32_t size;
        };

        struct Any {
            ChiType* type;
            struct {
                void* a;
                void* b;
            } data;
        };

        template<typename T>
        struct Array {
            T* data;
            uint32_t size;
            uint32_t capacity;
        };

        typedef Array<void> GenericArray;

        void string_set_data(String* dest, const char* data);

        void string_concat(String* dest, String s1, String s2);

        void string_format(String* dest, String format, Array<Any> values);

        void printf(String format, Array<Any> values);

        void array_construct(GenericArray* dest);

        void array_reserve(GenericArray* dest, uint32_t elem_size, uint32_t new_cap);

        void* array_add(GenericArray* dest, uint32_t elem_size);
    }
}