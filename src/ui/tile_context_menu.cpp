#include "ui/tile_context_menu.h"

#include "base/text_convert.h"
#include "platform/win32/popup_menu.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ktile {
namespace ui {
namespace {

using platform::win32::PopupMenuEntry;
using platform::win32::showPopupMenu;

// 「移动到其他页面」子菜单最多列多少页。
// 子菜单长到要滚动就不好用了 —— 页数超过这个数时，用户更适合用
// 编辑对话框里的「所属页面」下拉（那里可以滚）。
constexpr std::size_t kMaxTabMenuEntries = 32;

}  // namespace

// ---------------------------------------------------------------- 菜单
//
// 结构（参照 HLaunch 并保留 KTile 特有的磁贴尺寸）：
//     <项目名>            （禁用标题）
//     ─────────────
//     运行/打开
//     以管理员身份运行     （仅应用类型可用）
//     打开文件所在位置     （网址类型不可用）
//     ─────────────
//     磁贴大小 >
//     标题显示 >
//     移动到其他页面 >   （两页以上时才出现）
//     ─────────────
//     修改项目...
//     删除

TileMenuCommand showTileContextMenu(HWND owner,
                                    const POINT screenPosition,
                                    const core::LaunchItem& item,
                                    const std::vector<std::wstring>& tabNames,
                                    const std::size_t activeTabIndex) {
    std::vector<PopupMenuEntry> entries;

    // 标题项：显示磁贴名字，禁用状态（纯展示）
    std::wstring titleText = base::utf8ToWide(item.name.empty() ? item.target : item.name);
    if (titleText.size() > 40) {
        titleText = titleText.substr(0, 39) + L"...";
    }
    PopupMenuEntry title;
    title.text = titleText;
    title.enabled = false;
    entries.push_back(title);

    entries.push_back(PopupMenuEntry::separator());

    // ---- 启动类 ----
    // 网址用「打开」、文件夹用「打开」、其余「运行」——文字贴合语义。
    const wchar_t* runText =
        item.type == core::ItemType::Url ? L"打开网址"
        : item.type == core::ItemType::Folder ? L"打开文件夹"
                                              : L"运行/打开";
    entries.push_back(PopupMenuEntry::action(
        static_cast<UINT>(TileMenuCommand::Run), runText));

    // 以管理员身份运行只对可执行目标有意义；
    // 文档/文件夹/网址提权没有意义（UAC 也会拒绝）。
    entries.push_back(PopupMenuEntry::action(
        static_cast<UINT>(TileMenuCommand::RunAsAdministrator),
        L"以管理员身份运行",
        item.type == core::ItemType::Application));

    // 网址没有「文件位置」
    entries.push_back(PopupMenuEntry::action(
        static_cast<UINT>(TileMenuCommand::OpenFileLocation),
        L"打开文件所在位置",
        item.type != core::ItemType::Url));

    entries.push_back(PopupMenuEntry::separator());

    // ---- 磁贴大小子菜单（当前尺寸打勾） ----
    const auto checkFor = [&item](const core::TileSize size) {
        return item.tileSize == size;
    };

    std::vector<PopupMenuEntry> sizeEntries;
    sizeEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::ResizeSmall),
        core::tileSizeName(core::TileSize::Small), checkFor(core::TileSize::Small)));
    sizeEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::ResizeWide),
        core::tileSizeName(core::TileSize::Wide), checkFor(core::TileSize::Wide)));
    sizeEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::ResizeTall),
        core::tileSizeName(core::TileSize::Tall), checkFor(core::TileSize::Tall)));
    sizeEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::ResizeLarge),
        core::tileSizeName(core::TileSize::Large), checkFor(core::TileSize::Large)));

    entries.push_back(PopupMenuEntry::parent(L"磁贴大小", std::move(sizeEntries)));

    // ---- 标题显示子菜单（当前方式打勾） ----
    //
    // 每块磁贴单独控制：小磁贴想干净、大磁贴想常显，都用这一项切换，
    // 不必钻进「修改项目」对话框（那个入口更深）。
    std::vector<PopupMenuEntry> labelEntries;
    labelEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::LabelFollowGlobal),
        core::tileLabelModeName(core::TileLabelMode::FollowGlobal),
        item.labelMode == core::TileLabelMode::FollowGlobal));
    labelEntries.push_back(PopupMenuEntry::separator());
    labelEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::LabelAlways),
        core::tileLabelModeName(core::TileLabelMode::Always),
        item.labelMode == core::TileLabelMode::Always));
    labelEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::LabelOnHover),
        core::tileLabelModeName(core::TileLabelMode::OnHover),
        item.labelMode == core::TileLabelMode::OnHover));

    entries.push_back(PopupMenuEntry::parent(L"标题显示", std::move(labelEntries)));

    // ---- 移动到其他页面子菜单 ----
    //
    // 只有一页时不显示：无处可移，多一个永远是空的子菜单只会添乱。
    // 当前页自己不出现在列表里 —— 「移动到本页」没有意义。
    if (tabNames.size() > 1) {
        std::vector<PopupMenuEntry> tabEntries;
        const std::size_t targetCount = (std::min)(tabNames.size(), kMaxTabMenuEntries);
        for (std::size_t index = 0; index < targetCount; ++index) {
            if (index == activeTabIndex) {
                continue;
            }
            std::wstring label = tabNames[index];
            if (label.size() > 24) {
                label = label.substr(0, 23) + L"...";
            }
            tabEntries.push_back(PopupMenuEntry::action(
                static_cast<UINT>(TileMenuCommand::MoveToTabBase) + index,
                std::move(label)));
        }
        if (!tabEntries.empty()) {
            entries.push_back(PopupMenuEntry::parent(L"移动到其他页面",
                                                     std::move(tabEntries)));
        }
    }

    entries.push_back(PopupMenuEntry::separator());

    entries.push_back(PopupMenuEntry::action(
        static_cast<UINT>(TileMenuCommand::Edit), L"修改项目..."));

    entries.push_back(PopupMenuEntry::separator());

    entries.push_back(PopupMenuEntry::action(
        static_cast<UINT>(TileMenuCommand::Remove), L"删除"));

    const UINT command = showPopupMenu(owner, entries, screenPosition);

    // 「移动到其他页面」一支：命令 ID 是基址 + 目标页下标，直接放过去
    //（app 层按同一条规则解码）。上限保护：万一下标越界（页在菜单
    // 弹出期间被别处删掉），当成 None 处理，不做任何事。
    const UINT moveBase = static_cast<UINT>(TileMenuCommand::MoveToTabBase);
    if (command >= moveBase &&
        command < moveBase + kMaxTabMenuEntries) {
        return static_cast<TileMenuCommand>(command);
    }

    switch (static_cast<TileMenuCommand>(command)) {
        case TileMenuCommand::Run:
        case TileMenuCommand::RunAsAdministrator:
        case TileMenuCommand::OpenFileLocation:
        case TileMenuCommand::ResizeSmall:
        case TileMenuCommand::ResizeWide:
        case TileMenuCommand::ResizeTall:
        case TileMenuCommand::ResizeLarge:
        case TileMenuCommand::LabelFollowGlobal:
        case TileMenuCommand::LabelAlways:
        case TileMenuCommand::LabelOnHover:
        case TileMenuCommand::Edit:
        case TileMenuCommand::Remove:
            return static_cast<TileMenuCommand>(command);
        default:
            return TileMenuCommand::None;
    }
}

// ---------------------------------------------------------------- 删除确认

bool confirmItemRemoval(HWND owner, const std::wstring& itemName) {
    const std::wstring message =
        L"确定要从启动器中删除「" + itemName + L"」吗？\n\n"
        L"（不会删除磁盘上的原始文件）";

    const int result = MessageBoxW(
        owner,
        message.c_str(),
        L"删除磁贴",
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);

    return result == IDYES;
}

}  // namespace ui
}  // namespace ktile
