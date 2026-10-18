// KTile - Win32 平台层：全局热键实现
#include "platform/win32/global_hotkey.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 主键名 -> 虚拟键码映射表（覆盖常用键）
struct KeyEntry {
    const char* name;
    UINT virtualKey;
};

const KeyEntry kKeyTable[] = {
    // 功能区
    {"F1", VK_F1},   {"F2", VK_F2},   {"F3", VK_F3},   {"F4", VK_F4},
    {"F5", VK_F5},   {"F6", VK_F6},   {"F7", VK_F7},   {"F8", VK_F8},
    {"F9", VK_F9},   {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
    {"F13", VK_F13}, {"F14", VK_F14}, {"F15", VK_F15}, {"F16", VK_F16},
    {"F17", VK_F17}, {"F18", VK_F18}, {"F19", VK_F19}, {"F20", VK_F20},
    {"F21", VK_F21}, {"F22", VK_F22}, {"F23", VK_F23}, {"F24", VK_F24},

    // 编辑与导航
    {"Space", VK_SPACE},
    {"Enter", VK_RETURN},
    {"Return", VK_RETURN},
    {"Tab", VK_TAB},
    {"Escape", VK_ESCAPE},
    {"Esc", VK_ESCAPE},
    {"Backspace", VK_BACK},
    {"Delete", VK_DELETE},
    {"Del", VK_DELETE},
    {"Insert", VK_INSERT},
    {"Ins", VK_INSERT},
    {"Home", VK_HOME},
    {"End", VK_END},
    {"PageUp", VK_PRIOR},
    {"PageDown", VK_NEXT},
    {"Up", VK_UP},
    {"Down", VK_DOWN},
    {"Left", VK_LEFT},
    {"Right", VK_RIGHT},
    {"PrintScreen", VK_SNAPSHOT},
    {"Pause", VK_PAUSE},
    {"ScrollLock", VK_SCROLL},
    {"CapsLock", VK_CAPITAL},
    {"NumLock", VK_NUMLOCK},

    // 符号键（RegisterHotKey 用的是不含修饰的 VK）
    {"`", VK_OEM_3},
    {"-", VK_OEM_MINUS},
    {"=", VK_OEM_PLUS},
    {"[", VK_OEM_4},
    {"]", VK_OEM_6},
    {"\\", VK_OEM_5},
    {";", VK_OEM_1},
    {"'", VK_OEM_7},
    {",", VK_OEM_COMMA},
    {".", VK_OEM_PERIOD},
    {"/", VK_OEM_2},
};

}  // namespace

UINT modifiersToWin32(const std::vector<core::HotkeyModifier>& modifiers) noexcept {
    UINT flags = 0;
    for (const auto modifier : modifiers) {
        switch (modifier) {
            case core::HotkeyModifier::Alt:     flags |= MOD_ALT;     break;
            case core::HotkeyModifier::Control: flags |= MOD_CONTROL; break;
            case core::HotkeyModifier::Shift:   flags |= MOD_SHIFT;   break;
            case core::HotkeyModifier::Win:     flags |= MOD_WIN;     break;
        }
    }
    return flags;
}

UINT keyNameToVirtualKey(const std::string& key) noexcept {
    if (key.empty()) {
        return 0;
    }

    // 单字符 A-Z / 0-9 直接映射
    if (key.size() == 1) {
        const char upper = static_cast<char>(
            std::toupper(static_cast<unsigned char>(key[0])));
        if (upper >= 'A' && upper <= 'Z') {
            return static_cast<UINT>(upper - 'A' + 'A');  // 'A' == 0x41
        }
        if (upper >= '0' && upper <= '9') {
            return static_cast<UINT>(upper);  // '0' == 0x30
        }
    }

    // 表查找（大小写不敏感）
    for (const auto& entry : kKeyTable) {
        const char* left = entry.name;
        const char* right = key.c_str();
        bool equal = true;
        while (*left != '\0' && *right != '\0') {
            const char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(*left)));
            const char rc = static_cast<char>(std::tolower(static_cast<unsigned char>(*right)));
            if (lc != rc) {
                equal = false;
                break;
            }
            ++left;
            ++right;
        }
        if (equal && *left == '\0' && *right == '\0') {
            return entry.virtualKey;
        }
    }
    return 0;
}

