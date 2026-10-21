// KTile - 磁贴项目操作测试（尺寸/重命名/删除）
#include "core/item_operations.h"

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

static ItemsDocument makeDocument() {
    ItemsDocument document;
    auto& tab = document.ensureDefaultTab();
    tab.id = "default";
    tab.name = "常用";

    LaunchItem a("1", ItemType::Application, "记事本", "C:\\notepad.exe");
    a.tileSize = TileSize::Small;
    a.setGridPosition(0, 0);

    LaunchItem b("2", ItemType::Application, "计算器", "C:\\calc.exe");
    b.tileSize = TileSize::Large;
    b.setGridPosition(3, 1);

    LaunchItem c("3", ItemType::Folder, "文档", "C:\\Users");
    c.tileSize = TileSize::Tall;
    c.setGridPosition(6, 2);

    tab.items.push_back(a);
    tab.items.push_back(b);
    tab.items.push_back(c);
    return document;
}

// ---------------------------------------------------------------- 改名

static void testRenameBasic() {
    ItemsDocument document = makeDocument();
    CHECK(renameItem(document, 0, 0, "新名字"));
    CHECK(document.tabs[0].items[0].name == "新名字");
    // 其他项不受影响
    CHECK(document.tabs[0].items[1].name == "计算器");
}

static void testRenameTrimsWhitespace() {
    ItemsDocument document = makeDocument();
    CHECK(renameItem(document, 0, 0, "  留白  "));
    CHECK(document.tabs[0].items[0].name == "留白");
}

static void testRenameRejectsBlank() {
    ItemsDocument document = makeDocument();
    // 纯空白 → 拒绝，保持原名
    CHECK(!renameItem(document, 0, 0, "    "));
    CHECK(document.tabs[0].items[0].name == "记事本");

    CHECK(!renameItem(document, 0, 0, ""));
    CHECK(document.tabs[0].items[0].name == "记事本");

    CHECK(!isUsableItemName("   "));
    CHECK(isUsableItemName("ok"));
}

static void testRenameSameValueReturnsFalse() {
    ItemsDocument document = makeDocument();
    // 与原名相同（含空白差异）→ 视为未变化
    CHECK(!renameItem(document, 0, 0, "记事本"));
    CHECK(!renameItem(document, 0, 0, "  记事本  "));
}

static void testRenameTruncatesLongName() {
    ItemsDocument document = makeDocument();
    const std::string huge(kMaximumNameLength + 200, 'x');
    CHECK(renameItem(document, 0, 0, huge));
    CHECK(document.tabs[0].items[0].name.size() == kMaximumNameLength);
}

static void testRenameTruncatesOnUtf8Boundary() {
    ItemsDocument document = makeDocument();
    // 每个中文字符 3 字节。kMaximumNameLength(256) 不是 3 的倍数，
    // 若直接按字节 substr 会切出一个残缺的多字节字符。
    std::string name;
    while (name.size() + 3 <= kMaximumNameLength + 2) {
        name += "\xE4\xB8\xAD";  // "中"
    }
    CHECK(name.size() > kMaximumNameLength);   // 前提：确实超长
    CHECK(renameItem(document, 0, 0, name));

    const std::string& result = document.tabs[0].items[0].name;
    CHECK(result.size() <= kMaximumNameLength);
    CHECK(!result.empty());

    // 校验结果是**完整**的 UTF-8 序列：按 UTF-8 规则逐字符前进，
    // 必须恰好走到结尾（中途断开就会越界或走不完）。
    std::size_t pos = 0;
    bool valid = true;
    while (pos < result.size()) {
        const unsigned char lead = static_cast<unsigned char>(result[pos]);
        std::size_t length = 0;
        if (lead < 0x80U) {
            length = 1;
        } else if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
        } else {
            valid = false;
            break;
        }
        if (pos + length > result.size()) {
            valid = false;  // 尾部残缺
            break;
        }
        for (std::size_t k = 1; k < length; ++k) {
            if ((static_cast<unsigned char>(result[pos + k]) & 0xC0U) != 0x80U) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            break;
        }
        pos += length;
    }
    CHECK(valid);
    // 截断后应当只保留完整的「中」字，共 85 个（85*3 = 255 <= 256）
    CHECK(result.size() % 3 == 0);
}

