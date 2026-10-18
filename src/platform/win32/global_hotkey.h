// KTile - Win32 平台层：全局热键
//
// 使用 RegisterHotKey 注册系统级热键，配合 WM_HOTKEY 消息唤醒窗口。
//
// Win7 兼容：RegisterHotKey / UnregisterHotKey 均为 XP 时代 API。
//
#pragma once

#include "core/data_model.h"
#include "platform/win32/win32_error.h"

#include <Windows.h>

#include <string>
#include <vector>

namespace ktile {
namespace platform {
namespace win32 {

// 把热键配置转换成 RegisterHotKey 所需的修饰符标志
UINT modifiersToWin32(const std::vector<core::HotkeyModifier>& modifiers) noexcept;

// 把主键名（如 "Space" / "F1" / "A"）转换成虚拟键码。
// 无法识别时返回 0。
UINT keyNameToVirtualKey(const std::string& key) noexcept;

// 反向：虚拟键码转显示名
std::string virtualKeyToKeyName(UINT virtualKey);

// 把热键配置格式化成可读文本（如 "Alt+Space"），用于界面显示
std::wstring formatHotkey(const core::HotkeyConfig& config);

class GlobalHotkey {
public:
    GlobalHotkey() = default;

    GlobalHotkey(const GlobalHotkey&) = delete;
    GlobalHotkey& operator=(const GlobalHotkey&) = delete;

    ~GlobalHotkey();

    // 注册热键。同一个 GlobalHotkey 重复注册会先注销旧热键。
    // hotkeyId 用于区分 WM_HOTKEY 消息来源。
    Win32Status registerHotkey(HWND window,
                               int hotkeyId,
                               const core::HotkeyConfig& config);

    // 注销热键
    void unregister();

    bool isRegistered() const noexcept { return registered_; }

private:
    bool registered_ = false;
    HWND window_ = nullptr;
    int hotkeyId_ = 0;
};

// 手势唤醒：双击 Ctrl / 单击 Win 键。
//
// 为什么必须用低级键盘钩子：
//   RegisterHotKey 只能表达「修饰键 + 另一个键」，而这两种手势都是
//   「同一个键自己构成动作」，系统级热键 API 根本表达不了。
//
// 为什么只在选中手势时才装钩子：
//   钩子对每次按键都要回一次本进程，是有成本的。默认的 Alt+Space /
//   Ctrl+Space 仍然走零开销的 RegisterHotKey，绝大多数用户不受影响。
//
// 唤醒信号统一成「往 notifyWindow 投递 WM_HOTKEY(wParam=hotkeyId)」，
// 与 RegisterHotKey 完全同一条下游 —— 应用层不需要区分来源。
class GestureHotkey {
public:
    GestureHotkey() = default;

    GestureHotkey(const GestureHotkey&) = delete;
    GestureHotkey& operator=(const GestureHotkey&) = delete;

    ~GestureHotkey();

    // gesture 为 None 时等同 uninstall()。
    Win32Status install(HWND notifyWindow, int hotkeyId,
                        core::HotkeyGesture gesture);

    void uninstall();

    bool isInstalled() const noexcept { return hook_ != nullptr; }

    // 供钩子回调使用（文件级单例转发）。返回 true 表示「吞掉这个按键」。
    bool handleKeyEvent(WPARAM message, const KBDLLHOOKSTRUCT& event);

private:
    HHOOK hook_ = nullptr;
    HWND notifyWindow_ = nullptr;
    int hotkeyId_ = 0;
    core::HotkeyGesture gesture_ = core::HotkeyGesture::None;

    // ---- 双击 Ctrl 状态 ----
    ULONGLONG lastCtrlDownTick_ = 0;
    bool ctrlHeld_ = false;

    // ---- 单击 Win 状态 ----
    //
    // Win 键不能一按下就无条件吞掉：Win+E / Win+R / Win+D 都是常用组合，
    // 吞掉按下就等于把它们废了。所以按下时先吞（不让开始菜单弹出来），
    // 等「后续有没有别的键」来判定这到底是单击还是组合：
    //   Held     —— 按下了、还没别的键，抬起就是单击 → 唤醒面板
    //   Replayed —— 期间按了别的键，判定为组合 → 补发 Win 按下放行
    //   StartMenu—— Ctrl 按着时按下 Win，补发一次「干净」的 Win 按下+抬起
    //               让系统弹开始菜单（用户约定：Ctrl+Win 仍开开始菜单）
    enum class WinState : std::uint8_t {
        Idle = 0,
        Held = 1,
        Replayed = 2,
        StartMenu = 3,
    };
    WinState winState_ = WinState::Idle;

    bool startMenuByWinKey_ = false;  // install 时记录：Ctrl+Win 要不要开开始菜单

    void trigger() const;
    static void injectWinKey(bool down);
    static void injectKey(WORD virtualKey, bool down);
};


}  // namespace win32
}  // namespace platform
}  // namespace ktile
