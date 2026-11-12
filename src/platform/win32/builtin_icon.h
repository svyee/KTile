// KTile - Win32 平台层：内置项目图标
//
// D2D 现场渲染 128x128 PNG：Lucide 白线框（core/lucide_icons.h）+ 统一
// 蓝色圆角底。系统位置项优先用真实 shell 图标，取不到退回 Lucide；不用
// 系统图标做统一来源，原因：样式随系统漂移、裸 exe 名常取不到图、颜色
// 不可控。产物走 icon_store 的 PNG 管线（res\builtin-<id>.png），重复
// 添加复用同一份文件。
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
