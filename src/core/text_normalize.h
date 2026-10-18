// KTile - 核心层：文本规范化小工具
//
// 抽出来的原因：磁贴名字与标签页名字用的是同一套规则（裁首尾空白、
// 超长按 UTF-8 边界截断）。之前这两个函数躲在 item_operations.cpp 的
// 匿名命名空间里，标签页要用就只能复制一份 —— 复制出来的两份迟早漂移
// （比如一处截断到 256 字节、另一处忘了截断）。
//
// 做成 header-only：两个函数都只有几行，内联进调用点没有代价，
// 也不必往构建脚本里再加一个实现文件。
//
#pragma once

#include <cstddef>
#include <string>

namespace ktile {
namespace core {

// 裁掉首尾的 ASCII 空白。中文全角空格（U+3000）在 UTF-8 下是
// 0xE3 0x80 0x80，属于多字节序列，逐字节裁剪会破坏编码，
// 因此这里只处理 ASCII 空白 —— 对用户输入而言已足够，
// 且绝不会产生非法 UTF-8。
inline std::string trimAscii(const std::string& value) {
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
    while (end > begin && isSpace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

// 按 UTF-8 字符边界截断到最多 maxBytes 字节。
//
// 直接 substr 可能把一个多字节字符切成两半，产生非法 UTF-8 并让
// 后续 JSON 写入/渲染出问题。这里回退到最近的字符起始字节。
inline std::string truncateUtf8(const std::string& value,
                                const std::size_t maxBytes) {
    if (value.size() <= maxBytes) {
        return value;
    }
    std::size_t cut = maxBytes;
    // UTF-8 续字节形如 10xxxxxx；往前退到非续字节即为字符起点
    while (cut > 0 &&
           (static_cast<unsigned char>(value[cut]) & 0xC0U) == 0x80U) {
        --cut;
    }
    return value.substr(0, cut);
}

}  // namespace core
}  // namespace ktile
