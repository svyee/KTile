// KTile - UI 层：磁贴背景色选择
//
// 内置 8×6 = 48 色弹板（取色与 Windows「颜色」对话框基础色表同款），
// 不调用系统 ChooseColorW：系统对话框全屏居中，选完点「确定」之前
// 看不到磁贴实际效果；弹板贴在色块下方，点一格立即回调刷新且不关闭，
// 方便连续试色。设置窗口与「修改项目」对话框共用这一入口。
#pragma once

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <optional>

namespace ktile {
namespace ui {

// 打开内置调色板（调用期间阻塞，关闭后返回）。
//
// owner       ：宿主对话框，打开期间禁用，防止误操作底下的控件。
// anchor      ：定位基准控件，弹板贴在它下方；为空则出现在鼠标位置。
// currentArgb ：当前颜色，命中色表时画勾，便于确认「现在是哪一格」。
// onPick      ：每点选一格立即回调（主界面 / 预览的实时刷新）。
//               点选不关闭弹板；点别处、Esc、失去焦点时关闭。
//
// 颜色一律返回不透明 ARGB（0xFFRRGGBB），色表本身不提供 alpha。
void showColorPalette(HWND owner, HWND anchor, std::uint32_t currentArgb,
                      const std::function<void(std::uint32_t)>& onPick);

// 画一块颜色样本（SS_OWNERDRAW 的 STATIC 用）。
// rect 是控件 DC 内的矩形；空颜色画成「跟随皮肤 / 未指定」：
// 底色与对话框一致，加一条斜线与灰框。
void paintColorSwatch(HDC dc, const RECT& rect,
                      const std::optional<std::uint32_t>& argb);

}  // namespace ui
}  // namespace ktile
