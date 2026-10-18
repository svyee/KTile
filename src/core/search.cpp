#include "core/search.h"

#include <cctype>

namespace ktile {
namespace core {
namespace {

// 只处理 ASCII 的空白裁剪（同 item_operations.cpp 的说明：
// 逐字节裁剪多字节空白会破坏 UTF-8）。
std::string trimAscii(const std::string& value) {
    auto isSpace = [](const unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
               ch == '\v' || ch == '\f';
    };

    std::size_t begin = 0;
    while (begin < value.size() &&
           isSpace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           isSpace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

// 单字节小写化。
//
// 只对 ASCII 生效 —— 中文没有大小写概念，多字节序列保持原样才能保证
// 字节级匹配不会错位（若粗暴地对每个字节做 tolower，会把中文字节改坏）。
char asciiLower(const char ch) {
    const unsigned char value = static_cast<unsigned char>(ch);
    if (value < 0x80U) {
        return static_cast<char>(std::tolower(value));
    }
    return ch;
}

// 大小写不敏感的 ASCII 子串查找，返回字节偏移（未找到返回 npos）。
//
// 为什么不用 std::search + 自定义谓词：这里要的是「位置」，
// 而且要保持与「不做大小写折叠时」完全相同的字节长度语义
// （ASCII 折叠不会改变长度，所以位置可以直接用于高亮切片）。
std::size_t findAsciiInsensitive(const std::string& haystack,
                                 const std::string& needle) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return std::string::npos;
    }
    const std::size_t last = haystack.size() - needle.size();
    for (std::size_t start = 0; start <= last; ++start) {
        std::size_t offset = 0;
        while (offset < needle.size() &&
               asciiLower(haystack[start + offset]) ==
                   asciiLower(needle[offset])) {
            ++offset;
        }
        if (offset == needle.size()) {
            return start;
        }
    }
    return std::string::npos;
}

}  // namespace

std::string itemDisplayName(const LaunchItem& item) {
    return item.name.empty() ? item.target : item.name;
}

std::string normalizeSearchQuery(const std::string& rawQuery) {
    return trimAscii(rawQuery);
}

std::vector<SearchMatch> searchItems(const std::vector<LaunchItem>& items,
                                     const std::string& query) {
    std::vector<SearchMatch> results;
    results.reserve(items.size());

    const std::string normalized = normalizeSearchQuery(query);

    // 空查询：全部返回，不标高亮
    if (normalized.empty()) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            SearchMatch match;
            match.itemIndex = i;
            match.matchStart = std::string::npos;
            match.matchLength = 0;
            results.push_back(match);
        }
        return results;
    }

    for (std::size_t i = 0; i < items.size(); ++i) {
        const LaunchItem& item = items[i];
        const std::string name = itemDisplayName(item);

        std::size_t position = findAsciiInsensitive(name, normalized);

        // 名字没命中时再试 target —— 用户可能记得路径片段
        std::size_t targetPosition = std::string::npos;
        if (position == std::string::npos && !item.target.empty() &&
            item.target != name) {
            targetPosition = findAsciiInsensitive(item.target, normalized);
        }

        if (position == std::string::npos && targetPosition == std::string::npos) {
            continue;  // 不匹配，跳过
        }

        SearchMatch match;
        match.itemIndex = i;
        if (position != std::string::npos) {
            match.matchStart = position;
            match.matchLength = normalized.size();
        } else {
            // 命中在 target 上：名字里没有可高亮的位置，
            // 但仍要出现在结果里（高亮留给名字为空的情况）
            match.matchStart = std::string::npos;
            match.matchLength = 0;
        }
        results.push_back(match);
    }

    return results;
}

}  // namespace core
}  // namespace ktile