static void testRenameInvalidIndex() {
    ItemsDocument document = makeDocument();
    CHECK(!renameItem(document, 0, 99, "x"));
    CHECK(!renameItem(document, 5, 0, "x"));   // tab 越界
}

// ---------------------------------------------------------------- 尺寸

static void testResizeChangesSizeAndClearsPosition() {
    ItemsDocument document = makeDocument();
    // 第 0 项是 Small at (0,0)，改成 Large
    CHECK(setItemTileSize(document, 0, 0, TileSize::Large));
    CHECK(document.tabs[0].items[0].tileSize == TileSize::Large);
    // 关键：坐标必须被清掉，否则 Large 会与邻居重叠
    CHECK(!document.tabs[0].items[0].hasGridPosition());

    // 其他项的坐标不受影响
    CHECK(document.tabs[0].items[1].hasGridPosition());
    CHECK(*document.tabs[0].items[1].gridColumn == 3);
}

static void testResizeSameValueReturnsFalse() {
    ItemsDocument document = makeDocument();
    // 第 0 项本来就是 Small
    CHECK(!setItemTileSize(document, 0, 0, TileSize::Small));
    // 坐标不该被动过
    CHECK(document.tabs[0].items[0].hasGridPosition());
}

static void testResizeAllSizes() {
    // 第 0 项原本是 Small，因此改 Small 会返回 false（无变化），
    // 这里只验证「改成不同尺寸」的三种情况。
    const TileSize sizes[] = {TileSize::Wide, TileSize::Tall, TileSize::Large};
    for (const auto size : sizes) {
        ItemsDocument document = makeDocument();
        CHECK(setItemTileSize(document, 0, 0, size));
        CHECK(document.tabs[0].items[0].tileSize == size);
    }

    // 改回 Small（从 Large 起）也应成功
    ItemsDocument document = makeDocument();
    CHECK(setItemTileSize(document, 0, 1, TileSize::Small));  // 第 1 项是 Large
    CHECK(document.tabs[0].items[1].tileSize == TileSize::Small);
}

static void testResizeInvalidIndex() {
    ItemsDocument document = makeDocument();
    CHECK(!setItemTileSize(document, 0, 99, TileSize::Large));
    CHECK(!setItemTileSize(document, 3, 0, TileSize::Large));
}

// ---------------------------------------------------------------- 删除

static void testRemoveItem() {
    ItemsDocument document = makeDocument();
    const std::size_t before = document.tabs[0].items.size();

    CHECK(removeItem(document, 0, 1));  // 删掉「计算器」
    CHECK(document.tabs[0].items.size() == before - 1);
    CHECK(document.tabs[0].items[0].name == "记事本");
    CHECK(document.tabs[0].items[1].name == "文档");
}

static void testRemoveItemKeepsOthersPosition() {
    ItemsDocument document = makeDocument();
    CHECK(removeItem(document, 0, 1));
    // 其余磁贴坐标不变（不重排）—— 避免「删一块全体位移」的跳变
    CHECK(*document.tabs[0].items[0].gridColumn == 0);
    CHECK(*document.tabs[0].items[0].gridRow == 0);
    CHECK(*document.tabs[0].items[1].gridColumn == 6);
    CHECK(*document.tabs[0].items[1].gridRow == 2);
}

static void testRemoveItemInvalidIndex() {
    ItemsDocument document = makeDocument();
    CHECK(!removeItem(document, 0, 99));
    CHECK(!removeItem(document, 7, 0));
    CHECK(document.tabs[0].items.size() == 3);
}

static void testRemoveAllItems() {
    ItemsDocument document = makeDocument();
    CHECK(removeItem(document, 0, 0));
    CHECK(removeItem(document, 0, 0));
    CHECK(removeItem(document, 0, 0));
    CHECK(document.tabs[0].items.empty());
    CHECK(!removeItem(document, 0, 0));
}

// ---------------------------------------------------------------- 编辑

