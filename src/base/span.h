// KTile - C++17 兼容层：Span 与字符串视图工具
//
// 替代 C++20 的 std::span，以及提供一些字符串处理便利函数。
// VS2017 有 std::string_view，可直接使用。
//
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ktile {
namespace base {

// ---------------------------------------------------------------- Span
// 非拥有视图，替代 std::span。仅需覆盖 HLaunch 用到的只读场景。
template <typename T>
class Span {
public:
    using ElementType = T;
    using ValueType = typename std::remove_cv<T>::type;
    using Iterator = T*;

    Span() noexcept : data_(nullptr), size_(0) {}

    Span(T* data, std::size_t size) noexcept : data_(data), size_(size) {}

    template <typename Alloc>
    Span(std::vector<ValueType, Alloc>& container) noexcept
        : data_(container.data()), size_(container.size()) {}

    template <typename Alloc>
    Span(const std::vector<ValueType, Alloc>& container) noexcept
        : data_(container.data()), size_(container.size()) {}

    // 从 C 数组构造
    template <std::size_t N>
    Span(T (&array)[N]) noexcept : data_(array), size_(N) {}

    T* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    Iterator begin() const noexcept { return data_; }
    Iterator end() const noexcept { return data_ + size_; }

    T& operator[](std::size_t index) const noexcept { return data_[index]; }

    T& front() const noexcept { return data_[0]; }
    T& back() const noexcept { return data_[size_ - 1]; }

    Span subspan(std::size_t offset, std::size_t count) const noexcept {
        if (offset > size_) {
            return Span();
        }
        const std::size_t available = size_ - offset;
        return Span(data_ + offset, count < available ? count : available);
    }

    Span first(std::size_t count) const noexcept {
        return Span(data_, count < size_ ? count : size_);
    }

private:
    T* data_;
    std::size_t size_;
};

// ---------------------------------------------------------------- 字符串工具

// ASCII 空白判定（不处理 Unicode 空白）
inline bool isSpaceChar(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline bool isSpaceCharW(wchar_t c) noexcept {
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f' || c == L'\v';
}

// 去除首尾 ASCII 空白（宽字符）
inline std::wstring trim(const std::wstring& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && isSpaceCharW(value[begin])) {
        ++begin;
    }
    while (end > begin && isSpaceCharW(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

// 去除首尾 ASCII 空白（窄字符）
inline std::string trim(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && isSpaceChar(value[begin])) {
        ++begin;
    }
    while (end > begin && isSpaceChar(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

// 忽略大小写比较（ASCII）
inline bool equalsIgnoreCaseAscii(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) {
            return false;
        }
    }
    return true;
}

inline bool equalsIgnoreCaseAscii(const std::wstring& left, const std::wstring& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        wchar_t a = left[i];
        wchar_t b = right[i];
        if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b) {
            return false;
        }
    }
    return true;
}

// 前缀判定
inline bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

inline bool startsWith(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

// ---------------------------------------------------------------- UTF-8 <-> UTF-16
// 实现放在 base/text_convert.h + text_convert.cpp，这里透传方便使用。

#include "base/text_convert.h"

}  // namespace base
}  // namespace ktile
