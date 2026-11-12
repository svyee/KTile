// KTile - Win32 平台层：极简 SVG 形状片段 -> Direct2D 几何
//
// Windows 没有系统级 SVG 栅格化器，而图标只需形状本身：解析 Lucide 子集
// 后交给 D2D 描边，任意 DPI 清晰且省体积。支持 path/rect/circle/ellipse/
// line/polyline/polygon 与 M..Z 全部路径命令，transform 仅 translate/scale；
// 不支持 CSS、渐变、clipPath（图标用不到）。
#pragma once

#include "platform/win32/unique_handles.h"

#include <d2d1.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ktile {
namespace platform {
namespace win32 {

// 一段 SVG 片段解析出的几何：strokeGeometry 为调用方颜色的线框，
// fillGeometry 为实心图元，colorLayers 为显式 stroke="#RRGGBB" 的图元
// （按颜色分组）。单独建图层是因为 D2D 几何本身不带颜色，绘制时按层
// 换画笔即可，仍是矢量描边。
struct SvgColorLayer {
    ComPtr<ID2D1PathGeometry> geometry;
    // 0xAARRGGBB（解析时统一补成不透明）
    std::uint32_t color = 0;
};

struct SvgGlyph {
    ComPtr<ID2D1PathGeometry> strokeGeometry;
    ComPtr<ID2D1PathGeometry> fillGeometry;
    std::vector<SvgColorLayer> colorLayers;
    // 所有几何（描边 + 填充 + 彩色层）合并后的包围盒（已包含 scale/dx/dy）。
    D2D1_RECT_F bounds = D2D1::RectF(0.0F, 0.0F, 0.0F, 0.0F);
    // 弧长加权质心（描边图元的光学中心）。优先用它居中，其次退回 bounds。
    D2D1_POINT_2F centroid = D2D1::Point2F(0.0F, 0.0F);
    bool hasCentroid = false;
};

// 把 SVG 形状片段转成 D2D 几何。坐标映射 p' = p * scale + (dx, dy)，
// scale 为均匀缩放（描边宽度由调用方自行换算）。返回 false 表示解析或
// D2D 创建失败，调用方应退回「无图标」而不是崩溃。forceFilled 把所有
// 图元放进 fillGeometry（FILLED 起始）：D2D 的 FillGeometry 会跳过
// HOLLOW 起始的图元，实心版必须单独建几何。
bool buildSvgGlyph(ID2D1Factory* factory, const wchar_t* markup,
                   float scale, float dx, float dy, SvgGlyph* out,
                   bool forceFilled = false);

// Lucide 的线条风格：圆头 + 圆角。D2D 默认平头尖角，与 Lucide 差别明显。
ComPtr<ID2D1StrokeStyle> createRoundStrokeStyle(ID2D1Factory* factory);

// 读 .svg 文件的标记文本（UTF-8，自动去 BOM），失败返回 false。
// 放在平台层供磁贴与轮盘插件图标共用，避免各自实现时漏掉去 BOM。
// 纯文件读取，解析交给 buildSvgGlyph。
bool readSvgMarkupFile(const std::wstring& path, std::wstring* markup);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
