// KTile - 插件布局引擎测试
#include "core/plugin_layout.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
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

#define CHECK_NEAR(actual, expected, epsilon)                              \
    do {                                                                  \
        const double a_ = (actual);                                       \
        const double e_ = (expected);                                     \
        if (std::fabs(a_ - e_) > (epsilon)) {                             \
            wprintf(L"FAIL line %d: %ls = %.2f, want %.2f\n", __LINE__,   \
                    L#actual, a_, e_);                                    \
            ++g_failures;                                                 \
        }                                                                 \
    } while (false)

// 假测量：ASCII 字符宽度约为字号的 0.6 倍。
static float fakeMeasure(const std::string& text, float fontSize) {
    return static_cast<float>(text.size()) * fontSize * 0.6F;
}

static PluginValue makeRoot() {
    PluginValue root = PluginValue::makeObject();
    PluginValue current = PluginValue::makeObject();
    current.members.emplace_back("temp", PluginValue::makeNumber(23.4));
    current.members.emplace_back("city", PluginValue::makeString("Beijing"));
    current.members.emplace_back("icon", PluginValue::makeString("sun"));
    root.members.emplace_back("current", std::move(current));

    PluginValue days = PluginValue::makeArray();
    for (int index = 0; index < 3; ++index) {
        PluginValue day = PluginValue::makeObject();
        day.members.emplace_back(
            "date", PluginValue::makeString("11-0" + std::to_string(index + 3)));
        day.members.emplace_back("max", PluginValue::makeNumber(20.0 + index));
        day.members.emplace_back(
            "icon", PluginValue::makeString(index == 0 ? "sun" : "cloud"));
        days.elements.push_back(std::move(day));
    }
    root.members.emplace_back("forecast", std::move(days));
    return root;
}

static PluginManifest makeManifest() {
    PluginManifest manifest;
    manifest.id = "weather";
    manifest.icon = "cloud-sun";
    manifest.selectors.emplace_back("now", "current");
    manifest.selectors.emplace_back("days", "forecast");
    return manifest;
}

static PluginLayoutNode makeLeaf(const std::string& icon,
                                 const std::string& main,
                                 const std::string& sub) {
    PluginLayoutNode node;
    node.kind = PluginLayoutNode::Kind::Leaf;
    node.slots.icon = icon;
    node.slots.main = main;
    node.slots.sub = sub;
    return node;
}

static void testSingleLeaf() {
    PluginManifest manifest = makeManifest();
    PluginValue root = makeRoot();
    PluginLayoutNode layout = makeLeaf("${now.icon}", "${now.temp} C", "${now.city}");

    PluginDrawList list = buildPluginDrawList(manifest, root, layout, 0, 0, 200, 100,
                                              fakeMeasure);
    CHECK(!list.commands.empty());

    int textCount = 0;
    for (const auto& command : list.commands) {
        if (command.kind == PluginDrawKind::Text) {
            ++textCount;
        }
    }
    CHECK(textCount >= 1);

    bool sawMain = false;
    for (const auto& command : list.commands) {
        if (command.kind != PluginDrawKind::Text) {
            continue;
        }
        if (command.role == PluginTextRole::Main) {
            sawMain = true;
            CHECK(command.text == "23.4 C");
            CHECK(command.fontSize >= 9.0F && command.fontSize <= 26.0F);
        }
    }
    CHECK(sawMain);
}

static void testRowWeights() {
    PluginManifest manifest = makeManifest();
    PluginValue root = makeRoot();

    PluginLayoutNode layout;
    layout.kind = PluginLayoutNode::Kind::Container;
    layout.horizontal = true;
    PluginLayoutNode first = makeLeaf("", "${now.temp}", "");
    first.weight = 1;
    PluginLayoutNode second = makeLeaf("", "${now.city}", "");
    second.weight = 2;
    layout.children.push_back(first);
    layout.children.push_back(second);

    PluginDrawList list = buildPluginDrawList(manifest, root, layout, 10, 20, 200, 40,
                                              fakeMeasure);

    const PluginDrawCommand* left = nullptr;
    const PluginDrawCommand* right = nullptr;
    for (const auto& command : list.commands) {
        if (command.kind != PluginDrawKind::Text) {
            continue;
        }
        if (left == nullptr) {
            left = &command;
        } else if (right == nullptr) {
            right = &command;
        }
    }
    CHECK(left != nullptr && right != nullptr);
    if (left != nullptr && right != nullptr) {
        // 内边距 5：可用宽度 190 - gap 3 = 187；1:2 → 约 62.3 / 124.7
        CHECK_NEAR(left->right - left->left, 62.33, 1.0);
        CHECK_NEAR(right->right - right->left, 124.67, 1.0);
        CHECK_NEAR(right->left, left->right + 3.0, 0.1);
    }
}

static void testRepeatExpansion() {
    PluginManifest manifest = makeManifest();
    PluginValue root = makeRoot();

    PluginLayoutNode layout;
    layout.kind = PluginLayoutNode::Kind::Container;
    layout.horizontal = true;
    layout.hasRepeat = true;
    layout.repeatSource = "days";
    layout.repeatMap = "day";
    layout.repeatMax = 3;
    layout.children.push_back(makeLeaf("${day.icon}", "${day.max}", "${day.date}"));

    PluginDrawList list = buildPluginDrawList(manifest, root, layout, 0, 0, 120, 100,
                                              fakeMeasure);

    int icons = 0;
    int mains = 0;
    for (const auto& command : list.commands) {
        if (command.kind == PluginDrawKind::Icon) {
            ++icons;
            CHECK(command.icon == "sun" || command.icon == "cloud");
        }
        if (command.kind == PluginDrawKind::Text &&
            command.role == PluginTextRole::Main) {
            ++mains;
        }
    }
    CHECK(icons == 3);
    CHECK(mains == 3);
}

static void testRepeatMaxLimit() {
    PluginManifest manifest = makeManifest();
    PluginValue root = makeRoot();

    PluginLayoutNode layout;
    layout.kind = PluginLayoutNode::Kind::Container;
    layout.horizontal = true;
    layout.hasRepeat = true;
    layout.repeatSource = "days";
    layout.repeatMap = "day";
    layout.repeatMax = 2;
    layout.children.push_back(makeLeaf("${day.icon}", "${day.max}", ""));

    PluginDrawList list = buildPluginDrawList(manifest, root, layout, 0, 0, 120, 60,
                                              fakeMeasure);

    int icons = 0;
    for (const auto& command : list.commands) {
        if (command.kind == PluginDrawKind::Icon) {
            ++icons;
        }
    }
    CHECK(icons == 2);
}

static void testMeter() {
    PluginManifest manifest = makeManifest();
    PluginValue root = makeRoot();

    PluginLayoutNode layout = makeLeaf("", "${now.city}", "");
    layout.slots.hasMeter = true;
    layout.slots.meter.value = "50";
    layout.slots.meter.maximum = 100.0;
    layout.slots.meter.label = "load";
    layout.slots.meter.color = "#FF0000";

    PluginDrawList list = buildPluginDrawList(manifest, root, layout, 0, 0, 100, 80,
                                              fakeMeasure);
    int meters = 0;
    for (const auto& command : list.commands) {
        if (command.kind == PluginDrawKind::Meter) {
            ++meters;
            CHECK_NEAR(command.fraction, 0.5, 0.001);
            CHECK(command.color == 0xFFFF0000U);
        }
    }
    CHECK(meters == 1);
}

static void testTruncation() {
    PluginManifest manifest = makeManifest();
    PluginValue root = makeRoot();

    PluginLayoutNode layout = makeLeaf(
        "", "a very long string that cannot fit in a small tile", "");

    PluginDrawList list = buildPluginDrawList(manifest, root, layout, 0, 0, 40, 40,
                                              fakeMeasure);
    bool found = false;
    for (const auto& command : list.commands) {
        if (command.kind == PluginDrawKind::Text &&
            command.role == PluginTextRole::Main) {
            found = true;
            CHECK(command.text.size() < 55);
            CHECK(command.text.size() >= 3);
            const std::string tail = command.text.substr(command.text.size() - 3);
            CHECK(tail == "\xE2\x80\xA6");
        }
    }
    CHECK(found);
}

static void testMissingLayoutValuesAreSkipped() {
    PluginManifest manifest = makeManifest();
    PluginValue root = makeRoot();

    PluginLayoutNode layout = makeLeaf("", "${now.missing}", "${now.alsoMissing}");
    PluginDrawList list = buildPluginDrawList(manifest, root, layout, 0, 0, 100, 100,
                                              fakeMeasure);
    // 没有 icon 槽 + 文本全空 → 不产生任何绘制指令
    CHECK(list.commands.empty());
}

// 窄而矮的列（wide / large 的三列布局）里，图标 + 主 / 副 / 页脚四行
// 按基准字号放不下。必须整体缩小（或丢弃页脚），不能把页脚画到磁贴外。
static void testContentStaysInsideShortTile() {
    PluginManifest manifest = makeManifest();
    PluginValue root = makeRoot();

    PluginLayoutNode layout;
    layout.kind = PluginLayoutNode::Kind::Container;
    layout.horizontal = true;
    for (int index = 0; index < 3; ++index) {
        PluginLayoutNode leaf = makeLeaf("${now.icon}", "${now.temp}°",
                                         "${now.temp}°");
        leaf.slots.footer = "今天";
        layout.children.push_back(leaf);
    }

    // 复现现场尺寸：2x1 天气磁贴 134x64 DIP，三列各约 39 DIP 宽。
    const float tileWidth = 134.0F;
    const float tileHeight = 64.0F;
    // buildPluginDrawList 会内缩 5 DIP 避免文字贴边，内容必须停在内框里。
    const float kPadding = 5.0F;
    const PluginDrawList list = buildPluginDrawList(
        manifest, root, layout, 0, 0, tileWidth, tileHeight, fakeMeasure);

    // 必须带图标：无图标时只有三行文字，怎么排都放得下，
    // 测不出页脚被顶出磁贴的真实场景
    bool sawIcon = false;
    bool sawFooter = false;
    for (const PluginDrawCommand& command : list.commands) {
        CHECK(command.top >= kPadding - 0.01F);
        CHECK(command.bottom <= tileHeight - kPadding + 0.01F);
        CHECK(command.left >= kPadding - 0.01F);
        CHECK(command.right <= tileWidth - kPadding + 0.01F);
        if (command.kind == PluginDrawKind::Icon) {
            sawIcon = true;
        }
        if (command.kind == PluginDrawKind::Text &&
            command.role == PluginTextRole::Footer) {
            sawFooter = true;
        }
    }
    CHECK(sawIcon);
    // 页脚是日期标签（今天/明天/后天），缩小后仍应保留。
    CHECK(sawFooter);
}

// 2x2 大磁贴（134x134）有充足高度，主信息应比 wide 明显更大：
// 左列是放大的「今天」，右列上下排「明天 / 后天」。
static void testLargeLayoutUsesVerticalSpace() {
    PluginManifest manifest = makeManifest();
    PluginValue root = makeRoot();

    PluginLayoutNode today =
        makeLeaf("${now.icon}", "${now.temp}°", "${now.city}");
    today.slots.footer = "今天";

    PluginLayoutNode laterColumn;
    laterColumn.kind = PluginLayoutNode::Kind::Container;
    laterColumn.horizontal = false;
    for (int index = 1; index <= 2; ++index) {
        PluginLayoutNode day = makeLeaf(
            "${days." + std::to_string(index) + ".icon}",
            "${days." + std::to_string(index) + ".max}°",
            "${days." + std::to_string(index) + ".max}°");
        day.slots.footer = index == 1 ? "明天" : "后天";
        laterColumn.children.push_back(day);
    }

    PluginLayoutNode layout;
    layout.kind = PluginLayoutNode::Kind::Container;
    layout.horizontal = true;
    layout.children.push_back(today);
    layout.children.push_back(laterColumn);

    const float tileSize = 134.0F;
    const float kPadding = 5.0F;
    const PluginDrawList list = buildPluginDrawList(
        manifest, root, layout, 0, 0, tileSize, tileSize, fakeMeasure);

    float largestMain = 0.0F;
    for (const PluginDrawCommand& command : list.commands) {
        CHECK(command.top >= kPadding - 0.01F);
        CHECK(command.bottom <= tileSize - kPadding + 0.01F);
        CHECK(command.left >= kPadding - 0.01F);
        CHECK(command.right <= tileSize - kPadding + 0.01F);
        if (command.kind == PluginDrawKind::Text &&
            command.role == PluginTextRole::Main) {
            if (command.fontSize > largestMain) {
                largestMain = command.fontSize;
            }
        }
    }
    // wide 布局（134x64、三列）的主字号只有约 9.5 DIP。
    CHECK(largestMain >= 18.0F);
}

int wmain() {
    testContentStaysInsideShortTile();
    testLargeLayoutUsesVerticalSpace();
    testSingleLeaf();
    testRowWeights();
    testRepeatExpansion();
    testRepeatMaxLimit();
    testMeter();
    testTruncation();
    testMissingLayoutValuesAreSkipped();

    if (g_failures != 0) {
        wprintf(L"plugin_layout: %d failure(s)\n", g_failures);
        return 1;
    }
    wprintf(L"plugin_layout: all passed\n");
    return 0;
}
