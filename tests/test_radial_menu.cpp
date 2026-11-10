// KTile - 轮盘菜单数据模型测试
#include "core/radial_menu.h"

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
    RadialMenu global("global", "全局", 8);
    global.actions[0].name = "复制";
    global.actions[0].type = RadialActionType::Shortcut;
    global.actions[0].modifiers = {HotkeyModifier::Control};
    global.actions[0].key = "C";
    config.menus.push_back(global);

    RadialMenu chrome("chrome", "Chrome", 8);
    chrome.actions[0].name = "新标签页";
    chrome.actions[0].type = RadialActionType::Shortcut;
    chrome.actions[0].modifiers = {HotkeyModifier::Control};
    chrome.actions[0].key = "T";
    config.menus.push_back(chrome);

    config.profiles.push_back(RadialProfile("chrome.exe", "chrome"));
    config.profiles.push_back(RadialProfile("", "global"));  // 全局默认
    return config;
}

static void testFindMenu() {
    const auto config = makeTestConfig();
    CHECK(config.findMenu("global") != nullptr);
    CHECK(config.findMenu("chrome") != nullptr);
    CHECK(config.findMenu("nonexistent") == nullptr);
}

static void testMenuForProcess() {
    const auto config = makeTestConfig();
    const RadialMenu* chrome = config.menuForProcess("chrome.exe");
    CHECK(chrome != nullptr);
    CHECK(chrome->id == "chrome");

    const RadialMenu* def = config.menuForProcess("notepad.exe");
    CHECK(def != nullptr);
    CHECK(def->id == "global");  // 无匹配 → 全局默认
}

// ---------------------------------------------------------------- 配置校验

static void testValidateConfig() {
    RadialConfig config = makeTestConfig();
    CHECK(validateRadialConfig(config));

    // 扇区数非法
    RadialMenu bad("bad", "bad", 3);
    bad.actions.resize(3);
    config.menus.push_back(bad);
    CHECK(!validateRadialConfig(config));
}

static void testValidateActionsSizeMismatch() {
    RadialConfig config;
    RadialMenu menu("m", "m", 8);
    menu.actions.resize(4);  // 不匹配
    config.menus.push_back(menu);
    CHECK(!validateRadialConfig(config));
}

static void testValidateProfileBadMenuId() {
    RadialConfig config;
    RadialMenu menu("m", "m", 8);
    config.menus.push_back(menu);
    config.profiles.push_back(RadialProfile("app.exe", "nonexistent"));
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
    testFindMenu();
    testMenuForProcess();
    testValidateConfig();
    testValidateActionsSizeMismatch();
    testValidateProfileBadMenuId();
    testValidateThreshold();
    testEmptyAction();
    testActionTypeKey();
    testTriggerButtonKey();
    testTriggerModifierKey();

    wprintf(L"\n%d failures\n", g_failures);
    return g_failures;
}
