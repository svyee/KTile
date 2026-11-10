// KTile - 轮盘菜单数据模型测试
#include "core/radial_menu.h"
#include "core/radial_plugin.h"

#include <cmath>
#include <cstdio>

using namespace ktile::core;

static int g_failures = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            wprintf(L"FAIL line %d: %s\n", __LINE__, L#cond);    \
            ++g_failures;                                        \
        }                                                        \
    } while (false)

// ---------------------------------------------------------------- sectorFromAngle

static void testSectorFromAngle4() {
    // 4 扇区：0°=上, 90°=右, 180°=下, 270°=左
    CHECK(sectorFromAngle(0.0F, 4) == 0);    // 正上 → 0
    CHECK(sectorFromAngle(45.0F, 4) == 1);   // 右上 → 1
    CHECK(sectorFromAngle(90.0F, 4) == 1);   // 正右 → 1
    CHECK(sectorFromAngle(135.0F, 4) == 2);  // 右下 → 2
    CHECK(sectorFromAngle(180.0F, 4) == 2);  // 正下 → 2
    CHECK(sectorFromAngle(225.0F, 4) == 3);  // 左下 → 3
    CHECK(sectorFromAngle(270.0F, 4) == 3);  // 正左 → 3
    CHECK(sectorFromAngle(315.0F, 4) == 0);  // 左上 → 0
}

static void testSectorFromAngle8() {
    // 8 扇区：每 45° 一个，扇区 0 中心在 0°
    CHECK(sectorFromAngle(0.0F, 8) == 0);    // 上
    CHECK(sectorFromAngle(45.0F, 8) == 1);   // 右上
    CHECK(sectorFromAngle(90.0F, 8) == 2);   // 右
    CHECK(sectorFromAngle(135.0F, 8) == 3);  // 右下
    CHECK(sectorFromAngle(180.0F, 8) == 4);  // 下
    CHECK(sectorFromAngle(225.0F, 8) == 5);  // 左下
    CHECK(sectorFromAngle(270.0F, 8) == 6);  // 左
    CHECK(sectorFromAngle(315.0F, 8) == 7);  // 左上
}

static void testSectorFromAngleBoundary() {
    // 边界：8 扇区，扇区 0 覆盖 [-22.5°, +22.5°]
    CHECK(sectorFromAngle(22.0F, 8) == 0);
    CHECK(sectorFromAngle(23.0F, 8) == 1);
    CHECK(sectorFromAngle(-22.0F, 8) == 0);
    CHECK(sectorFromAngle(-23.0F, 8) == 7);

    // 负角度归一化
    CHECK(sectorFromAngle(-45.0F, 8) == 7);
    CHECK(sectorFromAngle(-90.0F, 8) == 6);
}

static void testSectorFromAngle6() {
    // 6 扇区：每 60°
    CHECK(sectorFromAngle(0.0F, 6) == 0);
    CHECK(sectorFromAngle(60.0F, 6) == 1);
    CHECK(sectorFromAngle(120.0F, 6) == 2);
    CHECK(sectorFromAngle(180.0F, 6) == 3);
    CHECK(sectorFromAngle(240.0F, 6) == 4);
    CHECK(sectorFromAngle(300.0F, 6) == 5);
}

// ---------------------------------------------------------------- hitTestSector

static void testHitTestCenter() {
    // 在内圈半径内 → nullopt
    auto r = hitTestSector(0.0F, 0.0F, 30.0F, 120.0F, 8);
    CHECK(!r.has_value());
    r = hitTestSector(10.0F, 10.0F, 30.0F, 120.0F, 8);
    CHECK(!r.has_value());
}

static void testHitTestOuter() {
    // 在外圈半径外 → nullopt（外甩取消）
    auto r = hitTestSector(200.0F, 0.0F, 30.0F, 120.0F, 8);
    CHECK(!r.has_value());
    r = hitTestSector(0.0F, -200.0F, 30.0F, 120.0F, 8);
    CHECK(!r.has_value());
}

