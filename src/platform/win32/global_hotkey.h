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

#include <functional>
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

    // 事件上报（可选）：应用层注入，把状态机关键迁移写进运行日志。
    //
    // 注意：回调运行在低级键盘钩子上下文里，实现必须只做纯内存操作
    // （日志模块的 info/warn 只写缓冲、不落盘），严禁任何文件 I/O，
    // 否则可能超过 LowLevelHooksTimeout 被系统静默卸载钩子。
    using EventSink = std::function<void(const char*)>;
    void setEventSink(EventSink sink);

    // 手势是否进行到一半（Win 按下未释放 / Ctrl 连击窗口内）。
    // 看门狗定期重装钩子时用：手势中途重装会丢状态，等它做完。
    bool isMidGesture() const noexcept {
        return winState_ != WinState::Idle || ctrlHeld_;
    }

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

    // Ctrl 状态（事件队列视图）。为什么不用 GetAsyncKeyState：
    //   那个 API 返回「调用那一刻」的物理状态，而判定 Ctrl+Win 要的是
    //   「事件顺序里 Ctrl 是否已按下且未松开」。两者在「刚按完 Ctrl+C
    //   立刻按 Win」这种时序下会得出不同结论 —— 用户心智里 Ctrl 已经
    //   结束，但物理键可能还没抬起来。用事件队列才能匹配用户预期。
    //
    // StartMenu 分支注入 Ctrl keyup 后必须同步把这里置 false，
    // 否则系统状态变了本地还不知道，下一次 Win 按下会误判成 Ctrl+Win。
    bool ctrlPressed_ = false;

    EventSink eventSink_;

    // 上报一条状态机事件（未注入 sink 时为空操作）
    void report(const char* message) const;
    // 状态对账：本地状态机与物理按键状态不一致时把账结清。
    // 触发场景见 cpp 注释（钩子回调被系统跳过会丢事件）。
    void reconcileWinState();
    void trigger() const;
    static void injectWinKey(bool down);
    static void injectKey(WORD virtualKey, bool down);
};


}  // namespace win32
}  // namespace platform
}  // namespace ktile
