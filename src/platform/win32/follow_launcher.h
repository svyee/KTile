// KTile - Win32 平台层：跟随启动的「已在运行」判定
//
// KTile 启动时顺手拉起常用程序，但要先判断它是否已在运行，否则托盘里
// 会出现两个实例。判定：枚举进程，先按完整映像路径比对（不区分大小写），
// 对不上再按文件名兜底。文件名兜底的必要性：商店应用和应用执行别名
//（notepad.exe / calc.exe）启动后映像路径在 WindowsApps 里，按用户填的
// 路径永远比不中，会导致每次启动都多开一个。代价是同名不同路径被判成
// 已运行，可接受：漏启动用户点一下磁贴就补回来，多开则留下重复窗口。
// 只对 .exe / .com / .lnk 有意义，.lnk 先解析真实目标；文档/文件夹/脚本
// 没有可靠的「已打开」判据，一律返回 false（宁可多开不要漏开）。
//
#pragma once

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

// 目标程序是否已经在运行。路径允许是相对 exe 目录的相对写法。
bool isProgramRunning(const std::wstring& targetPath);

// 解析 .lnk 指向的真实可执行文件（失败返回空串）。供上面的判定使用，
// 调用方一般不需要直接用。
std::wstring resolveShortcutTarget(const std::wstring& lnkPath);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
