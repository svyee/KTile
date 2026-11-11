// KTile - 持久化层测试
//
// 覆盖：
//   1. JsonWriter 输出格式（转义、嵌套、空容器）
//   2. JsonParser 解析（对象、数组、转义、数字、错误处理）
//   3. ItemsDocument 序列化 -> 反序列化 往返一致性
//   4. ApplicationConfig 往返一致性
//   5. 容错：非法 JSON、缺失字段、越界值
//   6. RadialConfig 往返一致性 + 触发参数越界夹取
//   7. 轮盘插件扫描、坏插件隔离、随包插件（slugin/）是否都还能加载
//
#include "core/data_model.h"
#include "infrastructure/json/config_store.h"
#include "infrastructure/json/json_parser.h"
#include "infrastructure/json/json_writer.h"
#include "infrastructure/json/radial_store.h"
#include "infrastructure/radial_plugin_store.h"

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
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
    CHECK_EQ(static_cast<long long>(restored.schemaVersion),
             static_cast<long long>(core::kCurrentSchemaVersion));
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

// 插件磁贴：pluginId + 参数表必须原样往返。
// 参数顺序也要保留 —— 缓存文件名按参数哈希，顺序变了会读不到旧缓存。
void testPluginItemRoundTrip() {
    core::ItemsDocument document;
    document.tabs.clear();

    core::Tab tab("default", "常用");
    core::LaunchItem item("plug", core::ItemType::Plugin, "天气", "weather");
    item.pluginId = std::string("weather");
    item.setPluginParam("city", "上海");
    item.setPluginParam("units", "metric");
    tab.items.push_back(item);
    document.tabs.push_back(tab);

    const std::string text = ConfigStore::serializeItems(document);
    const auto loaded = ConfigStore::deserializeItems(text);
    CHECK(loaded.ok);
    if (!loaded.ok) {
        return;
    }

    const auto& restored = loaded.value.tabs[0].items[0];
    CHECK(restored.type == core::ItemType::Plugin);
    CHECK(restored.pluginId.has_value());
    if (restored.pluginId.has_value()) {
        CHECK_STREQ(*restored.pluginId, "weather");
    }
    CHECK_EQ(static_cast<long long>(restored.pluginParams.size()), 2);
    CHECK_STREQ(restored.pluginParams[0].first, "city");
    CHECK_STREQ(restored.pluginParams[0].second, "上海");
    CHECK_STREQ(restored.pluginParams[1].first, "units");

    // 普通项目不应长出 pluginId（读旧数据时字段缺失，不写入默认值）
    CHECK(!loaded.value.tabs[0].items.empty());
    CHECK(item.pluginParam("missing").empty());
}

