// KTile - 持久化层测试
//
// 覆盖：
//   1. JsonWriter 输出格式（转义、嵌套、空容器）
//   2. JsonParser 解析（对象、数组、转义、数字、错误处理）
//   3. ItemsDocument 序列化 -> 反序列化 往返一致性
//   4. ApplicationConfig 往返一致性
//   5. 容错：非法 JSON、缺失字段、越界值
//
#include "core/data_model.h"
#include "infrastructure/json/config_store.h"
#include "infrastructure/json/json_parser.h"
#include "infrastructure/json/json_writer.h"

#include <cstdio>
#include <string>

using namespace ktile;
using namespace ktile::infrastructure;
using ktile::infrastructure::json::JsonWriter;
using ktile::infrastructure::json::parseJson;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL line %d: %s\n", line, expression);
    }
}

void checkEq(const std::string& actual, const std::string& expected, int line) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("  FAIL line %d: 期望 \"%s\" 实际 \"%s\"\n",
                    line, expected.c_str(), actual.c_str());
    }
}

void checkEq(long long actual, long long expected, int line) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("  FAIL line %d: 期望 %lld 实际 %lld\n",
                    line, expected, actual);
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_STREQ(a, b) checkEq((a), (b), __LINE__)
#define CHECK_EQ(a, b) checkEq((a), (b), __LINE__)

// ---------------------------------------------------------------- JsonWriter

void testWriterBasics() {
    JsonWriter writer;
    writer.beginObject();
    writer.member("name", "KTile");
    writer.member("version", 1);
    writer.member("enabled", true);
    writer.endObject();

    const auto parsed = parseJson(writer.text());
    CHECK(parsed.ok);
    CHECK(parsed.root.isObject());
    CHECK_STREQ(parsed.root.find("name").asString(), "KTile");
    CHECK_EQ(parsed.root.find("version").asInt(), 1);
    CHECK(parsed.root.find("enabled").asBool());
}

void testWriterEscaping() {
    JsonWriter writer;
    writer.beginObject();
    writer.member("quote", "say \"hi\"");
    writer.member("backslash", "C:\\Windows\\System32");
    writer.member("newline", "line1\nline2");
    writer.member("tab", "a\tb");
    writer.endObject();

    const auto parsed = parseJson(writer.text());
    CHECK(parsed.ok);
    CHECK_STREQ(parsed.root.find("quote").asString(), "say \"hi\"");
    CHECK_STREQ(parsed.root.find("backslash").asString(), "C:\\Windows\\System32");
    CHECK_STREQ(parsed.root.find("newline").asString(), "line1\nline2");
    CHECK_STREQ(parsed.root.find("tab").asString(), "a\tb");
}

void testWriterNested() {
    JsonWriter writer;
    writer.beginObject();
    writer.key("tabs").beginArray();
    writer.beginObject();
    writer.member("id", "t1");
    writer.key("items").beginArray();
    writer.value("a");
    writer.value("b");
    writer.endArray();
    writer.endObject();
    writer.endArray();
    writer.endObject();

    const auto parsed = parseJson(writer.text());
    CHECK(parsed.ok);
    const auto& tabs = parsed.root.find("tabs");
    CHECK(tabs.isArray());
    CHECK_EQ(static_cast<long long>(tabs.size()), 1);
    CHECK_STREQ(tabs.at(0).find("id").asString(), "t1");
    CHECK_EQ(static_cast<long long>(tabs.at(0).find("items").size()), 2);
}

void testWriterEmptyContainers() {
    JsonWriter writer;
    writer.beginObject();
    writer.key("emptyArray").beginArray();
    writer.endArray();
    writer.key("emptyObject").beginObject();
    writer.endObject();
    writer.endObject();

    const auto parsed = parseJson(writer.text());
    CHECK(parsed.ok);
    CHECK(parsed.root.find("emptyArray").isArray());
    CHECK_EQ(static_cast<long long>(parsed.root.find("emptyArray").size()), 0);
    CHECK(parsed.root.find("emptyObject").isObject());
}

// ---------------------------------------------------------------- JsonParser

