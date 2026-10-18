// KTile - Win32 平台层：通用类型与错误处理
//
// 设计原则：
//  1. Win7 兼容 —— 只使用 Win7 SP1 已存在的 API
//  2. 错误使用 Expected<T, Win32Error> 而非异常
//  3. RAII 管理句柄（不引入 WIL，自己写轻量封装）
//
#pragma once

#include "base/expected.h"

#include <Windows.h>

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

// ---------------------------------------------------------------- 错误类型

struct Win32Error {
    DWORD code = ERROR_SUCCESS;
    std::string message;

    Win32Error() = default;
    Win32Error(DWORD code_, std::string message_)
        : code(code_), message(std::move(message_)) {}

    // 便于日志输出
    std::string format() const {
        return "Win32Error(" + std::to_string(code) + "): " + message;
    }
};

// 构造错误：自动抓取 GetLastError
inline Win32Error lastError(std::string message) {
    return Win32Error(GetLastError(), std::move(message));
}

inline Win32Error hresultError(HRESULT hr, std::string message) {
    return Win32Error(static_cast<DWORD>(hr), std::move(message));
}

template <typename T>
using Win32Result = base::Expected<T, Win32Error>;

using Win32Status = base::Expected<void, Win32Error>;

// ---------------------------------------------------------------- 通用结果

// 把 bool 返回值 + GetLastError 转成 Win32Status
inline Win32Status statusFromBool(BOOL result, std::string message) {
    if (result != FALSE) {
        return {};
    }
    return base::makeUnexpected(lastError(std::move(message)));
}

inline Win32Status statusFromHr(HRESULT hr, std::string message) {
    if (SUCCEEDED(hr)) {
        return {};
    }
    return base::makeUnexpected(hresultError(hr, std::move(message)));
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