// 轮盘配置：多方案 + 按应用规则 + 各种动作类型必须原样往返。
//
// 最容易坏的是「空槽位」：actions 长度必须始终等于 sectorCount，少存一个
// 空动作，读回来所有扇区就整体错位，用户配的动作会跑到别的方向上去。
void testRadialConfigRoundTrip() {
    core::RadialConfig config;
    config.menu = core::RadialMenu("default", "通用", 8);
    config.menu.actions[0].type = core::RadialActionType::Shortcut;
    config.menu.actions[0].name = "复制";
    config.menu.actions[0].modifiers = {core::HotkeyModifier::Control};
    config.menu.actions[0].key = "C";
    config.menu.actions[2].type = core::RadialActionType::Url;
    config.menu.actions[2].name = "搜索";
    config.menu.actions[2].target = "https://example.com";
    config.menu.actions[7].type = core::RadialActionType::Builtin;
    config.menu.actions[7].name = "锁屏";
    config.menu.actions[7].target = "ktile:lock";

    config.trigger.button = core::RadialTriggerButton::Side2;
    config.trigger.modifier = core::RadialTriggerModifier::Shift;
    config.trigger.cancelRadiusPx = 200;
    config.blacklist = {"game.exe", "mstsc.exe"};

    const std::string text = RadialStore::serialize(config);
    const auto loaded = RadialStore::deserialize(text);
    CHECK(loaded.ok);
    if (!loaded.ok) {
        return;
    }

    CHECK_EQ(static_cast<long long>(loaded.value.schemaVersion),
             static_cast<long long>(core::kRadialSchemaVersion));
    CHECK(loaded.value.trigger.button == core::RadialTriggerButton::Side2);
    CHECK(loaded.value.trigger.modifier == core::RadialTriggerModifier::Shift);
    CHECK_EQ(static_cast<long long>(loaded.value.trigger.cancelRadiusPx), 200);

    const auto& restored = loaded.value.menu;
    CHECK_EQ(static_cast<long long>(restored.actions.size()), 8);
    CHECK(restored.actions[0].type == core::RadialActionType::Shortcut);
    CHECK(restored.actions[0].name == "复制");
    CHECK(restored.actions[0].key == "C");
    CHECK_EQ(static_cast<long long>(restored.actions[0].modifiers.size()), 1);
    if (!restored.actions[0].modifiers.empty()) {
        CHECK(restored.actions[0].modifiers[0] == core::HotkeyModifier::Control);
    }
    // 中间的空槽位必须还是空的，不能被后面的动作顶上
    CHECK(restored.actions[1].isEmpty());
    CHECK(restored.actions[2].type == core::RadialActionType::Url);
    CHECK(restored.actions[2].target == "https://example.com");
    CHECK(restored.actions[7].type == core::RadialActionType::Builtin);
    CHECK(restored.actions[7].target == "ktile:lock");

    // 黑名单往返 + 大小写不敏感命中
    CHECK_EQ(static_cast<long long>(loaded.value.blacklist.size()), 2);
    CHECK(loaded.value.isProcessBlocked("GAME.EXE"));
    CHECK(loaded.value.isProcessBlocked("mstsc.exe"));
    CHECK(!loaded.value.isProcessBlocked("chrome.exe"));
}

// 老文件（schemaVersion 2）里是多方案 + 按应用白名单。读进来必须：
// 优先取 id == "default" 的那份当通用轮盘（不是数组第一个），profiles 丢弃。
void testRadialLegacyMenusArrayMigrated() {
    const std::string json =
        "{\"schemaVersion\": 2,"
        "\"trigger\": {\"enabled\": true, \"button\": \"right\","
        "  \"modifier\": \"ctrl\"},"
        "\"menus\": ["
        "  {\"id\": \"other\", \"name\": \"别的方案\", \"sectorCount\": 4,"
        "   \"actions\": [{}, {}, {}, {}]},"
        "  {\"id\": \"default\", \"name\": \"全局默认\", \"sectorCount\": 8,"
        "   \"actions\": [{\"type\": \"shortcut\", \"name\": \"复制\","
        "     \"key\": \"C\", \"modifiers\": [\"control\"]},"
        "     {}, {}, {}, {}, {}, {}, {}]}],"
        "\"profiles\": [{\"processPattern\": \"notepad.exe\","
        "  \"menuId\": \"other\"}]}";

    const auto loaded = RadialStore::deserialize(json);
    CHECK(loaded.ok);
    if (!loaded.ok) {
        return;
    }
    // 优先 default，不是数组第一个（other）
    CHECK(loaded.value.menu.name == "全局默认");
    CHECK_EQ(static_cast<long long>(loaded.value.menu.actions.size()), 8);
    CHECK(loaded.value.menu.actions[0].name == "复制");
    CHECK(loaded.value.menu.actions[0].key == "C");
    // profiles 整段丢弃：按应用差异化已交给 slugin 插件，不转成黑名单
    CHECK(loaded.value.blacklist.empty());
    // 版本号盖成当前值，下次保存就是新格式
    CHECK_EQ(static_cast<long long>(loaded.value.schemaVersion),
             static_cast<long long>(core::kRadialSchemaVersion));
}

