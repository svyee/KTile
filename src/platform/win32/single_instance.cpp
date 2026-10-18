// KTile - Win32 平台层：单实例实现
#include "platform/win32/single_instance.h"

#include <vector>

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 按标题查找已运行实例的窗口（大小写不敏感由系统处理）
HWND findWindowByTitle(const std::wstring& title) {
    return FindWindowW(nullptr, title.c_str());
}

// 按窗口类名查找已运行实例的消息窗口（标题不管）
HWND findWindowByClass(const std::wstring& className) {
    return FindWindowW(className.c_str(), nullptr);
}

}  // namespace

bool SingleInstance::tryAcquire(const std::wstring& mutexName) {
    mutex_.reset();
    HANDLE handle = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (handle == nullptr) {
        // 创建失败，保守起见当作无法取得实例权
        return false;
    }

    const DWORD lastError = GetLastError();
    mutex_.reset(handle);

    // ERROR_ALREADY_EXISTS 表示已有实例持有了这个互斥体
    if (lastError == ERROR_ALREADY_EXISTS) {
        return false;
    }
    return true;
}

void SingleInstance::activateExistingInstance(
    const std::wstring& messageWindowClass, const std::wstring& windowTitle,
    const UINT activationMessage, const WPARAM activationParam) {
    // ---- 首选：让已有实例自己「唤出面板」----
    //
    // 与热键走同一条通路（见 application.cpp 里 WM_HOTKEY 的处理）：
    // 面板会正常 show()，键盘钩子、前台窗口关系、动画都照旧。
    // 直接对主窗口 ShowWindow 做不到这些 —— 那样窗口是「露出来」了，
    // 但它自己以为还隐藏着，输入与自动收起都会错乱。
    if (!messageWindowClass.empty()) {
        const HWND messageWindow = findWindowByClass(messageWindowClass);
        if (messageWindow != nullptr &&
            PostMessageW(messageWindow, activationMessage, activationParam, 0) !=
                FALSE) {
            return;
        }
    }

    // ---- 兜底：按标题找主窗口，抢到前台 ----
    HWND window = findWindowByTitle(windowTitle);
    if (window == nullptr) {
        return;
    }

    if (IsIconic(window) != FALSE) {
        ShowWindow(window, SW_RESTORE);
    }

    // SetForegroundWindow 有前台锁定限制：后台进程调用可能被系统拒绝。
    // 这里配合 AllowSetForegroundWindow 的常见规避手法：
    // 先尝试普通激活，失败则用附加线程输入的方式强制激活。
    if (SetForegroundWindow(window) != FALSE) {
        return;
    }

    const DWORD foregroundThread = GetWindowThreadProcessId(
        GetForegroundWindow(), nullptr);
    const DWORD currentThread = GetCurrentThreadId();
    if (foregroundThread != currentThread) {
        AttachThreadInput(foregroundThread, currentThread, TRUE);
        SetForegroundWindow(window);
        BringWindowToTop(window);
        AttachThreadInput(foregroundThread, currentThread, FALSE);
    }
}

void SingleInstance::release() {
    mutex_.reset();
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
