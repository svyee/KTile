#include "ui/dialog_style.h"

#include <algorithm>

namespace ktile {
namespace ui {
namespace dialog_style {
namespace {

// 中性浅灰底 + 白卡片。数值刻意接近主流「现代设置页」：
// 底色略带蓝灰（不是系统 COLOR_BTNFACE 的黄灰），白卡片才显得干净。
constexpr COLORREF kBackground = RGB(243, 245, 248);
constexpr COLORREF kCard = RGB(255, 255, 255);
constexpr COLORREF kBorder = RGB(224, 227, 233);
constexpr COLORREF kSecondaryText = RGB(110, 117, 128);

COLORREF blend(const COLORREF base, const COLORREF over, const float t) {
    const float clamped = t < 0.0F ? 0.0F : (t > 1.0F ? 1.0F : t);
    auto mix = [clamped](const int a, const int b) {
        return static_cast<int>(
            static_cast<float>(a) * (1.0F - clamped) +
            static_cast<float>(b) * clamped + 0.5F);
    };
    return RGB(mix(GetRValue(base), GetRValue(over)),
               mix(GetGValue(base), GetGValue(over)),
               mix(GetBValue(base), GetBValue(over)));
}

}  // namespace

COLORREF background() { return kBackground; }
COLORREF card() { return kCard; }
COLORREF border() { return kBorder; }
COLORREF text() { return GetSysColor(COLOR_WINDOWTEXT); }
COLORREF secondaryText() { return kSecondaryText; }
COLORREF accent() { return GetSysColor(COLOR_HIGHLIGHT); }
COLORREF accentText() { return GetSysColor(COLOR_HIGHLIGHTTEXT); }

HBRUSH backgroundBrush() {
    static HBRUSH brush = CreateSolidBrush(kBackground);
    return brush;
}

HBRUSH cardBrush() {
    static HBRUSH brush = CreateSolidBrush(kCard);
    return brush;
}

void paintBackground(HDC dc, const RECT& client) {
    if (dc == nullptr) {
        return;
    }
    FillRect(dc, &client, backgroundBrush());
}

void paintCard(HDC dc, const RECT& rect, const int radius) {
    if (dc == nullptr || rect.right <= rect.left ||
        rect.bottom <= rect.top) {
        return;
    }
    HBRUSH brush = cardBrush();
    HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
    const HGDIOBJ oldBrush = SelectObject(dc, brush);
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2,
              radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

void paintButton(HDC dc, const RECT& rect, const wchar_t* label, HFONT font,
                 const ButtonKind kind, const bool pressed,
                 const bool disabled, const bool focused) {
    if (dc == nullptr || rect.right <= rect.left ||
        rect.bottom <= rect.top) {
        return;
    }

    const COLORREF accentColor = accent();
    COLORREF fill = kind == ButtonKind::Primary ? accentColor : kCard;
    COLORREF edge = kind == ButtonKind::Primary ? accentColor : kBorder;
    COLORREF labelColor =
        kind == ButtonKind::Primary ? accentText() : text();

    if (disabled) {
        // 禁用态：主按钮退成淡强调色，次按钮保持白底灰字。
        fill = kind == ButtonKind::Primary
                   ? blend(accentColor, kCard, 0.55F)
                   : kCard;
        edge = kind == ButtonKind::Primary ? fill : kBorder;
        labelColor = kind == ButtonKind::Primary
                         ? blend(accentText(), fill, 0.25F)
                         : kSecondaryText;
    } else if (pressed) {
        fill = kind == ButtonKind::Primary
                   ? blend(accentColor, RGB(0, 0, 0), 0.14F)
                   : blend(kCard, accentColor, 0.10F);
        edge = kind == ButtonKind::Primary ? fill : accentColor;
    }

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    const HGDIOBJ oldBrush = SelectObject(dc, brush);
    const HGDIOBJ oldPen = SelectObject(dc, pen);

    // 轻微圆角即可：参考界面的按钮是 6~8px 圆角，不是全圆胶囊。
    const int height = static_cast<int>(rect.bottom - rect.top);
    const int radius = std::max(2, std::min(6, height / 4));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2,
              radius * 2);

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    if (label != nullptr) {
        const int oldMode = SetBkMode(dc, TRANSPARENT);
        const COLORREF oldColor = SetTextColor(dc, labelColor);
        HGDIOBJ oldFont = nullptr;
        if (font != nullptr) {
            oldFont = SelectObject(dc, font);
        }
        RECT textRect = rect;
        DrawTextW(dc, label, -1, &textRect,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        if (oldFont != nullptr) {
            SelectObject(dc, oldFont);
        }
        SetTextColor(dc, oldColor);
        SetBkMode(dc, oldMode);
    }

    if (focused && !disabled) {
        RECT focusRect = rect;
        InflateRect(&focusRect, -3, -3);
        DrawFocusRect(dc, &focusRect);
    }
}

}  // namespace dialog_style
}  // namespace ui
}  // namespace ktile