// 手改过的 JSON：离谱值必须夹回可用区间，且不能因为校验失败就把整份配置
// 丢掉换默认（用户配的几十个动作不能因为一个滑杆值就没了）。
void testRadialTriggerValuesClamped() {
    const std::string text =
        "{\"schemaVersion\": 2,"
        "\"trigger\": {\"enabled\": true, \"button\": \"bogus\","
        "\"modifier\": \"ctrl\", \"activationThresholdPx\": 0,"
        "\"cancelRadiusPx\": 40},"
        "\"menus\": [{\"id\": \"m\", \"name\": \"M\", \"sectorCount\": 99,"
        "\"actions\": []}],"
        "\"profiles\": []}";
    const auto loaded = RadialStore::deserialize(text);
    CHECK(loaded.ok);
    if (!loaded.ok) {
        return;
    }

    // 阈值 0 等于「按下即呼出」，右键会被彻底吃掉 → 夹到 1
    CHECK_EQ(static_cast<long long>(loaded.value.trigger.activationThresholdPx), 1);
    // 取消半径比花瓣外缘（142 DIP）还小会把花瓣自己判成已甩出 → 夹到下限
    CHECK_EQ(static_cast<long long>(loaded.value.trigger.cancelRadiusPx),
             static_cast<long long>(core::kMinCancelRadiusDip));
    // 认不出的触发键回落到右键（安全的默认，不会突然吞中键）
    CHECK(loaded.value.trigger.button == core::RadialTriggerButton::Right);
    // 扇区数夹进 4~8，动作补齐到扇区数
    CHECK_EQ(static_cast<long long>(loaded.value.menu.sectorCount), 8);
    CHECK_EQ(static_cast<long long>(loaded.value.menu.actions.size()), 8);
}

// 抑制键往返：JSON 里写成 alt/control/shift/win，读回来必须一一对应。
void testRadialSuppressionRoundTrip() {
    core::RadialConfig config;
    config.menu = core::RadialMenu("default", "全局默认", 4);
    config.trigger.modifier = core::RadialTriggerModifier::Ctrl;
    config.trigger.suppressWhileHeld = {core::HotkeyModifier::Alt,
                                        core::HotkeyModifier::Win};

    const std::string text = RadialStore::serialize(config);
    const auto loaded = RadialStore::deserialize(text);
    CHECK(loaded.ok);
    if (!loaded.ok) {
        return;
    }
    CHECK_EQ(static_cast<long long>(loaded.value.trigger.suppressWhileHeld.size()),
             2);
    CHECK(loaded.value.trigger.suppressWhileHeld[0] == core::HotkeyModifier::Alt);
    CHECK(loaded.value.trigger.suppressWhileHeld[1] == core::HotkeyModifier::Win);
}

// 手写配置的容错：撞车项、重复项、认不出的名字都要在加载期剔除，
// 而不是把整份配置打回默认（那会把用户辛苦配的扇区全清掉）。
void testRadialSuppressionFilteredOnLoad() {
    const std::string json =
        "{"
        "\"schemaVersion\": 2,"
        "\"trigger\": {\"enabled\": true, \"button\": \"right\","
        "  \"modifier\": \"ctrl\","
        "  \"suppressWhileHeld\": [\"alt\", \"alt\", \"control\", \"bogus\", \"shift\"]},"
        "\"menus\": [{\"id\": \"default\", \"name\": \"全局\", \"sectorCount\": 4,"
        "  \"actions\": [{}, {}, {}, {}]}],"
        "\"profiles\": []"
        "}";

    const auto loaded = RadialStore::deserialize(json);
    CHECK(loaded.ok);
    if (!loaded.ok) {
        return;
    }
    // alt 保留、重复的 alt 去掉、control 与触发修饰键撞车剔除、
    // bogus 认不出剔除、shift 保留 —— 只剩两项且顺序按文件顺序。
    const auto& held = loaded.value.trigger.suppressWhileHeld;
    CHECK_EQ(static_cast<long long>(held.size()), 2);
    if (held.size() == 2) {
        CHECK(held[0] == core::HotkeyModifier::Alt);
        CHECK(held[1] == core::HotkeyModifier::Shift);
    }
}

