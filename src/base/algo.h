// KTile - C++17 兼容层：算法工具
//
// 提供 HLaunch 中使用 C++20 <ranges> 的等效替代。
// 避免在业务代码里到处写冗长的迭代器循环，同时保持 C++17 可编译。
//
// 设计要点：
//  - 全部为薄封装，行为对齐 std::ranges 对应算法
//  - 返回索引而非迭代器（业务侧更常需要索引）
//  - 不使用 C++17 之后的特性
//
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace ktile {
namespace base {

// ---------------------------------------------------------------- 查找

// 在 [begin,end) 中查找首个满足谓词的元素，返回迭代器
template <typename It, typename Pred>
It findIf(It begin, It end, Pred pred) {
    for (; begin != end; ++begin) {
        if (pred(*begin)) {
            return begin;
        }
    }
    return end;
}

// 返回首个满足谓词的元素索引，未找到返回 size
template <typename Container, typename Pred>
std::size_t findIndexIf(const Container& container, Pred pred) {
    const std::size_t size = container.size();
    for (std::size_t i = 0; i < size; ++i) {
        if (pred(container[i])) {
            return i;
        }
    }
    return size;
}

// 在容器中查找指定成员值，返回索引
template <typename Container, typename T, typename Member>
std::size_t findIndexByMember(const Container& container, const T& wanted, Member member) {
    const std::size_t size = container.size();
    for (std::size_t i = 0; i < size; ++i) {
        if (container[i].*member == wanted) {
            return i;
        }
    }
    return size;
}

// 在容器中查找指定值，返回索引
template <typename Container, typename T>
std::size_t findIndex(const Container& container, const T& wanted) {
    const std::size_t size = container.size();
    for (std::size_t i = 0; i < size; ++i) {
        if (container[i] == wanted) {
            return i;
        }
    }
    return size;
}

// ---------------------------------------------------------------- 判定

// 任意元素满足谓词
template <typename Container, typename Pred>
bool anyOf(const Container& container, Pred pred) {
    for (const auto& element : container) {
        if (pred(element)) {
            return true;
        }
    }
    return false;
}

// 所有元素满足谓词
template <typename Container, typename Pred>
bool allOf(const Container& container, Pred pred) {
    for (const auto& element : container) {
        if (!pred(element)) {
            return false;
        }
    }
    return true;
}

// 不存在元素满足谓词
template <typename Container, typename Pred>
bool noneOf(const Container& container, Pred pred) {
    return !anyOf(container, pred);
}

// 包含指定值
template <typename Container, typename T>
bool contains(const Container& container, const T& value) {
    return std::find(container.begin(), container.end(), value) != container.end();
}

// ---------------------------------------------------------------- 排序

// 稳定排序（按投影键），对齐 std::ranges::stable_sort
template <typename Container, typename Key>
void stableSortBy(Container& container, Key key) {
    std::stable_sort(container.begin(), container.end(),
                     [&key](const typename Container::value_type& left,
                            const typename Container::value_type& right) {
                         return key(left) < key(right);
                     });
}

// 排序（按投影键）
template <typename Container, typename Key>
void sortBy(Container& container, Key key) {
    std::sort(container.begin(), container.end(),
              [&key](const typename Container::value_type& left,
                     const typename Container::value_type& right) {
                  return key(left) < key(right);
              });
}

// 按比较器排序
template <typename Container, typename Compare>
void sortWith(Container& container, Compare compare) {
    std::sort(container.begin(), container.end(), compare);
}

// 稳定排序（按比较器）
template <typename Container, typename Compare>
void stableSortWith(Container& container, Compare compare) {
    std::stable_sort(container.begin(), container.end(), compare);
}

// ---------------------------------------------------------------- 变换/过滤

// 映射：生成新容器（保留顺序）
template <typename Result, typename Container, typename Mapper>
std::vector<Result> transform(const Container& container, Mapper mapper) {
    std::vector<Result> out;
    out.reserve(container.size());
    for (const auto& element : container) {
        out.push_back(mapper(element));
    }
    return out;
}

// 过滤：生成新容器
template <typename Container, typename Pred>
std::vector<typename Container::value_type> filter(const Container& container, Pred pred) {
    std::vector<typename Container::value_type> out;
    out.reserve(container.size());
    for (const auto& element : container) {
        if (pred(element)) {
            out.push_back(element);
        }
    }
    return out;
}

// ---------------------------------------------------------------- 极值

// 按投影键求最小值元素索引，空容器返回 size
template <typename Container, typename Key>
std::size_t minIndexBy(const Container& container, Key key) {
    const std::size_t size = container.size();
    if (size == 0) {
        return size;
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i < size; ++i) {
        if (key(container[i]) < key(container[best])) {
            best = i;
        }
    }
    return best;
}

// 按投影键求最大值元素索引，空容器返回 size
template <typename Container, typename Key>
std::size_t maxIndexBy(const Container& container, Key key) {
    const std::size_t size = container.size();
    if (size == 0) {
        return size;
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i < size; ++i) {
        if (key(container[best]) < key(container[i])) {
            best = i;
        }
    }
    return best;
}

// ---------------------------------------------------------------- 去重

// 判断值是否在容器内唯一出现（用于 ID 冲突检测）
template <typename Container, typename T>
bool countOccurrences(const Container& container, const T& value) {
    std::size_t count = 0;
    for (const auto& element : container) {
        if (element == value) {
            ++count;
        }
    }
    return count;
}

}  // namespace base
}  // namespace ktile
