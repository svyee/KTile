// KTile - 标签页操作测试（新建/改名/删除/调序/跨页搬移）
//
// 这些是「多标签页」能力的核心不变量：
//   - 页 id 全文档唯一（否则「记住上次在哪一页」会指错页）
//   - 至少留一页（否则面板没有任何内容可显示）
//   - 页名规范化后不能为空（否则标签条上是一片点不中的空白）
//   - 同一份 items.json 里允许两页放同一个程序（分标的本质）
#include "core/tab_operations.h"

#include <cstdio>
#include <string>

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

static LaunchItem makeItem(const char* id, const char* name,
                           const char* target) {
    return LaunchItem(id, ItemType::Application, name, target);
}

// 两页文档：第一页 2 项，第二页 1 项（含与第一页相同的目标）
static ItemsDocument makeDocument() {
    ItemsDocument document;
    auto& first = document.ensureDefaultTab();
    first.id = "default";
    first.name = "常用";
    first.items.push_back(makeItem("i1", "记事本", "C:\\notepad.exe"));
    first.items.push_back(makeItem("i2", "计算器", "C:\\calc.exe"));

    document.tabs.push_back(Tab("tab-1", "工具"));
    document.tabs[1].items.push_back(
        makeItem("i3", "记事本", "C:\\notepad.exe"));  // 与第一页同目标
    return document;
}

// ---------------------------------------------------------------- 新建

static void testAddTabAppendsAndNames() {
    ItemsDocument document = makeDocument();
    const std::size_t index = addTab(document, "游戏");
    CHECK(index == 2);
    CHECK(document.tabs.size() == 3);
    CHECK(document.tabs[2].name == "游戏");
    CHECK(!document.tabs[2].id.empty());
}

static void testAddTabAutomaticName() {
    ItemsDocument document = makeDocument();
    const std::size_t index = addTab(document, "   ");  // 空白 → 自动取名
    CHECK(document.tabs[index].name == "页面 3");
}

static void testAddTabAutomaticNameAvoidsDuplicate() {
    ItemsDocument document = makeDocument();
    document.tabs.push_back(Tab("tab-9", "页面 3"));  // 名字已被占
    const std::size_t index = addTab(document, "");
    CHECK(document.tabs[index].name == "页面 4");
}

static void testAddTabIdIsUnique() {
    ItemsDocument document = makeDocument();
    // 手工塞一个与自动编号会撞车的 id
    document.tabs.push_back(Tab("tab-3", "手工页"));
    const std::size_t index = addTab(document, "新页");
    CHECK(document.tabs[index].id != "tab-3");
    // 全文档唯一性
    for (std::size_t i = 0; i < document.tabs.size(); ++i) {
        for (std::size_t j = i + 1; j < document.tabs.size(); ++j) {
            CHECK(document.tabs[i].id != document.tabs[j].id);
        }
    }
}

static void testAddTabTruncatesLongName() {
    ItemsDocument document = makeDocument();
    const std::string longName(600, 'x');
    const std::size_t index = addTab(document, longName);
    CHECK(document.tabs[index].name.size() <= kMaximumNameLength);
}

static void testAddTabIntoEmptyDocument() {
    ItemsDocument document;
    const std::size_t index = addTab(document, "第一页");
    CHECK(index == 0);
    CHECK(document.tabs.size() == 1);
}

// ---------------------------------------------------------------- 改名

static void testRenameBasic() {
    ItemsDocument document = makeDocument();
    CHECK(renameTab(document, 1, "开发工具"));
    CHECK(document.tabs[1].name == "开发工具");
    CHECK(document.tabs[0].name == "常用");  // 其他页不受影响
}

static void testRenameTrimsWhitespace() {
    ItemsDocument document = makeDocument();
    CHECK(renameTab(document, 0, "  办公  "));
    CHECK(document.tabs[0].name == "办公");
}

static void testRenameRejectsEmpty() {
    ItemsDocument document = makeDocument();
    CHECK(!renameTab(document, 0, "   "));
    CHECK(document.tabs[0].name == "常用");  // 保持原名
}

static void testRenameSameValueReturnsFalse() {
    ItemsDocument document = makeDocument();
    CHECK(!renameTab(document, 0, "常用"));
}

static void testRenameInvalidIndex() {
    ItemsDocument document = makeDocument();
    CHECK(!renameTab(document, 9, "越界"));
}

// ---------------------------------------------------------------- 删除

static void testRemoveTab() {
    ItemsDocument document = makeDocument();
    CHECK(removeTab(document, 1));
    CHECK(document.tabs.size() == 1);
    // 删掉的是第二页，第一页与它的磁贴原样保留
    CHECK(document.tabs[0].name == "常用");
    CHECK(document.tabs[0].items.size() == 2);
}

