// KTile - Win32 平台层：键盘快捷键注入
//
// 供轮盘菜单的 Shortcut 动作向前台窗口注入组合键（Ctrl+C 等）。轮盘窗口
// 是 WS_EX_NOACTIVATE，前台窗口保持焦点，注入的按键会被它正常接收。
// 键名解析复用 global_hotkey 的 keyNameToVirtualKey，与热键配置同套命名。
//
#pragma once

#include "core/data_model.h"   // HotkeyModifier

#include <string>
#include <vector>

namespace ktile {
namespace platform {
namespace win32 {

// 注入一组修饰键 + 主键（按下 → 抬起）。
// modifiers 为空时只注入主键。
void injectShortcut(const std::vector<core::HotkeyModifier>& modifiers,
                    const std::string& key);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
