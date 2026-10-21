// KTile - 核心层：磁贴项目操作（纯逻辑，无平台依赖）
//
// 这些操作都是「对 ItemsDocument 的一次原子修改」，不涉及 UI 与磁盘。
// 之所以单独放一个模块而不是塞在 Application 里：
//   1. 可以单测 —— 改尺寸是否重排、删除是否搬走坐标、重命名是否去空白
//   2. Application 只负责「拿到结果 → 落盘 → 通知 UI 重绘」
//
#pragma once

#include "core/data_model.h"

#include <cstddef>
#include <optional>
#include <string>

namespace ktile {
namespace core {

// 修改某个磁贴的尺寸。
//
// 尺寸变化会改变它占据的格子数，因此必须清掉网格坐标让布局引擎重排 ——
// 否则会出现「2x2 挪到只有 1 格空间的位置」这种重叠。
//
// 返回 true 表示确实发生了变化（尺寸原本就相同则返回 false）。
bool setItemTileSize(ItemsDocument& document,
                     std::size_t tabIndex,
                     std::size_t itemIndex,
                     TileSize size);

// 重命名磁贴。
//
// 规则：
//   - 首尾空白一律裁掉（用户手输容易带空格）
//   - 裁完为空则拒绝（保持原名字，返回 false）
//   - 超过 kMaximumNameLength 的按上限截断，而不是拒绝
//
// 返回 true 表示名字确实变了。
bool renameItem(ItemsDocument& document,
                std::size_t tabIndex,
                std::size_t itemIndex,
                const std::string& newName);

// 删除磁贴。
//
// 删除后不重排其余磁贴：保留它们原有的网格坐标，空缺就空缺。
// 这是刻意的 —— 用户删除一块后，其他磁贴突然全体位移会让人找不到北。
// 返回 true 表示删除成功。
bool removeItem(ItemsDocument& document,
                std::size_t tabIndex,
                std::size_t itemIndex);

// 新增一块磁贴（追加到指定页面末尾）。
//
// 用于两条入口：把文件/文件夹拖进面板（快加），以及右键「添加新项目」。
//
// 规则：
//   - tabIndex 非法 → 返回 false
//   - target 规范化后为空 → 返回 false（没有目标的磁贴什么都启动不了）
//   - id 为空，或与**任意页面**已有的磁贴重复 → 返回 false
//     （id 是跨会话的稳定标识，撞了就分不清是谁的图标缓存/启动统计）
//   - name 可为空（显示层回落到 target），与 editItem 同一套规范化
//   - 一律清掉网格坐标：新磁贴的位置由布局引擎按空位安置，
//     否则「从别处复制来的 items.json 片段」会带着幽灵坐标叠在别人身上
//
// outIndex：非空时写出新磁贴的下标（同一页面内）。
// 返回 true 表示确实插入了。
bool addItem(ItemsDocument& document,
             std::size_t tabIndex,
             LaunchItem item,
             std::size_t* outIndex = nullptr);

// 「修改项目」对话框提交的字段集合。
//
// 与 LaunchItem 的区别：这里全是「编辑框（含预览区）里能改的东西」，
// 不含网格坐标与统计 —— 对话框不动它们。
struct ItemEditFields {
    // 显示名。规范化后允许为空 —— 留空表示用目标名推导（与 HLaunch 一致：
    // 对话框底部提示「名称留空时自动使用目标名称」）。
    std::string name;

    // 类型不由用户手选，而是由目标推断（见 ui::inferItemType）。
    // 推断不出时调用方应填回原类型，避免「只是改个名字」把类型改坏。
    ItemType type = ItemType::Application;

    std::string target;

    // 启动参数，每行一条。提交时已裁空白、丢弃空行。
    std::vector<std::string> arguments;

    // 空串表示未设置（映射到 LaunchItem 里的 optional）。
    std::string workingDirectory;

    // 自定义图标。存的是相对 data 目录的路径（"res\xx.png"），
    // 空串表示使用目标自身的图标。
    std::string icon;

    bool runAsAdministrator = false;

    // 磁贴尺寸。有值时表示对话框里改过尺寸 ——
    // 尺寸变化会改变占用的格子数，因此必须清掉网格坐标让布局引擎重排
    // （与 setItemTileSize 同一规则），否则会出现磁贴重叠。
    std::optional<TileSize> tileSize;

    // 标题显示方式（跟随全局 / 始终显示 / 移入时显示）。
    //
    // 同样用 optional：没有值时不动原设置，避免别的调用方顺手构造
    // ItemEditFields 时把用户单独设过的「移入时显示」悄悄重置成默认值。
    std::optional<TileLabelMode> labelMode;

    // 图标相对磁贴的显示比例（%）。0 = 跟随全局设置；
    // 50~100 = 本项覆盖。nullopt 时不动原值。
    std::optional<int> iconScalePercent;

    // 磁贴背景色（ARGB）。
    //
    // 三层语义，与对话框的「背景色」一行严格对应：
    //   nullopt            = 本次不改动（其它调用方可不填）
    //   有值但内层为空      = 清除自定义色，回到全局默认 / 皮肤
    //   有值且内层非空      = 显式指定这一块的颜色
    std::optional<std::optional<std::uint32_t>> tileColor;
};

// 把对话框提交的字段写回某个磁贴。
//
// 规则：
//   - target 规范化后为空 → 拒绝（返回 false），保持原样
//   - name 为空 → 清空 item.name，由显示层回落到 target
//   - workingDirectory/icon 为空 → 清掉 optional（对话框清空即「不指定」）
//   - tileSize 有值且与当前不同 → 改尺寸并清掉网格坐标（等布局引擎重排）
//   - labelMode 有值且与当前不同 → 改标题显示方式（不影响尺寸与位置）
//   - 不触碰 gridColumn / gridRow（除改尺寸那一支）/ launchCount
//
// 返回 true 表示至少有一个字段确实变了。
bool editItem(ItemsDocument& document,
              std::size_t tabIndex,
              std::size_t itemIndex,
              const ItemEditFields& fields);

// 规范化名字：裁掉首尾空白，超长截断。供 UI 层预览用。
std::string normalizeItemName(const std::string& rawName);

// 名字是否可用（规范化后非空）
bool isUsableItemName(const std::string& rawName);

}  // namespace core
}  // namespace ktile
