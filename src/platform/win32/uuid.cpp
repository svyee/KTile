#include "platform/win32/uuid.h"

#include <Windows.h>
#include <objbase.h>

#include <cstdio>

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 把 GUID 格式化为 36 字符小写串（无花括号）。
//
// 不用 StringFromGUID2：它输出的是 "{...}" 带花括号的大写形式，
// 还得再去掉花括号、再转小写，绕一圈不如自己按位拼接来得直接。
std::string formatGuid(const GUID& value) {
    char text[40] = {};
    const int written = std::snprintf(
        text, sizeof(text),
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        static_cast<unsigned long>(value.Data1),
        static_cast<unsigned>(value.Data2),
        static_cast<unsigned>(value.Data3),
        static_cast<unsigned>(value.Data4[0]),
        static_cast<unsigned>(value.Data4[1]),
        static_cast<unsigned>(value.Data4[2]),
        static_cast<unsigned>(value.Data4[3]),
        static_cast<unsigned>(value.Data4[4]),
        static_cast<unsigned>(value.Data4[5]),
        static_cast<unsigned>(value.Data4[6]),
        static_cast<unsigned>(value.Data4[7]));
    if (written <= 0) {
        return std::string();
    }
    return std::string(text, static_cast<std::size_t>(written));
}

// 兜底序列：CoCreateGuid 万一失败（系统 COM 组件异常），
// 也不能让「添加项目」失败 —— 用「时间戳 + 进程内自增」拼一个。
// 它不保证跨机器唯一，但在本机数据文件里绝对够用，
// 且比「返回空串导致添加被拒」体验好得多。
LONG g_fallbackCounter = 0;

std::string fallbackId() {
    const DWORD now = GetTickCount();
    const LONG seq = InterlockedIncrement(&g_fallbackCounter);
    char text[40] = {};
    const int written = std::snprintf(text, sizeof(text), "fallback-%08lx-%04lx",
                                     static_cast<unsigned long>(now),
                                     static_cast<unsigned long>(seq));
    if (written <= 0) {
        return std::string("fallback");
    }
    return std::string(text, static_cast<std::size_t>(written));
}

}  // namespace

std::string createGuidString() {
    GUID value{};
    if (SUCCEEDED(CoCreateGuid(&value))) {
        const std::string formatted = formatGuid(value);
        if (!formatted.empty()) {
            return formatted;
        }
    }
    return fallbackId();
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
