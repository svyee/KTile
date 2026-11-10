// KTile - Win32 平台层：键盘快捷键注入
//
// 用途：轮盘菜单的 Shortcut 类型动作需要向当前前台窗口注入组合键
// （如 Ctrl+C 复制、Ctrl+V 粘贴）。因为轮盘窗口是 WS_EX_NOACTIVATE，
// 前台窗口始终保持焦点，注入的按键会被它正常接收。
//
// 复用 global_hotkey 的 keyNameToVirtualKey 做键名解析，保持与热键配置
// 一致的命名（"C", "F4", "Space" 等）。
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
