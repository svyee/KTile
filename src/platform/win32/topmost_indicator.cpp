#include "platform/win32/topmost_indicator.h"

#include <cstdint>

#include <vector>

namespace ktile {
namespace platform {
namespace win32 {

namespace {

// --- 标志登记表 ---

struct Mark {
    HWND target;
    HWND overlay;
};

std::vector<Mark>& marks() {
    static std::vector<Mark> table;
    return table;
}

constexpr COLORREF kFrameColor = RGB(235, 140, 40);  // 钉子橙
constexpr int kFramePx = 6;                          // 描边厚度（物理像素）
constexpr UINT kValidateTimerId = 1;
// 这个定时器既兜底目标失效，也负责维持描边的 z 序：用户激活别的窗口后，
// 描边要尽快重新插回目标窗口上方，慢了会看到后置窗口的描边浮在前面。
constexpr UINT kValidateIntervalMs = 100;

HWINEVENTHOOK g_locationHook = nullptr;

void renderFrame(HWND overlay, const int left, const int top, const int width,
                 const int height);

// 把描边对齐到目标当前的位置与尺寸；目标最小化 / 隐藏时藏起来
void syncOverlay(HWND overlay) {
    const HWND target = reinterpret_cast<HWND>(
        GetWindowLongPtrW(overlay, GWLP_USERDATA));
    if (target == nullptr || !IsWindow(target)) {
        return;
    }
    if (!IsWindowVisible(target) || IsIconic(target)) {
        ShowWindow(overlay, SW_HIDE);
        return;
    }
    RECT rect = {};
    if (GetWindowRect(target, &rect) == FALSE) {
        return;
    }
    const int width = rect.right - rect.left + kFramePx * 2;
    const int height = rect.bottom - rect.top + kFramePx * 2;
    // 尺寸变了才重渲染（分层位图跟尺寸走），纯移动复用现成内容
    RECT current = {};
    if (GetWindowRect(overlay, &current) == FALSE ||
        current.right - current.left != width ||
        current.bottom - current.top != height) {
        renderFrame(overlay, rect.left - kFramePx, rect.top - kFramePx, width,
                    height);
    }
    // 描边停在目标窗口正上方，跟着目标一起参与遮挡：两个窗口同时置顶时，
    // 被前面那个压住的一份描边会一起被盖掉，而不是浮在屏幕上。
    // z 序 API 只能「插到某窗口下面」，所以拿目标的上邻窗口当锚点
    // （目标本身在最上层时退回 HWND_TOP）。只有描边自己在动。
    const HWND above = GetWindow(target, GW_HWNDPREV);
    if (above == overlay) {
        SetWindowPos(overlay, nullptr, rect.left - kFramePx,
                     rect.top - kFramePx, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOZORDER);
        return;
    }
    SetWindowPos(overlay, above != nullptr ? above : HWND_TOP,
                 rect.left - kFramePx, rect.top - kFramePx, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// 定时校验：目标没了 / 不再置顶就地收尾。位置跟随即时走 WinEvent 钩子，
// 这里只兜底（也覆盖被 UIPI 挡掉的管理员窗口事件）。
void overlayValidate(HWND overlay) {
    const HWND target = reinterpret_cast<HWND>(
        GetWindowLongPtrW(overlay, GWLP_USERDATA));
    if (target == nullptr || !IsWindow(target) ||
        (GetWindowLongW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
        setTopmostMark(target, false);  // 目标失效自动撤
        return;
    }
    syncOverlay(overlay);
}

// 渲染一帧描边（外圈实色、内部全透明），并按 (left, top, w, h) 落到屏幕
void renderFrame(HWND overlay, const int left, const int top, const int width,
                 const int height) {
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = width;
    header.bV5Height = -height;  // 负高 = 自上而下
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateDIBSection(screenDc,
                                      reinterpret_cast<BITMAPINFO*>(&header),
                                      DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap != nullptr && bits != nullptr) {
        const HBITMAP old = static_cast<HBITMAP>(
            SelectObject(memoryDc, bitmap));
        auto* pixels = static_cast<std::uint32_t*>(bits);
        // 外圈 kFramePx 厚的实色环，内部保持全零（透明）
        const COLORREF color = kFrameColor;
        const std::uint32_t argb =
            0xFF000000u | (GetRValue(color) << 16) |
            (GetGValue(color) << 8) | GetBValue(color);
        for (int y = 0; y < height; ++y) {
            std::uint32_t* row = pixels + y * width;
            const bool inBandY = y < kFramePx || y >= height - kFramePx;
            for (int x = 0; x < width; ++x) {
                if (inBandY || x < kFramePx || x >= width - kFramePx) {
                    row[x] = argb;
                }
            }
        }
        POINT source = {};
        SIZE size = {width, height};
        POINT destination = {left, top};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(overlay, screenDc, &destination, &size, memoryDc,
                            &source, 0, &blend, ULW_ALPHA);
        SelectObject(memoryDc, old);
    }
    if (bitmap != nullptr) {
        DeleteObject(bitmap);
    }
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
}

LRESULT CALLBACK overlayProc(HWND window, UINT message, WPARAM wParam,
                             LPARAM lParam) {
    switch (message) {
        case WM_TIMER:
            if (wParam == kValidateTimerId) {
                overlayValidate(window);
                return 0;
            }
            break;

        case WM_DESTROY:
            KillTimer(window, kValidateTimerId);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// 目标移动 / 缩放 / 显示时立即跟手（定时器只做兜底）
void CALLBACK locationChangedProc(HWINEVENTHOOK, DWORD, HWND window,
                                  LONG objectId, LONG, DWORD, DWORD) {
    if (window == nullptr || objectId != OBJID_WINDOW) {
        return;
    }
    for (const Mark& mark : marks()) {
        if (mark.target != window || mark.overlay == nullptr) {
            continue;
        }
        if (IsWindow(mark.overlay)) {
            syncOverlay(mark.overlay);
        }
        return;
    }
}

ATOM ensureOverlayClass() {
    WNDCLASSEXW desc = {};
    desc.cbSize = sizeof(desc);
    desc.lpfnWndProc = overlayProc;
    desc.hInstance = GetModuleHandleW(nullptr);
    desc.lpszClassName = L"KTile.TopmostMarkFrame";
    return RegisterClassExW(&desc);
}

HWND createOverlay(HWND target) {
    ensureOverlayClass();
    RECT rect = {};
    if (GetWindowRect(target, &rect) == FALSE) {
        return nullptr;
    }
    const int width = rect.right - rect.left + kFramePx * 2;
    const int height = rect.bottom - rect.top + kFramePx * 2;
    // LAYERED 分层合成 / TRANSPARENT+NOACTIVATE 点击全穿透 / TOPMOST
    // 与置顶目标同处最上层（否则会被普通窗口盖住）/ TOOLWINDOW 不进 Alt+Tab
    HWND overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"KTile.TopmostMarkFrame", L"", WS_POPUP,
        rect.left - kFramePx, rect.top - kFramePx, width, height,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (overlay == nullptr) {
        return nullptr;
    }
    SetWindowLongPtrW(overlay, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(target));
    renderFrame(overlay, rect.left - kFramePx, rect.top - kFramePx, width,
                height);
    ShowWindow(overlay, SW_SHOWNOACTIVATE);
    SetTimer(overlay, kValidateTimerId, kValidateIntervalMs, nullptr);
    return overlay;
}

void removeMarkFor(HWND target) {
    std::vector<Mark>& table = marks();
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].target != target) {
            continue;
        }
        if (table[i].overlay != nullptr && IsWindow(table[i].overlay)) {
            DestroyWindow(table[i].overlay);
        }
        table.erase(table.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }
    if (table.empty() && g_locationHook != nullptr) {
        UnhookWinEvent(g_locationHook);
        g_locationHook = nullptr;
    }
}

}  // namespace

void setTopmostMark(HWND target, bool marked) {
    if (target == nullptr || !IsWindow(target)) {
        return;
    }
    if (!marked) {
        removeMarkFor(target);
        return;
    }
    std::vector<Mark>& table = marks();
    for (const Mark& mark : table) {
        if (mark.target == target) {
            return;  // 已有标志，不重复挂
        }
    }

    const HWND overlay = createOverlay(target);
    if (overlay == nullptr) {
        return;
    }
    if (table.empty() && g_locationHook == nullptr) {
        g_locationHook = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
            locationChangedProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    table.push_back({target, overlay});
}

void clearAllTopmostMarks() {
    std::vector<Mark> table;
    table.swap(marks());
    for (const Mark& mark : table) {
        if (mark.overlay != nullptr && IsWindow(mark.overlay)) {
            DestroyWindow(mark.overlay);
        }
    }
    if (g_locationHook != nullptr) {
        UnhookWinEvent(g_locationHook);
        g_locationHook = nullptr;
    }
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
