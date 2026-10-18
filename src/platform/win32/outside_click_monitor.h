#pragma once

// 窗口外点击监视器。
//
// 用途：启动器是「弹出式」面板 —— 点击面板以外的任何地方都应当收起它，
// 除非用户把窗口「钉住」（钉住时点击外部不收起）。
//
// ================================================================
// 为什么不用 WM_KILLFOCUS / WM_ACTIVATE
// ================================================================
//
// 主窗口是 WS_EX_NOACTIVATE：鼠标点击本就不会激活它，系统也就不会因为
// 用户点了别处而发 WM_KILLFOCUS / WM_ACTIVATE。这是刻意的 —— 启动器不该
// 抢走当前应用的焦点（否则启动完一个程序，焦点会被切走）。
//
// 所以「外部点击」必须自己观测。
//
// ================================================================
// 为什么用 WH_MOUSE_LL 而不是窗口矩形比较
// ================================================================
//
// 曾经的错误做法：钩子里拿 GetWindowRect 与点击坐标比较。
// 问题有二：
//   1. 与 SetWindowPos / 显示器切换存在时序竞争，可能读到旧矩形
//   2. 判断「是否属于本程序」只能靠矩形，一旦有子窗口 / 弹出菜单就误判
//
// 现在改为 WindowFromPoint + GetAncestor 判定「被点的窗口是不是本窗口」：
// 这是按窗口归属判断，不受矩形时序影响，也不必维护任何几何状态。
// 属于本程序的子窗口、弹出菜单、提示条都会经 GetAncestor(GA_ROOT) 归到
// 主窗口，因此点菜单不会被误判为「外部点击」。
//
// ================================================================
// 线程模型
// ================================================================
//
// WH_MOUSE_LL 是全局钩子，回调在「注册它的线程」上执行（不注入其它进程），
// 也就是本程序的消息循环线程 —— 与窗口过程同线程，因此回调里可以直接
// PostMessage 给窗口，不必加锁、不必跨线程访问。
//
// 回调里一律不调用 hide() 等重活，只 PostMessage 一个自定义消息，
// 让窗口在主循环里从容处理。钩子回调有系统超时限制（LowLevelHooksTimeout），
// 超时会被静默摘除，所以必须极快返回。

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

    // 单活动实例。启动器是单实例程序，且钩子本身是全局的 ——
    // 用文件级静态指针把回调转发到实例，避免在 lParam 里塞 this
    // （WH_MOUSE_LL 的 lParam 是 MSLLHOOKSTRUCT，不能挪用）。
    static OutsideClickMonitor* active_;

    HHOOK hook_ = nullptr;
    HWND target_ = nullptr;
};

}  // namespace win32
}  // namespace platform
}  // namespace ktile
