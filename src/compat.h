/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <unordered_map>
#include <functional>
#include <fstream>
#include <sstream>

#include "include/optional.h"
#include "include/enum.h"
#include "include/tsl/hopscotch_map.h"

#include "util.h"

namespace cx {
    using nonstd::optional;
    using fmt::print;
    template<typename T> using func = std::function<T>;
    template<typename T> using box = std::unique_ptr<T>;
    using std::string;

    // part of this is from zig by Andrew Kelley
    // http://opensource.org/licenses/MIT
    template<typename T>
    struct array {
        size_t size = 0;
        size_t capacity = 0;
        T* items = nullptr;

        ~array() {
            free(items);
        }

        array() {}

        array(const array& other) {
            if (other.size) {
                items = (T*) malloc(other.size * sizeof(T));
                memcpy(items, other.items, other.size * sizeof(T));
            }
        }

        array(std::initializer_list<T> values) {
            reserve(values.size());
            for (auto& value: values) {
                add(value);
            }
        }

        template<typename... Args>
        T* emplace(Args&& ... args) {
            resize(size + 1);
            return new(&last()) T(std::forward<Args>(args)...);
        }

        T* add(T&& item) {
            resize(size + 1);
            last() = item;
            return &last();
        }

        T* add(const T& item) {
            resize(size + 1);
            last() = item;
            return &last();
        }

        T& operator[](size_t index) {
            return at(index);
        }

        const T& operator[](size_t index) const {
            return at(index);
        }

        T* begin() { return items; }

        T* end() { return items + size; }

        const T& at(size_t index) const {
            assert(index != SIZE_MAX);
            assert(index < size);
            return items[index];
        }

        T& at(size_t index) {
            assert(index != SIZE_MAX);
            assert(index < size);
            return items[index];
        }

        T pop() {
            assert(size >= 1);
            return items[--size];
        }

        const T& last() const {
            assert(size >= 1);
            return items[size - 1];
        }

        T& last() {
            assert(size >= 1);
            return items[size - 1];
        }

        void resize(size_t new_length) {
            assert(new_length != SIZE_MAX);
            reserve(new_length);
            size = new_length;
        }

        void clear() {
            size = 0;
        }

        void reserve(size_t new_capacity) {
            if (capacity >= new_capacity)
                return;

            size_t better_capacity = capacity;
            do {
                better_capacity = better_capacity * 5 / 2 + 8;
            } while (better_capacity < new_capacity);

            items = reallocate_nonzero(items, capacity, better_capacity);
            capacity = better_capacity;
        }
    };

    template<typename K, typename V>
    struct map {
        typedef std::unordered_map<K, V> Map;

        template<typename... Args>
        void emplace(const K& key, Args&& ... args) {
            data.emplace(std::piecewise_construct,
                         std::forward_as_tuple(key),
                         std::forward_as_tuple(std::forward<Args>(args)...));
        }

        V& operator[](const K& key) {
            return data.operator[](key);
        }

        bool is_empty() {
            return data.empty();
        }

        V* get(const K& key) {
            auto iter = data.find(key);
            if (iter != data.end()) {
                return &iter->second;
            } else {
                return nullptr;
            }
        }

    private:
        Map data;
    };

    namespace io {
        enum class Error : int {
            unknown,
            eof
        };

        static constexpr Error eof = Error::eof;

        class Buffer {
            box<std::istream> m_stream;

            Buffer(std::istream* stream) {
                m_stream.reset(stream);
            }

        public:
            static Buffer from_file(string file_name) {
                auto stream = new std::fstream(file_name);
                if (stream->fail()) {
                    print("unable to open file {}", file_name);
                }
                return {stream};
            }

            static Buffer from_string(string str) {
                return {new std::stringstream(str)};
            }

            void reset() {
                m_stream->clear();
            }

            optional<Error> read(char* ch) {
                m_stream->get(*ch);
                if (m_stream->fail()) {
                    if (m_stream->eof()) {
                        return eof;
                    }
                    return Error::unknown;
                }
                return {};
            };
        };
    }
}
