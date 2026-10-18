// KTile - Win32 平台层：托盘图标
//
// 使用 Shell_NotifyIcon 实现系统托盘图标与右键菜单。
//
// Win7 兼容：NOTIFYICONDATAW 的 cbSize 在 Win7 用 NOTIFYICONDATA_V3_SIZE。
// 使用 NIM_SETVERSION 可获得气泡通知等较新行为。
//
#pragma once

#include "platform/win32/unique_handles.h"
#include "platform/win32/win32_error.h"

#include <Windows.h>
#include <shellapi.h>

#include <functional>
#include <string>
#include <vector>

namespace ktile {
namespace platform {
namespace win32 {

// 托盘菜单项
struct TrayMenuItem {
    UINT commandId = 0;
    std::wstring text;
    bool enabled = true;
    bool isSeparator = false;
    bool isChecked = false;
    bool hasSubmenu = false;

    static TrayMenuItem separator() {
        TrayMenuItem item;
        item.isSeparator = true;
        return item;
    }
};

class TrayIcon {
public:
    // 用户点击托盘图标（左键单击默认行为：切换主窗口）
    using ActivateHandler = std::function<void()>;
    // 用户选择菜单项
    using CommandHandler = std::function<void(UINT commandId)>;

    TrayIcon() = default;

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    ~TrayIcon();

    // 添加托盘图标。
    // window 接收托盘消息（通常是主窗口），callbackMessage 是自定义消息 ID。
    Win32Status create(HWND window,
                       UINT callbackMessage,
                       const std::wstring& tooltip,
                       HICON icon);

    // 更新提示文字
    Win32Status setTooltip(const std::wstring& tooltip);

    // 更新图标
    Win32Status setIcon(HICON icon);

    // 移除托盘图标
    void destroy();

    // 弹出右键菜单（在指定坐标）
    void showContextMenu(const std::vector<TrayMenuItem>& items,
                         POINT screenPosition);

    // 设置回调
    void setActivateHandler(ActivateHandler handler) {
        activateHandler_ = std::move(handler);
    }
    void setCommandHandler(CommandHandler handler) {
        commandHandler_ = std::move(handler);
    }

    // 供窗口过程调用：处理托盘相关的鼠标消息
    // 返回 true 表示消息已被处理
    bool handleMessage(WPARAM wParam, LPARAM lParam);

    bool isCreated() const noexcept { return created_; }

    // 托盘回调消息 ID（由调用方分配，用于窗口过程过滤）
    UINT callbackMessage() const noexcept { return callbackMessage_; }

private:
    NOTIFYICONDATAW makeIconData() const;

    HWND window_ = nullptr;
    UINT callbackMessage_ = 0;
    bool created_ = false;
    std::wstring tooltip_;
    HICON icon_ = nullptr;

    ActivateHandler activateHandler_;
    CommandHandler commandHandler_;
};

// 应用图标资源 ID（与 resources 中的定义保持一致）
inline constexpr int kApplicationIconResourceId = 101;

// 加载内嵌应用图标（从 EXE 资源）。
// 失败时返回 nullptr，调用方可回退到系统默认图标。
UniqueHIcon loadApplicationIcon(HINSTANCE instance, int resourceId);

// 创建简单的纯色圆形图标（无资源时的兜底方案）
UniqueHIcon createFallbackIcon(int sizePixels, COLORREF color);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
