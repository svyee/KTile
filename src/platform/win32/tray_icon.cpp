// KTile - Win32 平台层：托盘图标实现
#include "platform/win32/tray_icon.h"

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 托盘图标的唯一 ID（单实例场景下只需一个）
constexpr UINT kTrayIconId = 1;

// 自定义消息：托盘图标事件
// 注意：WM_APP 起始，避免与系统消息冲突
constexpr UINT kTrayCallbackMessage = WM_APP + 1;

// 根据系统版本选择合适的 NOTIFYICONDATA 结构大小。
// Win7 用 V3；更早的系统降级（本项目最低 Win7，但保守处理）。
DWORD notifyIconSize() {
    return NOTIFYICONDATAW_V3_SIZE;
}

}  // namespace

TrayIcon::~TrayIcon() {
    destroy();
}

NOTIFYICONDATAW TrayIcon::makeIconData() const {
    NOTIFYICONDATAW data{};
    data.cbSize = notifyIconSize();
    data.hWnd = window_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = callbackMessage_;
    data.hIcon = icon_;

    // 安全复制 tooltip（szTip 在 V3 中是 128 个宽字符）
    constexpr std::size_t kTipCapacity = 128;
    const std::size_t copyLength =
        tooltip_.size() < (kTipCapacity - 1) ? tooltip_.size() : (kTipCapacity - 1);
    for (std::size_t i = 0; i < copyLength; ++i) {
        data.szTip[i] = tooltip_[i];
    }
    data.szTip[copyLength] = L'\0';

    return data;
}

Win32Status TrayIcon::create(HWND window,
                             const UINT callbackMessage,
                             const std::wstring& tooltip,
                             HICON icon) {
    destroy();

    if (window == nullptr) {
        return base::makeUnexpected(
            Win32Error(ERROR_INVALID_WINDOW_HANDLE, "托盘图标需要有效窗口"));
    }

    window_ = window;
    callbackMessage_ = callbackMessage;
    tooltip_ = tooltip;
    icon_ = icon;

    NOTIFYICONDATAW data = makeIconData();
    if (Shell_NotifyIconW(NIM_ADD, &data) == FALSE) {
        window_ = nullptr;
        return base::makeUnexpected(lastError("托盘图标添加失败"));
    }

    // 设置版本以启用较新的交互行为（如气泡通知）
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);

    created_ = true;
    return {};
}

Win32Status TrayIcon::setTooltip(const std::wstring& tooltip) {
    tooltip_ = tooltip;
    if (!created_) {
        return {};
    }
    NOTIFYICONDATAW data = makeIconData();
    data.uFlags = NIF_TIP;
    if (Shell_NotifyIconW(NIM_MODIFY, &data) == FALSE) {
        return base::makeUnexpected(lastError("托盘提示更新失败"));
    }
    return {};
}

Win32Status TrayIcon::setIcon(HICON icon) {
    icon_ = icon;
    if (!created_) {
        return {};
    }
    NOTIFYICONDATAW data = makeIconData();
    data.uFlags = NIF_ICON;
    if (Shell_NotifyIconW(NIM_MODIFY, &data) == FALSE) {
        return base::makeUnexpected(lastError("托盘图标更新失败"));
    }
    return {};
}

void TrayIcon::destroy() {
    if (created_) {
        NOTIFYICONDATAW data = makeIconData();
        Shell_NotifyIconW(NIM_DELETE, &data);
        created_ = false;
    }
    window_ = nullptr;
    callbackMessage_ = 0;
}

void TrayIcon::showContextMenu(const std::vector<TrayMenuItem>& items,
                               const POINT screenPosition) {
    if (items.empty()) {
        return;
    }

    UniqueHMenu menu;
    HMENU rawMenu = CreatePopupMenu();
    if (rawMenu == nullptr) {
        return;
    }
    menu.reset(rawMenu);

    for (const auto& item : items) {
        if (item.isSeparator) {
            AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
            continue;
        }

        UINT flags = MF_STRING;
        if (!item.enabled) {
            flags |= MF_GRAYED;
        }
        if (item.isChecked) {
            flags |= MF_CHECKED;
        }
        if (item.hasSubmenu) {
            flags |= MF_POPUP;
        }
        AppendMenuW(menu.get(), flags, item.commandId, item.text.c_str());
    }

    // 托盘菜单需要先设置前台窗口，否则点击别处无法关闭菜单
    SetForegroundWindow(window_);

    const UINT command = TrackPopupMenu(
        menu.get(),
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        screenPosition.x,
        screenPosition.y,
        0,
        window_,
        nullptr);

    // 标准做法：发送一条空消息让菜单正常消失
    PostMessageW(window_, WM_NULL, 0, 0);

    if (command != 0 && commandHandler_) {
        commandHandler_(command);
    }
}

