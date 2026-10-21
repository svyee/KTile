// KTile - UI 层：磁贴背景色选择
//
// 内置一块 8×6 = 48 色的调色板（与 Windows「颜色」对话框的基础色表
// 同款取色，见用户提供的截图），不再调用系统 ChooseColorW。
//
// 为什么改用内置弹板：
//   - 系统对话框是全屏居中的大窗口，打开时把主界面挡得严严实实，
//     选完点「确定」之前看不到磁贴实际效果；
//   - 内置弹板贴在色块下方，点一格立刻回调上层刷新
//     主窗口 / 预览，不关闭，方便连续试色比较；
//   - 只有 48 个色块，不需要颜色空间、取色器这些重活。
// 设置窗口与「修改项目」对话框共用这一入口。
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
// anchor      ：定位基准控件（就是那个可点击的色块），弹板贴在它下方；
//               为空则出现在鼠标位置。
// currentArgb ：当前颜色，命中色表时画勾，方便用户确认「现在是哪一格」。
// onPick      ：每点选一格立即回调（用于主界面 / 预览的实时刷新）。
//               点选不会关闭弹板；点击别处、Esc、失去焦点时关闭。
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