void testParserBasics() {
    const auto result = parseJson(
        "{\"a\": 1, \"b\": \"text\", \"c\": true, \"d\": null, "
        "\"e\": [1,2,3], \"f\": {\"nested\": 42}}");

    CHECK(result.ok);
    CHECK_EQ(result.root.find("a").asInt(), 1);
    CHECK_STREQ(result.root.find("b").asString(), "text");
    CHECK(result.root.find("c").asBool());
    CHECK(result.root.find("d").isNull());
    CHECK_EQ(static_cast<long long>(result.root.find("e").size()), 3);
    CHECK_EQ(result.root.find("f").find("nested").asInt(), 42);
}

void testParserUnicodeEscape() {
    const auto result = parseJson("{\"cn\": \"\\u4E2D\\u6587\"}");
    CHECK(result.ok);
    // 中 = U+4E2D -> E4 B8 AD；文 = U+6587 -> E6 96 87
    CHECK_STREQ(result.root.find("cn").asString(), "\xE4\xB8\xAD\xE6\x96\x87");
}

void testParserRawUtf8() {
    const auto result = parseJson("{\"name\": \"记事本\"}");
    CHECK(result.ok);
    CHECK_STREQ(result.root.find("name").asString(), "记事本");
}

void testParserNumbers() {
    const auto result = parseJson(
        "{\"int\": 42, \"neg\": -17, \"float\": 3.5, \"exp\": 1e3}");
    CHECK(result.ok);
    CHECK_EQ(result.root.find("int").asInt(), 42);
    CHECK_EQ(result.root.find("neg").asInt(), -17);
    CHECK(result.root.find("float").asDouble() > 3.49);
    CHECK(result.root.find("float").asDouble() < 3.51);
    CHECK_EQ(result.root.find("exp").asInt(), 1000);
}

void testParserErrors() {
    // 未闭合对象
    CHECK(!parseJson("{\"a\": 1").ok);
    // 未闭合字符串
    CHECK(!parseJson("{\"a\": \"oops}").ok);
    // 尾随内容
    CHECK(!parseJson("{} extra").ok);
    // 空输入
    CHECK(!parseJson("").ok);
    // 非法字面量
    CHECK(!parseJson("{\"a\": tru}").ok);
    // 键不是字符串
    CHECK(!parseJson("{a: 1}").ok);
    // 错误含行号
    const auto result = parseJson("{\n\"a\": 1,\n\"b\" 2\n}");
    CHECK(!result.ok);
    CHECK(result.errorLine > 0);
}

void testParserMissingKeyReturnsNull() {
    const auto result = parseJson("{\"a\": 1}");
    CHECK(result.ok);
    const auto& missing = result.root.find("nonexistent");
    CHECK(missing.isNull());
    CHECK_EQ(missing.asInt(-1), -1);
    CHECK_STREQ(missing.asString(), "");
    // 对非容器调用 find / at 应安全
    CHECK(result.root.find("a").find("x").isNull());
    CHECK(result.root.find("a").at(0).isNull());
}

// ---------------------------------------------------------------- 往返一致性

core::ItemsDocument makeSampleDocument() {
    core::ItemsDocument document;
    document.schemaVersion = core::kCurrentSchemaVersion;

    core::Tab tab("default", "常用");

    // 覆盖 4 种尺寸 + 各种可选字段
    core::LaunchItem small("1", core::ItemType::Application, "记事本",
                           "C:\\Windows\\notepad.exe");
    small.tileSize = core::TileSize::Small;
    small.tileColor = 0xFF2D7DD2u;
    small.launchCount = 7;
    small.lastLaunchedAt = "2026-10-14T09:00:00";
    tab.items.push_back(small);

    core::LaunchItem wide("2", core::ItemType::Application, "工具箱",
                          "C:\\Tools\\toolbox.exe");
    wide.tileSize = core::TileSize::Wide;
    wide.arguments.push_back("--verbose");
    wide.arguments.push_back("--no-gui");
    wide.workingDirectory = "C:\\Tools";
    wide.runAsAdministrator = true;
    wide.setGridPosition(2, 0);
    wide.labelMode = core::TileLabelMode::Always;
    tab.items.push_back(wide);

    core::LaunchItem tall("3", core::ItemType::Folder, "文档",
                          "D:\\Documents");
    tall.tileSize = core::TileSize::Tall;
    tall.labelMode = core::TileLabelMode::OnHover;
    tab.items.push_back(tall);

    core::LaunchItem large("4", core::ItemType::Url, "官网",
                           "https://example.com");
    large.tileSize = core::TileSize::Large;
    large.icon = "C:\\Icons\\web.ico";
    tab.items.push_back(large);

    document.tabs.push_back(tab);

    core::Tab second("tools", "工具");
    core::LaunchItem terminal("5", core::ItemType::Application, "终端",
                              "C:\\Windows\\System32\\cmd.exe");
    terminal.tileSize = core::TileSize::Wide;
    second.items.push_back(terminal);
    document.tabs.push_back(second);

    return document;
}

