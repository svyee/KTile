// KTile - Win32 平台层：极简 SVG 形状片段 -> Direct2D 几何
//
// 背景：Windows 没有系统级 SVG 栅格化器（WIC 不支持 SVG），引入
// librsvg/resvg 会让「单 exe 1~2MB」的硬指标失控。但图标需要的只是
// 形状本身 —— Lucide 的每个图标都只是几十个字符的路径数据，把它编译
// 进 exe、交给 D2D 直接描边，反而比「打包 PNG」更清晰（任意 DPI 都不糊）
// 且更省体积。
//
// 支持范围刻意只覆盖图标用到的子集：
//   元素：path / rect / circle / ellipse / line / polyline / polygon
//   路径命令：M m L l H h V v C c S s Q q T t A a Z z
//   属性：元素上的 transform="translate(tx ty) scale(sx sy)"（仅这两种）
//   填充：fill="currentColor" 的图元进「填充几何」，其余进「描边几何」
//
// 不支持 CSS、渐变、clipPath、use、路径动画 —— 图标用不到。
#pragma once

#include "platform/win32/unique_handles.h"

#include <d2d1.h>

namespace ktile {
namespace platform {
namespace win32 {

// 一段 SVG 片段解析出的两条几何：
//   strokeGeometry —— 线框（Lucide 的常态，用圆头圆角描边画）
//   fillGeometry   —— 实心图元（Lucide 里只有个别小圆点标了 currentColor）
struct SvgGlyph {
    ComPtr<ID2D1PathGeometry> strokeGeometry;
    ComPtr<ID2D1PathGeometry> fillGeometry;
};

// 把 SVG 形状片段转成 D2D 几何。
//
// 坐标映射：p' = p * scale + (dx, dy)，scale 为均匀缩放（描边宽度由调用方
// 按 scale 自行换算）。返回 false 表示解析失败或 D2D 创建失败 —— 调用方
// 应当退回「无图标」而不是崩溃。
//
// forceFilled：把所有图元都放进 fillGeometry（FILLED 起始）。用于「同一个
// 形状既要描边又要实心」的场景（如钉子的钉住态）—— 注意 D2D 的
// FillGeometry 会跳过 HOLLOW 起始的图元，所以实心版必须单独建几何。
bool buildSvgGlyph(ID2D1Factory* factory, const wchar_t* markup,
                   float scale, float dx, float dy, SvgGlyph* out,
                   bool forceFilled = false);

// Lucide 的线条风格：圆头（round cap）+ 圆角（round join）。
// D2D 默认是平头 + 尖角，直接画会显得生硬，和 Lucide 原版差别明显。
ComPtr<ID2D1StrokeStyle> createRoundStrokeStyle(ID2D1Factory* factory);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
