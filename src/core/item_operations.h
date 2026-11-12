// KTile - 核心层：磁贴项目操作（纯逻辑，无平台依赖）
//
// 每个函数都是对 ItemsDocument 的一次原子修改，不碰 UI 与磁盘。
// 单独成模块是为了可单测；落盘与通知重绘由 Application 负责。
//
#pragma once

#include "core/data_model.h"

#include <cstddef>
#include <optional>
#include <string>

namespace ktile {
namespace core {

// 修改某个磁贴的尺寸。
// 尺寸变化会改变占据的格子数，必须清掉网格坐标让布局引擎重排，
// 否则 2x2 可能挪进只剩 1 格空间的位置。
// 返回 true 表示尺寸确实变了。
bool setItemTileSize(ItemsDocument& document,
                     std::size_t tabIndex,
                     std::size_t itemIndex,
                     TileSize size);

// 重命名磁贴。裁掉首尾空白（用户手输容易带空格），裁完为空则拒绝并
// 保持原名，超过 kMaximumNameLength 按上限截断而不是拒绝。
// 返回 true 表示名字确实变了。
bool renameItem(ItemsDocument& document,
                std::size_t tabIndex,
                std::size_t itemIndex,
                const std::string& newName);

// 删除磁贴。其余磁贴不重排，空缺就空缺：删一块后全体位移会让人找不到位置。
// 返回 true 表示删除成功。
bool removeItem(ItemsDocument& document,
                std::size_t tabIndex,
                std::size_t itemIndex);

// 新增一块磁贴，追加到指定页面末尾。拖入文件与右键「添加新项目」共用。
// tabIndex 非法、target 规范化后为空、id 为空或与任意页面重复时返回 false：
// id 是跨会话的稳定标识，撞了就分不清图标缓存与启动统计。
// name 可为空，显示层回落到 target。一律清掉网格坐标，由布局引擎安置。
// outIndex 非空时写出新磁贴下标。返回 true 表示确实插入。
bool addItem(ItemsDocument& document,
             std::size_t tabIndex,
             LaunchItem item,
             std::size_t* outIndex = nullptr);

// 「修改项目」对话框提交的字段集合。只含编辑框与预览区里能改的东西，
// 不含网格坐标与统计，对话框不动它们。
struct ItemEditFields {
    // 显示名。留空表示由目标名推导。
    std::string name;

    // 类型由目标推断（见 ui::inferItemType），推断不出时填回原类型，
    // 避免「只是改个名字」把类型改坏。
    ItemType type = ItemType::Application;

    std::string target;

    // 启动参数，每行一条。提交时已裁空白、丢弃空行。
    std::vector<std::string> arguments;

    // 空串表示未设置（映射到 LaunchItem 里的 optional）。
    std::string workingDirectory;

    // 自定义图标，存相对 data 目录的路径（"res\xx.png"）；空串用目标自身图标。
    std::string icon;

    bool runAsAdministrator = false;

    // 有值表示对话框改过尺寸。改尺寸必须清网格坐标等重排（同 setItemTileSize）。
    std::optional<TileSize> tileSize;

    // 标题显示方式（跟随全局 / 始终显示 / 移入时显示）。
    // 用 optional 是为了让别的调用方不填时不动原设置，不把用户的选择重置成默认。
    std::optional<TileLabelMode> labelMode;

    // 图标相对磁贴的显示比例（%）。0 = 跟随全局，50~100 = 本项覆盖。
    std::optional<int> iconScalePercent;

    // 磁贴背景色（ARGB），与对话框「背景色」一行严格对应。
    // nullopt = 本次不改动；内层为空 = 清除自定义色；内层有值 = 指定颜色。
    std::optional<std::optional<std::uint32_t>> tileColor;
};

// 把对话框提交的字段写回某个磁贴。target 规范化后为空则拒绝，name 为空
// 则清空 item.name，workingDirectory/icon 为空则清掉 optional。
// tileSize 有值且与当前不同则改尺寸并清网格坐标，等布局引擎重排。
// 不触碰 gridColumn/gridRow（改尺寸除外）与 launchCount。返回 true 表示有字段变了。
bool editItem(ItemsDocument& document,
              std::size_t tabIndex,
              std::size_t itemIndex,
              const ItemEditFields& fields);

// 裁掉首尾空白并截断到上限，供 UI 层预览用。
std::string normalizeItemName(const std::string& rawName);

// 名字是否可用（规范化后非空）。
bool isUsableItemName(const std::string& rawName);

}  // namespace core
}  // namespace ktile