void testItemsRoundTrip() {
    const auto original = makeSampleDocument();
    const std::string text = ConfigStore::serializeItems(original);

    // 输出应能被解析
    const auto parsed = parseJson(text);
    CHECK(parsed.ok);

    const auto loaded = ConfigStore::deserializeItems(text);
    CHECK(loaded.ok);
    if (!loaded.ok) {
        return;
    }

    const auto& restored = loaded.value;
    CHECK_EQ(static_cast<long long>(restored.schemaVersion), 1);
    CHECK_EQ(static_cast<long long>(restored.tabs.size()), 2);

    const auto& tab0 = restored.tabs[0];
    CHECK_STREQ(tab0.id, "default");
    CHECK_STREQ(tab0.name, "常用");
    CHECK_EQ(static_cast<long long>(tab0.items.size()), 4);

    // Small
    const auto& item0 = tab0.items[0];
    CHECK_STREQ(item0.id, "1");
    CHECK_STREQ(item0.name, "记事本");
    CHECK_STREQ(item0.target, "C:\\Windows\\notepad.exe");
    CHECK(item0.type == core::ItemType::Application);
    CHECK(item0.tileSize == core::TileSize::Small);
    CHECK(item0.tileColor.has_value());
    CHECK_EQ(static_cast<long long>(*item0.tileColor), 0xFF2D7DD2LL);
    CHECK_EQ(static_cast<long long>(item0.launchCount), 7);
    CHECK(item0.lastLaunchedAt.has_value());
    CHECK_STREQ(*item0.lastLaunchedAt, "2026-10-14T09:00:00");
    // 没设过的项默认跟随全局 —— 保证「不改就是升级前的样子」
    CHECK(item0.labelMode == core::TileLabelMode::FollowGlobal);

    // Wide（含参数、工作目录、管理员、显式坐标）
    const auto& item1 = tab0.items[1];
    CHECK_STREQ(item1.id, "2");
    CHECK(item1.tileSize == core::TileSize::Wide);
    CHECK_EQ(static_cast<long long>(item1.arguments.size()), 2);
    CHECK_STREQ(item1.arguments[0], "--verbose");
    CHECK_STREQ(item1.arguments[1], "--no-gui");
    CHECK(item1.workingDirectory.has_value());
    CHECK_STREQ(*item1.workingDirectory, "C:\\Tools");
    CHECK(item1.runAsAdministrator);
    CHECK(item1.hasGridPosition());
    CHECK_EQ(static_cast<long long>(*item1.gridColumn), 2);
    CHECK_EQ(static_cast<long long>(*item1.gridRow), 0);
    CHECK(item1.labelMode == core::TileLabelMode::Always);

    // Tall
    const auto& item2 = tab0.items[2];
    CHECK(item2.tileSize == core::TileSize::Tall);
    CHECK(item2.type == core::ItemType::Folder);
    CHECK(!item2.tileColor.has_value());
    CHECK(!item2.workingDirectory.has_value());
    CHECK(item2.labelMode == core::TileLabelMode::OnHover);

    // Large（URL + 自定义图标）
    const auto& item3 = tab0.items[3];
    CHECK(item3.tileSize == core::TileSize::Large);
    CHECK(item3.type == core::ItemType::Url);
    CHECK(item3.icon.has_value());
    CHECK_STREQ(*item3.icon, "C:\\Icons\\web.ico");

    // 第二个 Tab
    CHECK_STREQ(restored.tabs[1].id, "tools");
    CHECK_STREQ(restored.tabs[1].name, "工具");
    CHECK_EQ(static_cast<long long>(restored.tabs[1].items.size()), 1);
    CHECK_STREQ(restored.tabs[1].items[0].name, "终端");
}