static void testEditUpdatesAllFields() {
    ItemsDocument document = makeDocument();

    ItemEditFields fields;
    fields.name = "  浏览器  ";
    fields.type = ItemType::Url;
    fields.target = "  https://example.com  ";
    fields.arguments = {" --private ", "", "  --start-maximized "};
    fields.workingDirectory = "C:\\temp";
    fields.icon = "D:\\icons\\web.ico";
    fields.runAsAdministrator = true;

    CHECK(editItem(document, 0, 0, fields));

    const LaunchItem& item = document.tabs[0].items[0];
    CHECK(item.name == "浏览器");
    CHECK(item.type == ItemType::Url);
    CHECK(item.target == "https://example.com");
    CHECK(item.arguments.size() == 2);       // 空行被丢弃
    CHECK(item.arguments[0] == "--private");
    CHECK(item.arguments[1] == "--start-maximized");
    CHECK(item.workingDirectory.has_value() &&
          *item.workingDirectory == "C:\\temp");
    CHECK(item.icon.has_value() && *item.icon == "D:\\icons\\web.ico");
    CHECK(item.runAsAdministrator);

    // 磁贴属性不受编辑影响
    CHECK(item.tileSize == TileSize::Small);
    CHECK(item.hasGridPosition());
}

static void testEditRejectsEmptyTarget() {
    ItemsDocument document = makeDocument();
    const std::string originalTarget = document.tabs[0].items[0].target;

    ItemEditFields fields;
    fields.target = "   ";   // 全空白
    CHECK(!editItem(document, 0, 0, fields));
    CHECK(document.tabs[0].items[0].target == originalTarget);
}

static void testEditEmptyNameClearsName() {
    ItemsDocument document = makeDocument();

    ItemEditFields fields;
    fields.name = "   ";     // 名称留空 = 用目标名
    fields.target = "C:\\newpath.exe";

    CHECK(editItem(document, 0, 0, fields));
    CHECK(document.tabs[0].items[0].name.empty());
    CHECK(document.tabs[0].items[0].target == "C:\\newpath.exe");
}

static void testEditEmptyOptionalClearsThem() {
    ItemsDocument document = makeDocument();
    document.tabs[0].items[0].workingDirectory = "C:\\old";
    document.tabs[0].items[0].icon = "D:\\old.ico";

    ItemEditFields fields;
    fields.name = "记事本";
    fields.target = "C:\\notepad.exe";   // 其余全空

    CHECK(editItem(document, 0, 0, fields));
    CHECK(!document.tabs[0].items[0].workingDirectory.has_value());
    CHECK(!document.tabs[0].items[0].icon.has_value());
    CHECK(!document.tabs[0].items[0].runAsAdministrator);
}

static void testEditNoChangeReturnsFalse() {
    ItemsDocument document = makeDocument();

    // 填入与现值完全一致的字段 → false
    ItemEditFields fields;
    const LaunchItem& item = document.tabs[0].items[2];  // 文档（Folder）
    fields.name = item.name;
    fields.type = item.type;
    fields.target = item.target;
    CHECK(!editItem(document, 0, 2, fields));
}

// 编辑对话框里的「磁贴大小」下拉：改尺寸必须清坐标（否则可能与邻座重叠）
static void testEditTileSizeChangesAndClearsPosition() {
    ItemsDocument document = makeDocument();

    ItemEditFields fields;
    fields.target = document.tabs[0].items[0].target;  // 目标不变，只改尺寸
    fields.tileSize = TileSize::Large;

    CHECK(editItem(document, 0, 0, fields));

    const LaunchItem& item = document.tabs[0].items[0];
    CHECK(item.tileSize == TileSize::Large);
    CHECK(!item.hasGridPosition());   // 坐标被清掉，等布局引擎重排
}

// 尺寸没变时不该动坐标 —— 「只改名字」不能顺手把磁贴挪走
static void testEditSameTileSizeKeepsPosition() {
    ItemsDocument document = makeDocument();

    const LaunchItem& before = document.tabs[0].items[2];
    ItemEditFields fields;
    fields.name = before.name;
    fields.type = before.type;
    fields.target = before.target;
    fields.tileSize = before.tileSize;  // 与现值相同

    CHECK(!editItem(document, 0, 2, fields));   // 全无变化 → false
    const LaunchItem& item = document.tabs[0].items[2];
    CHECK(item.tileSize == TileSize::Tall);
    CHECK(item.hasGridPosition());
}

