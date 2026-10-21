#include "ui/color_picker.h"

#include <windowsx.h>

#include <algorithm>

namespace ktile {
namespace ui {
namespace {

constexpr int kColumns = 8;
constexpr int kRows = 6;
constexpr int kColorCount = kColumns * kRows;

// 8×6 基础色表，取值自 Windows「颜色」对话框（用户提供的截图）：
// 按色谱列排布 —— 暖色 → 红粉紫 → 蓝 → 青绿 → 灰绿 → 灰/橄榄。
// 换成内置弹板后保持同样的取色顺序，用户不必重新适应。
const std::uint32_t kPaletteColors[kColorCount] = {
    0xFFFFB900U, 0xFFFF8C00U, 0xFFF7630CU, 0xFFCA5010U,
    0xFFDA3B01U, 0xFFEF6950U, 0xFFD13438U, 0xFFFF4343U,

    0xFFE74856U, 0xFFE81123U, 0xFFEA005EU, 0xFFC30052U,
    0xFFE3008CU, 0xFFBF0077U, 0xFFC239B3U, 0xFF9A0089U,

    0xFF0078D7U, 0xFF0063B1U, 0xFF8E8CD8U, 0xFF6B69D6U,
    0xFF8764B8U, 0xFF744DA9U, 0xFFB146C2U, 0xFF881798U,

    0xFF0099BCU, 0xFF2D7D9AU, 0xFF00B7C3U, 0xFF038387U,
    0xFF00B294U, 0xFF018574U, 0xFF00CC6AU, 0xFF10893EU,

    0xFF7A7574U, 0xFF5D5A58U, 0xFF68768AU, 0xFF515C6BU,
    0xFF567C73U, 0xFF486860U, 0xFF498205U, 0xFF107C10U,

    0xFF767676U, 0xFF4C4A48U, 0xFF69797EU, 0xFF4A5459U,
    0xFF647C64U, 0xFF525E54U, 0xFF847545U, 0xFF7E735FU,
};

const wchar_t* const kPaletteClass = L"KTile.ColorPalette";

// 弹板全部尺寸（客户区，像素）。按 DPI 缩放后算一次，绘制与命中测试共用。
struct PaletteContext {
    std::optional<std::uint32_t> selected;
    int hovered = -1;
    const std::function<void(std::uint32_t)>* onPick = nullptr;
    int cell = 0;
    int gap = 0;
    int margin = 0;
    int hintHeight = 0;
    int width = 0;
    int height = 0;
    bool closing = false;
};

COLORREF toColorRef(const std::uint32_t argb) {
    return RGB(static_cast<BYTE>((argb >> 16) & 0xFFU),
               static_cast<BYTE>((argb >> 8) & 0xFFU),
               static_cast<BYTE>(argb & 0xFFU));
}

// 判断底色深浅，决定勾选标记用黑还是白（浅黄上用白勾等于没画）。
bool isDarkColor(const std::uint32_t argb) {
    const int red = static_cast<int>((argb >> 16) & 0xFFU);
    const int green = static_cast<int>((argb >> 8) & 0xFFU);
    const int blue = static_cast<int>(argb & 0xFFU);
    return (red * 299 + green * 587 + blue * 114) / 1000 < 140;
}

PaletteContext* contextOf(const HWND hwnd) {
    return reinterpret_cast<PaletteContext*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

RECT cellRect(const PaletteContext& ctx, const int index) {
    const int column = index % kColumns;
    const int row = index / kColumns;
    RECT rect{};
    rect.left = ctx.margin + column * (ctx.cell + ctx.gap);
    rect.top = ctx.margin + row * (ctx.cell + ctx.gap);
    rect.right = rect.left + ctx.cell;
    rect.bottom = rect.top + ctx.cell;
    return rect;
}

// 返回命中的色格下标；落在间隙 / 窗口外返回 -1。
int hitTest(const PaletteContext& ctx, const int x, const int y) {
    if (x < ctx.margin || y < ctx.margin) {
        return -1;
    }
    const int step = ctx.cell + ctx.gap;
    const int column = (x - ctx.margin) / step;
    const int row = (y - ctx.margin) / step;
    if (column < 0 || column >= kColumns || row < 0 || row >= kRows) {
        return -1;
    }
    const int cellX = ctx.margin + column * step;
    const int cellY = ctx.margin + row * step;
    if (x >= cellX + ctx.cell || y >= cellY + ctx.cell) {
        return -1;  // 点在色块之间的空隙里，不算选中
    }
    return row * kColumns + column;
}

// 勾选标记：三笔画一个对勾。颜色与格子对比，深浅都能看清。
void drawCheckMark(const HDC dc, const RECT& rect, const bool dark) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    POINT points[3];
    points[0].x = rect.left + width * 24 / 100;
    points[0].y = rect.top + height * 50 / 100;
    points[1].x = rect.left + width * 43 / 100;
    points[1].y = rect.top + height * 69 / 100;
    points[2].x = rect.left + width * 76 / 100;
    points[2].y = rect.top + height * 30 / 100;

    HPEN pen = CreatePen(PS_SOLID, (std::max)(2, width / 10),
                         dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00));
    if (pen == nullptr) {
        return;
    }
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Polyline(dc, points, 3);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void paintPalette(const HWND hwnd, const PaletteContext& ctx) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    if (dc == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(hwnd, &client);

    // 先整块刷白：色块之间是 4px 空隙，不清底会留下上一帧的残影。
    HBRUSH background = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    if (background != nullptr) {
        FillRect(dc, &client, background);
        DeleteObject(background);
    }

    for (int index = 0; index < kColorCount; ++index) {
        const RECT rect = cellRect(ctx, index);
        const std::uint32_t color = kPaletteColors[index];
        HBRUSH fill = CreateSolidBrush(toColorRef(color));
        if (fill != nullptr) {
            FillRect(dc, &rect, fill);
            DeleteObject(fill);
        }
        // 每一格描边：相邻的深色块否则会糊成一片，分不清格子。
        FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

        if (ctx.selected.has_value() &&
            (*ctx.selected & 0x00FFFFFFU) == (color & 0x00FFFFFFU)) {
            drawCheckMark(dc, rect, isDarkColor(color));
        }
        if (ctx.hovered == index) {
            RECT inner = rect;
            InflateRect(&inner, -1, -1);
            // 虚线框在任何底色上都可见，且不会改变颜色本身
            DrawFocusRect(dc, &inner);
        }
    }

    // 底部操作提示：点选不关窗，需要告诉用户怎么退出。
    RECT hint{};
    hint.left = ctx.margin;
    hint.right = client.right - ctx.margin;
    hint.top = ctx.margin + kRows * ctx.cell + (kRows - 1) * ctx.gap +
               ctx.gap;
    hint.bottom = hint.top + ctx.hintHeight;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    HGDIOBJ oldFont =
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(dc, L"点选即生效，点击其他位置关闭", -1, &hint,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);

    EndPaint(hwnd, &paint);
}

void closePalette(const HWND hwnd, PaletteContext* ctx) {
    if (ctx != nullptr) {
        if (ctx->closing) {
            return;
        }
        ctx->closing = true;
    }
    if (GetCapture() == hwnd) {
        ReleaseCapture();
    }
    DestroyWindow(hwnd);
}

LRESULT CALLBACK paletteProc(const HWND hwnd, const UINT message,
                             const WPARAM wParam, const LPARAM lParam) {
    PaletteContext* ctx = contextOf(hwnd);

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }

