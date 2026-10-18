// KTile - Win32 平台层：内置项目图标
//
// 「添加系统项目」时给磁贴用的图标，来源只有一个：内嵌的 Lucide 线框
// 图标（core/lucide_icons.h）+ 统一的蓝色圆角底，用 D2D 现场描边成
// 128x128 PNG。
//
// 为什么不用系统图标（IShellItemImageFactory 那条路已删除）：
//   1. 样式随系统漂移 —— Win7 拟物、Win11 扁平，22 个磁贴摆一起像拼盘；
//   2. 裸 exe 名（calc.exe / notepad.exe / control.exe）和部分 CLSID
//      经常取不到图，取不到整项就填不上图标；
//   3. 颜色不可控，压在自定义背景图上容易糊。
// 矢量图标一次解决三条，且任意 DPI 都清晰。
//
// 产物统一走 icon_store 的 PNG 管线（res\builtin-<id>.png）：
// 渲染层零改动，绿色可搬，重复添加复用同一份文件。
#pragma once

#include "core/builtin_items.h"

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

// 生成（或复用已生成的）内置项图标。
// item     ：内置项定义（决定形状与文件名）
// 返回存储值（形如 "res\builtin-lock.png"）；失败返回空串并写 error。
std::wstring ensureBuiltinIcon(const core::BuiltinItem& item,
                               std::wstring* error);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
