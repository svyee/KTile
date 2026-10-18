// KTile - 核心层：搜索过滤（纯逻辑，无平台依赖）
//
// 设计取舍：
//   - 大小写不敏感 + 子串匹配（不是前缀匹配）—— 用户记得「浏览器」但未必
//     记得开头是「浏」，子串匹配命中率更高
//   - 支持拼音首字母？暂不做 —— 需要拼音表，体积代价大。
//     先把子串匹配做扎实，后续真需要再单独加一层索引。
//   - 不排序、不评分：保持原有网格顺序。磁贴是空间记忆，位置一变用户就懵了。
//
// 过滤结果用「下标集合」表达，而不是拷贝一份 LaunchItem —— 避免每敲一个
// 字符就复制整个文档，也保证过滤结果能直接映射回原索引。
//
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

// 在给定项目列表上执行搜索。
//
// query 为空（或全空白）时返回全部项目，且不标记高亮 ——
// 这样「清空搜索框」等价于「恢复完整列表」，调用方无需分支处理。
//
// displayName 用于匹配的文本由调用方提供（通常是 item.name，为空时回退
// item.target），本函数只做匹配，不关心名字从哪来。
std::vector<SearchMatch> searchItems(
    const std::vector<LaunchItem>& items,
    const std::string& query);

// 规范化查询串：裁掉首尾空白。空结果表示「无查询」。
std::string normalizeSearchQuery(const std::string& rawQuery);

// 取名：name 非空用 name，否则回退 target。
// 抽成函数是为了让 UI 与搜索用同一套规则，避免「显示的名字」与
// 「能搜到的名字」不一致。
std::string itemDisplayName(const LaunchItem& item);

}  // namespace core
}  // namespace ktile