void testConfigRoundTrip() {
    core::ApplicationConfig original;
    original.schemaVersion = core::kCurrentSchemaVersion;
    original.loggingEnabled = false;
    original.appearance.gridColumns = 8;
    original.appearance.gridRows = 5;
    original.appearance.defaultTileSize = core::TileSize::Wide;
    original.appearance.themeName = "石墨青";
    original.appearance.backgroundImage = "themes\\backgrounds\\ab12.png";
    original.appearance.showTileLabels = false;
    original.appearance.alwaysShowTileLabels = false;
    original.appearance.accentColor = 0xFF00AAFFu;
    original.activation.startWithWindows = true;
    original.activation.screenEdgeEnabled = true;
    original.activation.hotkey.enabled = true;
    original.activation.hotkey.modifiers = {core::HotkeyModifier::Control,
                                            core::HotkeyModifier::Shift};
    original.activation.hotkey.key = "K";

    const std::string text = ConfigStore::serializeConfig(original);
    const auto parsed = parseJson(text);
    CHECK(parsed.ok);

    const auto loaded = ConfigStore::deserializeConfig(text);
    CHECK(loaded.ok);
    if (!loaded.ok) {
        return;
    }

    const auto& config = loaded.value;
    CHECK(!config.loggingEnabled);
    CHECK_EQ(static_cast<long long>(config.appearance.gridColumns), 8);
    CHECK_EQ(static_cast<long long>(config.appearance.gridRows), 5);
    CHECK(config.appearance.defaultTileSize == core::TileSize::Wide);
    CHECK_STREQ(config.appearance.themeName.c_str(), "石墨青");
    CHECK_STREQ(config.appearance.backgroundImage.c_str(),
                "themes\\backgrounds\\ab12.png");
    CHECK(!config.appearance.showTileLabels);
    CHECK(!config.appearance.alwaysShowTileLabels);
    CHECK(config.appearance.accentColor.has_value());
    CHECK_EQ(static_cast<long long>(*config.appearance.accentColor), 0xFF00AAFFLL);
    CHECK(config.activation.startWithWindows);
    CHECK(config.activation.screenEdgeEnabled);
    CHECK(config.activation.hotkey.enabled);
    CHECK_EQ(static_cast<long long>(config.activation.hotkey.modifiers.size()), 2);
    CHECK(config.activation.hotkey.modifiers[0] == core::HotkeyModifier::Control);
    CHECK(config.activation.hotkey.modifiers[1] == core::HotkeyModifier::Shift);
    CHECK_STREQ(config.activation.hotkey.key, "K");
}

// ---------------------------------------------------------------- 容错

void testCorruptJsonIsRejected() {
    const auto result = ConfigStore::deserializeItems("{ not valid json ");
    CHECK(!result.ok);
    CHECK(!result.error.empty());
    // 失败时 value 应为默认构造（已 ensureDefaultTab）
    CHECK_EQ(static_cast<long long>(result.value.tabs.size()), 1);
}

void testMissingFieldsUseDefaults() {
    const auto result = ConfigStore::deserializeConfig("{}");
    CHECK(result.ok);
    CHECK_EQ(static_cast<long long>(result.value.appearance.gridColumns),
             core::kDefaultGridColumns);
    CHECK_EQ(static_cast<long long>(result.value.appearance.gridRows),
             core::kDefaultGridRows);
    CHECK(result.value.appearance.defaultTileSize == core::TileSize::Small);
    // 老配置里没有 alwaysShowTileLabels —— 必须按「标题常驻」升级，
    // 而不是让老用户升级后标题突然全都不见了
    CHECK(result.value.appearance.showTileLabels);
    CHECK(result.value.appearance.alwaysShowTileLabels);
    CHECK(result.value.activation.hotkey.enabled);
    CHECK_STREQ(result.value.activation.hotkey.key, "Space");
}

void testOutOfRangeValuesClamped() {
    const std::string text =
        "{\"appearance\": {\"gridColumns\": 999, \"gridRows\": 0}}";
    const auto result = ConfigStore::deserializeConfig(text);
    CHECK(result.ok);
    CHECK_EQ(static_cast<long long>(result.value.appearance.gridColumns),
             core::kMaximumGridColumns);
    CHECK_EQ(static_cast<long long>(result.value.appearance.gridRows),
             core::kMinimumGridRows);
}