// 没有迁移函数是有意的：字段缺失走默认值、认不出的枚举回落、越界值夹取，
// 老文件本来就能读。但版本号得跟着升，否则下次保存又原样写回旧值，
// 以后真要做破坏性变更时就没有判断依据了。
// （唯一的例外是 v3 → v4 的阈值抬升，见 testRadialThresholdMigration。）
void testRadialSchemaVersionStamped() {
    const std::string text =
        "{\"schemaVersion\": 1,"
        "\"trigger\": {\"button\": \"right\"},"
        "\"menus\": [{\"id\": \"m\", \"name\": \"M\", \"sectorCount\": 4,"
        "\"actions\": [{}, {}, {}, {}]}],"
        "\"profiles\": []}";
    const auto loaded = RadialStore::deserialize(text);
    CHECK(loaded.ok);
    if (!loaded.ok) {
        return;
    }
    CHECK_EQ(static_cast<long long>(loaded.value.schemaVersion),
             static_cast<long long>(core::kRadialSchemaVersion));
    // 缺省的 trigger 字段走默认值：Ctrl + 右键，阈值 15，取消半径 160
    CHECK(loaded.value.trigger.modifier == core::RadialTriggerModifier::Ctrl);
    CHECK_EQ(static_cast<long long>(loaded.value.trigger.activationThresholdPx), 15);
    CHECK_EQ(static_cast<long long>(loaded.value.trigger.cancelRadiusPx), 160);
}

// v3 → v4：旧默认阈值 8 被抬到 15；用户自己调过的值一个都不许动。
void testRadialThresholdMigration() {
    const char* kTail =
        "\"menus\": [{\"id\": \"m\", \"name\": \"M\", \"sectorCount\": 4,"
        "\"actions\": [{}, {}, {}, {}]}],"
        "\"profiles\": []}";

    auto loadWith = [&](int version, int threshold) {
        std::string text = "{\"schemaVersion\": " + std::to_string(version) +
                           ",\"trigger\": {\"button\": \"right\", "
                           "\"activationThresholdPx\": " +
                           std::to_string(threshold) + "}, " + kTail;
        return RadialStore::deserialize(text);
    };

    // 旧版本 + 恰好是旧默认值 → 抬到 15
    const auto legacy = loadWith(3, 8);
    CHECK(legacy.ok);
    if (legacy.ok) {
        CHECK_EQ(static_cast<long long>(legacy.value.trigger.activationThresholdPx), 15);
    }

    // 旧版本 + 用户自己调过 → 原样保留
    for (int custom : {4, 12, 30}) {
        const auto kept = loadWith(3, custom);
        CHECK(kept.ok);
        if (kept.ok) {
            CHECK_EQ(static_cast<long long>(kept.value.trigger.activationThresholdPx),
                     custom);
        }
    }

    // 已经是 v4 的配置里写 8 → 那是用户自己选的，不动
    const auto current = loadWith(4, 8);
    CHECK(current.ok);
    if (current.ok) {
        CHECK_EQ(static_cast<long long>(current.value.trigger.activationThresholdPx), 8);
    }
}

// ---------------------------------------------------------------- 轮盘插件扫描
//
// 这一段真的碰磁盘：接口语义（相对图标路径在加载期变绝对、坏插件只跳过自己、
// 没有 radial.json 的目录不算插件）没法用纯字符串测出来。
void writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
}

