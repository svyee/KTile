// KTile - UI 层：项目编辑对话框（「修改项目」/「添加新项目」共用）
//
// 参考自 HLaunch 的 item_editor_dialog（同仓库 参考/hlaunch-0.1.22），
// 但按 KTile 的约定重新实现：
//   - 不带 .rc 资源，控件全部代码生成（与重命名/设置窗口同一套路）
//   - 不用 DialogBoxParam 模板 + DWLP_USER，用自注册窗口类 + GWLP_USERDATA
//   - 布局为「左侧图标/磁贴预览 + 右侧字段」的 620x430 客户区
//
// 同一个窗口承担两种用途（见 ItemEditorMode）：
//   「修改项目」从磁贴右键进，字段预填该磁贴的现值；
//   「添加新项目」从空白处右键进，字段全空、尺寸取默认值。
//
// 几处刻意的「简化」：
//   1. 没有「类型」下拉框 —— 类型由目标路径推断（ui::inferItemType）。
//      用户不需要理解「应用/文件/文件夹/网址」的差别，程序自己判断。
//   2. 没有图标路径输入框 —— 顶部就是图标预览，点一下即可换图；
//      选中的图片按原比例缩放到最长边 128 存进 data\res
//      （见 platform/win32/icon_store），磁贴上会整块铺满。
//   3. 磁贴大小改成下拉 + 实时预览，改完保存即生效（不必再回右键菜单）。
//   4. 标题显示方式也是下拉：跟随全局 / 始终显示 / 移入时显示 ——
//      每块磁贴自己说了算，因为小磁贴和大磁贴的最优解常常不一样。
//
// 主窗口保持可见：对话框以主窗口为 owner，OutsideClickMonitor 的
// owner 链判定会把点在对话框上的点击当作「窗口内」，不会隐藏主窗口。
//
#pragma once

#include "core/data_model.h"

#include <Windows.h>

#include <optional>
#include <string>
#include <vector>

namespace ktile {
namespace ui {

// 对话框提交的字段集合（宽字符版本，直接对接 UI；
// Application 层负责转 UTF-8 后交给 core::editItem）。
struct ItemEditorFields {
    std::wstring name;                 // 允许为空（空 = 用目标名推导）

    // 类型不由用户填写：对话框按目标推断后回填；推断不出时保留这里的初值。
    core::ItemType type = core::ItemType::Application;

    std::wstring target;
    std::vector<std::wstring> arguments;  // 每行一条
    std::wstring workingDirectory;     // 允许为空

    // 自定义图标。存的是相对 data 目录的路径（"res\xx.png"），
    // 空 = 使用目标自身的图标。
    std::wstring icon;

    bool runAsAdministrator = false;
    std::size_t tabIndex = 0;          // 所属页面
    core::TileSize tileSize = core::TileSize::Small;  // 磁贴尺寸

    // 标题显示方式（每项独立控制，见 core::TileLabelMode）
    core::TileLabelMode labelMode = core::TileLabelMode::FollowGlobal;

    // 图标相对磁贴的显示比例（%）。0 = 按统一设置（设置窗口里的滑块），
    // 否则夹在 50~100 之间，只影响方框图标，铺满型自定义图片不受限。
    int iconScalePercent = 0;
};

// 对话框用途。
//
// 「添加新项目」与「修改项目」是同一批字段、同一套校验、同一个预览，
// 唯一差别是标题与确认按钮的文案（外加保存后是插入还是改写）。
// 因此不另做一个窗口 —— 两份几乎一样的窗口代码必然各自演化，
// 改了一边忘了另一边，用户就会看到「同一个界面对话框，两处规则不一致」。
enum class ItemEditorMode : std::uint8_t {
    Edit = 0,    // 修改已有项目
    Create = 1,  // 新建项目
};

// 弹出「编辑项目」对话框。
//
// tabNames   ：所有页面的显示名（下拉框内容）
// initialTab ：当前磁贴所在的页面索引
// initial    ：现有字段值（新建时全空，名称/目标留空即可）
// globalAlwaysShowLabels ：设置里的「默认显示标题」当前值。
//                          预览要按它来决定画不画标题 —— 选了
//                          「跟随全局设置」时，只有知道全局值才能
//                          预览出真实效果。
// globalIconScale        ：设置里的「图标大小」当前值（%）。
//                          勾选框选「按统一设置」时，滑块显示它作参照。
// outResult  ：用户点「保存」时写出新值
// mode       ：新建 / 修改，只影响标题与按钮文案（见 ItemEditorMode）
//
// 返回 true 表示用户确认；取消/关闭返回 false。
// 期间主窗口保持可见（owner 关系保证点击判定）。
bool showItemEditorDialog(HWND owner,
                          const std::vector<std::wstring>& tabNames,
                          std::size_t initialTab,
                          const ItemEditorFields& initial,
                          bool globalAlwaysShowLabels,
                          int globalIconScale,
                          ItemEditorFields* outResult,
                          ItemEditorMode mode = ItemEditorMode::Edit);

// 目标路径 → 类型推断。
//
// 返回 nullopt 表示「根据目标判断不出来」—— 调用方应保留原类型，
// 否则「只想改个名字」会把类型改坏。
//
// 判断依据（按优先级）：
//   - URL（带 http:// 等协议前缀）        → Url
//   - 存在的目录                          → Folder
//   - .exe/.com/.bat/.cmd/.lnk            → Application
//   - 存在的文件                          → File
//   - 不像路径（无分隔符/无盘符）且判断不出 → nullopt
//     （例如 "notepad"、"office.farxin.cn" —— 可能是裸命令、域名或
//       待创建的文件，硬猜会改坏右键菜单文案与图标）
//   - 其余（像路径但不存在的文件）          → File
std::optional<core::ItemType> inferItemType(const std::wstring& target);

// 类型的中文名（预览提示行用）
const wchar_t* itemTypeDisplayName(core::ItemType type);

}  // namespace ui
}  // namespace ktile
