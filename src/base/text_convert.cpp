// KTile - C++17 兼容层：UTF-8 / UTF-16 转换实现
#include "base/text_convert.h"

#include <Windows.h>

#include <cstddef>

namespace ktile {
namespace base {

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), static_cast<int>(utf8.size()), &wide[0], needed);
    if (written <= 0) {
        return std::wstring();
    }
    wide.resize(static_cast<std::size_t>(written));
    return wide;
}

std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), static_cast<int>(wide.size()),
        &utf8[0], needed, nullptr, nullptr);
    if (written <= 0) {
        return std::string();
    }
    utf8.resize(static_cast<std::size_t>(written));
    return utf8;
}

// ---------------------------------------------------------------- 宽松版本
// 不校验非法字节：Windows 会用 U+FFFD 之类替换，保证总有输出。
// 注意 MB_ERR_INVALID_CHARS 在 UTF-8 场景下 Win7 也支持，
// 但宽松版本刻意不带该标志，以容忍脏数据。

std::wstring utf8ToWideLossy(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &wide[0], needed);
    if (written <= 0) {
        return std::wstring();
    }
    wide.resize(static_cast<std::size_t>(written));
    return wide;
}

std::string wideToUtf8Lossy(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        &utf8[0], needed, nullptr, nullptr);
    if (written <= 0) {
        return std::string();
    }
    utf8.resize(static_cast<std::size_t>(written));
    return utf8;
}

}  // namespace base
}  // namespace ktile
