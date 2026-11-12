// KTile - 单按 Win 手势的状态机测试
//
// 重点是两件容易回归的事：
//   1. 按住 Win 时系统每 30ms 补发一次 keydown（自动重复），这些重复事件
//      不能再结算成一次单击，否则面板会闪成一片；
//   2. Win+其他键是组合，不能顺带把面板唤醒。
// 走真实的低级键盘钩子与 SendInput，不手工喂事件：判定依赖 GetAsyncKeyState
// 的物理状态，脱离真实注入就测不出对账逻辑。
//
#include "core/data_model.h"
#include "platform/win32/global_hotkey.h"

#include <Windows.h>

#include <cstdio>

namespace {

using namespace ktile;

int g_failures = 0;
int g_checks = 0;
int g_hotkeys = 0;

// 钩子回调里不能做 I/O（会超过 LowLevelHooksTimeout 被系统卸载），
// 状态机迁移先攒在这里，测试结束后统一打印。
char g_trace[4096] = {};
unsigned g_traceLen = 0;

void trace(const char* text) {
    const unsigned length = static_cast<unsigned>(lstrlenA(text));
    if (g_traceLen + length + 2 >= sizeof(g_trace)) {
        return;
    }
    for (unsigned i = 0; i < length; ++i) {
        g_trace[g_traceLen++] = text[i];
    }
    g_trace[g_traceLen++] = '\n';
    g_trace[g_traceLen] = '\0';
}

void resetTrace() {
    g_traceLen = 0;
    g_trace[0] = '\0';
}

void dumpTrace() {
    if (g_traceLen == 0) {
        return;
    }
    std::printf("%s", g_trace);
    g_traceLen = 0;
    g_trace[0] = '\0';
}

void check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

constexpr wchar_t kWndClass[] = L"KTileGestureTest";

LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_HOTKEY) {
        ++g_hotkeys;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void pump(const int milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
    MSG msg;
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

// 不带 kInjectSignature，钩子会当物理按键处理。
// 返回 false：SendInput 被环境拒绝（锁屏 / 沙箱不在可交互桌面），注入不进去，
// 此时整个测试应跳过而不是报假失败（0 触发会让三个用例看起来像回归）。
bool inject(const WORD virtualKey, const bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    input.ki.dwExtraInfo = 0;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

// 会话里注入被整体拒绝时（OpenInputDesktop 失败是同源信号）跳过全部用例。
bool inputInjectionAvailable() {
    return OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS) != nullptr;
}

HWND createMessageWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);
    return CreateWindowExW(0, kWndClass, L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
}

// 按住 Win 不放：首次按下之后是系统的自动重复
void testAutoRepeatTriggersOnce() {
    const HWND hwnd = createMessageWindow();
    CHECK(hwnd != nullptr);
    if (hwnd == nullptr) {
        return;
    }

    platform::win32::GestureHotkey gesture;
    gesture.setEventSink(trace);
    const auto status = gesture.install(hwnd, 1, core::HotkeyGesture::WinKey);
    CHECK(static_cast<bool>(status));
    if (!static_cast<bool>(status)) {
        DestroyWindow(hwnd);
        return;
    }

    g_hotkeys = 0;
    inject(VK_LWIN, true);
    pump(60);
    for (int i = 0; i < 8; ++i) {
        inject(VK_LWIN, true);  // 自动重复
        pump(35);
    }
    inject(VK_LWIN, false);
    pump(150);

    if (g_hotkeys != 1) {
        std::printf("    自动重复后触发次数 = %d（期望 1）\n", g_hotkeys);
        dumpTrace();
    }
    resetTrace();
    CHECK(g_hotkeys == 1);

    gesture.uninstall();
    pump(50);
    DestroyWindow(hwnd);
}

// Win+F13（无系统绑定）：判定为组合，不能唤醒
void testCombinationDoesNotTrigger() {
    const HWND hwnd = createMessageWindow();
    CHECK(hwnd != nullptr);
    if (hwnd == nullptr) {
        return;
    }

    platform::win32::GestureHotkey gesture;
    const auto status = gesture.install(hwnd, 2, core::HotkeyGesture::WinKey);
    CHECK(static_cast<bool>(status));
    if (!static_cast<bool>(status)) {
        DestroyWindow(hwnd);
        return;
    }

    g_hotkeys = 0;
    inject(VK_LWIN, true);
    pump(40);
    inject(VK_F13, true);
    pump(40);
    inject(VK_F13, false);
    pump(40);
    inject(VK_LWIN, false);
    pump(150);

    if (g_hotkeys != 0) {
        std::printf("    Win+F13 后触发次数 = %d（期望 0）\n", g_hotkeys);
        dumpTrace();
    }
    resetTrace();
    CHECK(g_hotkeys == 0);

    gesture.uninstall();
    pump(50);
    DestroyWindow(hwnd);
}

// 唤醒方式切走后钩子要干净卸载，且不留下按住的 Win
void testUninstallClearsState() {
    const HWND hwnd = createMessageWindow();
    CHECK(hwnd != nullptr);
    if (hwnd == nullptr) {
        return;
    }

    platform::win32::GestureHotkey gesture;
    CHECK(static_cast<bool>(gesture.install(hwnd, 3, core::HotkeyGesture::WinKey)));
    inject(VK_LWIN, true);
    pump(40);
    inject(VK_F13, true);  // 进入 Replayed：补发过 Win 按下
    pump(40);
    inject(VK_F13, false);
    pump(40);

    g_hotkeys = 0;
    gesture.uninstall();  // 必须补发 Win 抬起
    pump(100);
    CHECK(g_hotkeys == 0);
    CHECK(!gesture.isInstalled());

    DestroyWindow(hwnd);
}

}  // namespace

int main() {
    std::printf("test_gesture_win\n");

    if (!inputInjectionAvailable()) {
        std::printf("  跳过：当前会话注入不了输入（锁屏或非交互桌面），"
                    "换到正常桌面会话再跑\n");
        return 0;
    }

    testAutoRepeatTriggersOnce();
    testCombinationDoesNotTrigger();
    testUninstallClearsState();

    if (g_failures == 0) {
        std::printf("  全部通过 (%d 项检查)\n", g_checks);
        return 0;
    }
    std::printf("  失败 %d / %d\n", g_failures, g_checks);
    return 1;
}
