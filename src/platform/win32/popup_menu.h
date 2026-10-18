// KTile - Win32 平台层：通用弹出菜单
//
// 与 TrayIcon::showContextMenu 的区别：
//   - TrayIcon 的菜单是「托盘图标的菜单」，必须先把托盘窗口设为前台；
//   - 本模块是「任意窗口的上下文菜单」，需要把**调用方窗口**设为前台，
//     否则菜单弹出后点别处关不掉（TrackPopupMenu 的前台窗口语义）。
//
// 之所以抽成独立模块：磁贴右键菜单、将来可能的画布空白处菜单都要用，
// 写在 Application 里会让 app 层堆满 Win32 菜单代码。
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

// 弹出菜单项
//
// 支持三种形态：普通项、分隔线、带子菜单的项。
// 子菜单用 items 递归描述，避免调用方手工操作 HMENU。
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

// 弹出菜单并返回用户选中的命令 ID。
//
// 返回值 0 表示用户取消（点击别处 / ESC / 再点一次右键）。
//
// 实现要点：
//   - TPM_RETURNCMD：同步返回选中项，省去 WM_COMMAND 转发；
//   - TPM_RIGHTBUTTON：允许用右键选择菜单项（用户按着右键拖到项上松开）；
//   - owner 必须是发起菜单的窗口，且要先 SetForegroundWindow，
//     否则菜单不会在点击外部时自动关闭（Win32 的著名坑）。
UINT showPopupMenu(HWND owner,
                   const std::vector<PopupMenuEntry>& entries,
                   POINT screenPosition,
                   UINT flags = 0);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
