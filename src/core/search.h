// KTile - 核心层：搜索过滤（纯逻辑，无平台依赖）
//
// 大小写不敏感 + 子串匹配（用户记得「浏览器」但未必记得开头），不排序
// 不评分，保持网格顺序：磁贴是空间记忆，位置一变就认不出来。拼音首字母
// 暂不做，需要拼音表，真需要时再加一层索引。结果用下标集合表达，避免
// 每敲一个字符就复制整个文档，也保证结果能映射回原索引。
#pragma once

#include "core/data_model.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ktile {
namespace core {

// 单个磁贴的匹配结果
struct SearchMatch {
    std::size_t itemIndex = 0;
    // 命中位置（在「显示名」中的字节区间），用于高亮。
    // start == npos 表示没有命中（此时该项不应出现在结果里）。
    std::size_t matchStart = std::string::npos;
    std::size_t matchLength = 0;
};

// 在给定项目列表上执行搜索。query 为空（或全空白）时返回全部项目且不
// 标记高亮，这样「清空搜索框」等价于「恢复完整列表」，调用方无需分支。
// 匹配文本由调用方提供（通常是 item.name，为空时回退 item.target），
// 本函数只做匹配，不关心名字从哪来。
std::vector<SearchMatch> searchItems(
    const std::vector<LaunchItem>& items,
    const std::string& query);

// 规范化查询串：裁掉首尾空白。空结果表示「无查询」。
std::string normalizeSearchQuery(const std::string& rawQuery);

// 取名：name 非空用 name，否则回退 target。UI 与搜索共用一套规则，
// 避免「显示的名字」与「能搜到的名字」不一致。
std::string itemDisplayName(const LaunchItem& item);

}  // namespace core
}  // namespace ktile
