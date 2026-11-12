// KTile - 对话框通用视觉样式
//
// 只做「低成本现代化」：统一的浅色中性底 + 白色圆角卡片 + 细边框 +
// 一枚强调色主按钮。控件仍用原生 Win32：全部自绘需重做焦点链、键盘
// 操作与 IME，收益不抵风险。
//
// 取舍：
// - 颜色写死而不是取 COLOR_BTNFACE：系统灰对比度低，卡片层次做不出来；
//   但文字仍取 COLOR_WINDOWTEXT、强调色取 COLOR_HIGHLIGHT，高对比度
//   主题下文字与主按钮依然可读。
// - 画刷缓存在静态对象里：WM_CTLCOLOR* 返回的画刷由窗口系统复用，
//   不能每次 CreateSolidBrush（会泄漏），也不该由调用方 DeleteObject。
#pragma once

#include <windows.h>

namespace ktile {
namespace ui {
namespace dialog_style {

// --- 配色 ---
COLORREF background();     // 对话框底色（浅灰）
COLORREF card();           // 卡片底色（白）
COLORREF border();         // 卡片 / 次级按钮描边
COLORREF text();           // 正文
COLORREF secondaryText();  // 说明文字
COLORREF accent();         // 强调色（主按钮 / 选中标签）
COLORREF accentText();     // 强调色上的文字

// 缓存画刷（不要 DeleteObject）
HBRUSH backgroundBrush();
HBRUSH cardBrush();

enum class ButtonKind {
    Primary,    // 强调色填充（确定 / 保存）
    Secondary,  // 白底细边（取消 / 浏览 / 删除）
};

// 画对话框底色（WM_ERASEBKGND）
void paintBackground(HDC dc, const RECT& client);

// 画一张圆角卡片
void paintCard(HDC dc, const RECT& rect, int radius);

// 画自绘按钮。pressed / disabled 来自 DRAWITEMSTRUCT 的状态位；
// focused 为真时补一个虚焦点框（owner-draw 按钮默认没有）。
void paintButton(HDC dc, const RECT& rect, const wchar_t* label, HFONT font,
                 ButtonKind kind, bool pressed, bool disabled, bool focused);

}  // namespace dialog_style
}  // namespace ui
}  // namespace ktile
