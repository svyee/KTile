// KTile - Win32 平台层：跟随启动的「已在运行」判定
//
// 「跟随启动」要解决的问题很朴素：KTile 起来时顺手把几个常用程序拉起来，
// 但开机那一会儿这些程序常常已经被系统 / 用户自己启动过一遍了 —— 再拉
// 一次就是两个实例，托盘里出现两条一模一样的图标。所以启动之前先问一句
// 「它是不是已经在跑了」。
//
// 判定方式：枚举进程，先按**完整映像路径**比对（不区分大小写），
// 对不上再按**文件名**兜底。
//
// 为什么需要文件名兜底：商店应用和 System32 下的「应用执行别名」
// （notepad.exe / calc.exe 这类）启动之后，真实进程的映像路径在
// WindowsApps 里，拿用户填的路径去比永远比不中 —— 结果就是每次启动都
// 多开一个。代价是「同名不同路径」会被当成已运行（便携版同名程序），
// 这个方向可以接受：漏启动一次用户点一下磁贴就补回来，而多开一路实例
// 会留下重复窗口、甚至两份托盘图标抢同一份配置。
//
// 只对 .exe / .com / .lnk 有意义：
//   - .lnk 先解析出真实目标再比；
//   - 文档 / 文件夹 / .bat / .cmd 没有可靠的「是否已打开」判据，
//     一律返回 false（照常启动，宁可多开一次也不要该开的不开）。
//
#pragma once

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

// 目标程序是否已经在运行。路径允许是相对 exe 目录的相对写法。
bool isProgramRunning(const std::wstring& targetPath);

// 解析 .lnk 指向的真实可执行文件（失败返回空串）。
//
// 单独暴露出来是给上面那个判定用的；调用方一般不需要直接用。
std::wstring resolveShortcutTarget(const std::wstring& lnkPath);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
