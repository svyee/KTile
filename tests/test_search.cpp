// KTile - 搜索过滤测试
#include "core/search.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace ktile::core;

static int g_failures = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            wprintf(L"FAIL line %d: %s\n", __LINE__, L#cond);    \
            ++g_failures;                                        \
        }                                                        \
    } while (false)

// ---------------------------------------------------------------- 辅助

static std::vector<LaunchItem> makeItems() {
    std::vector<LaunchItem> items;
    items.push_back(LaunchItem("1", ItemType::Application, "记事本",
                               "C:\\Windows\\System32\\notepad.exe"));
    items.push_back(LaunchItem("2", ItemType::Application, "计算器",
                               "C:\\Windows\\System32\\calc.exe"));
    items.push_back(LaunchItem("3", ItemType::Application, "Chrome 浏览器",
                               "C:\\Program Files\\chrome.exe"));
    items.push_back(LaunchItem("4", ItemType::Folder, "用户文档",
                               "C:\\Users"));
    items.push_back(LaunchItem("5", ItemType::Application, "Visual Studio Code",
                               "C:\\Program Files\\Microsoft VS Code\\Code.exe"));
    return items;
}

static bool contains(const std::vector<SearchMatch>& results,
                     const std::size_t index) {
    for (const auto& match : results) {
        if (match.itemIndex == index) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- 基础

static void testEmptyQueryReturnsAll() {
    const auto items = makeItems();
    const auto results = searchItems(items, "");
    CHECK(results.size() == items.size());
    // 空查询不标高亮
    for (const auto& match : results) {
        CHECK(match.matchStart == std::string::npos);
    }
}

static void testWhitespaceQueryIsEmpty() {
    const auto items = makeItems();
    const auto results = searchItems(items, "   \t  ");
    CHECK(results.size() == items.size());  // 纯空白等同无查询
    CHECK(normalizeSearchQuery("  hi  ") == "hi");
    CHECK(normalizeSearchQuery("   ").empty());
}

static void testSubstringMatch() {
    const auto items = makeItems();
    // "记" 命中「记事本」
    const auto results = searchItems(items, "记");
    CHECK(results.size() == 1);
    CHECK(results[0].itemIndex == 0);
    CHECK(results[0].matchStart == 0);
    CHECK(results[0].matchLength == std::string("记").size());
}

static void testChineseSubstringNotAtStart() {
    const auto items = makeItems();
    // "本" 在「记事本」的第 3 个字符位置（UTF-8 下第 6 字节）
    const auto results = searchItems(items, "本");
    CHECK(results.size() == 1);
    CHECK(results[0].itemIndex == 0);
    CHECK(results[0].matchStart == 6);  // 两个中文字符 = 6 字节
}

static void testAsciiCaseInsensitive() {
    const auto items = makeItems();
    // 小写 chrome 应命中 "Chrome 浏览器"
    const auto lower = searchItems(items, "chrome");
    CHECK(contains(lower, 2));

    // 全大写也应命中
    const auto upper = searchItems(items, "CHROME");
    CHECK(contains(upper, 2));

    // 混合大小写
    const auto mixed = searchItems(items, "ChRoMe");
    CHECK(contains(mixed, 2));
}

static void testMultipleMatches() {
    const auto items = makeItems();
    // "o" 出现在多条目标里（notepad / chrome / code / Users...）
    const auto results = searchItems(items, "o");
    // 至少命中记事本(target 有 o) 与 chrome
    CHECK(contains(results, 0) || contains(results, 2));
    CHECK(results.size() >= 2);
}

static void testNoMatch() {
    const auto items = makeItems();
    const auto results = searchItems(items, "zzzz不存在zzzz");
    CHECK(results.empty());
}

static void testMatchInTargetOnly() {
    const auto items = makeItems();
    // "calc" 只在 target 里，不在名字「计算器」里
    const auto results = searchItems(items, "calc");
    CHECK(results.size() == 1);
    CHECK(results[0].itemIndex == 1);
    // 命中在 target 上：名字里没有高亮位置
    CHECK(results[0].matchStart == std::string::npos);
}

static void testDisplayNameFallback() {
    LaunchItem named("1", ItemType::Application, "有名字", "C:\\x.exe");
    CHECK(itemDisplayName(named) == "有名字");

    LaunchItem unnamed("2", ItemType::Application, "", "C:\\y.exe");
    CHECK(itemDisplayName(unnamed) == "C:\\y.exe");
}

static void testOrderPreserved() {
    const auto items = makeItems();
    // 结果必须保持原有顺序（磁贴位置是空间记忆，不能因搜索而重排）
    const auto results = searchItems(items, "o");
    for (std::size_t i = 1; i < results.size(); ++i) {
        CHECK(results[i - 1].itemIndex < results[i].itemIndex);
    }
}

static void testEmptyItems() {
    const std::vector<LaunchItem> empty;
    CHECK(searchItems(empty, "x").empty());
    CHECK(searchItems(empty, "").empty());
}

static void testMatchHighlightsCorrectRange() {
    const auto items = makeItems();
    // "Visual" 在 "Visual Studio Code" 的开头
    const auto results = searchItems(items, "Visual");
    CHECK(contains(results, 4));
    for (const auto& match : results) {
        if (match.itemIndex == 4) {
            CHECK(match.matchStart == 0);
            CHECK(match.matchLength == std::string("Visual").size());
        }
    }
}

static void testPartialWordMatch() {
    const auto items = makeItems();
    // "Studio" 是中间词
    const auto results = searchItems(items, "Studio");
    CHECK(contains(results, 4));
    for (const auto& match : results) {
        if (match.itemIndex == 4) {
            // "Visual " = 7 字节
            CHECK(match.matchStart == 7);
        }
    }
}

int wmain() {
    testEmptyQueryReturnsAll();
    testWhitespaceQueryIsEmpty();
    testSubstringMatch();
    testChineseSubstringNotAtStart();
    testAsciiCaseInsensitive();
    testMultipleMatches();
    testNoMatch();
    testMatchInTargetOnly();
    testDisplayNameFallback();
    testOrderPreserved();
    testEmptyItems();
    testMatchHighlightsCorrectRange();
    testPartialWordMatch();

    if (g_failures == 0) {
        wprintf(L"===== SEARCH: ALL PASSED =====\n");
        return 0;
    }
    wprintf(L"===== SEARCH: %d FAILURES =====\n", g_failures);
    return g_failures;
}
