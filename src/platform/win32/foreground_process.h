// KTile - Win32 平台层：轮盘目标窗口解析
//
// 供轮盘菜单按窗口归属匹配方案使用。核心是 radialTargetWindow()：光标点
// 命中的窗口优先，回退前台窗口。所用 API（GetCursorPos / WindowFromPoint /
// GetForegroundWindow / OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) /
// QueryFullProcessImageNameW）均在 Win7 SP1 可用范围内。
//
#pragma once

#include <Windows.h>

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

// 获取前台窗口所属进程的可执行文件名（小写，含扩展名，如 "chrome.exe"）。
// 失败（无前台窗口 / 无法打开进程）返回空串。
std::wstring foregroundProcessName();

// 获取前台窗口的窗口类名（小写）。与进程名配合使用：explorer.exe 的
// 文件窗口类是 CabinetWClass，桌面是 Progman / WorkerW，轮盘插件靠它
// 区分同进程的两类界面。取不到返回空串。
std::wstring foregroundWindowClass();

// 轮盘的目标窗口快照：进程名 + 窗口类名（均小写，可都为空）。
// window 保留原始句柄：explorer 多窗口时，查「选中了哪些文件」必须定位
// 到光标下的那一个，只有进程名和类名区分不出来。
struct WindowTarget {
    std::wstring processName;
    std::wstring windowClass;
    HWND window = nullptr;
};

// 按「鼠标所在位置」解析轮盘的目标窗口：光标点命中的根窗口优先，
// 不可见 / 最小化 / 属于本进程（自己的覆盖层）时回退前台窗口。
// 悬停不改变前台，只看 GetForegroundWindow 会把轮盘配给错误的窗口，
// 双窗口场景（此电脑 + 浏览器）尤其明显。
WindowTarget radialTargetWindow();

}  // namespace win32
}  // namespace platform
}  // namespace ktile
