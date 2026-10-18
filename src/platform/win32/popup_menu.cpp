#include "platform/win32/popup_menu.h"

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 递归构建菜单。
//
// 注意所有权：子菜单挂到父菜单后，由父菜单负责销毁（DestroyMenu 会递归释放），
// 因此这里用 release() 把所有权交出去，不能再用 RAII 包装。
void appendEntries(HMENU menu, const std::vector<PopupMenuEntry>& entries) {
    for (const auto& entry : entries) {
        if (entry.isSeparator) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }

        if (!entry.submenu.empty()) {
            HMENU submenu = CreatePopupMenu();
            if (submenu == nullptr) {
                continue;
            }
            appendEntries(submenu, entry.submenu);
            AppendMenuW(menu, MF_STRING | MF_POPUP,
                        reinterpret_cast<UINT_PTR>(submenu), entry.text.c_str());
            continue;
        }

        UINT flags = MF_STRING;
        if (!entry.enabled) {
            flags |= MF_GRAYED;
        }
        // MF_CHECKED 配合单选语义使用（Win32 没有真正的 radio 标记，
        // 但勾选标记在视觉上足够表达「当前生效项」）
        if (entry.isChecked) {
            flags |= MF_CHECKED;
        }
        AppendMenuW(menu, flags, entry.commandId, entry.text.c_str());
    }
}

}  // namespace

UINT showPopupMenu(HWND owner,
                   const std::vector<PopupMenuEntry>& entries,
                   POINT screenPosition,
                   UINT flags) {
    if (owner == nullptr || entries.empty()) {
        return 0;
    }

    UniqueHMenu menu(CreatePopupMenu());
    if (!menu.valid()) {
        return 0;
    }
    appendEntries(menu.get(), entries);

    // 关键：先把 owner 设为前台窗口，菜单才会在点击外部时自动关闭。
    // 这是 TrackPopupMenu 的既有行为（不是 bug）—— 菜单归属于前台窗口的
    // 消息队列，没有前台归属就无法收到「点击外部」的通知。
    SetForegroundWindow(owner);

    const UINT baseFlags =
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | flags;
    const UINT command = TrackPopupMenu(
        menu.get(),
        baseFlags,
        screenPosition.x,
        screenPosition.y,
        0,
        owner,
        nullptr);

    // 标准做法：补一条空消息，让菜单的模态循环干净退出
    PostMessageW(owner, WM_NULL, 0, 0);

    return command;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