void testItemsWithGarbageEntriesSkipped() {
    const std::string text =
        "{\"tabs\": [{\"id\": \"t\", \"name\": \"T\", \"items\": ["
        "  {\"id\": \"keep\", \"name\": \"Good\", \"target\": \"x.exe\"},"
        "  {\"id\": \"drop\"},"
        "  \"not an object\","
        "  {\"name\": \"\", \"target\": \"\"}"
        "]}]}";
    const auto result = ConfigStore::deserializeItems(text);
    CHECK(result.ok);
    CHECK_EQ(static_cast<long long>(result.value.tabs.size()), 1);
    CHECK_EQ(static_cast<long long>(result.value.tabs[0].items.size()), 1);
    CHECK_STREQ(result.value.tabs[0].items[0].name, "Good");
}

void testEmptyDocumentGetsDefaultTab() {
    const auto result = ConfigStore::deserializeItems("{\"tabs\": []}");
    CHECK(result.ok);
    CHECK_EQ(static_cast<long long>(result.value.tabs.size()), 1);
    CHECK(!result.value.tabs[0].id.empty());
}

void testUnknownTileSizeFallsBack() {
    const std::string text =
        "{\"tabs\": [{\"id\":\"t\",\"name\":\"T\",\"items\":["
        "{\"id\":\"a\",\"name\":\"A\",\"target\":\"a.exe\",\"tileSize\": 77}]}]}";
    const auto result = ConfigStore::deserializeItems(text);
    CHECK(result.ok);
    CHECK(result.value.tabs[0].items[0].tileSize == core::TileSize::Small);
}

// labelMode 的往返 + 老数据回落
void testLabelModeRoundTripAndFallback() {
    const std::string text =
        "{\"tabs\": [{\"id\":\"t\",\"name\":\"T\",\"items\":["
        "{\"id\":\"a\",\"name\":\"A\",\"target\":\"a.exe\","
        "\"labelMode\":\"always\"},"
        "{\"id\":\"b\",\"name\":\"B\",\"target\":\"b.exe\","
        "\"labelMode\":\"hover\"},"
        "{\"id\":\"c\",\"name\":\"C\",\"target\":\"c.exe\","
        "\"labelMode\":\"bogus\"},"
        "{\"id\":\"d\",\"name\":\"D\",\"target\":\"d.exe\"}"
        "]}]}";
    const auto result = ConfigStore::deserializeItems(text);
    CHECK(result.ok);
    if (!result.ok) {
        return;
    }
    const auto& items = result.value.tabs[0].items;
    CHECK_EQ(static_cast<long long>(items.size()), 4);
    CHECK(items[0].labelMode == core::TileLabelMode::Always);
    CHECK(items[1].labelMode == core::TileLabelMode::OnHover);
    // 认不出的字符串 → 跟随全局（安全的默认：行为与升级前一致）
    CHECK(items[2].labelMode == core::TileLabelMode::FollowGlobal);
    // 老数据根本没有这个键 → 也是跟随全局，升级后显示行为不变
    CHECK(items[3].labelMode == core::TileLabelMode::FollowGlobal);

    // 写出去再读回来必须一致
    const std::string serialized = ConfigStore::serializeItems(result.value);
    const auto again = ConfigStore::deserializeItems(serialized);
    CHECK(again.ok);
    if (again.ok) {
        CHECK(again.value.tabs[0].items[0].labelMode ==
              core::TileLabelMode::Always);
        CHECK(again.value.tabs[0].items[1].labelMode ==
              core::TileLabelMode::OnHover);
    }
}

}  // namespace

int wmain() {
    std::printf("===== 持久化层测试 =====\n");

    testWriterBasics();
    testWriterEscaping();
    testWriterNested();
    testWriterEmptyContainers();

    testParserBasics();
    testParserUnicodeEscape();
    testParserRawUtf8();
    testParserNumbers();
    testParserErrors();
    testParserMissingKeyReturnsNull();

    testItemsRoundTrip();
    testConfigRoundTrip();

    testCorruptJsonIsRejected();
    testMissingFieldsUseDefaults();
    testOutOfRangeValuesClamped();
    testItemsWithGarbageEntriesSkipped();
    testEmptyDocumentGetsDefaultTab();
    testUnknownTileSizeFallsBack();
    testLabelModeRoundTripAndFallback();

    std::printf("检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("===== 持久化: 全部通过 =====\n");
        return 0;
    }
    std::printf("===== 持久化: %d 项失败 =====\n", g_failures);
    return 1;
}
