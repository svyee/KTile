#pragma once

// 窗口外点击监视器：点击面板以外任何地方都收起启动器（钉住时除外）。
//
// 不能用 WM_KILLFOCUS / WM_ACTIVATE：主窗口是 WS_EX_NOACTIVATE，点击本就
// 不会激活它（刻意不抢当前应用焦点），系统不会发这两个消息，「外部点击」
// 只能自己观测。
// 用 WH_MOUSE_LL 而非钩子里拿 GetWindowRect 比较：与 SetWindowPos / 显示器
// 切换有读旧矩形的时序竞争，且纯矩形判断会把子窗口、弹出菜单误判成外部。
// 改用 WindowFromPoint + GetAncestor(GA_ROOT) 按窗口归属判断，子窗口、
// 菜单都会归到主窗口，不受几何时序影响。
// 线程模型：回调在注册钩子的线程（主消息循环）执行，可直接 PostMessage
// 给窗口；回调只投递自定义消息，不做 hide() 等重活。钩子回调有系统超时
//（LowLevelHooksTimeout），超时会被静默摘除，必须极快返回。

#include <windows.h>

namespace ktile {
namespace platform {
namespace win32 {

// 自定义消息：检测到窗口外点击。wParam/lParam 未用。
inline constexpr UINT kOutsideClickMessage = WM_APP + 0x70U;

class OutsideClickMonitor {
public:
    OutsideClickMonitor() = default;
    ~OutsideClickMonitor();

    OutsideClickMonitor(const OutsideClickMonitor&) = delete;
    OutsideClickMonitor& operator=(const OutsideClickMonitor&) = delete;

    // 开始监视：target 是「本程序的主窗口」。
    // 重复调用（同一 target）是安全的空操作。
    void start(HWND target);

    // 停止监视并卸钩子。可重复调用。
    void stop() noexcept;

    bool active() const noexcept { return hook_ != nullptr; }

private:
    static LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam);

    // 单活动实例。启动器是单实例程序，且钩子本身是全局的，
    // 用文件级静态指针把回调转发到实例，避免在 lParam 里塞 this
    // （WH_MOUSE_LL 的 lParam 是 MSLLHOOKSTRUCT，不能挪用）。
    static OutsideClickMonitor* active_;

    HHOOK hook_ = nullptr;
    HWND target_ = nullptr;
};

}  // namespace win32
}  // namespace platform
}  // namespace ktile