std::string virtualKeyToKeyName(const UINT virtualKey) {
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        return std::string(1, static_cast<char>(virtualKey));
    }
    if (virtualKey >= '0' && virtualKey <= '9') {
        return std::string(1, static_cast<char>(virtualKey));
    }
    for (const auto& entry : kKeyTable) {
        if (entry.virtualKey == virtualKey) {
            return entry.name;
        }
    }
    return "Space";
}

std::wstring formatHotkey(const core::HotkeyConfig& config) {
    // 手势优先：选了手势时 modifiers + key 没有意义，显示成手势本身
    switch (config.gesture) {
        case core::HotkeyGesture::DoubleCtrl: return L"双击 Ctrl";
        case core::HotkeyGesture::WinKey:     return L"Win 键";
        case core::HotkeyGesture::None:       break;
    }

    std::wstring text;
    for (const auto modifier : config.modifiers) {
        switch (modifier) {
            case core::HotkeyModifier::Alt:     text += L"Alt+";     break;
            case core::HotkeyModifier::Control: text += L"Ctrl+";    break;
            case core::HotkeyModifier::Shift:   text += L"Shift+";   break;
            case core::HotkeyModifier::Win:     text += L"Win+";     break;
        }
    }
    // 键名转宽字符
    const std::string key = config.key.empty() ? std::string("Space") : config.key;
    for (const char c : key) {
        text += static_cast<wchar_t>(static_cast<unsigned char>(c));
    }
    return text;
}

// ---------------------------------------------------------------- GlobalHotkey

GlobalHotkey::~GlobalHotkey() {
    unregister();
}

Win32Status GlobalHotkey::registerHotkey(HWND window,
                                        const int hotkeyId,
                                        const core::HotkeyConfig& config) {
    unregister();

    if (window == nullptr) {
        return base::makeUnexpected(
            Win32Error(ERROR_INVALID_WINDOW_HANDLE, "热键注册需要有效窗口"));
    }
    if (!config.enabled) {
        return {};
    }

    const UINT modifiers = modifiersToWin32(config.modifiers);
    const UINT virtualKey = keyNameToVirtualKey(config.key);
    if (virtualKey == 0) {
        return base::makeUnexpected(
            Win32Error(ERROR_INVALID_PARAMETER, "无法识别的主键: " + config.key));
    }

    if (RegisterHotKey(window, hotkeyId, modifiers, virtualKey) == FALSE) {
        const DWORD code = GetLastError();
        // ERROR_HOTKEY_ALREADY_REGISTERED (1409) 表示被其他程序占用
        std::string message = (code == 1409)
            ? "热键已被其他程序占用"
            : "热键注册失败";
        return base::makeUnexpected(Win32Error(code, std::move(message)));
    }

    registered_ = true;
    window_ = window;
    hotkeyId_ = hotkeyId;
    return {};
}

void GlobalHotkey::unregister() {
    if (registered_ && window_ != nullptr) {
        UnregisterHotKey(window_, hotkeyId_);
    }
    registered_ = false;
    window_ = nullptr;
    hotkeyId_ = 0;
}

