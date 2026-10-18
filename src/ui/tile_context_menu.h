// KTile - UI 层：磁贴右键菜单
//
// 把「磁贴 → 菜单条目」的构造逻辑从 Application 里抽出来：
//   - Application 只管「菜单选了哪一项」然后改数据
//   - 菜单长什么样、什么顺序，都在这里
//
// 这样 Application 不必包含任何 HMENU / 菜单 ID 细节。
//
#pragma once

#include "core/data_model.h"

#include <Windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace ktile {
namespace ui {

// 菜单命令 ID（自带一段高位前缀，避免与托盘菜单 ID 冲突）
enum class TileMenuCommand : UINT {
    None = 0,

    // ---- 启动类 ----
    Run = 3005,               // 运行/打开
    RunAsAdministrator = 3006, // 以管理员身份运行
    OpenFileLocation = 3007,   // 打开文件所在位置

    // ---- 磁贴布局 ----
    ResizeSmall = 3001,
    ResizeWide = 3002,
    ResizeTall = 3003,
    ResizeLarge = 3004,

    // ---- 标题显示（每块磁贴独立） ----
    LabelFollowGlobal = 3030,
    LabelAlways = 3031,
    LabelOnHover = 3032,

    // ---- 管理 ----
    Edit = 3010,    // 修改项目（完整编辑对话框）
    Remove = 3020,  // 删除

    // ---- 移动到其他标签页 ----
    //
    // 子菜单里第 i 项的命令 ID = MoveToTabBase + i（i 为目标页下标）。
    // 用「基址 + 下标」而不是给每页一个固定 ID：页数不限，
    // 固定 ID 表根本写不全。
    MoveToTabBase = 3040,
};

// 弹出磁贴右键菜单，返回用户选择的命令。
//
// item 用来显示当前尺寸的勾选标记、按类型禁用不适用的启动项、
// 并拼出带名称的菜单标题。取消时返回 TileMenuCommand::None。
//
// tabNames / activeTabIndex 用于「移动到其他页面」子菜单：
// 只有一页时不显示该子菜单（无处可移）。目标页下标由返回值减去
// MoveToTabBase 得到。
TileMenuCommand showTileContextMenu(HWND owner,
                                    POINT screenPosition,
                                    const core::LaunchItem& item,
                                    const std::vector<std::wstring>& tabNames,
                                    std::size_t activeTabIndex);

// 弹出删除确认框。用户点「是」返回 true。
bool confirmItemRemoval(HWND owner, const std::wstring& itemName);

}  // namespace ui
}  // namespace ktile