static void testHitTestSectors() {
    // 8 扇区，内圈 30，外圈 120
    // 上
    auto r = hitTestSector(0.0F, -80.0F, 30.0F, 120.0F, 8);
    CHECK(r.has_value() && *r == 0);
    // 右
    r = hitTestSector(80.0F, 0.0F, 30.0F, 120.0F, 8);
    CHECK(r.has_value() && *r == 2);
    // 下
    r = hitTestSector(0.0F, 80.0F, 30.0F, 120.0F, 8);
    CHECK(r.has_value() && *r == 4);
    // 左
    r = hitTestSector(-80.0F, 0.0F, 30.0F, 120.0F, 8);
    CHECK(r.has_value() && *r == 6);
    // 右上
    r = hitTestSector(60.0F, -60.0F, 30.0F, 120.0F, 8);
    CHECK(r.has_value() && *r == 1);
}

static void testHitTestAtRadiusBoundary() {
    // 内圈内 → nullopt
    auto r = hitTestSector(20.0F, 0.0F, 30.0F, 120.0F, 8);
    CHECK(!r.has_value());
    // 略大于内圈 → 命中
    r = hitTestSector(31.0F, 0.0F, 30.0F, 120.0F, 8);
    CHECK(r.has_value());
    // 在外圈外 → nullopt（外甩取消）
    r = hitTestSector(121.0F, 0.0F, 30.0F, 120.0F, 8);
    CHECK(!r.has_value());
}

// ---------------------------------------------------------------- 配置查找

static RadialConfig makeTestConfig() {
    RadialConfig config;
    config.menu = RadialMenu("default", "通用", 8);
    config.menu.actions[0].name = "复制";
    config.menu.actions[0].type = RadialActionType::Shortcut;
    config.menu.actions[0].modifiers = {HotkeyModifier::Control};
    config.menu.actions[0].key = "C";
    // 故意一条大写一条小写，用来盯大小写不敏感
    config.blacklist = {"game.exe", "MSTSC.EXE"};
    return config;
}

static void testBlacklist() {
    const auto config = makeTestConfig();
    CHECK(config.isProcessBlocked("game.exe"));
    CHECK(config.isProcessBlocked("GAME.EXE"));
    // 前台报的永远是小写，配置里怎么写都得能命中
    CHECK(config.isProcessBlocked("mstsc.exe"));
    CHECK(!config.isProcessBlocked("chrome.exe"));
    // 空进程名（查不到前台进程）不算命中：宁可弹一次，也别让功能静默失效
    CHECK(!config.isProcessBlocked(""));
    // 只做整名比较，不做前缀/子串匹配
    CHECK(!config.isProcessBlocked("game"));
    CHECK(!config.isProcessBlocked("game.exe.bak"));
}

static void testProcessNameEquals() {
    CHECK(processNameEquals("chrome.exe", "chrome.exe"));
    CHECK(processNameEquals("Chrome.EXE", "chrome.exe"));
    CHECK(!processNameEquals("chrome.exe", "chrome2.exe"));
    CHECK(!processNameEquals("", "chrome.exe"));
    CHECK(!processNameEquals("chrome", "chrome.exe"));
}

// ---------------------------------------------------------------- 配置校验

static void testValidateConfig() {
    RadialConfig config = makeTestConfig();
    CHECK(validateRadialConfig(config));

    // 扇区数非法（3 不在 4/6/8 里）
    config.menu.sectorCount = 3;
    config.menu.actions.resize(3);
    CHECK(!validateRadialConfig(config));
}

static void testValidateActionsSizeMismatch() {
    RadialConfig config;
    config.menu = RadialMenu("m", "m", 8);
    config.menu.actions.resize(4);  // 与 sectorCount 不匹配
    CHECK(!validateRadialConfig(config));
}

static void testValidateEmptyBlacklistEntry() {
    RadialConfig config = makeTestConfig();
    config.blacklist.push_back("");
    // 空进程名永远匹配不上，属于无意义配置
    CHECK(!validateRadialConfig(config));
}

static void testValidateThreshold() {
    RadialConfig config = makeTestConfig();
    config.trigger.activationThresholdPx = 0;
    CHECK(!validateRadialConfig(config));

    config = makeTestConfig();
    config.trigger.cancelRadiusPx = config.trigger.activationThresholdPx;
    CHECK(!validateRadialConfig(config));
}

// ---------------------------------------------------------------- 空槽位

