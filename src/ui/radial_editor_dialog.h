// KTile - UI 层：轮盘菜单编辑器
//
// 编辑 core::RadialConfig：轮盘方案（每个扇区挂什么动作）与「按应用匹配」
// （哪个进程用哪个方案）。单独成对话框而非塞进设置窗口：设置窗口是窄
// 单列表单，摆不下方案列表、扇区列表和动作表单；设置页只留「编辑扇区
// 动作…」入口（同 item_editor_dialog / builtin_item_dialog 的做法）。
//
// 数据流：进来先把 config 复制成草稿，所有编辑都改草稿；点确定才写回
// config，点取消直接丢弃，调用方无需回滚。
//
// 不变量（改代码时别破坏）：
//   1. 每个方案的 actions.size() 恒等于 sectorCount，增删扇区时同步
//      resize，否则 validateRadialConfig 判非法，轮盘算不准扇区；
//   2. 每个 profile.menuId 必须指向存在的方案，删除方案时连带清掉
//      引用它的 profile，否则「按应用」静默失效。
//
#pragma once

#include "core/radial_menu.h"

#include <Windows.h>

namespace ktile {
namespace ui {

// 打开轮盘编辑器（模态）。用户点「确定」返回 true 并把草稿写回 config；
// 点「取消」/ 关闭返回 false，config 保持不变。
bool showRadialEditorWindow(HWND owner, core::RadialConfig* config);

}  // namespace ui
}  // namespace ktile