// ---------------------------------------------------------------- GestureHotkey
namespace {

// 低级键盘钩子回调是 C 函数，拿不到 this —— 只能走文件级单例。
// 进程内同时只会有一个手势钩子（用户只能选一种唤醒方式），
// 所以单例不会冲突。
GestureHotkey* g_gestureHotkey = nullptr;

// 双击判定窗口。400ms 是「像双击」与「两次无关的 Ctrl」之间的
// 折中：Windows 默认双击间隔是 500ms，但键盘上连按比鼠标慢，
// 取 400ms 手感更稳，也不至于把两次独立操作误判成双击。
constexpr ULONGLONG kDoubleCtrlWindowMs = 400;

// 我们自己补发的按键用这个签名打标（dwExtraInfo 会原样传到钩子）。
//
// 为什么要打标而不是看 LLKHF_INJECTED：钩子必须放行**自己**注入的
// Win 键（否则补发出去又被吞掉，死循环），但不能顺手把别的程序
// 注入的按键也一起放过 —— 自动化脚本模拟双击 Ctrl 时是真按键，
// 按 LLKHF_INJECTED 一刀切会让「脚本压不出手势」变成假故障。
// 只认自己的签名，两种情形都正确。
constexpr ULONG_PTR kInjectSignature = 0x4B4C4155;  // 'KLAU'

bool isCtrlKey(const DWORD virtualKey) noexcept {
    return virtualKey == VK_CONTROL || virtualKey == VK_LCONTROL ||
           virtualKey == VK_RCONTROL;
}

bool isWinKey(const DWORD virtualKey) noexcept {
    return virtualKey == VK_LWIN || virtualKey == VK_RWIN;
}

LRESULT CALLBACK gestureHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_gestureHotkey != nullptr) {
        const auto* event = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (event != nullptr && g_gestureHotkey->handleKeyEvent(wParam, *event)) {
            return 1;  // 吞掉
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // namespace

GestureHotkey::~GestureHotkey() {
    uninstall();
}

Win32Status GestureHotkey::install(const HWND notifyWindow, const int hotkeyId,
                                   const core::HotkeyGesture gesture) {
    uninstall();

    if (gesture == core::HotkeyGesture::None) {
        return {};
    }
    if (notifyWindow == nullptr) {
        return base::makeUnexpected(
            Win32Error(ERROR_INVALID_WINDOW_HANDLE, "手势唤醒需要有效窗口"));
    }

    notifyWindow_ = notifyWindow;
    hotkeyId_ = hotkeyId;
    gesture_ = gesture;
    // 「Ctrl+Win 开开始菜单」只在选了单按 Win 时才需要 —— 其它方式下
    // Win 键本来就归系统管，不需要我们代劳。
    startMenuByWinKey_ = (gesture == core::HotkeyGesture::WinKey);

    g_gestureHotkey = this;
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &gestureHookProc,
                              GetModuleHandleW(nullptr), 0);
    if (hook_ == nullptr) {
        const DWORD code = GetLastError();
        g_gestureHotkey = nullptr;
        notifyWindow_ = nullptr;
        hotkeyId_ = 0;
        gesture_ = core::HotkeyGesture::None;
        return base::makeUnexpected(
            Win32Error(code, "安装键盘钩子失败（可能被安全软件拦截）"));
    }
    return {};
}

void GestureHotkey::uninstall() {
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (g_gestureHotkey == this) {
        g_gestureHotkey = nullptr;
    }
    notifyWindow_ = nullptr;
    hotkeyId_ = 0;
    gesture_ = core::HotkeyGesture::None;
    lastCtrlDownTick_ = 0;
    ctrlHeld_ = false;
    winState_ = WinState::Idle;
    startMenuByWinKey_ = false;
}

void GestureHotkey::trigger() const {
    if (notifyWindow_ == nullptr) {
        return;
    }
    // 与 RegisterHotKey 完全相同的下游：走 WM_HOTKEY，应用层不区分来源。
    PostMessageW(notifyWindow_, WM_HOTKEY, static_cast<WPARAM>(hotkeyId_), 0);
}

void GestureHotkey::injectWinKey(const bool down) {
    injectKey(VK_LWIN, down);
}

void GestureHotkey::injectKey(const WORD virtualKey, const bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    input.ki.dwExtraInfo = kInjectSignature;
    SendInput(1, &input, sizeof(INPUT));
}

bool GestureHotkey::handleKeyEvent(const WPARAM message,
                                   const KBDLLHOOKSTRUCT& event) {
    // 自己补发的按键必须原样放行，否则补发 Win 会再被自己吞掉（死循环）。
    if (event.dwExtraInfo == kInjectSignature) {
        return false;
    }

    const bool isDown = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
    const bool isUp = (message == WM_KEYUP || message == WM_SYSKEYUP);
    if (!isDown && !isUp) {
        return false;
    }

    // ---------------------------------------------------------- 双击 Ctrl
    if (gesture_ == core::HotkeyGesture::DoubleCtrl) {
        if (isCtrlKey(event.vkCode)) {
            if (isDown) {
                // 长按产生的重复按下来不算一次新点击，否则按住 Ctrl 就唤醒了
                if (!ctrlHeld_) {
                    ctrlHeld_ = true;
                    const ULONGLONG now = GetTickCount64();
                    if (lastCtrlDownTick_ != 0 &&
                        now - lastCtrlDownTick_ <= kDoubleCtrlWindowMs) {
                        lastCtrlDownTick_ = 0;
                        trigger();
                    } else {
                        lastCtrlDownTick_ = now;
                    }
                }
            } else {
                ctrlHeld_ = false;
            }
            // Ctrl 本身永远放行：Ctrl+C / Ctrl+点击 这些不能受影响
            return false;
        }
        if (isDown) {
            // 中间按了别的键 → 这两次 Ctrl 不是一组双击
            lastCtrlDownTick_ = 0;
        }
        return false;
    }

    // ---------------------------------------------------------- 单按 Win 键
    if (gesture_ == core::HotkeyGesture::WinKey) {
        if (isWinKey(event.vkCode)) {
            if (isDown) {
                if (winState_ == WinState::Idle) {
                    const bool ctrlDown =
                        (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (startMenuByWinKey_ && ctrlDown) {
                        // Ctrl+Win：用户约定它仍开开始菜单。原生的组合
                        // 被我们吞掉了，这里代劳。
                        //
                        // 注意不能「补发一个 Win 键」了事 —— Ctrl 还物理
                        // 按着，系统看到的仍是 Ctrl+Win，仍然不弹菜单
                        // （实测坐实）。必须先把 Ctrl「松开」（注入 keyup，
                        // 物理键的抬起随后到来，重复 keyup 无害），
                        // 再发一次干净的 Win 按下+抬起，由系统自己弹菜单。
                        // 代价：这一次 Ctrl 的系统状态被提前置为松开，
                        // 用户继续按住 Ctrl 打字不再算组合键 —— 但这正是
                        // 「按 Ctrl+Win 开菜单」这个动作的语义终点。
                        injectKey(VK_CONTROL, false);
                        injectWinKey(true);
                        injectWinKey(false);
                        winState_ = WinState::StartMenu;
                    } else {
                        winState_ = WinState::Held;
                    }
                }
                return true;  // 按下先吞，绝不让开始菜单自己弹出来
            }
            // ---- 抬起 ----
            switch (winState_) {
                case WinState::Held:
                    winState_ = WinState::Idle;
                    trigger();
                    break;
                case WinState::Replayed:
                    // 组合键已经放行给系统了，这里必须补一个抬起，
                    // 否则系统会认为 Win 一直按着
                    injectWinKey(false);
                    winState_ = WinState::Idle;
                    break;
                case WinState::StartMenu:
                    winState_ = WinState::Idle;
                    break;
                case WinState::Idle:
                    break;
            }
            return true;
        }

        // 其它键：Win 还按着时按了别的键 → 判定为 Win+X 组合
        if (isDown && winState_ == WinState::Held) {
            winState_ = WinState::Replayed;
            injectWinKey(true);  // 把「Win 按着」这件事补给系统
        }
        return false;
    }

    return false;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