void testRadialPluginStoreDiscovery() {
    std::error_code error;
    wchar_t tempBuffer[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempBuffer);
    const std::filesystem::path root =
        std::filesystem::path(tempBuffer) / L"ktile_slugin_test";
    std::filesystem::remove_all(root, error);

    std::filesystem::create_directories(root / L"editor" / L"icons", error);
    std::filesystem::create_directories(root / L"broken", error);
    // 只有图标目录、没有 radial.json：必须被忽略而不是当成坏插件报错
    std::filesystem::create_directories(root / L"empty-dir" / L"icons", error);

    writeTextFile(root / L"editor" / L"radial.json",
                  "{\"schemaVersion\": 1, \"name\": \"Editor\","
                  " \"processes\": [\"editor.exe\"], \"priority\": 2,"
                  " \"menu\": {\"name\": \"m\", \"sectorCount\": 4,"
                  " \"actions\": ["
                  "  {\"type\": \"shortcut\", \"name\": \"copy\","
                  "   \"modifiers\": [\"control\"], \"key\": \"C\","
                  "   \"icon\": \"icons/copy.svg\"},"
                  "  {\"type\": \"shortcut\", \"name\": \"save\","
                  "   \"icon\": \"C:/absolute/save.svg\"},"
                  "  {}, {} ]}}");
    writeTextFile(root / L"broken" / L"radial.json",
                  "{\"schemaVersion\": 1, \"name\": \"Broken\","
                  " \"processes\": [], \"menu\": {\"name\": \"m\","
                  " \"sectorCount\": 4, \"actions\": [{}, {}, {}, {}]}}");

    RadialPluginStore store;
    store.discover(root.wstring());
    const auto& plugins = store.plugins();
    CHECK_EQ(static_cast<long long>(plugins.size()), 2);

    // 目录名即 id；带不带 radial.json 决定它算不算插件
    bool sawEditor = false;
    bool sawBroken = false;
    for (const auto& plugin : plugins) {
        if (plugin.id == "editor") {
            sawEditor = true;
            CHECK(plugin.valid);
            CHECK(plugin.name == "Editor");
            CHECK_EQ(static_cast<long long>(plugin.priority), 2);
            CHECK_EQ(static_cast<long long>(plugin.processes.size()), 1);
            CHECK(!plugin.directory.empty());
            CHECK_EQ(static_cast<long long>(plugin.menu.sectorCount), 4);
            CHECK_EQ(static_cast<long long>(plugin.menu.actions.size()), 4);

            // 相对图标 → 加载期就解析成绝对路径（相对插件目录）
            const auto& first = plugin.menu.actions[0];
            CHECK(first.icon.has_value());
            if (first.icon.has_value()) {
                const std::filesystem::path iconPath(*first.icon);
                CHECK(iconPath.is_absolute());
                CHECK(iconPath.filename() == L"copy.svg");
                CHECK(iconPath.parent_path().filename() == L"icons");
            }
            // 绝对路径原样保留
            const auto& second = plugin.menu.actions[1];
            CHECK(second.icon.has_value());
            if (second.icon.has_value()) {
                CHECK(second.icon->find("\\absolute\\save.svg") !=
                          std::string::npos ||
                      second.icon->find("/absolute/save.svg") !=
                          std::string::npos);
            }
            // 快捷键动作的修饰键解析成枚举（"control" 不能掉成 Alt）
            CHECK_EQ(static_cast<long long>(first.modifiers.size()), 1);
            if (!first.modifiers.empty()) {
                CHECK(first.modifiers[0] == core::HotkeyModifier::Control);
            }
        } else if (plugin.id == "broken") {
            sawBroken = true;
            CHECK(!plugin.valid);
            CHECK(!plugin.error.empty());
        }
    }
    CHECK(sawEditor);
    CHECK(sawBroken);

    // 目录不存在：空结果，不是错误
    RadialPluginStore missing;
    missing.discover((root / L"no-such-dir").wstring());
    CHECK_EQ(static_cast<long long>(missing.plugins().size()), 0);

    std::filesystem::remove_all(root, error);
}

// schemaVersion 比本程序新：整份不加载（字段语义可能已经变了）
void testRadialPluginRejectsFutureSchema() {
    std::error_code error;
    wchar_t tempBuffer[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempBuffer);
    const std::filesystem::path root =
        std::filesystem::path(tempBuffer) / L"ktile_slugin_future";
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / L"future", error);
    writeTextFile(root / L"future" / L"radial.json",
                  "{\"schemaVersion\": 99, \"name\": \"Future\","
                  " \"processes\": [\"future.exe\"], \"menu\": {\"name\": \"m\","
                  " \"sectorCount\": 4, \"actions\": [{}, {}, {}, {}]}}");

    RadialPluginStore store;
    store.discover(root.wstring());
    CHECK_EQ(static_cast<long long>(store.plugins().size()), 1);
    if (!store.plugins().empty()) {
        CHECK(!store.plugins()[0].valid);
        CHECK(!store.plugins()[0].error.empty());
    }
    std::filesystem::remove_all(root, error);
}

