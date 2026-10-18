// KTile - 核心层：标签页操作（纯逻辑，无平台依赖）
//
// 与 item_operations 同一套设计：每个函数都是「对 ItemsDocument 的一次
// 原子修改」，不碰 UI 也不碰磁盘。Application 与 LauncherWindow 只负责
// 「调它 → 落盘 → 重绘」。
//
// 为什么标签页要单独一个模块：标签页是磁贴的**容器**，它的不变量
// （至少一页、id 唯一、名字不空）与磁贴自身的不变量完全不同，
// 混在一个文件里容易互相牵连。
//
#pragma once

#include "core/data_model.h"

#include <cstddef>
#include <string>

namespace ktile {
namespace core {

// 新建标签页（追加到末尾），返回新页下标。
//
// 规则：
//   - rawName 裁首尾空白后为空 → 自动取名「页面 N」（N = 现有页数 + 1）
//   - 超过 kMaximumNameLength 截断
//   - id 自动生成并保证不与**任何已有页面**重复（含手改过 items.json 的情况）
//
// 不做「重名检查」：两页都叫「工具」并不影响任何功能，而强行改名
// 反而会让用户觉得「我明明起的名字被改了」。
std::size_t addTab(ItemsDocument& document, const std::string& rawName);

// 重命名标签页。
//
// 规则同 addTab 的命名部分：裁空白、超长截断。
// 裁完为空 → 拒绝（返回 false，保持原名字）—— 空白名字在标签条上
// 是一片点不中的空白，必须挡住。
// 名字（规范化后）与原来相同 → 返回 false（视为无变化）。
bool renameTab(ItemsDocument& document, std::size_t index,
               const std::string& rawName);

// 删除标签页（连同页内所有磁贴）。
//
// 最后一个页面不可删：面板必须至少有一页，否则整个窗口没有任何内容
// 可显示，用户会以为程序坏了。返回 false 表示拒绝（越界或只剩一页）。
bool removeTab(ItemsDocument& document, std::size_t index);

// 把标签页从 from 挪到 to（顺序调整）。
// from == to 或任一下标越界 → 返回 false。
bool moveTab(ItemsDocument& document, std::size_t from, std::size_t to);

// 把一个磁贴搬到另一个标签页（追加到目标页末尾）。
//
// 保留网格坐标 —— 布局引擎遇到冲突会就近避让，比清掉坐标后
// 「随机扔到某个空位」更符合用户预期（他会希望它大概还在原来的位置）。
// 搬走之后源页的空缺不补：与 removeItem 同一策略，避免其余磁贴集体位移。
bool moveItemToTab(ItemsDocument& document, std::size_t fromTab,
                   std::size_t itemIndex, std::size_t toTab);

// 规范化标签页名字：裁首尾空白 + 超长截断。供 UI 层预览/校验用。
std::string normalizeTabName(const std::string& rawName);

// 标签页名字是否可用（规范化后非空）
bool isUsableTabName(const std::string& rawName);

}  // namespace core
}  // namespace ktile
