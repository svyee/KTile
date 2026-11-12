// KTile - 核心层：标签页操作（纯逻辑，无平台依赖）
//
// 每个函数都是对 ItemsDocument 的一次原子修改，不碰 UI 也不碰磁盘，
// Application 与 LauncherWindow 只负责「调它 → 落盘 → 重绘」。标签页
// 单独一个模块：它是磁贴的容器，不变量（至少一页、id 唯一、名字不空）
// 与磁贴的不同，混在一起容易互相牵连。
#pragma once

#include "core/data_model.h"

#include <cstddef>
#include <string>

namespace ktile {
namespace core {

// 新建标签页（追加到末尾），返回新页下标。rawName 裁空白后为空则自动
// 取名「页面 N」，超长截断，id 自动生成且不与任何已有页面重复。不做
// 重名检查：两页同名不影响功能。namePrefix 是自动取名的前缀，由调用方
// 按当前语言传入（核心层不依赖 i18n）。
std::size_t addTab(ItemsDocument& document, const std::string& rawName,
                   const std::string& namePrefix = "页面");

// 重命名标签页：裁空白、超长截断；裁完为空则拒绝（空白名字在标签条上
// 点不中），名字与原来相同返回 false（视为无变化）。
bool renameTab(ItemsDocument& document, std::size_t index,
               const std::string& rawName);

// 删除标签页（连同页内所有磁贴）。最后一个页面不可删：面板必须至少有
// 一页，否则整个窗口没有任何内容可显示。返回 false 表示拒绝。
bool removeTab(ItemsDocument& document, std::size_t index);

// 把标签页从 from 挪到 to（顺序调整）。
// from == to 或任一下标越界 → 返回 false。
bool moveTab(ItemsDocument& document, std::size_t from, std::size_t to);

// 把一个磁贴搬到另一个标签页（追加到目标页末尾）。保留网格坐标：布局
// 引擎遇到冲突会就近避让，比清坐标后随机扔到空位更符合预期。搬走后
// 源页空缺不补：与 removeItem 同一策略，避免其余磁贴集体位移。
bool moveItemToTab(ItemsDocument& document, std::size_t fromTab,
                   std::size_t itemIndex, std::size_t toTab);

// 把一个磁贴复制到另一个标签页（追加到目标页末尾），源页不动。复制件
// 内容与源相同但 id 必须是新的：id 是跨会话的稳定标识（图标缓存、启动
// 统计），共用会互相串。newId 由调用方生成，传空串视为拒绝。网格坐标
// 同样保留，冲突时由布局引擎就近避让。
bool copyItemToTab(ItemsDocument& document, std::size_t fromTab,
                   std::size_t itemIndex, std::size_t toTab,
                   const std::string& newId);

// 规范化标签页名字：裁首尾空白 + 超长截断。供 UI 层预览/校验用。
std::string normalizeTabName(const std::string& rawName);

// 标签页名字是否可用（规范化后非空）
bool isUsableTabName(const std::string& rawName);

}  // namespace core
}  // namespace ktile
