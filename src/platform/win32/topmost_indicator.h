// KTile - Win32 平台层：置顶窗口的视觉标志
//
// 快捷键把窗口钉到最前后给它一个看得见的记号。优先用 Win11 的
// DWMWA_BORDER_COLOR（系统原生描边，跟随圆角）；老系统退化为自绘描边
// 悬浮框（分层窗口 + 定时校验目标状态，目标没了就自动撤）。
//
#pragma once

#include <Windows.h>

namespace ktile {
namespace platform {
namespace win32 {

// 给目标窗口加上 / 撤掉置顶标志。目标是销毁中的窗口也安全。
void setTopmostMark(HWND target, bool marked);

// 撤掉全部标志。程序退出前调用：DWM 描边写在别的进程的窗口上，
// 不撤会一直留在用户窗口上。
void clearAllTopmostMarks();

}  // namespace win32
}  // namespace platform
}  // namespace ktile
