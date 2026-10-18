#include "ui/builtin_item_dialog.h"

#include "core/builtin_items.h"
#include "platform/win32/builtin_icon.h"
#include "platform/win32/icon_store.h"

#include <commctrl.h>

#include <map>
#include <string>
#include <vector>

namespace ktile {
namespace ui {
namespace {

// ---- 布局常量（96 DPI 逻辑像素，scaleFor 统一缩放）--------------------

constexpr int kMargin = 20;
constexpr int kCellWidth = 78;    // 单个条目按钮
constexpr int kCellHeight = 88;
constexpr int kCellGap = 6;
constexpr int kColumns = 6;
constexpr int kSectionHeaderHeight = 24;
constexpr int kSectionGap = 8;
constexpr int kIconSize = 64;     // 按钮内的图标绘制尺寸。
                                  // 注意 PNG 自带 12.5% 透明内边距（128 画布
                                  // 里内容只占 96），可见图形约 48 —— 别只看
                                  // 这个数判断视觉大小。
constexpr int kIconTop = 4;       // 图标上边距
constexpr int kLabelHeight = 26;  // 名字区高度（贴按钮底部，紧挨图标）

constexpr const wchar_t* kDialogClass = L"KTile.BuiltinItemDialog";
constexpr UINT kDialogCloseMessage = WM_APP + 0x92U;

// 把图标位图挂到按钮的窗口属性上，WM_DRAWITEM 里取用
constexpr const wchar_t* kPropIconBitmap = L"KTileBuiltinIconBitmap";

int scaleFor(HWND window, const int value) {
    HDC dc = GetDC(window);
    if (dc == nullptr) {
        return value;
    }
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(window, dc);
    return MulDiv(value, dpi > 0 ? dpi : 96, 96);
}

HFONT dialogFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(NONCLIENTMETRICSW);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                              &metrics, 0) != FALSE) {
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

HWND addControl(const HWND parent, const wchar_t* className,
                const wchar_t* text, const DWORD style, const int x,
                const int y, const int width, const int height,
                const int id, const HFONT font) {
    HWND control = CreateWindowExW(
        0, className, text, WS_CHILD | WS_VISIBLE | style,
        scaleFor(parent, x), scaleFor(parent, y), scaleFor(parent, width),
        scaleFor(parent, height), parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    if (control != nullptr && font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return control;
}

// ---- 布局：先算好每一格的位置，再照着建控件 ---------------------------
//
// 之前控件位置和窗口高度是两处写死的常量，加/删内置项后高度公式
// 必须手工重算（上一版就是按「5 行」硬编码的）。现在只有这一份
// 算法：建控件和算窗口尺寸都从这里取，条目增减自动跟上。
struct CellEntry {
    const core::BuiltinItem* item = nullptr;  // nullptr = 分组标题
    const wchar_t* text = nullptr;            // 标题文字（item 为空时用）
    int x = 0;
    int y = 0;
};

struct DialogLayout {
    std::vector<CellEntry> entries;
    int width = 0;
    int height = 0;
};

DialogLayout computeLayout() {
    DialogLayout layout;
    layout.width =
        kMargin * 2 + kColumns * kCellWidth + (kColumns - 1) * kCellGap;

    int y = kMargin;
    int column = 0;
    const wchar_t* currentCategory = nullptr;

    for (const core::BuiltinItem& item : core::builtinItems()) {
        if (currentCategory == nullptr ||
            wcscmp(currentCategory, item.category) != 0) {
            if (currentCategory != nullptr) {
                // 上一组没填满一行：先把这行的高度补上，再留组间距
                if (column != 0) {
                    y += kCellHeight + kCellGap;
                    column = 0;
                }
                y += kSectionGap;
            }
            currentCategory = item.category;
            CellEntry header;
            header.text = item.category;
            header.x = kMargin + 2;
            header.y = y;
            layout.entries.push_back(header);
            y += kSectionHeaderHeight;
        }

        CellEntry cell;
        cell.item = &item;
        cell.x = kMargin + column * (kCellWidth + kCellGap);
        cell.y = y;
        layout.entries.push_back(cell);

        ++column;
        if (column >= kColumns) {
            column = 0;
            y += kCellHeight + kCellGap;
        }
    }

    int bottom = y;
    if (column == 0) {
        bottom -= kCellGap;  // 末行刚好填满：y 里多算了一个行间距
    } else {
        bottom += kCellHeight;
    }
    layout.height = bottom + kMargin;
    return layout;
}

// 为一个内置项准备图标位图（预乘 DIB，可直接给 AlphaBlend）。
// 失败返回 nullptr —— 条目仍会显示，只是没有图标。
HBITMAP prepareIconBitmap(const core::BuiltinItem& item) {
    std::wstring error;
    const std::wstring stored =
        platform::win32::ensureBuiltinIcon(item, &error);
    if (stored.empty()) {
        return nullptr;
    }

    const std::wstring absolutePath =
        platform::win32::resolveStoredIconPath(stored);
    platform::win32::ImagePixels pixels;
    if (!platform::win32::decodeImageFile(absolutePath, &pixels) ||
        !pixels.valid()) {
        return nullptr;
    }

    // AlphaBlend(AC_SRC_OVER) 要求预乘
    const std::vector<std::uint32_t> premultiplied =
        platform::win32::premultiplyBgra(pixels);

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(BITMAPV5HEADER);
    header.bV5Width = pixels.width;
    header.bV5Height = -pixels.height;  // top-down
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000U;
    header.bV5GreenMask = 0x0000FF00U;
    header.bV5BlueMask = 0x000000FFU;
    header.bV5AlphaMask = 0xFF000000U;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                      DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (bitmap == nullptr || bits == nullptr) {
        return nullptr;
    }
    memcpy(bits, premultiplied.data(),
           premultiplied.size() * sizeof(std::uint32_t));
    return bitmap;
}

// ---- 窗口过程 ---------------------------------------------------------

struct BuiltinDialogState {
    BuiltinPickResult* out = nullptr;
    bool accepted = false;
    HFONT font = nullptr;
};

LRESULT CALLBACK builtinDialogProc(HWND hwnd, UINT message, WPARAM wParam,
                                   LPARAM lParam) {
    auto* state = reinterpret_cast<BuiltinDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;  // 必须交给 DefWindowProc 存标题（与设置窗口同一坑）
        }

        case WM_CREATE: {
            if (state == nullptr) {
                break;
            }
            state->font = dialogFont();

            // 按钮计数器：控件 ID 从 2001 递增
            int nextId = 2001;

            for (const CellEntry& entry : computeLayout().entries) {
                if (entry.item == nullptr) {
                    addControl(hwnd, L"STATIC", entry.text, 0, entry.x,
                               entry.y, 200, 18, 0, state->font);
                    continue;
                }
                const core::BuiltinItem& item = *entry.item;

                // owner-draw 按钮：WM_DRAWITEM 里画图标与名字
                HWND button = addControl(hwnd, L"BUTTON", item.name,
                                         BS_OWNERDRAW, entry.x, entry.y,
                                         kCellWidth, kCellHeight, nextId,
                                         state->font);
                if (button == nullptr) {
                    ++nextId;
                    continue;
                }
                HBITMAP bitmap = prepareIconBitmap(item);
                if (bitmap != nullptr) {
                    // 位图挂在按钮属性上，WM_DRAWITEM 里取用，
                    // WM_NCDESTROY 统一删除
                    SetPropW(button, kPropIconBitmap, bitmap);
                }
                // 按钮与内置项的对应关系用属性存。属性值是 HANDLE，
                // 静态表里的 wchar_t* 转过去、取出时转回来 —— 指向
                // 的条目与进程同生命周期，没有悬垂问题。
                SetPropW(button, L"KTileBuiltinId",
                         reinterpret_cast<HANDLE>(const_cast<wchar_t*>(item.id)));
                SetPropW(button, L"KTileBuiltinName",
                         reinterpret_cast<HANDLE>(
                             const_cast<wchar_t*>(item.name)));
                SetPropW(button, L"KTileBuiltinTarget",
                         reinterpret_cast<HANDLE>(
                             const_cast<wchar_t*>(item.target)));
                ++nextId;
            }
            return 0;
        }

        case WM_DRAWITEM: {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (draw == nullptr || draw->CtlType != ODT_BUTTON) {
                break;
            }
            HDC dc = draw->hDC;
            const RECT& rect = draw->rcItem;

            // rcItem 是物理像素，布局常量是 96 DPI 逻辑值 —— 必须换算。
            // 之前直接用逻辑值当物理值，高 DPI 下图标比设计小了一圈
            //（150% 时 64 逻辑只画了约 43 逻辑），图标和文字之间
            // 看起来空出一大截就是这个原因。
            const int iconSize = scaleFor(draw->hwndItem, kIconSize);
            const int iconTop = scaleFor(draw->hwndItem, kIconTop);
            const int labelHeight = scaleFor(draw->hwndItem, kLabelHeight);

            // 选中反馈：实线边框（按下瞬间）
            const bool focused = (draw->itemState & ODS_SELECTED) != 0;
            HBRUSH background = GetSysColorBrush(COLOR_BTNFACE);
            FillRect(dc, &rect, background);
            if (focused) {
                FrameRect(dc, &rect,
                          reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }

            // 图标（预乘 DIB，AlphaBlend 走 per-pixel alpha）。
            // 源矩形必须是**整张位图**（生成的是 128x128）—— 上一版把
            // 目标尺寸 40 顺手当成了源尺寸，结果每格只显示原图左上角。
            HBITMAP bitmap = static_cast<HBITMAP>(
                GetPropW(draw->hwndItem, kPropIconBitmap));
            if (bitmap != nullptr) {
                BITMAP info{};
                if (GetObjectW(bitmap, sizeof(info), &info) != 0) {
                    HDC mem = CreateCompatibleDC(dc);
                    HGDIOBJ old = SelectObject(mem, bitmap);
                    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                    const int iconX = rect.left +
                                      ((rect.right - rect.left) - iconSize) / 2;
                    AlphaBlend(dc, iconX, rect.top + iconTop, iconSize,
                               iconSize, mem, 0, 0, info.bmWidth,
                               info.bmHeight, blend);
                    SelectObject(mem, old);
                    DeleteDC(mem);
                }
            }

            // 名字（单行居中）
            wchar_t name[64] = {};
            GetWindowTextW(draw->hwndItem, name, 64);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            RECT textRect = rect;
            textRect.top = rect.bottom - labelHeight;
            textRect.left += 2;
            textRect.right -= 2;
            DrawTextW(dc, name, -1, &textRect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            return TRUE;
        }

        case WM_COMMAND: {
            // 点击任一条目：读回属性里的信息，关闭并返回
            if (HIWORD(wParam) != BN_CLICKED || state == nullptr) {
                break;
            }
            const HWND control = GetDlgItem(hwnd, LOWORD(wParam));
            if (control == nullptr) {
                break;
            }
            const wchar_t* id = reinterpret_cast<const wchar_t*>(
                GetPropW(control, L"KTileBuiltinId"));
            const wchar_t* name = reinterpret_cast<const wchar_t*>(
                GetPropW(control, L"KTileBuiltinName"));
            const wchar_t* target = reinterpret_cast<const wchar_t*>(
                GetPropW(control, L"KTileBuiltinTarget"));
            if (id == nullptr || name == nullptr || target == nullptr) {
                break;
            }
            if (state->out != nullptr) {
                state->out->id = id;
                state->out->name = name;
                state->out->target = target;
                state->out->icon = core::builtinIconFileName(state->out->id);
            }
            state->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_CLOSE:
            if (state != nullptr) {
                state->accepted = false;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_NCDESTROY: {
            // 清理挂在按钮上的位图与属性
            HWND child = GetWindow(hwnd, GW_CHILD);
            while (child != nullptr) {
                HBITMAP bitmap =
                    static_cast<HBITMAP>(RemovePropW(child, kPropIconBitmap));
                if (bitmap != nullptr) {
                    DeleteObject(bitmap);
                }
                RemovePropW(child, L"KTileBuiltinId");
                RemovePropW(child, L"KTileBuiltinName");
                RemovePropW(child, L"KTileBuiltinTarget");
                child = GetWindow(child, GW_HWNDNEXT);
            }
            if (state != nullptr && state->font != nullptr) {
                DeleteObject(state->font);
                state->font = nullptr;
            }
            PostMessageW(hwnd, kDialogCloseMessage, 0, 0);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ensureDialogClass(const HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(WNDCLASSEXW);
    windowClass.lpfnWndProc = &builtinDialogProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kDialogClass;
    if (RegisterClassExW(&windowClass) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
}

}  // namespace

bool showBuiltinItemDialog(HWND owner, BuiltinPickResult* out) {
    if (out == nullptr) {
        return false;
    }

    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    ensureDialogClass(instance);

    BuiltinDialogState state;
    state.out = out;

    const DialogLayout layout = computeLayout();
    RECT desired{0, 0, scaleFor(owner, layout.width),
                 scaleFor(owner, layout.height)};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&desired, style, FALSE, WS_EX_DLGMODALFRAME);

    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kDialogClass, L"添加系统项目", style,
        0, 0, desired.right - desired.left, desired.bottom - desired.top,
        owner, nullptr, instance, &state);
    if (dialog == nullptr) {
        return false;
    }

    // 居中于 owner
    if (owner != nullptr) {
        RECT ownerRect{};
        if (GetWindowRect(owner, &ownerRect) != FALSE) {
            const int width = desired.right - desired.left;
            const int height = desired.bottom - desired.top;
            SetWindowPos(dialog, nullptr,
                         ownerRect.left +
                             ((ownerRect.right - ownerRect.left) - width) / 2,
                         ownerRect.top +
                             ((ownerRect.bottom - ownerRect.top) - height) / 2,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);

    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kDialogCloseMessage) {
            continue;
        }
        if (message.hwnd == dialog || IsChild(dialog, message.hwnd)) {
            if (!IsDialogMessageW(dialog, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return state.accepted;
}

}  // namespace ui
}  // namespace ktile
