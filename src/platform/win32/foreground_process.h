// KTile - Win32 平台层：获取前台进程名
//
// 供轮盘菜单按前台进程匹配方案使用。所用 API（GetForegroundWindow /
// OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) / QueryFullProcessImageNameW）
// 均在 Win7 SP1 可用范围内。
//
#pragma once

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

}  // namespace win32
}  // namespace platform
}  // namespace ktile
