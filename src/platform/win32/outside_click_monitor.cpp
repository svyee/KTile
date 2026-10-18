#include "platform/win32/outside_click_monitor.h"

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <cwchar>  // wcscmp

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 钩子装上后的静默期（毫秒）。
//
// 为什么需要：钩子在「用户点击热键/托盘唤出窗口」之后安装，而那次唤出
// 本身可能还带着尚未派发的鼠标消息（尤其用鼠标点托盘图标时）。若不设
// 静默期，这些滞留消息会被当成「外部点击」，窗口刚显示就立刻消失。
constexpr DWORD kStartupGraceMs = 200;

DWORD g_startedAt = 0;

// 被点的窗口是否属于本程序（含子窗口、弹出菜单、提示条）。
//
// 这是「点击窗口外自动隐藏」的核心判定，必须覆盖三类情况：
//
//   1. 本窗口自身及其子窗口 —— GetAncestor(GA_ROOT) 即可归到主窗口
//   2. TrackPopupMenu 建立的菜单窗口 —— **GA_ROOT 与 owner 链都查不到主窗口**，
//      它是独立的顶层窗口（类名固定为 "#32768"，即 Win32 的标准菜单类），
//      只能靠类名识别。实测数据：
//          hwnd class='#32768'  GA_ROOT=自己  owner链=自己
//      若不特判，用户点自己的右键菜单时窗口会被当成「点了外部」而隐藏 ——
//      这正是「点右键菜单主窗口就关了」的根因。
//   3. 菜单常见的阴影窗口 "SysShadow" —— 同样不属于主窗口，但它是菜单的
//      附属物，点它也不该关窗。
//
// 判定顺序：先看是不是自家窗口（最可靠），再看是不是菜单相关窗口。
bool belongsToTarget(HWND candidate, HWND target) noexcept {
    if (candidate == nullptr || target == nullptr) {
        return false;
    }

    // 自家窗口：GA_ROOT 归到主窗口即可（覆盖主窗口与全部子窗口）
    if (GetAncestor(candidate, GA_ROOT) == target) {
        return true;
    }

    // 沿 owner 链向上找（处理 owner 关系而非父子关系的弹出窗口）。
    //
    // 起点必须是**被点窗口的顶层窗口**，不能直接用 WindowFromPoint 的
    // 结果 —— 它给的是最深的那个子控件（对话框上的按钮、输入框……），
    // 而 owner 关系只挂在顶层窗口上：对一个按钮取 GW_OWNER 永远是
    // nullptr，owner 链一步就走完了。于是「点自己对话框里的确定按钮」
    // 被判成点了窗口外面，面板当场收起（与上面注释承诺的行为相反）。
    const HWND root = GetAncestor(candidate, GA_ROOT);
    for (HWND current = (root != nullptr ? root : candidate);
         current != nullptr; current = GetWindow(current, GW_OWNER)) {
        if (current == target) {
            return true;
        }
    }

    // 菜单相关窗口：Win32 菜单类 "#32768" 及其阴影 "SysShadow"。
    // 它们由本进程的 TrackPopupMenu 创建，但既不是主窗口的子窗口，
    // 也不在主窗口的 owner 链上。
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
    // 用消息自身的时间戳而不是 GetTickCount()，判断才准确 ——
    // 钩子回调可能滞后于消息产生时间。
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

    // 按窗口归属判定，而不是矩形比较 —— 不受窗口移动的时序影响
    const POINT point{info->pt.x, info->pt.y};
    const HWND hit = WindowFromPoint(point);
    if (belongsToTarget(hit, active_->target_)) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // 命中外部：交给窗口处理。这里只投递消息，绝不做耗时操作 ——
    // 钩子回调超时会被系统静默摘除。
    PostMessageW(active_->target_, kOutsideClickMessage, 0, 0);
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