static void testEmptyAction() {
    RadialAction a;
    CHECK(a.isEmpty());
    a.name = "test";
    CHECK(!a.isEmpty());
}

// ---------------------------------------------------------------- 枚举转换

static void testActionTypeKey() {
    CHECK(std::string(radialActionTypeKey(RadialActionType::Shortcut)) == "shortcut");
    bool recognized = false;
    CHECK(radialActionTypeFromKey("shortcut", &recognized) == RadialActionType::Shortcut);
    CHECK(recognized);
    recognized = false;
    radialActionTypeFromKey("unknown", &recognized);
    CHECK(!recognized);
}

static void testTriggerButtonKey() {
    CHECK(std::string(radialTriggerButtonKey(RadialTriggerButton::Middle)) == "middle");
    CHECK(radialTriggerButtonFromKey("side1") == RadialTriggerButton::Side1);
    CHECK(radialTriggerButtonFromKey("unknown") == RadialTriggerButton::Right);
}

static void testTriggerModifierKey() {
    CHECK(std::string(radialTriggerModifierKey(RadialTriggerModifier::Ctrl)) == "ctrl");
    CHECK(radialTriggerModifierFromKey("alt") == RadialTriggerModifier::Alt);
    CHECK(radialTriggerModifierFromKey("none") == RadialTriggerModifier::None);
}

// ---------------------------------------------------------------- 抑制键

static void testSuppressionConflict() {
    RadialTriggerConfig trigger;
    trigger.modifier = RadialTriggerModifier::Ctrl;

    // 没有抑制键 → 不冲突
    CHECK(!suppressionConflictsTrigger(trigger));

    // Ctrl 触发 + Ctrl 抑制 = 永远弹不出来，必须判冲突
    trigger.suppressWhileHeld = {HotkeyModifier::Control};
    CHECK(suppressionConflictsTrigger(trigger));

    // Alt 抑制与 Ctrl 触发无关
    trigger.suppressWhileHeld = {HotkeyModifier::Alt};
    CHECK(!suppressionConflictsTrigger(trigger));

    // Alt 触发 + Alt 抑制 → 冲突；Ctrl 抑制不冲突
    trigger.modifier = RadialTriggerModifier::Alt;
    CHECK(suppressionConflictsTrigger(trigger));
    trigger.suppressWhileHeld = {HotkeyModifier::Control, HotkeyModifier::Win};
    CHECK(!suppressionConflictsTrigger(trigger));

    // 触发修饰键为「无」时任何抑制键都不冲突（没有可撞的键）
    trigger.modifier = RadialTriggerModifier::None;
    trigger.suppressWhileHeld = {HotkeyModifier::Alt, HotkeyModifier::Control};
    CHECK(!suppressionConflictsTrigger(trigger));

    // 单项判断：Control 只跟 Ctrl 触发对应，Win 永远不冲突
    CHECK(modifierIsTriggerModifier(HotkeyModifier::Control,
                                    RadialTriggerModifier::Ctrl));
    CHECK(!modifierIsTriggerModifier(HotkeyModifier::Control,
                                     RadialTriggerModifier::Alt));
    CHECK(!modifierIsTriggerModifier(HotkeyModifier::Win,
                                     RadialTriggerModifier::Ctrl));
}

static void testValidateRejectsSuppressionConflict() {
    RadialConfig config;
    config.menu = RadialMenu("default", "全局", 4);
    config.trigger.modifier = RadialTriggerModifier::Alt;
    config.trigger.suppressWhileHeld = {HotkeyModifier::Alt};
    CHECK(!validateRadialConfig(config));

    config.trigger.suppressWhileHeld = {HotkeyModifier::Shift};
    CHECK(validateRadialConfig(config));
}

// ---------------------------------------------------------------- 轮盘插件

// 造一个最小可用插件
static RadialPlugin makePlugin(const char* id, std::vector<std::string> processes,
                               int priority, std::uint8_t sectors = 4) {
    RadialPlugin plugin;
    plugin.id = id;
    plugin.name = id;
    plugin.processes = std::move(processes);
    plugin.priority = priority;
    plugin.menu = RadialMenu(id, id, sectors);
    // 扇区动作留空（isEmpty 的动作是合法的，校验只看数量）
    plugin.valid = validateRadialPlugin(plugin);
    return plugin;
}

