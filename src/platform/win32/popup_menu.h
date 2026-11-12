// KTile - Win32 平台层：通用弹出菜单
//
// 与 TrayIcon::showContextMenu 不同：托盘菜单须先把托盘窗口设为前台，
// 本模块针对任意调用方窗口。owner 不先 SetForegroundWindow，菜单弹出后
// 点外部关不掉（TrackPopupMenu 的前台窗口语义）。独立成模块是为了让
// 磁贴右键菜单等复用，避免 app 层堆满 Win32 菜单代码。
//
#pragma once

#include "platform/win32/unique_handles.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

namespace ktile {
namespace platform {
namespace win32 {

// 弹出菜单项：普通项、分隔线、带子菜单的项。
// 子菜单用 items 递归描述，调用方无需手工操作 HMENU。
struct PopupMenuEntry {
    UINT commandId = 0;
    std::wstring text;
    bool enabled = true;
    bool isSeparator = false;
    bool isChecked = false;
    // 子菜单条目。非空时本项成为弹出式子菜单（text 为子菜单标题）。
    std::vector<PopupMenuEntry> submenu;

    static PopupMenuEntry separator() {
        PopupMenuEntry entry;
        entry.isSeparator = true;
        return entry;
    }

    // 带单选标记的普通项
    static PopupMenuEntry radio(UINT commandId_, std::wstring text_, bool checked_) {
        PopupMenuEntry entry;
        entry.commandId = commandId_;
        entry.text = std::move(text_);
        entry.isChecked = checked_;
        return entry;
    }

    static PopupMenuEntry action(UINT commandId_, std::wstring text_, bool enabled_ = true) {
        PopupMenuEntry entry;
        entry.commandId = commandId_;
        entry.text = std::move(text_);
        entry.enabled = enabled_;
        return entry;
    }

    static PopupMenuEntry parent(std::wstring text_, std::vector<PopupMenuEntry> children) {
        PopupMenuEntry entry;
        entry.text = std::move(text_);
        entry.submenu = std::move(children);
        return entry;
    }
};

// 弹出菜单并返回用户选中的命令 ID，0 表示取消（点外部 / ESC / 再点右键）。
//
// owner 必须是发起菜单的窗口，且要先 SetForegroundWindow，否则菜单不会
// 在点击外部时自动关闭。flags 用 TPM_RETURNCMD（同步返回选中项）与
// TPM_RIGHTBUTTON（允许右键选择）。
UINT showPopupMenu(HWND owner,
                   const std::vector<PopupMenuEntry>& entries,
                   POINT screenPosition,
                   UINT flags = 0);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
