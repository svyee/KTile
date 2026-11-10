#include "platform/win32/foreground_process.h"

#include <Windows.h>
#include <Psapi.h>

#include <cwctype>

namespace ktile {
namespace platform {
namespace win32 {

std::wstring foregroundProcessName() {
    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return std::wstring();
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (processId == 0) {
        return std::wstring();
    }

    // PROCESS_QUERY_LIMITED_INFORMATION 比 PROCESS_QUERY_INFORMATION 权限更低，
    // 在 UAC 下对非提升进程也更容易成功。
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return std::wstring();
    }

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &size) != FALSE) {
        // 取文件名部分（最后一个反斜杠之后）
        const wchar_t* name = path;
        for (const wchar_t* p = path; *p != L'\0'; ++p) {
            if (*p == L'\\' || *p == L'/') {
                name = p + 1;
            }
        }
        result = name;
        // 转小写，便于与配置中的进程名比较
        for (wchar_t& ch : result) {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
    }

    CloseHandle(process);
    return result;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
