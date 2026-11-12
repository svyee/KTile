// KTile - UI 层：项目编辑对话框（修改/添加项目共用同一窗口）
//
// 不用 .rc 模板，控件全部代码生成（自注册窗口类 + GWLP_USERDATA）。
// 布局为左侧图标/磁贴预览 + 右侧字段的 620x430 客户区。
// 以主窗口为 owner：主窗口保持可见，点击对话框不会被判定为窗口外。
//
#pragma once

#include "core/data_model.h"
#include "core/plugin_model.h"

#include <Windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ktile {
namespace ui {

// 对话框提交的字段集合（宽字符，Application 层转 UTF-8 后交给 core::editItem）。
struct ItemEditorFields {
    std::wstring name;                 // 允许为空（空 = 用目标名推导）

    // 类型不由用户填写：对话框按目标推断后回填；推断不出时保留这里的初值。
    core::ItemType type = core::ItemType::Application;

    std::wstring target;
    std::vector<std::wstring> arguments;  // 每行一条
    std::wstring workingDirectory;     // 允许为空

    // 自定义图标：相对 data 目录的路径（"res\xx.png"），空 = 用目标自身图标。
    std::wstring icon;

    bool runAsAdministrator = false;
    std::size_t tabIndex = 0;          // 所属页面
    core::TileSize tileSize = core::TileSize::Small;  // 磁贴尺寸

    // 标题显示方式（每项独立控制，见 core::TileLabelMode）
    core::TileLabelMode labelMode = core::TileLabelMode::FollowGlobal;

    // 图标相对磁贴的显示比例（%）。0 = 按统一设置（设置窗口里的滑块），
    // 否则夹在 50~100 之间，只影响方框图标，铺满型自定义图片不受限。
    int iconScalePercent = 0;

    // 磁贴背景色（ARGB）。空 = 跟随设置里的全局默认底色 / 皮肤。
    std::optional<std::uint32_t> tileColor;
};

// 对话框用途。两种模式共用同一批字段、校验与预览，只差标题与按钮文案，
// 不另做一个窗口（两份窗口代码必然各自演化、规则不一致）。
enum class ItemEditorMode : std::uint8_t {
    Edit = 0,    // 修改已有项目
    Create = 1,  // 新建项目
};

// 弹出「编辑项目」对话框，用户确认时把新值写入 outResult 并返回 true
// （取消/关闭返回 false）。三个全局设置值供「跟随全局」档位的预览使用：
// 不知道全局值画不出真实效果。pluginPreview 是插件磁贴的预览渲染回调
// （app 层绑定主窗口渲染路径，保证与磁贴真实效果一致），非插件传空。
// 期间主窗口保持可见（owner 关系保证点击判定）。
using PluginPreviewRenderer =
    std::function<bool(HDC, const RECT&, core::TileSize)>;

bool showItemEditorDialog(HWND owner,
                          const std::vector<std::wstring>& tabNames,
                          std::size_t initialTab,
                          const ItemEditorFields& initial,
                          bool globalAlwaysShowLabels,
                          int globalIconScale,
                          const std::optional<std::uint32_t>& globalTileColor,
                          ItemEditorFields* outResult,
                          ItemEditorMode mode = ItemEditorMode::Edit,
                          PluginPreviewRenderer pluginPreview = nullptr);

// 目标路径 → 类型推断。返回 nullopt 表示判断不出，调用方应保留原类型，
// 否则「只改个名字」可能把类型改坏。优先级：URL（带协议前缀）→ 存在的
// 目录 → .exe/.com/.bat/.cmd/.lnk → 存在的文件 → 像路径但不存在的文件。
// 不像路径（无分隔符/无盘符）又判断不出的（裸命令、域名）也返回 nullopt。
std::optional<core::ItemType> inferItemType(const std::wstring& target);

// 类型的中文名（预览提示行用）
const wchar_t* itemTypeDisplayName(core::ItemType type);

}  // namespace ui
}  // namespace ktile
