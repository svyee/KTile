// KTile - Win32 平台层：获取前台进程名
//
// 用途：轮盘菜单的 Per-App Profile 需要知道当前前台窗口属于哪个进程，
// 从而匹配对应的轮盘方案。
//
// Win7 兼容：
//   - GetForegroundWindow / GetWindowThreadProcessId：XP+
//   - OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)：Vista+
//   - QueryFullProcessImageNameW：Vista+
// 全部在 Win7 SP1 可用范围内。
//
#pragma once

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

// 获取前台窗口所属进程的可执行文件名（小写，含扩展名，如 "chrome.exe"）。
// 失败（无前台窗口 / 无法打开进程）返回空串。
std::wstring foregroundProcessName();

}  // namespace win32
}  // namespace platform
}  // namespace ktile
