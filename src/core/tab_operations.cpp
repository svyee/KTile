// KTile - 核心层：标签页操作实现
#include "core/tab_operations.h"

#include "core/text_normalize.h"

#include <algorithm>

namespace ktile {
namespace core {
namespace {

// 生成一个全文档唯一的标签页 id。
//
// 形如 "tab-3"：可读、便于手改 items.json 时对照。逐个试到不冲突为止 ——
// 页数是个位数到几十的量级，线性试探的开销可以忽略，
// 而比「随机 UUID」多一个好处：调试时一眼能看出新旧。
//
// 之所以要防冲突：id 是「记住上次在哪一页」的持久化依据，
// 也是将来可能的页级引用键。手工编辑过的 items.json 里完全可能
// 出现与自动编号撞车的 id。
std::string makeUniqueTabId(const ItemsDocument& document) {
    for (std::size_t suffix = 1;; ++suffix) {
        std::string candidate = "tab-" + std::to_string(suffix);
        if (document.findTabIndex(candidate) == document.tabs.size()) {
            // findTabIndex 找不到时返回 tabs.size()，即「未占用」
            return candidate;
        }
    }
}

// 自动页名：第一个未被占用的「页面 N」。
//
// 之所以不直接用「页数 + 1」：删掉中间某页后再新建会出现重名，
// 用户看着两个「页面 3」会分不清哪个是新的。
std::string automaticTabName(const ItemsDocument& document) {
    for (std::size_t suffix = document.tabs.size() + 1;; ++suffix) {
        const std::string candidate = "页面 " + std::to_string(suffix);
        const bool taken = std::any_of(
            document.tabs.begin(), document.tabs.end(),
            [&candidate](const Tab& tab) { return tab.name == candidate; });
        if (!taken) {
            return candidate;
        }
    }
}

}  // namespace

std::string normalizeTabName(const std::string& rawName) {
    return truncateUtf8(trimAscii(rawName), kMaximumNameLength);
}

bool isUsableTabName(const std::string& rawName) {
    return !normalizeTabName(rawName).empty();
}

std::size_t addTab(ItemsDocument& document, const std::string& rawName) {
    Tab tab;
    tab.id = makeUniqueTabId(document);

    const std::string normalized = normalizeTabName(rawName);
    tab.name = normalized.empty() ? automaticTabName(document) : normalized;

    document.tabs.push_back(std::move(tab));
    return document.tabs.size() - 1;
}

bool renameTab(ItemsDocument& document, const std::size_t index,
               const std::string& rawName) {
    if (index >= document.tabs.size()) {
        return false;
    }

    const std::string normalized = normalizeTabName(rawName);
    if (normalized.empty()) {
        return false;  // 空名字不接受：标签条上会变成一片点不中的空白
    }
    if (document.tabs[index].name == normalized) {
        return false;
    }

    document.tabs[index].name = normalized;
    return true;
}

bool removeTab(ItemsDocument& document, const std::size_t index) {
    if (index >= document.tabs.size()) {
        return false;
    }
    // 至少留一页：面板必须有内容可显示
    if (document.tabs.size() <= 1) {
        return false;
    }

    document.tabs.erase(document.tabs.begin() +
                        static_cast<std::ptrdiff_t>(index));
    return true;
}

bool moveTab(ItemsDocument& document, const std::size_t from,
             const std::size_t to) {
    if (from >= document.tabs.size() || to >= document.tabs.size()) {
        return false;
    }
    if (from == to) {
        return false;
    }

    Tab moved = std::move(document.tabs[from]);
    document.tabs.erase(document.tabs.begin() +
                        static_cast<std::ptrdiff_t>(from));
    document.tabs.insert(document.tabs.begin() +
                             static_cast<std::ptrdiff_t>(to),
                         std::move(moved));
    return true;
}

bool moveItemToTab(ItemsDocument& document, const std::size_t fromTab,
                   const std::size_t itemIndex, const std::size_t toTab) {
    if (fromTab >= document.tabs.size() || toTab >= document.tabs.size()) {
        return false;
    }
    if (fromTab == toTab) {
        return false;  // 原地搬家没有意义
    }
    auto& sourceItems = document.tabs[fromTab].items;
    if (itemIndex >= sourceItems.size()) {
        return false;
    }

    LaunchItem moved = std::move(sourceItems[itemIndex]);
    sourceItems.erase(sourceItems.begin() +
                      static_cast<std::ptrdiff_t>(itemIndex));
    document.tabs[toTab].items.push_back(std::move(moved));
    return true;
}

}  // namespace core
}  // namespace ktile
