#include "ui/tile_context_menu.h"

#include "base/text_convert.h"
#include "infrastructure/i18n.h"
#include "platform/win32/popup_menu.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace ktile {
namespace ui {
namespace {

using platform::win32::PopupMenuEntry;
using platform::win32::showPopupMenu;

// 取词助手：只在本文件用，省掉一长串命名空间前缀。
const wchar_t* T(const char* key) {
    return infrastructure::i18n::trw(key).c_str();
}

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
//     跟随启动 ✓           （仅应用类型可用；已添加的显示勾，再点即移除）
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
                                    const std::size_t activeTabIndex,
                                    const bool followingStartup) {
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
        item.type == core::ItemType::Url ? T("tilemenu.openUrl")
        : item.type == core::ItemType::Folder ? T("tilemenu.openFolder")
                                              : T("tilemenu.run");
    entries.push_back(PopupMenuEntry::action(
        static_cast<UINT>(TileMenuCommand::Run), runText));

    // 以管理员身份运行只对可执行目标有意义；
    // 文档/文件夹/网址提权没有意义（UAC 也会拒绝）。
    entries.push_back(PopupMenuEntry::action(
        static_cast<UINT>(TileMenuCommand::RunAsAdministrator),
        T("tilemenu.runAsAdmin"),
        item.type == core::ItemType::Application));

    // 网址没有「文件位置」
    entries.push_back(PopupMenuEntry::action(
        static_cast<UINT>(TileMenuCommand::OpenFileLocation),
        T("tilemenu.openLocation"),
        item.type != core::ItemType::Url));

    // ---- 跟随启动 ----
    //
    // 勾选式：已加入清单的打勾，再点一次就是移除 —— 与设置窗口里的
    // 「移除」等效，省得为一个勾去翻两层设置页。
    //
    // 只对可执行目标开放：网址、文件夹在 KTile 启动时自动弹出来是骚扰；
    // 插件磁贴没有可执行文件，也无从「启动」。
    PopupMenuEntry follow = PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::ToggleFollowStartup),
        T("tilemenu.followStartup"), followingStartup);
    follow.enabled = item.type == core::ItemType::Application;
    entries.push_back(std::move(follow));

    entries.push_back(PopupMenuEntry::separator());

    // ---- 磁贴大小子菜单（当前尺寸打勾） ----
    const auto checkFor = [&item](const core::TileSize size) {
        return item.tileSize == size;
    };

    std::vector<PopupMenuEntry> sizeEntries;
    sizeEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::ResizeSmall),
        infrastructure::i18n::trw(core::tileSizeLabelKey(core::TileSize::Small)),
        checkFor(core::TileSize::Small)));
    sizeEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::ResizeWide),
        infrastructure::i18n::trw(core::tileSizeLabelKey(core::TileSize::Wide)),
        checkFor(core::TileSize::Wide)));
    sizeEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::ResizeTall),
        infrastructure::i18n::trw(core::tileSizeLabelKey(core::TileSize::Tall)),
        checkFor(core::TileSize::Tall)));
    sizeEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::ResizeLarge),
        infrastructure::i18n::trw(core::tileSizeLabelKey(core::TileSize::Large)),
        checkFor(core::TileSize::Large)));

    entries.push_back(PopupMenuEntry::parent(T("tilemenu.tileSize"), std::move(sizeEntries)));

    // ---- 标题显示子菜单（当前方式打勾） ----
    //
    // 每块磁贴单独控制：小磁贴想干净、大磁贴想常显，都用这一项切换，
    // 不必钻进「修改项目」对话框（那个入口更深）。
    std::vector<PopupMenuEntry> labelEntries;
    labelEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::LabelFollowGlobal),
        infrastructure::i18n::trw(
            core::tileLabelModeLabelKey(core::TileLabelMode::FollowGlobal)),
        item.labelMode == core::TileLabelMode::FollowGlobal));
    labelEntries.push_back(PopupMenuEntry::separator());
    labelEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::LabelAlways),
        infrastructure::i18n::trw(
            core::tileLabelModeLabelKey(core::TileLabelMode::Always)),
        item.labelMode == core::TileLabelMode::Always));
    labelEntries.push_back(PopupMenuEntry::radio(
        static_cast<UINT>(TileMenuCommand::LabelOnHover),
        infrastructure::i18n::trw(
            core::tileLabelModeLabelKey(core::TileLabelMode::OnHover)),
        item.labelMode == core::TileLabelMode::OnHover));

    entries.push_back(PopupMenuEntry::parent(T("tilemenu.labelMode"), std::move(labelEntries)));

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
            entries.push_back(PopupMenuEntry::parent(T("tilemenu.moveToTab"),
                                                     std::move(tabEntries)));
        }
    }

    // ---- 复制到其他页面子菜单 ----
    //
    // 与「移动」并列放在它下面：目标页与条件完全一致（只有一页时无处
    // 可放，当前页不出现在列表里），但语义不同 —— 移动是搬家，复制是
    // 克隆一份（原磁贴留在本页，目标页多出一个内容相同的新磁贴）。
    if (tabNames.size() > 1) {
        std::vector<PopupMenuEntry> copyEntries;
        const std::size_t targetCount = (std::min)(tabNames.size(), kMaxTabMenuEntries);
        for (std::size_t index = 0; index < targetCount; ++index) {
            if (index == activeTabIndex) {
                continue;
            }
            std::wstring label = tabNames[index];
            if (label.size() > 24) {
                label = label.substr(0, 23) + L"...";
            }
            copyEntries.push_back(PopupMenuEntry::action(
                static_cast<UINT>(TileMenuCommand::CopyToTabBase) + index,
                std::move(label)));
        }
        if (!copyEntries.empty()) {
            entries.push_back(PopupMenuEntry::parent(T("tilemenu.copyToTab"),
                                                     std::move(copyEntries)));
        }
    }

    entries.push_back(PopupMenuEntry::separator());

    entries.push_back(PopupMenuEntry::action(
        static_cast<UINT>(TileMenuCommand::Edit), T("tilemenu.edit")));

    entries.push_back(PopupMenuEntry::separator());

    entries.push_back(PopupMenuEntry::action(
        static_cast<UINT>(TileMenuCommand::Remove), T("tilemenu.remove")));

    const UINT command = showPopupMenu(owner, entries, screenPosition);

    // 「移动到其他页面 / 复制到其他页面」两支：命令 ID 是各自基址 +
    // 目标页下标，直接放过去（app 层按同一条规则解码）。上限保护：
    // 万一下标越界（页在菜单弹出期间被别处删掉），当成 None 处理。
    const UINT moveBase = static_cast<UINT>(TileMenuCommand::MoveToTabBase);
    const UINT copyBase = static_cast<UINT>(TileMenuCommand::CopyToTabBase);
    if ((command >= moveBase && command < moveBase + kMaxTabMenuEntries) ||
        (command >= copyBase && command < copyBase + kMaxTabMenuEntries)) {
        return static_cast<TileMenuCommand>(command);
    }

    switch (static_cast<TileMenuCommand>(command)) {
        case TileMenuCommand::Run:
        case TileMenuCommand::RunAsAdministrator:
        case TileMenuCommand::OpenFileLocation:
        case TileMenuCommand::ToggleFollowStartup:
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

bool confirmItemRemoval(HWND owner, const std::wstring& itemName,
                        const bool alsoFollowsStartup) {
    // {0} 是磁贴名，语序交给语言文件决定（英文要把名字放句中）。
    // 两条文案各写一整句、不拼接：中文的语序关系词没法由一个前后缀
    // 拼出来，英文更是如此。
    const char* const key = alsoFollowsStartup
                                ? "tilemenu.removeConfirmFollow"
                                : "tilemenu.removeConfirm";
    const std::wstring message =
        infrastructure::i18n::formatW(key, {itemName});

    const int result = MessageBoxW(
        owner,
        message.c_str(),
        T("tilemenu.removeTitle"),
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);

    return result == IDYES;
}

}  // namespace ui
}  // namespace ktile