// 不带 tileSize（nullopt）时按「不动尺寸」处理，兼容旧调用方
static void testEditWithoutTileSizeLeavesItAlone() {
    ItemsDocument document = makeDocument();

    ItemEditFields fields;
    fields.target = "C:\\renamed.exe";
    fields.name = "新名字";

    CHECK(editItem(document, 0, 1, fields));
    const LaunchItem& item = document.tabs[0].items[1];
    CHECK(item.tileSize == TileSize::Large);
    CHECK(item.hasGridPosition());
    CHECK(item.name == "新名字");
}

// 「每块磁贴单独控制标题」：改 labelMode 应生效，且不动位置与尺寸
static void testEditLabelModeChanges() {
    ItemsDocument document = makeDocument();

    const LaunchItem& before = document.tabs[0].items[1];
    ItemEditFields fields;
    fields.name = before.name;
    fields.type = before.type;
    fields.target = before.target;
    fields.labelMode = TileLabelMode::OnHover;

    CHECK(editItem(document, 0, 1, fields));
    const LaunchItem& item = document.tabs[0].items[1];
    CHECK(item.labelMode == TileLabelMode::OnHover);
    // 只改标题显示方式 → 位置和尺寸都该原封不动
    CHECK(item.hasGridPosition());
    CHECK(item.tileSize == TileSize::Large);
    // 其他磁贴不受影响
    CHECK(document.tabs[0].items[0].labelMode == TileLabelMode::FollowGlobal);
}

// 三种取值都能存取（枚举 ↔ JSON 字符串是对称的）
static void testEditAllLabelModes() {
    ItemsDocument document = makeDocument();

    const TileLabelMode modes[3] = {TileLabelMode::Always,
                                    TileLabelMode::OnHover,
                                    TileLabelMode::FollowGlobal};
    for (const TileLabelMode mode : modes) {
        const LaunchItem& before = document.tabs[0].items[0];
        ItemEditFields fields;
        fields.name = before.name;
        fields.type = before.type;
        fields.target = before.target;
        fields.labelMode = mode;

        // 第一次一定变（默认是 FollowGlobal，三种里两种不同）；
        // 第二次同值应返回 false
        const bool first = editItem(document, 0, 0, fields);
        CHECK(document.tabs[0].items[0].labelMode == mode);
        if (mode != TileLabelMode::FollowGlobal) {
            CHECK(first);
            CHECK(!editItem(document, 0, 0, fields));
        }
    }
}

// 不带 labelMode（nullopt）→ 别把用户单独设过的值冲掉
static void testEditWithoutLabelModeLeavesItAlone() {
    ItemsDocument document = makeDocument();
    document.tabs[0].items[1].labelMode = TileLabelMode::OnHover;

    const LaunchItem& before = document.tabs[0].items[1];
    ItemEditFields fields;
    fields.name = "计算器（改名）";
    fields.type = before.type;
    fields.target = before.target;

    CHECK(editItem(document, 0, 1, fields));
    CHECK(document.tabs[0].items[1].name == "计算器（改名）");
    CHECK(document.tabs[0].items[1].labelMode == TileLabelMode::OnHover);
}

// 「每块磁贴自定义背景色」：三层语义（不改 / 清除 / 指定）都要成立。
// 外层 optional 表达「这次编辑是否处理颜色」，内层 optional 才是值本身 ——
// 少了这层，「清除自定义色」与「没动过」就无法区分。
static void testEditTileColorSetAndClear() {
    ItemsDocument document = makeDocument();

    const LaunchItem& before = document.tabs[0].items[1];
    ItemEditFields fields;
    fields.name = before.name;
    fields.type = before.type;
    fields.target = before.target;

    // 指定颜色
    fields.tileColor = std::optional<std::optional<std::uint32_t>>(
        std::optional<std::uint32_t>(0xFF112233U));
    CHECK(editItem(document, 0, 1, fields));
    CHECK(document.tabs[0].items[1].tileColor.has_value());
    CHECK(*document.tabs[0].items[1].tileColor == 0xFF112233U);

    // 同值再提交 → 无变化
    CHECK(!editItem(document, 0, 1, fields));

    // 清除（回到全局默认 / 皮肤）
    fields.tileColor = std::optional<std::optional<std::uint32_t>>(
        std::optional<std::uint32_t>());
    CHECK(editItem(document, 0, 1, fields));
    CHECK(!document.tabs[0].items[1].tileColor.has_value());

    // 再次清除 → 已经是空，无变化
    CHECK(!editItem(document, 0, 1, fields));
}