bool TrayIcon::handleMessage(const WPARAM wParam, const LPARAM lParam) {
    // 只处理本图标的消息
    if (static_cast<UINT>(wParam) != kTrayIconId) {
        return false;
    }

    // 回调消息 ID 过滤
    // 注意：NOTIFYICON_VERSION_4 下事件在 LOWORD(lParam)
    const UINT event = LOWORD(lParam);
    switch (event) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            if (activateHandler_) {
                activateHandler_();
            }
            return true;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            POINT cursor{};
            if (GetCursorPos(&cursor) != FALSE) {
                // 菜单内容由外部通过 commandHandler 提供，
                // 这里只负责触发：由调用方在 CommandHandler 之前填充菜单。
            }
            return true;  // 具体菜单由调用方构造并调用 showContextMenu
        }
        default:
            break;
    }
    return false;
}

// ---------------------------------------------------------------- 图标工具

UniqueHIcon loadApplicationIcon(HINSTANCE instance, const int resourceId) {
    // 用 LR_DEFAULTSIZE 让系统按 DPI 选择合适尺寸
    HICON icon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(resourceId),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));

    if (icon == nullptr) {
        icon = static_cast<HICON>(LoadImageW(
            instance,
            MAKEINTRESOURCEW(resourceId),
            IMAGE_ICON,
            0, 0,
            LR_DEFAULTSIZE | LR_DEFAULTCOLOR));
    }
    return UniqueHIcon(icon);
}

UniqueHIcon createFallbackIcon(const int sizePixels, const COLORREF color) {
    const int size = (sizePixels > 0 && sizePixels <= 256) ? sizePixels : 32;

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        return UniqueHIcon();
    }

    HDC memoryDc = CreateCompatibleDC(screenDc);
    if (memoryDc == nullptr) {
        ReleaseDC(nullptr, screenDc);
        return UniqueHIcon();
    }

    // 创建 32 位 ARGB 位图（支持透明）
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(BITMAPV5HEADER);
    header.bV5Width = size;
    header.bV5Height = -size;  // 负值表示自上而下
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HBITMAP colorBitmap = CreateDIBSection(
        screenDc,
        reinterpret_cast<BITMAPINFO*>(&header),
        DIB_RGB_COLORS,
        &bits,
        nullptr,
        0);

    ReleaseDC(nullptr, screenDc);

    if (colorBitmap == nullptr || bits == nullptr) {
        DeleteDC(memoryDc);
        if (colorBitmap != nullptr) {
            DeleteObject(colorBitmap);
        }
        return UniqueHIcon();
    }

    // 填充圆形（带抗锯齿的边缘过渡简化为直接绘制）
    const std::uint32_t r = GetRValue(color);
    const std::uint32_t g = GetGValue(color);
    const std::uint32_t b = GetBValue(color);
    const std::uint32_t pixel = 0xFF000000U | (r << 16) | (g << 8) | b;

    std::uint32_t* pixels = static_cast<std::uint32_t*>(bits);
    const int center = size / 2;
    // 用整数平方比较避免浮点开方
    const int radiusSquared = (center - 1) * (center - 1);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int dx = x - center;
            const int dy = y - center;
            const int distanceSquared = dx * dx + dy * dy;
            if (distanceSquared <= radiusSquared) {
                // 边缘 1 像素做透明过渡
                const int innerSquared = (center - 2) * (center - 2);
                if (distanceSquared > innerSquared && center > 2) {
                    // 半透明边缘
                    pixels[y * size + x] = (pixel & 0x00FFFFFFU) | 0x80000000U;
                } else {
                    pixels[y * size + x] = pixel;
                }
            } else {
                pixels[y * size + x] = 0;  // 全透明
            }
        }
    }

    // 创建掩码位图（全 0 表示全部由 alpha 通道决定）
    HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, nullptr);

    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = maskBitmap;

    HICON icon = CreateIconIndirect(&iconInfo);

    DeleteObject(colorBitmap);
    if (maskBitmap != nullptr) {
        DeleteObject(maskBitmap);
    }
    DeleteDC(memoryDc);

    return UniqueHIcon(icon);
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