static void testFindPluginForProcess() {
    std::vector<RadialPlugin> plugins;
    plugins.push_back(makePlugin("explorer", {"explorer.exe"}, 0));
    plugins.push_back(makePlugin("code", {"Code.exe", "code - insiders.exe"}, 0));

    // 命中（大小写不敏感）
    const RadialPlugin* hit = findPluginForProcess(plugins, "EXPLORER.EXE");
    CHECK(hit != nullptr && hit->id == "explorer");
    hit = findPluginForProcess(plugins, "code.exe");
    CHECK(hit != nullptr && hit->id == "code");
    hit = findPluginForProcess(plugins, "Code - Insiders.exe");
    CHECK(hit != nullptr && hit->id == "code");

    // 未命中 / 空进程名
    CHECK(findPluginForProcess(plugins, "notepad.exe") == nullptr);
    CHECK(findPluginForProcess(plugins, "") == nullptr);
    CHECK(findPluginForProcess({}, "explorer.exe") == nullptr);

    // priority 大的赢
    plugins.push_back(makePlugin("explorer-pro", {"explorer.exe"}, 5));
    hit = findPluginForProcess(plugins, "explorer.exe");
    CHECK(hit != nullptr && hit->id == "explorer-pro");

    // 同优先级按 id 字典序，与插入顺序无关（结果必须确定）
    plugins.push_back(makePlugin("aaa", {"explorer.exe"}, 5));
    hit = findPluginForProcess(plugins, "explorer.exe");
    CHECK(hit != nullptr && hit->id == "aaa");

    // 无效插件即使 priority 更高也不参与匹配
    RadialPlugin broken = makePlugin("zzz", {"explorer.exe"}, 99);
    broken.valid = false;
    plugins.push_back(broken);
    hit = findPluginForProcess(plugins, "explorer.exe");
    CHECK(hit != nullptr && hit->id == "aaa");
}

static void testValidateRadialPlugin() {
    RadialPlugin plugin = makePlugin("ok", {"a.exe"}, 0);
    CHECK(validateRadialPlugin(plugin));

    // id / processes 为空
    RadialPlugin noId = makePlugin("", {"a.exe"}, 0);
    CHECK(!validateRadialPlugin(noId));
    RadialPlugin noProcess = makePlugin("x", {}, 0);
    CHECK(!validateRadialPlugin(noProcess));

    // 进程名里有空串：会让「按进程匹配」把所有人都当成命中
    RadialPlugin emptyName = makePlugin("x", {"a.exe", ""}, 0);
    CHECK(!validateRadialPlugin(emptyName));

    // 扇区数越界 / 动作数与扇区数不一致
    RadialPlugin twoSectors = makePlugin("x", {"a.exe"}, 0);
    twoSectors.menu.sectorCount = 2;
    twoSectors.menu.actions.resize(2);
    CHECK(!validateRadialPlugin(twoSectors));

    RadialPlugin mismatch = makePlugin("x", {"a.exe"}, 0, 4);
    mismatch.menu.actions.resize(6);
    CHECK(!validateRadialPlugin(mismatch));

    RadialPlugin nine = makePlugin("x", {"a.exe"}, 0, 8);
    nine.menu.sectorCount = 9;
    nine.menu.actions.resize(9);
    CHECK(!validateRadialPlugin(nine));
}

// ---------------------------------------------------------------- 入口

int wmain() {
    testSectorFromAngle4();
    testSectorFromAngle8();
    testSectorFromAngle6();
    testSectorFromAngleBoundary();
    testHitTestCenter();
    testHitTestOuter();
    testHitTestSectors();
    testHitTestAtRadiusBoundary();
    testBlacklist();
    testProcessNameEquals();
    testValidateConfig();
    testValidateActionsSizeMismatch();
    testValidateEmptyBlacklistEntry();
    testValidateThreshold();
    testEmptyAction();
    testActionTypeKey();
    testTriggerButtonKey();
    testTriggerModifierKey();
    testSuppressionConflict();
    testValidateRejectsSuppressionConflict();
    testFindPluginForProcess();
    testValidateRadialPlugin();

    wprintf(L"\n%d failures\n", g_failures);
    return g_failures;
}
