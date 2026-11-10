// KTile - UI 层：轮盘菜单编辑器
//
// 职责：编辑 core::RadialConfig —— 轮盘方案（每个扇区挂什么动作）与
// 「按应用匹配」（哪个进程用哪个方案）。
//
// 为什么单独一个对话框，而不是塞进设置窗口：
//   设置窗口的表单是「一行一个开关」的窄单列（内容区 542px），而这里要同时
//   摆方案列表、扇区列表和选中扇区的动作表单。挤进去只会两边都看不清。
//   沿用 item_editor_dialog / builtin_item_dialog 的做法：设置页只留一个
//   「编辑扇区动作…」入口。
//
// 数据流：进来先把 config 复制成草稿（draft），所有编辑都改草稿；点确定才
//   写回 config 指向的对象。点取消 = 直接丢弃，调用方不需要回滚任何东西。
//
// 不变量（决定「配置永远合法」的两条，改代码时别破坏）：
//   1. 每个方案的 actions.size() 恒等于 sectorCount —— 增删扇区时同步
//      resize，否则 validateRadialConfig 会判非法，轮盘算不准扇区；
//   2. 每个 profile.menuId 必须指向存在的方案 —— 删除方案时连带清掉
//      引用它的 profile（菜单都没了，「按应用」指向空气只会静默失效）。
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