        case WM_PAINT:
            if (ctx != nullptr) {
                paintPalette(hwnd, *ctx);
            }
            return 0;

        case WM_MOUSEMOVE: {
            if (ctx == nullptr) {
                break;
            }
            const int index =
                hitTest(*ctx, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (index != ctx->hovered) {
                ctx->hovered = index;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            return 0;
        }

        case WM_MOUSELEAVE:
            if (ctx != nullptr && ctx->hovered != -1) {
                ctx->hovered = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            if (ctx == nullptr) {
                break;
            }
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const int index = hitTest(*ctx, x, y);
            if (index >= 0) {
                const std::uint32_t color = kPaletteColors[index];
                ctx->selected = color;
                // 只重画变化区域，弹板很小时整窗重画也无所谓
                InvalidateRect(hwnd, nullptr, FALSE);
                if (ctx->onPick != nullptr) {
                    (*ctx->onPick)(color);
                }
            } else {
                // 捕获期间点在弹板外（坐标可能为负）→ 关闭；
                // 点在格间空隙不关，避免手抖。
                RECT client{};
                GetClientRect(hwnd, &client);
                if (x < 0 || y < 0 || x >= client.right ||
                    y >= client.bottom) {
                    closePalette(hwnd, ctx);
                }
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                closePalette(hwnd, ctx);
                return 0;
            }
            break;

        case WM_KILLFOCUS:
        case WM_ACTIVATE:
            // 焦点跑到别处（Alt+Tab、点其它程序）就收起，避免留下一个
            // 悬空的弹板。注意只在真正失活时关，不能把激活消息也当关闭。
            if (message == WM_KILLFOCUS ||
                LOWORD(wParam) == WA_INACTIVE) {
                closePalette(hwnd, ctx);
                return 0;
            }
            break;

        case WM_CAPTURECHANGED:
            if (ctx != nullptr && !ctx->closing) {
                closePalette(hwnd, ctx);
            }
            return 0;

        case WM_CLOSE:
            closePalette(hwnd, ctx);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ensurePaletteClass(const HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = paletteProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kPaletteClass;
    RegisterClassExW(&windowClass);
    registered = true;
}

}  // namespace

void showColorPalette(const HWND owner, const HWND anchor,
                      const std::uint32_t currentArgb,
                      const std::function<void(std::uint32_t)>& onPick) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    ensurePaletteClass(instance);

    // 以主显示器 DPI 缩放：弹板只出现在当前屏幕，跟宿主对话框一致。
    int dpi = 96;
    HDC screenDc = GetDC(nullptr);
    if (screenDc != nullptr) {
        dpi = GetDeviceCaps(screenDc, LOGPIXELSX);
        ReleaseDC(nullptr, screenDc);
    }
    if (dpi <= 0) {
        dpi = 96;
    }

    PaletteContext ctx;
    ctx.selected = currentArgb;
    ctx.onPick = &onPick;
    ctx.cell = MulDiv(22, dpi, 96);
    ctx.gap = MulDiv(4, dpi, 96);
    ctx.margin = MulDiv(8, dpi, 96);
    ctx.hintHeight = MulDiv(18, dpi, 96);
    ctx.width = ctx.margin * 2 + kColumns * ctx.cell +
                (kColumns - 1) * ctx.gap;
    ctx.height = ctx.margin + kRows * ctx.cell + (kRows - 1) * ctx.gap +
                 ctx.gap + ctx.hintHeight + MulDiv(4, dpi, 96);

    RECT windowRect{0, 0, ctx.width, ctx.height};
    const DWORD style = WS_POPUP | WS_BORDER;
    const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);
    const int windowWidth = windowRect.right - windowRect.left;
    const int windowHeight = windowRect.bottom - windowRect.top;

    // 贴在 anchor（按钮）下方；下方的空间不够就翻到上方。
    RECT anchorRect{};
    int x = 0;
    int y = 0;
    const int offset = MulDiv(4, dpi, 96);
    if (anchor != nullptr && GetWindowRect(anchor, &anchorRect) != FALSE) {
        x = anchorRect.left;
        y = anchorRect.bottom + offset;
    } else {
        POINT cursor{};
        GetCursorPos(&cursor);
        x = cursor.x;
        y = cursor.y;
    }

    HMONITOR monitor = MonitorFromWindow(
        owner != nullptr ? owner : anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo) != FALSE) {
        const RECT& work = monitorInfo.rcWork;
        if (x + windowWidth > work.right) {
            x = work.right - windowWidth;
        }
        if (x < work.left) {
            x = work.left;
        }
        if (y + windowHeight > work.bottom) {
            y = anchorRect.top - windowHeight - offset;
        }
        if (y < work.top) {
            y = work.top;
        }
    }