// 随包提供的插件（`slugin/`）必须全部能加载、图标一个不缺。
//
// 为什么值得单测：手写 JSON 和手写 SVG 的错法都很安静。少一个逗号，整个插件被
// 跳过、轮盘静默回落成通用那份；图标名打错，那张卡片静默换成内置符号。两者都
// 不影响编译、不报错，只有真机在浏览器里划出来才看得见。测试里兜住三件事：
//   - 清单合法（valid：扇区数 4~8、动作数与扇区数一致、processes 非空）
//   - 每个动作都有名字（空标签在轮盘上就是一块白板）
//   - 每个 icon 指向的文件真的存在
// 找不到 slugin 目录就跳过——只拿到 src/ 的人不该因此红一片。
std::filesystem::path findBundledPluginRoot() {
    std::error_code error;
    if (std::filesystem::is_directory(L"slugin", error)) {
        return std::filesystem::path(L"slugin");   // 从仓库根运行测试
    }
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        // 测试 exe 在 build/ 下，同级 slugin/ 是构建时从仓库拷过去的发布形态
        const std::filesystem::path besideExe =
            std::filesystem::path(buffer).parent_path() / L"slugin";
        if (std::filesystem::is_directory(besideExe, error)) {
            return besideExe;
        }
    }
    return {};
}

void testBundledPluginsLoad() {
    const std::filesystem::path root = findBundledPluginRoot();
    if (root.empty()) {
        std::printf("  （没有 slugin 目录，跳过随包插件检查）\n");
        return;
    }

    RadialPluginStore store;
    store.discover(root.wstring());
    const auto& plugins = store.plugins();
    CHECK(plugins.size() >= 2);   // explorer + browser

    bool sawBrowser = false;
    for (const auto& plugin : plugins) {
        CHECK(plugin.valid);
        if (!plugin.valid) {
            std::printf("  插件 %s 加载失败：%s\n", plugin.id.c_str(),
                        plugin.error.c_str());
            continue;
        }
        CHECK(!plugin.processes.empty());
        CHECK_EQ(static_cast<long long>(plugin.menu.actions.size()),
                 static_cast<long long>(plugin.menu.sectorCount));

        for (const auto& action : plugin.menu.actions) {
            CHECK(!action.name.empty());
            // 随包插件都自带图标：没有 icon 字段就说明写漏了
            CHECK(action.icon.has_value());
            if (!action.icon.has_value()) {
                std::printf("  插件 %s 的动作「%s」缺图标\n", plugin.id.c_str(),
                            action.name.c_str());
                continue;
            }
            // 清单里是 UTF-8 路径，u8path 直接按 UTF-8 解释
            std::error_code iconError;
            const bool existed = std::filesystem::is_regular_file(
                std::filesystem::u8path(*action.icon), iconError);
            CHECK(existed);
            if (!existed) {
                std::printf("  插件 %s 找不到图标：%s\n", plugin.id.c_str(),
                            action.icon->c_str());
            }
        }
        if (plugin.id == "browser") {
            sawBrowser = true;
        }
    }
    CHECK(sawBrowser);

    // 浏览器插件要真能按进程名命中（大小写不敏感，与运行期同一条路径）
    const core::RadialPlugin* hit =
        core::findPluginForProcess(plugins, "Chrome.exe");
    CHECK(hit != nullptr);
    if (hit != nullptr) {
        CHECK(hit->id == "browser");
    }
    hit = core::findPluginForProcess(plugins, "360ChromeX.exe");
    CHECK(hit != nullptr);
    if (hit != nullptr) {
        CHECK(hit->id == "browser");
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
    testPluginItemRoundTrip();
    testRadialConfigRoundTrip();
    testRadialTriggerValuesClamped();
    testRadialSuppressionRoundTrip();
    testRadialSuppressionFilteredOnLoad();
    testRadialLegacyMenusArrayMigrated();
    testRadialSchemaVersionStamped();
    testRadialThresholdMigration();
    testRadialPluginStoreDiscovery();
    testRadialPluginRejectsFutureSchema();
    testBundledPluginsLoad();

    std::printf("检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("===== 持久化: 全部通过 =====\n");
        return 0;
    }
    std::printf("===== 持久化: %d 项失败 =====\n", g_failures);
    return 1;
}
