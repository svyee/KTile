#include "platform/win32/topmost_indicator.h"

#include <cstdint>

#include <string>
#include <vector>

namespace ktile {
namespace platform {
namespace win32 {

namespace {

// --- 标志登记表 ---

struct Mark {
    HWND target;
    HWND overlay;  // 自绘方式才有；DWM 方式为 nullptr
    bool dwm;      // true = DWMWA_BORDER_COLOR 描边
};

std::vector<Mark>& marks() {
    static std::vector<Mark> table;
    return table;
}

constexpr COLORREF kFrameColor = RGB(235, 140, 40);  // 钉子橙
constexpr int kFramePx = 6;                          // 描边厚度（物理像素）
constexpr UINT kValidateTimerId = 1;
constexpr UINT kValidateIntervalMs = 200;

// --- DWM 路径（Win11）：原生窗口描边 ---

using PFN_DwmSetWindowAttribute =
    HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

constexpr DWORD kDwmwaBorderColor = 34;
// 官方约定值：恢复系统默认描边
constexpr COLORREF kDwmColorDefault = COLORREF(0xFFFFFFFE);

PFN_DwmSetWindowAttribute loadDwmSetWindowAttribute() {
    // 只探一次：地址和 dwmapi.dll 都缓存住，退出时随进程释放
    static const PFN_DwmSetWindowAttribute cached = []() -> PFN_DwmSetWindowAttribute {
        const HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
        if (dwmapi == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<PFN_DwmSetWindowAttribute>(
            GetProcAddress(dwmapi, "DwmSetWindowAttribute"));
    }();
    return cached;
}

bool tryDwmMark(HWND target, bool marked) {
    const PFN_DwmSetWindowAttribute setAttribute = loadDwmSetWindowAttribute();
    if (setAttribute == nullptr) {
        return false;
    }
    const COLORREF color = marked ? kFrameColor : kDwmColorDefault;
    // 老系统上 DwmSetWindowAttribute 存在但不认识这个属性，返回错误码
    const HRESULT result = setAttribute(target, kDwmwaBorderColor, &color,
                                        sizeof(color));
    return result == S_OK;
}

// --- 自绘路径：分层窗口描边悬浮框 ---

const wchar_t kOverlayClassName[] = L"KTile.TopmostMarkFrame";

void renderFrame(HWND overlay, const int left, const int top, const int width,
                 const int height);

void overlayValidate(HWND overlay) {
    const HWND target = reinterpret_cast<HWND>(
        GetWindowLongPtrW(overlay, GWLP_USERDATA));
    bool healthy = target != nullptr && IsWindow(target) &&
                   IsWindowVisible(target) && !IsIconic(target) &&
                   (GetWindowLongW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    RECT rect = {};
    if (healthy && GetWindowRect(target, &rect) == FALSE) {
        healthy = false;
    }
    if (!healthy) {
        setTopmostMark(target, false);  // 目标失效自动撤
        return;
    }

    const int width = rect.right - rect.left + kFramePx * 2;
    const int height = rect.bottom - rect.top + kFramePx * 2;
    // 尺寸变了才重渲染（分层位图跟尺寸走），纯移动复用现成内容
    RECT current = {};
    if (GetWindowRect(overlay, &current) == FALSE ||
        current.right - current.left != width ||
        current.bottom - current.top != height) {
        renderFrame(overlay, rect.left - kFramePx, rect.top - kFramePx,
                    width, height);
    }
    SetWindowPos(overlay, HWND_TOPMOST, rect.left - kFramePx,
                 rect.top - kFramePx, width, height, SWP_NOACTIVATE);
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

ATOM ensureOverlayClass() {
    WNDCLASSEXW desc = {};
    desc.cbSize = sizeof(desc);
    desc.lpfnWndProc = overlayProc;
    desc.hInstance = GetModuleHandleW(nullptr);
    desc.lpszClassName = kOverlayClassName;
    return RegisterClassExW(&desc);
}

HWND createOverlay(HWND target, const RECT& rect) {
    ensureOverlayClass();
    const int width = rect.right - rect.left + kFramePx * 2;
    const int height = rect.bottom - rect.top + kFramePx * 2;
    // LAYERED 分层合成 / TRANSPARENT+NOACTIVATE 点击全穿透 / TOPMOST
    // 保证盖在同为置顶的目标上面 / TOOLWINDOW 不进 Alt+Tab
    HWND overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kOverlayClassName, L"", WS_POPUP,
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
        if (table[i].dwm) {
            tryDwmMark(target, false);
        } else if (table[i].overlay != nullptr) {
            DestroyWindow(table[i].overlay);
        }
        table.erase(table.begin() + static_cast<std::ptrdiff_t>(i));
        return;
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

    if (tryDwmMark(target, true)) {
        table.push_back({target, nullptr, true});
        return;
    }

    RECT rect = {};
    if (GetWindowRect(target, &rect) == FALSE) {
        return;
    }
    const HWND overlay = createOverlay(target, rect);
    if (overlay != nullptr) {
        table.push_back({target, overlay, false});
    }
}

void clearAllTopmostMarks() {
    std::vector<Mark> table;
    table.swap(marks());
    for (const Mark& mark : table) {
        if (mark.dwm) {
            tryDwmMark(mark.target, false);
        } else if (mark.overlay != nullptr && IsWindow(mark.overlay)) {
            DestroyWindow(mark.overlay);
        }
    }
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
