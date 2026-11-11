#include "platform/win32/program_paths.h"

#include <Windows.h>
#include <shlwapi.h>

#include <vector>

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 去掉首尾空白。
//
// 用户在「修改项目」的目标框里手写路径时，前后带空格是常事（从别处
// 复制过来的尤其多）。ShellExecute 不会替你裁，多一个空格就是「找不到」。
std::wstring trimSpaces(const std::wstring& value) {
    const std::size_t begin = value.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) {
        return std::wstring();
    }
    const std::size_t end = value.find_last_not_of(L" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

// 展开 %windir% 这类环境变量；没有百分号就原样返回。
//
// 与解析 .lnk 时同一套处理（application.cpp 的 resolveShortcut）：系统
// 快捷方式里常见 %ProgramFiles%\...，不展开 shell 也认，但那会把「路径
// 到底指向哪儿」藏起来，判断「是否已在运行」时更没法比对。
std::wstring expandEnvironment(const std::wstring& value) {
    if (value.find(L'%') == std::wstring::npos) {
        return value;
    }
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(
        value.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written > buffer.size()) {
        return value;  // 展开失败（变量名写错等）：保留原文，让报错看得见原文
    }
    // 返回值含结尾 NUL
    return std::wstring(buffer.data(), static_cast<std::size_t>(written) - 1);
}

}  // namespace

std::wstring executableDirectory() {
    // 进程的 exe 路径在一次运行里不会变，算一次即可。
    static const std::wstring cached = [] {
        std::vector<wchar_t> buffer(MAX_PATH, L'\0');
        for (;;) {
            const DWORD length = GetModuleFileNameW(
                nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                return std::wstring();
            }
            // 装得下（不含 NUL）就是完整路径；等于缓冲区大小说明被截断
            if (length < buffer.size()) {
                break;
            }
            buffer.resize(buffer.size() * 2, L'\0');
        }
        std::wstring path(buffer.data());
        const std::size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            path.resize(slash);
        }
        return path;
    }();
    return cached;
}

bool isProgramRelativePath(const std::wstring& path) {
    const std::wstring trimmed = trimSpaces(path);
    if (trimmed.empty()) {
        return false;
    }
    // URL（"https://..."）里有分隔符，但显然不是本地相对路径
    if (trimmed.find(L"://") != std::wstring::npos) {
        return false;
    }
    // C:\... / C:relative —— 带盘符就是绝对（或盘符相对，交给系统）
    if (trimmed.size() >= 2 && trimmed[1] == L':') {
        return false;
    }
    // UNC（\\server\share）与根相对（\foo）
    if (trimmed.front() == L'\\' || trimmed.front() == L'/') {
        return false;
    }
    // 裸名字（notepad.exe / office.farxin.cn）没给「在哪儿」的线索，
    // 交给 ShellExecute 走 PATH 解析 —— 当成相对路径会让原本能用的
    // 「notepad.exe」突然变成「<exe目录>\notepad.exe 不存在」。
    if (trimmed.find_first_of(L"\\/") == std::wstring::npos) {
        return false;
    }
    return true;
}

std::wstring resolveProgramRelativePath(const std::wstring& path) {
    std::wstring trimmed = trimSpaces(path);
    if (trimmed.empty()) {
        return trimmed;
    }
    trimmed = expandEnvironment(trimmed);
    if (!isProgramRelativePath(trimmed)) {
        return trimmed;
    }
    const std::wstring base = executableDirectory();
    if (base.empty()) {
        return trimmed;
    }
    const std::wstring combined = base + L"\\" + trimmed;
    // 规范化 ".." 与 "."：不规范化的话报错信息里会出现
    // "<exe目录>\..\bin\x.exe"，也没法和进程路径比对。
    std::vector<wchar_t> buffer(32768, L'\0');
    if (PathCanonicalizeW(buffer.data(), combined.c_str()) != FALSE) {
        return std::wstring(buffer.data());
    }
    return combined;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