// 不带 tileColor（外层 nullopt）→ 别把用户单独设过的颜色冲掉
static void testEditWithoutTileColorLeavesItAlone() {
    ItemsDocument document = makeDocument();
    document.tabs[0].items[1].tileColor = 0xFF445566U;

    const LaunchItem& before = document.tabs[0].items[1];
    ItemEditFields fields;
    fields.name = "计算器（改名）";
    fields.type = before.type;
    fields.target = before.target;

    CHECK(editItem(document, 0, 1, fields));
    CHECK(document.tabs[0].items[1].tileColor.has_value());
    CHECK(*document.tabs[0].items[1].tileColor == 0xFF445566U);
}

static void testEditInvalidIndex() {
    ItemsDocument document = makeDocument();
    ItemEditFields fields;
    fields.target = "C:\\x.exe";
    CHECK(!editItem(document, 0, 99, fields));
    CHECK(!editItem(document, 7, 0, fields));
}

// ---------------------------------------------------------------- 新增

static void testAddItemAppendsAndClearsPosition() {
    ItemsDocument document = makeDocument();

    LaunchItem item("9", ItemType::Application, "画图", "C:\\mspaint.exe");
    // 故意带上坐标：新增必须无视它（否则从别处抄来的坐标会叠在别人身上）
    item.setGridPosition(4, 4);
    item.tileSize = TileSize::Wide;

    std::size_t index = 999;
    CHECK(addItem(document, 0, item, &index));
    CHECK(index == 3);
    CHECK(document.tabs[0].items.size() == 4);
    CHECK(document.tabs[0].items[3].name == "画图");
    CHECK(document.tabs[0].items[3].target == "C:\\mspaint.exe");
    CHECK(document.tabs[0].items[3].tileSize == TileSize::Wide);
    CHECK(!document.tabs[0].items[3].hasGridPosition());
}

static void testAddItemNormalizesNameAndTarget() {
    ItemsDocument document = makeDocument();

    LaunchItem item("10", ItemType::Application, "  画图  ", "  C:\\mspaint.exe  ");
    item.arguments = {"  /x  ", "", "   ", "/y"};
    item.workingDirectory = "   ";
    item.icon = "  ";

    CHECK(addItem(document, 0, item));
    const LaunchItem& added = document.tabs[0].items[3];
    CHECK(added.name == "画图");
    CHECK(added.target == "C:\\mspaint.exe");
    CHECK(added.arguments.size() == 2);
    CHECK(added.arguments[0] == "/x");
    CHECK(added.arguments[1] == "/y");
    // 空白即「不指定」
    CHECK(!added.workingDirectory.has_value());
    CHECK(!added.icon.has_value());
}

static void testAddItemAllowsEmptyName() {
    ItemsDocument document = makeDocument();
    // 名称留空是合法的：显示层回落到 target（与 HLaunch 一致）
    LaunchItem item("11", ItemType::Folder, "", "C:\\Users");
    CHECK(addItem(document, 0, item));
    CHECK(document.tabs[0].items[3].name.empty());
    CHECK(document.tabs[0].items[3].target == "C:\\Users");
}

static void testAddItemRejectsEmptyTarget() {
    ItemsDocument document = makeDocument();
    LaunchItem blank("12", ItemType::Application, "空目标", "   ");
    CHECK(!addItem(document, 0, blank));
    CHECK(document.tabs[0].items.size() == 3);
}