    HWND palette = CreateWindowExW(exStyle, kPaletteClass, L"", style, x, y,
                                   windowWidth, windowHeight, owner, nullptr,
                                   instance, &ctx);
    if (palette == nullptr) {
        return;
    }

    const BOOL ownerWasEnabled =
        owner != nullptr && IsWindowEnabled(owner) != FALSE;
    ShowWindow(palette, SW_SHOW);
    if (owner != nullptr) {
        EnableWindow(owner, FALSE);
    }
    SetForegroundWindow(palette);
    SetFocus(palette);
    SetCapture(palette);

    // 本地模态循环（与设置窗口 / 编辑框同一套思路）：
    // 只听“弹板是否还活着”，不碰线程级 WM_QUIT。
    MSG message{};
    while (IsWindow(palette) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (owner != nullptr) {
        EnableWindow(owner, ownerWasEnabled ? TRUE : FALSE);
        SetForegroundWindow(owner);
    }
}

void paintColorSwatch(const HDC dc, const RECT& rect,
                      const std::optional<std::uint32_t>& argb) {
    if (dc == nullptr || rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }

    // 有颜色：直接填色；空颜色：填成对话框底色，看起来像「透明/未选」。
    const COLORREF fillColor =
        argb.has_value() ? toColorRef(*argb) : GetSysColor(COLOR_BTNFACE);
    HBRUSH fill = CreateSolidBrush(fillColor);
    if (fill != nullptr) {
        FillRect(dc, &rect, fill);
        DeleteObject(fill);
    }

    if (!argb.has_value()) {
        // 斜线表示「没有指定颜色」——与「浅灰也是一种颜色」区分开。
        HPEN slash = CreatePen(PS_SOLID, 1, RGB(140, 140, 140));
        if (slash != nullptr) {
            HGDIOBJ oldPen = SelectObject(dc, slash);
            MoveToEx(dc, rect.left + 1, rect.bottom - 2, nullptr);
            LineTo(dc, rect.right - 2, rect.top + 1);
            SelectObject(dc, oldPen);
            DeleteObject(slash);
        }
    }

    FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
}

}  // namespace ui
}  // namespace ktile
