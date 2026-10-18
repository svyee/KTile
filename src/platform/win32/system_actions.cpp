#include "platform/win32/system_actions.h"

#include "base/text_convert.h"

#include <shellapi.h>
#include <winuser.h>

#include <string>

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// ExitWindowsEx 的 EWX_SHUTDOWN / EWX_REBOOT 需要 SE_SHUTDOWN_NAME 特权。
// 普通用户进程默认没有，必须先自己打开 token 把它启用 —— 不启用时
// API 直接失败并返回 ERROR_NOT_READY 这类含糊的错误码，很难排查。
bool enableShutdownPrivilege() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) == FALSE) {
        return false;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME,
                              &privileges.Privileges[0].Luid) == FALSE) {
        CloseHandle(token);
        return false;
    }

    const BOOL adjusted = AdjustTokenPrivileges(token, FALSE, &privileges, 0,
                                                nullptr, nullptr);
    const bool ok = adjusted != FALSE && GetLastError() != ERROR_NOT_ALL_ASSIGNED;
    CloseHandle(token);
    return ok;
}

// 把系统错误码转成可读描述（与 item_launcher 的报告风格一致）
std::wstring lastErrorText(const wchar_t* action) {
    const DWORD code = GetLastError();
    wchar_t* buffer = nullptr;
    std::wstring detail;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                           FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       reinterpret_cast<LPWSTR>(&buffer), 0, nullptr) != 0 &&
        buffer != nullptr) {
        detail = buffer;
        LocalFree(buffer);
        while (!detail.empty() &&
               (detail.back() == L'\r' || detail.back() == L'\n')) {
            detail.pop_back();
        }
    }
    std::wstring message(action);
    if (!detail.empty()) {
        message += L"：";
        message += detail;
    } else {
        message += L"失败（错误码 " + std::to_wstring(code) + L"）。";
    }
    return message;
}

// 注入一个多媒体/音量虚拟键。 这些键的语义由系统与前台媒体程序解释，
// 不需要也不应该去操纵混音器 API。
LaunchResult sendVirtualKey(const WORD virtualKey, const int repeats = 1) {
    for (int i = 0; i < repeats; ++i) {
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = virtualKey;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = virtualKey;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        if (SendInput(2, inputs, sizeof(INPUT)) != 2) {
            LaunchResult result;
            result.ok = false;
            result.error = L"按键注入失败。";
            return result;
        }
    }
    LaunchResult result;
    result.ok = true;
    return result;
}

}  // namespace

LaunchResult runBuiltinAction(const std::wstring& id) {
    LaunchResult result;
    result.ok = false;

    if (id == L"lock") {
        if (LockWorkStation() == FALSE) {
            result.error = lastErrorText(L"锁定失败");
            return result;
        }
        result.ok = true;
        return result;
    }

    if (id == L"shutdown" || id == L"restart" || id == L"logoff") {
        if (!enableShutdownPrivilege()) {
            // 注销不需要特权也能成功 —— 权限启用失败只影响关机/重启，
            // 但统一先报出来，比「点了没反应」强。
            result.error = L"未能取得关机特权（当前账户可能受限）。";
            if (id == L"logoff") {
                result.error.clear();
            } else {
                return result;
            }
        }
        UINT flags = 0;
        if (id == L"shutdown") {
            flags = EWX_SHUTDOWN | EWX_POWEROFF;
        } else if (id == L"restart") {
            flags = EWX_REBOOT;
        } else {
            flags = EWX_LOGOFF;
        }
        if (ExitWindowsEx(flags, SHTDN_REASON_MAJOR_APPLICATION |
                                    SHTDN_REASON_MINOR_MAINTENANCE |
                                    SHTDN_REASON_FLAG_PLANNED) == FALSE) {
            result.error = lastErrorText(L"系统操作失败");
            return result;
        }
        result.ok = true;
        return result;
    }

    // 睡眠 / 休眠 / 关闭显示器按用户要求移除：
    //   - 休眠在 Windows 上经常被 powercfg 关掉，点了没反应像程序坏了；
    //   - 关显示器是「看不见的状态」，误点后很难判断到底生效没有。
    // 这里不留半死的分支 —— 命令表里也不再有这些 id。

    if (id == L"volume-up") {
        return sendVirtualKey(VK_VOLUME_UP, 2);
    }
    if (id == L"volume-down") {
        return sendVirtualKey(VK_VOLUME_DOWN, 2);
    }
    if (id == L"volume-mute") {
        return sendVirtualKey(VK_VOLUME_MUTE, 1);
    }
    if (id == L"media-prev") {
        return sendVirtualKey(VK_MEDIA_PREV_TRACK, 1);
    }
    if (id == L"media-playpause") {
        return sendVirtualKey(VK_MEDIA_PLAY_PAUSE, 1);
    }
    if (id == L"media-next") {
        return sendVirtualKey(VK_MEDIA_NEXT_TRACK, 1);
    }

    if (id == L"hosts") {
        // hosts 是无扩展名文件，直接启动会弹「选择打开方式」；
        // 固定用记事本打开 —— 与 Maye 的行为一致，也符合「手改配置」
        // 的使用场景。修改保存需要管理员权限，由记事本自己提示。
        wchar_t systemDirectory[MAX_PATH] = {};
        UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            result.error = L"无法定位系统目录。";
            return result;
        }
        const std::wstring hostsPath =
            std::wstring(systemDirectory) + L"\\drivers\\etc\\hosts";

        HINSTANCE instance = ShellExecuteW(
            nullptr, L"open", L"notepad.exe", hostsPath.c_str(), nullptr,
            SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(instance) <= 32) {
            result.error = L"无法启动记事本。";
            return result;
        }
        result.ok = true;
        return result;
    }

    result.error = L"未知的内置命令：" + id;
    return result;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
