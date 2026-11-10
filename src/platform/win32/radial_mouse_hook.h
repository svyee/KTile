// KTile - Win32 平台层：轮盘菜单鼠标钩子
//
// 职责：
//   1. 用 WH_MOUSE_LL 监听指定鼠标键（右 / 中 / 侧键）的按下/移动/抬起。
//   2. 按下时检查修饰键条件（Ctrl/Shift/Alt），不满足则放行（让 MouseInc 等
//      手势软件正常工作）；满足则吞掉事件并进入跟踪。
//   3. 拖动超过阈值时通知主线程显示轮盘；继续移动更新光标位置。
//   4. 抬起时通知主线程最终位置（由主线程判定命中哪个扇区或取消）。
//   5. 若未超过阈值且触发方式无修饰键，重新注入一次原始点击（让原生右键菜单
//      正常弹出）；有修饰键时不注入（用户明确是想做手势，不弹原生菜单）。
//
// 钩子回调纪律（与 GestureHotkey / OutsideClickMonitor 一致）：
//   - 只做纯内存操作 + PostMessage，严禁文件 I/O 或任何耗时操作。
//   - 回调在注册线程（消息循环线程）上执行，与窗口过程同线程，无需加锁。
//
#pragma once

#include "core/radial_menu.h"  // RadialTriggerConfig

#include <Windows.h>

namespace ktile {
namespace platform {
namespace win32 {

// 通知消息（投递到 notifyWindow_ 的窗口过程）
//   kRadialShowMessage    —— 拖动超过阈值，wParam=起点X, lParam=起点Y
//   kRadialMoveMessage    —— 光标移动，wParam=当前X, lParam=当前Y
//   kRadialReleaseMessage —— 按键抬起，wParam=当前X, lParam=当前Y
inline constexpr UINT kRadialShowMessage    = WM_APP + 0x71U;
inline constexpr UINT kRadialMoveMessage    = WM_APP + 0x72U;
inline constexpr UINT kRadialReleaseMessage = WM_APP + 0x73U;

class RadialMouseHook {
public:
    RadialMouseHook() = default;

    RadialMouseHook(const RadialMouseHook&) = delete;
    RadialMouseHook& operator=(const RadialMouseHook&) = delete;

    ~RadialMouseHook();

    // 安装钩子。trigger 为 enabled=false 时等同 uninstall()。
    bool install(HWND notifyWindow, const core::RadialTriggerConfig& trigger);

    void uninstall() noexcept;

    bool isInstalled() const noexcept { return hook_ != nullptr; }

    // 供钩子回调使用（文件级单例转发）
    bool handleMouseEvent(WPARAM message, const MSLLHOOKSTRUCT& event);

private:
    enum class State : std::uint8_t {
        Idle = 0,
        Tracking = 1,   // 按键已按下，未超阈值
        Shown = 2,      // 已超阈值，轮盘已显示
    };

    HHOOK hook_ = nullptr;
    HWND notifyWindow_ = nullptr;
    core::RadialTriggerConfig trigger_;

    State state_ = State::Idle;
    POINT startPoint_{};
    POINT currentPoint_{};
    // 触发时按下的是哪个键（WM_RBUTTONDOWN / WM_MBUTTONDOWN / WM_XBUTTONDOWN）
    WPARAM triggerMessage_ = 0;

    // 自身注入的事件签名（避免重复处理自己注入的点击）
    static constexpr ULONG_PTR kInjectSignature = 0x4B545244;  // "KTRD"

    static RadialMouseHook* active_;

    // 低级鼠标钩子回调（文件级静态，转发到 active_ 实例）
    static LRESULT CALLBACK mouseHookProc(int code, WPARAM wParam, LPARAM lParam);

    // 检查修饰键是否满足配置
    bool modifierSatisfied() const noexcept;
    // 把虚拟键码对应的鼠标按键重新注入（用于阈值内抬起时恢复原生点击）
    void reinjectClick() const;
    // 当前配置的按键对应的按下/抬起消息
    WPARAM downMessage() const noexcept;
    WPARAM upMessage() const noexcept;
};

}  // namespace win32
}  // namespace platform
}  // namespace ktile
