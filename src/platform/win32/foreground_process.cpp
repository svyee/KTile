#include "platform/win32/foreground_process.h"

#include <Windows.h>
#include <Psapi.h>

#include <cwctype>

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 文件名部分（最后一个反斜杠之后）转小写。与配置中的进程名比较用。
std::wstring imageBaseNameToLower(const wchar_t* path) {
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') {
            name = p + 1;
        }
    }
    std::wstring result = name;
    for (wchar_t& ch : result) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return result;
}

std::wstring classNameToLower(HWND window) {
    wchar_t className[256] = {};
    if (GetClassNameW(window, className, 256) == 0) {
        return std::wstring();
    }
    std::wstring result = className;
    for (wchar_t& ch : result) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return result;
}

// 单个窗口的进程名快照。PROCESS_QUERY_LIMITED_INFORMATION 权限更低，
// 在 UAC 下对非提升进程也更容易成功。
std::wstring processNameForWindow(HWND window) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0) {
        return std::wstring();
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return std::wstring();
    }

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &size) != FALSE) {
        result = imageBaseNameToLower(path);
    }
    CloseHandle(process);
    return result;
}

WindowTarget targetForWindow(HWND window) {
    WindowTarget target;
    if (window == nullptr) {
        return target;
    }
    target.window = window;
    target.processName = processNameForWindow(window);
    target.windowClass = classNameToLower(window);
    return target;
}

// 光标点命中的窗口是否可以作为轮盘目标。排除最小化与自己的覆盖层：
// 前者是无效目标，后者会在轮盘已显示时再次触发时命中自己。
bool isEligibleTarget(HWND window) {
    if (window == nullptr || IsWindowVisible(window) == FALSE || IsIconic(window) != FALSE) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId != GetCurrentProcessId();
}

}  // namespace

std::wstring foregroundProcessName() {
    return targetForWindow(GetForegroundWindow()).processName;
}

std::wstring foregroundWindowClass() {
    return targetForWindow(GetForegroundWindow()).windowClass;
}

WindowTarget radialTargetWindow() {
    POINT point = {};
    if (GetCursorPos(&point) != FALSE) {
        HWND hit = WindowFromPoint(point);
        HWND root = (hit != nullptr) ? GetAncestor(hit, GA_ROOT) : nullptr;
        if (isEligibleTarget(root)) {
            return targetForWindow(root);
        }
    }
    // 光标位置取不到（多屏边界等）或命中的是覆盖层：回退前台窗口。
    return targetForWindow(GetForegroundWindow());
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
