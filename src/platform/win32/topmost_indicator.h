// KTile - Win32 平台层：置顶窗口的视觉标志
//
// 快捷键把窗口钉到最前后给它一个看得见的记号：目标外围一圈橙色描边。
// 用自绘的分层窗口而不是 DWM 的窗口描边属性——后者在窗口失去激活后会被
// 系统的非激活边框覆盖（Win11 实测橙框随点击别的窗口消失），且老系统
// 没有这个属性。自绘描边与激活状态无关，Win7 起表现一致。
//
#pragma once

#include <Windows.h>

namespace ktile {
namespace platform {
namespace win32 {

// 给目标窗口加上 / 撤掉置顶标志。目标是销毁中的窗口也安全。
void setTopmostMark(HWND target, bool marked);

// 撤掉全部标志。程序退出前调用，别把描边留在用户屏幕上。
void clearAllTopmostMarks();

}  // namespace win32
}  // namespace platform
}  // namespace ktile