static void testRemoveLastTabIsRefused() {
    ItemsDocument document = makeDocument();
    CHECK(removeTab(document, 1));
    CHECK(!removeTab(document, 0));  // 只剩一页：不可再删
    CHECK(document.tabs.size() == 1);
}

static void testRemoveInvalidIndex() {
    ItemsDocument document = makeDocument();
    CHECK(!removeTab(document, 5));
}

// ---------------------------------------------------------------- 调序

static void testMoveTab() {
    ItemsDocument document = makeDocument();
    CHECK(moveTab(document, 0, 1));
    CHECK(document.tabs[0].name == "工具");
    CHECK(document.tabs[1].name == "常用");
}

static void testMoveTabKeepsContents() {
    ItemsDocument document = makeDocument();
    CHECK(moveTab(document, 1, 0));
    CHECK(document.tabs[0].items.size() == 1);
    CHECK(document.tabs[1].items.size() == 2);
}

static void testMoveTabNoOpAndInvalid() {
    ItemsDocument document = makeDocument();
    CHECK(!moveTab(document, 1, 1));   // 原地不动
    CHECK(!moveTab(document, 0, 7));   // 越界
    CHECK(!moveTab(document, 7, 0));   // 越界
}

// ---------------------------------------------------------------- 跨页搬移

static void testMoveItemToTab() {
    ItemsDocument document = makeDocument();
    CHECK(moveItemToTab(document, 0, 0, 1));  // 记事本：第一页 → 工具页
    CHECK(document.tabs[0].items.size() == 1);
    CHECK(document.tabs[1].items.size() == 2);
    // 搬过去的是原来那一项（连 id 一起），不是复制
    CHECK(document.tabs[1].items[1].id == "i1");
    CHECK(document.tabs[0].items[0].id == "i2");
}

static void testMoveItemToTabKeepsGridPosition() {
    ItemsDocument document = makeDocument();
    document.tabs[0].items[0].setGridPosition(3, 2);
    CHECK(moveItemToTab(document, 0, 0, 1));
    // 保留坐标：布局引擎遇到冲突会就近避让，比清掉坐标更符合预期
    CHECK(document.tabs[1].items[1].hasGridPosition());
    CHECK(document.tabs[1].items[1].gridColumn.value() == 3);
    CHECK(document.tabs[1].items[1].gridRow.value() == 2);
}

static void testMoveItemToTabRejectsSameTab() {
    ItemsDocument document = makeDocument();
    CHECK(!moveItemToTab(document, 0, 0, 0));
    CHECK(document.tabs[0].items.size() == 2);
}

static void testMoveItemToTabInvalidIndices() {
    ItemsDocument document = makeDocument();
    CHECK(!moveItemToTab(document, 0, 9, 1));  // 项越界
    CHECK(!moveItemToTab(document, 0, 0, 9));  // 目标页越界
    CHECK(!moveItemToTab(document, 9, 0, 0));  // 源页越界
}

// ---------------------------------------------------------------- 分标（核心诉求）

static void testSameTargetAllowedInTwoTabs() {
    ItemsDocument document = makeDocument();
    // 两页都放着记事本 —— 这正是多标签页存在的意义
    const std::string firstTarget = document.tabs[0].items[0].target;
    const std::string secondTarget = document.tabs[1].items[0].target;
    CHECK(firstTarget == secondTarget);
    // 但 id 必须不同（id 才是唯一标识）
    CHECK(document.tabs[0].items[0].id != document.tabs[1].items[0].id);
}

// ---------------------------------------------------------------- 名字工具

static void testNormalizeTabName() {
    CHECK(normalizeTabName("  x  ") == "x");
    CHECK(normalizeTabName("   ") == "");
    CHECK(isUsableTabName("工具"));
    CHECK(!isUsableTabName("\t\n"));
}

int main() {
    testAddTabAppendsAndNames();
    testAddTabAutomaticName();
    testAddTabAutomaticNameAvoidsDuplicate();
    testAddTabIdIsUnique();
    testAddTabTruncatesLongName();
    testAddTabIntoEmptyDocument();

    testRenameBasic();
    testRenameTrimsWhitespace();
    testRenameRejectsEmpty();
    testRenameSameValueReturnsFalse();
    testRenameInvalidIndex();

    testRemoveTab();
    testRemoveLastTabIsRefused();
    testRemoveInvalidIndex();

    testMoveTab();
    testMoveTabKeepsContents();
    testMoveTabNoOpAndInvalid();

    testMoveItemToTab();
    testMoveItemToTabKeepsGridPosition();
    testMoveItemToTabRejectsSameTab();
    testMoveItemToTabInvalidIndices();

    testSameTargetAllowedInTwoTabs();

    testNormalizeTabName();

    if (g_failures == 0) {
        wprintf(L"===== TAB OPERATIONS: ALL PASSED =====\n");
        return 0;
    }
    wprintf(L"===== TAB OPERATIONS: %d FAILURES =====\n", g_failures);
    return g_failures;
}