static void testAddItemRejectsDuplicateId() {
    ItemsDocument document = makeDocument();

    // 与同页面已有项撞 id
    LaunchItem sameTab("1", ItemType::Application, "重名", "C:\\x.exe");
    CHECK(!addItem(document, 0, sameTab));

    // 与**别的页面**已有项撞 id 也不行 —— id 是跨页面唯一标识
    document.tabs.push_back(Tab("tools", "工具"));
    LaunchItem otherTab("1", ItemType::Application, "另一个页面", "C:\\y.exe");
    CHECK(!addItem(document, 1, otherTab));

    // 空 id 同样拒绝
    LaunchItem noId("", ItemType::Application, "无 id", "C:\\z.exe");
    CHECK(!addItem(document, 0, noId));

    CHECK(document.tabs[0].items.size() == 3);
    CHECK(document.tabs[1].items.empty());
}

static void testAddItemInvalidTab() {
    ItemsDocument document = makeDocument();
    LaunchItem item("20", ItemType::Application, "x", "C:\\x.exe");
    CHECK(!addItem(document, 7, item));
    CHECK(!addItem(document, 1, item));  // 只有一个页面
}

static void testAddItemIntoEmptyDocument() {
    ItemsDocument document;
    CHECK(!addItem(document, 0, LaunchItem("1", ItemType::Application, "a",
                                           "C:\\a.exe")));
    // ensureDefaultTab 之后才能加（调用方负责保证页面存在）
    document.ensureDefaultTab();
    CHECK(addItem(document, 0, LaunchItem("1", ItemType::Application, "a",
                                          "C:\\a.exe")));
    CHECK(document.totalItemCount() == 1);
}

static void testAddItemKeepsExistingItemsUntouched() {
    ItemsDocument document = makeDocument();
    CHECK(addItem(document, 0, LaunchItem("21", ItemType::Application, "新的",
                                          "C:\\new.exe")));
    // 老磁贴的坐标与顺序都不动 —— 新增不该把用户摆好的布局弄乱
    CHECK(*document.tabs[0].items[0].gridColumn == 0);
    CHECK(*document.tabs[0].items[0].gridRow == 0);
    CHECK(*document.tabs[0].items[1].gridColumn == 3);
    CHECK(*document.tabs[0].items[2].gridColumn == 6);
}

int wmain() {
    testRenameBasic();
    testRenameTrimsWhitespace();
    testRenameRejectsBlank();
    testRenameSameValueReturnsFalse();
    testRenameTruncatesLongName();
    testRenameTruncatesOnUtf8Boundary();
    testRenameInvalidIndex();

    testResizeChangesSizeAndClearsPosition();
    testResizeSameValueReturnsFalse();
    testResizeAllSizes();
    testResizeInvalidIndex();

    testRemoveItem();
    testRemoveItemKeepsOthersPosition();
    testRemoveItemInvalidIndex();
    testRemoveAllItems();

    testAddItemAppendsAndClearsPosition();
    testAddItemNormalizesNameAndTarget();
    testAddItemAllowsEmptyName();
    testAddItemRejectsEmptyTarget();
    testAddItemRejectsDuplicateId();
    testAddItemInvalidTab();
    testAddItemIntoEmptyDocument();
    testAddItemKeepsExistingItemsUntouched();

    testEditUpdatesAllFields();
    testEditRejectsEmptyTarget();
    testEditEmptyNameClearsName();
    testEditEmptyOptionalClearsThem();
    testEditNoChangeReturnsFalse();
    testEditTileSizeChangesAndClearsPosition();
    testEditSameTileSizeKeepsPosition();
    testEditWithoutTileSizeLeavesItAlone();
    testEditLabelModeChanges();
    testEditAllLabelModes();
    testEditWithoutLabelModeLeavesItAlone();
    testEditTileColorSetAndClear();
    testEditWithoutTileColorLeavesItAlone();
    testEditInvalidIndex();

    if (g_failures == 0) {
        wprintf(L"===== ITEM OPERATIONS: ALL PASSED =====\n");
        return 0;
    }
    wprintf(L"===== ITEM OPERATIONS: %d FAILURES =====\n", g_failures);
    return g_failures;
}
