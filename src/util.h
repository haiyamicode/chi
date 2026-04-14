/*
 * Copyright (c) 2018 Hai Thanh Nguyen
 *
 * This file is part of chi, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#pragma once

#include <algorithm>
#include <cassert>
#include <climits>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/enum.h"
#include "include/optional.h"
#include "include/variant.h"

#ifdef CHI_RUNTIME_HAS_BACKTRACE
#include "runtime/trace.h"
#else
static inline void backtrace() {}
#endif // CHI_RUNTIME_HAS_BACKTRACE

namespace fs = std::filesystem;

namespace cx {
using fmt::print;
template <typename T> using func = std::function<T>;
template <typename T> using box = std::unique_ptr<T>;
using std::string;
using stx::optional;
using namespace mpark;
using std::stringstream;

#define VARIANT_TRY(value, type, output)                                                           \
    const auto output(get_if<type>(&value));                                                       \
    output

template <typename... Args> static inline void panic(const char *format, const Args &...args) {
    fmt::print(format, args...);
    fmt::print("\n");
    backtrace();
    abort();
}

template <typename T>
static inline T *reallocate_nonzero(T *old, size_t old_count, size_t new_count) {
    T *ptr = reinterpret_cast<T *>(realloc(old, new_count * sizeof(T)));
    if (!ptr)
        panic("allocation failed");
    return ptr;
}

static inline void unreachable() { panic("unreachable"); }

static inline std::string string_replace(std::string subject, const std::string &search,
                                         const std::string &replace) {
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != std::string::npos) {
        subject.replace(pos, search.length(), replace);
        pos += replace.length();
    }
    return subject;
}

static inline std::string format_span_prefix(bool is_mut, const std::string &lifetime_name = "") {
    if (!lifetime_name.empty() && is_mut) {
        return fmt::format("&(mut, '{}) ", lifetime_name);
    }
    if (!lifetime_name.empty()) {
        return fmt::format("&'{} ", lifetime_name);
    }
    if (is_mut) {
        return "&mut ";
    }
    return "&";
}

template <typename T> class array {
    std::vector<T> data_;

  public:
    array() = default;
    array(const array<T> &) = default;
    array(array<T> &&) noexcept = default;
    array<T> &operator=(const array<T> &) = default;
    array<T> &operator=(array<T> &&) noexcept = default;
    array(std::initializer_list<T> values) : data_(values) {}

    // std::vector-compatible interface
    size_t size() const { return data_.size(); }
    bool empty() const { return data_.empty(); }
    size_t capacity() const { return data_.capacity(); }
    T *data() { return data_.data(); }
    const T *data() const { return data_.data(); }

    T &operator[](size_t index) { return at(index); }
    const T &operator[](size_t index) const { return at(index); }

    const T &at(size_t index) const {
        assert(index != SIZE_MAX);
        assert(index < data_.size());
        return data_[index];
    }

    T &at(size_t index) {
        assert(index != SIZE_MAX);
        assert(index < data_.size());
        return data_[index];
    }

    T *begin() { return data_.data(); }
    T *end() { return data_.data() + data_.size(); }
    const T *begin() const { return data_.data(); }
    const T *end() const { return data_.data() + data_.size(); }

    T &back() {
        assert(!data_.empty());
        return data_.back();
    }
    const T &back() const {
        assert(!data_.empty());
        return data_.back();
    }
    T &front() {
        assert(!data_.empty());
        return data_.front();
    }
    const T &front() const {
        assert(!data_.empty());
        return data_.front();
    }

    void push_back(const T &item) { data_.push_back(item); }
    void push_back(T &&item) { data_.push_back(std::move(item)); }

    template <typename... Args> T &emplace_back(Args &&...args) {
        return data_.emplace_back(std::forward<Args>(args)...);
    }

    void pop_back() {
        assert(!data_.empty());
        data_.pop_back();
    }

    void clear() { data_.clear(); }

    void reserve(size_t new_capacity) { data_.reserve(new_capacity); }

    // Shrink-only resize: truncate to new_length.
    void resize(size_t new_length) {
        assert(new_length != SIZE_MAX);
        assert(new_length <= data_.size());
        data_.erase(data_.begin() + new_length, data_.end());
    }

    // chi-specific conveniences
    template <typename... Args> T *emplace(Args &&...args) {
        data_.emplace_back(std::forward<Args>(args)...);
        return &data_.back();
    }

    T *add(T &&item) {
        data_.push_back(std::move(item));
        return &data_.back();
    }

    T *add(const T &item) {
        data_.push_back(item);
        return &data_.back();
    }

    void add_all(array<T> other) {
        data_.reserve(data_.size() + other.data_.size());
        for (auto &item : other.data_) {
            data_.push_back(item);
        }
    }

    // pop() returns the popped value (unlike std::vector::pop_back which returns void).
    T pop() {
        assert(!data_.empty());
        T value = std::move(data_.back());
        data_.pop_back();
        return value;
    }

    const T &last() const {
        assert(!data_.empty());
        return data_.back();
    }

    T &last() {
        assert(!data_.empty());
        return data_.back();
    }

    array<T> slice(size_t start, int32_t n = -1) const {
        auto length = n >= 0 ? static_cast<size_t>(n) : data_.size() - start;

        if (start >= data_.size() || length == 0) {
            return array<T>();
        }

        size_t end = start + length;
        if (end > data_.size()) {
            end = data_.size();
        }

        array<T> result;
        size_t actual_length = end - start;
        result.data_.reserve(actual_length);
        for (size_t i = start; i < end; i++) {
            result.data_.push_back(data_[i]);
        }
        return result;
    }
};

template <typename K, typename V> struct map {
    typedef std::unordered_map<K, V> Map;
    Map data = {};

    template <typename... Args> V *emplace(const K &key, Args &&...args) {
        return &data.emplace(std::piecewise_construct, std::forward_as_tuple(key),
                             std::forward_as_tuple(std::forward<Args>(args)...))
                    .first->second;
    }

    V &operator[](const K &key) { return data.operator[](key); }

    V &at(const K &key) { return data.at(key); }

    bool is_empty() { return data.empty(); }

    bool has_key(const K &key) { return data.find(key) != data.end(); }

    bool unset(const K &key) { return data.erase(key); }

    V *get(const K &key) {
        auto iter = data.find(key);
        if (iter != data.end()) {
            return &iter->second;
        } else {
            return nullptr;
        }
    }

    const V *get(const K &key) const {
        auto iter = data.find(key);
        if (iter != data.end()) {
            return &iter->second;
        } else {
            return nullptr;
        }
    }

    void clear() { data.clear(); }
    size_t size() { return data.size(); }
    Map get() { return data; }
};

static inline array<string> get_known_platform_tags() {
    return {
        "platform.windows",
        "platform.linux",
        "platform.macos",
        "platform.unix",
        "arch.x64",
        "arch.arm64",
        "arch.x86",
        "arch.arm",
    };
}

static inline array<string> get_active_platform_tags() {
    array<string> tags = {};
#if defined(_WIN32)
    tags.add("platform.windows");
#elif defined(__APPLE__)
    tags.add("platform.macos");
    tags.add("platform.unix");
#elif defined(__linux__)
    tags.add("platform.linux");
    tags.add("platform.unix");
#endif

#if defined(__x86_64__) || defined(_M_X64)
    tags.add("arch.x64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    tags.add("arch.arm64");
#elif defined(__i386__) || defined(_M_IX86)
    tags.add("arch.x86");
#elif defined(__arm__) || defined(_M_ARM)
    tags.add("arch.arm");
#endif

    return tags;
}

static inline array<string> string_split(string str, string sep) {
    char *cstr = const_cast<char *>(str.c_str());
    char *current;
    array<string> arr;
    current = strtok(cstr, sep.c_str());
    while (current != NULL) {
        arr.add(current);
        current = strtok(NULL, sep.c_str());
    }
    return arr;
}

static inline string string_join(array<string> arr, string sep) {
    string str;
    for (size_t i = 0; i < arr.size(); i++) {
        if (i != 0)
            str += sep;
        str += arr[i];
    }
    return str;
}

namespace io {
enum class Error : int { unknown, eof };

static constexpr Error eof = Error::eof;

class Buffer {
    box<std::istream> m_stream;

    Buffer(std::istream *stream) { m_stream.reset(stream); }

  public:
    static Buffer from_file(string file_name) {
        auto stream = new std::fstream(file_name);
        if (stream->fail()) {
            print("unable to open file '{}'", file_name);
            exit(2);
        }
        return {stream};
    }

    static Buffer from_string(string str) { return {new std::stringstream(str)}; }

    void reset() { m_stream->clear(); }

    optional<Error> read(char *ch) {
        m_stream->get(*ch);
        if (m_stream->fail()) {
            if (m_stream->eof()) {
                return eof;
            }
            return Error::unknown;
        }
        return {};
    };

    string read_all() {
        std::stringstream ss;
        ss << m_stream->rdbuf();
        return ss.str();
    }
};

struct CharBuf : public array<char> {
    char *write(const string &s) {
        if (s.empty())
            return 0;
        auto old_size = size();
        for (char ch : s) {
            add(ch);
        }
        add(0);
        return data() + old_size;
    }
};

} // namespace io

// Portable glob pattern matching function
// Supports patterns like "*.c", "file*.cpp", "test?.h", etc.
static inline bool match_glob_pattern(const string &pattern, const string &text) {
    const char *p = pattern.c_str();
    const char *t = text.c_str();
    const char *p_backup = nullptr;
    const char *t_backup = nullptr;

    while (*t != '\0') {
        if (*p == '*') {
            // Skip consecutive '*' characters
            while (*p == '*')
                p++;
            if (*p == '\0')
                return true; // Pattern ends with '*', match everything

            // Remember positions for backtracking
            p_backup = p;
            t_backup = t;
        } else if (*p == '?' || *p == *t) {
            // '?' matches any character, or characters match exactly
            p++;
            t++;
        } else {
            // No match, try backtracking if we have a '*'
            if (p_backup == nullptr)
                return false;
            p = p_backup;
            t = ++t_backup;
        }
    }

    // Skip any trailing '*' in pattern
    while (*p == '*')
        p++;
    return *p == '\0';
}

static inline string normalize_glob_path(string path) {
    path = string_replace(std::move(path), "\\", "/");
    while (path.rfind("./", 0) == 0) {
        path = path.substr(2);
    }
    if (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

static inline std::vector<string> split_glob_path(const string &path) {
    std::vector<string> parts;
    string current;
    for (char ch : normalize_glob_path(path)) {
        if (ch == '/') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

static inline bool match_glob_path_segments(const std::vector<string> &pattern_parts,
                                            size_t pattern_index,
                                            const std::vector<string> &path_parts,
                                            size_t path_index) {
    while (pattern_index < pattern_parts.size()) {
        if (pattern_parts[pattern_index] == "**") {
            while (pattern_index + 1 < pattern_parts.size() &&
                   pattern_parts[pattern_index + 1] == "**") {
                pattern_index++;
            }
            if (pattern_index + 1 == pattern_parts.size()) {
                return true;
            }
            for (size_t i = path_index; i <= path_parts.size(); i++) {
                if (match_glob_path_segments(pattern_parts, pattern_index + 1, path_parts, i)) {
                    return true;
                }
            }
            return false;
        }

        if (path_index >= path_parts.size() ||
            !match_glob_pattern(pattern_parts[pattern_index], path_parts[path_index])) {
            return false;
        }

        pattern_index++;
        path_index++;
    }

    return path_index == path_parts.size();
}

static inline bool match_glob_path(const string &pattern, const string &path) {
    auto pattern_parts = split_glob_path(pattern);
    auto path_parts = split_glob_path(path);
    return match_glob_path_segments(pattern_parts, 0, path_parts, 0);
}

// Glob pattern matching using std::filesystem and portable pattern matching.
// Supports patterns like "*.c", "dir/*.cpp", "assets/**/*.h", etc.
static inline array<string> glob_files(const fs::path &base_path, const string &pattern) {
    array<string> matched_files;
    std::error_code ec;
    if (!fs::exists(base_path, ec) || ec || !fs::is_directory(base_path, ec) || ec) {
        return matched_files;
    }

    auto normalized_pattern = normalize_glob_path(pattern);
    for (auto it = fs::recursive_directory_iterator(
             base_path, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) {
            continue;
        }

        auto relative = fs::relative(it->path(), base_path, ec);
        if (ec) {
            continue;
        }

        if (match_glob_path(normalized_pattern, relative.string())) {
            matched_files.add(it->path().string());
        }
    }

    std::sort(matched_files.begin(), matched_files.end());
    return matched_files;
}

} // namespace cx
