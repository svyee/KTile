#include "platform/win32/popup_menu.h"

namespace ktile {
namespace platform {
namespace win32 {
namespace {

// 递归构建菜单。子菜单挂到父菜单后由父菜单销毁（DestroyMenu 递归释放），
// 因此用 release() 交出所有权，不能用 RAII 包装。
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
        // Win32 没有真正的 radio 标记，勾选标记足够表达「当前生效项」
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

    // 先把 owner 设为前台窗口，菜单才会在点击外部时自动关闭：TrackPopupMenu
    // 的菜单归属于前台窗口的消息队列，没有前台归属就收不到「点击外部」通知。
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
