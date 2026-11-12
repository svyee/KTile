#include "platform/win32/outside_click_monitor.h"

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <cwchar>  // wcscmp

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 钩子装上后的静默期（毫秒）。钩子在唤出窗口之后安装，那次唤出可能还
// 带着未派发的鼠标消息（鼠标点托盘图标时尤其常见）；不过滤的话窗口刚
// 显示就立刻消失。
constexpr DWORD kStartupGraceMs = 200;

DWORD g_startedAt = 0;

// 被点的窗口是否属于本程序（含子窗口、弹出菜单、提示条），覆盖三类：
// 主窗口及子窗口（GA_ROOT 归到主窗口）、TrackPopupMenu 菜单窗口（类名
// "#32768"，GA_ROOT 与 owner 链都查不到主窗口，只能靠类名识别，不特判会
// 导致点自己的右键菜单就被当成外部点击而隐藏）、菜单阴影窗口 "SysShadow"
//（菜单附属物，点它也不该关窗）。判定顺序：先自家窗口，再菜单相关窗口。
bool belongsToTarget(HWND candidate, HWND target) noexcept {
    if (candidate == nullptr || target == nullptr) {
        return false;
    }

    // 自家窗口：GA_ROOT 归到主窗口即可（覆盖主窗口与全部子窗口）
    if (GetAncestor(candidate, GA_ROOT) == target) {
        return true;
    }

    // 沿 owner 链向上找（处理 owner 关系而非父子关系的弹出窗口）。起点
    // 必须是被点窗口的顶层窗口，不能直接用 WindowFromPoint 的结果：那是
    // 最深的子控件，owner 关系只挂在顶层窗口上，从子控件走 owner 链一步
    // 就到 nullptr，「点自己对话框里的按钮」会被误判成外部点击。
    const HWND root = GetAncestor(candidate, GA_ROOT);
    for (HWND current = (root != nullptr ? root : candidate);
         current != nullptr; current = GetWindow(current, GW_OWNER)) {
        if (current == target) {
            return true;
        }
    }

    // 菜单相关窗口：Win32 菜单类 "#32768" 及其阴影 "SysShadow"。由本进程
    // 的 TrackPopupMenu 创建，但既不是主窗口的子窗口，也不在 owner 链上。
    wchar_t className[32] = {};
    if (GetClassNameW(candidate, className, 32) > 0) {
        if (wcscmp(className, L"#32768") == 0) {
            return true;
        }
        if (wcscmp(className, L"SysShadow") == 0) {
            return true;
        }
    }

    return false;
}

}  // namespace

OutsideClickMonitor* OutsideClickMonitor::active_ = nullptr;

OutsideClickMonitor::~OutsideClickMonitor() {
    stop();
}

void OutsideClickMonitor::start(const HWND target) {
    if (target == nullptr) {
        return;
    }
    if (hook_ != nullptr && target_ == target) {
        return;  // 已在监视同一窗口
    }
    stop();

    target_ = target;
    g_startedAt = GetTickCount();

    // SetWindowsHookExW 的 hMod 传 nullptr 对 WH_MOUSE_LL 是合法的：
    // 低级鼠标钩子不注入进程，回调由系统在本进程线程上调用。
    hook_ = SetWindowsHookExW(WH_MOUSE_LL, &OutsideClickMonitor::mouseProc,
                              nullptr, 0);
    if (hook_ != nullptr) {
        active_ = this;
    } else {
        target_ = nullptr;
    }
}

void OutsideClickMonitor::stop() noexcept {
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (active_ == this) {
        active_ = nullptr;
    }
    target_ = nullptr;
}

LRESULT CALLBACK OutsideClickMonitor::mouseProc(const int code,
                                               const WPARAM wParam,
                                               const LPARAM lParam) {
    // code < 0 表示必须直接转发，不得做任何处理（文档要求）
    if (code < 0 || active_ == nullptr || active_->target_ == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // 只关心「按下」：抬起时窗口可能已经受理过一次隐藏，重复处理无意义。
    //
    // 用消息自身的时间戳而不是 GetTickCount()：钩子回调可能滞后于消息
    // 产生时间，用消息时间戳判断才准确。
    const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    const bool isButtonDown =
        wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
        wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN;
    if (!isButtonDown) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // 静默期：过滤掉「唤出窗口」那次点击的滞留消息
    if (static_cast<DWORD>(info->time - g_startedAt) < kStartupGraceMs) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // 按窗口归属判定，而不是矩形比较，不受窗口移动的时序影响
    const POINT point{info->pt.x, info->pt.y};
    const HWND hit = WindowFromPoint(point);
    if (belongsToTarget(hit, active_->target_)) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // 命中外部：交给窗口处理。这里只投递消息，绝不做耗时操作，
    // 钩子回调超时会被系统静默摘除。
    PostMessageW(active_->target_, kOutsideClickMessage, 0, 0);
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
