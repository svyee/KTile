// KTile - 插件绑定求值测试
#include "core/plugin_binding.h"

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

#define CHECK_STR(actual, expected)                                       \
    do {                                                                  \
        const std::string a_ = (actual);                                  \
        const std::string e_ = (expected);                                \
        if (a_ != e_) {                                                   \
            wprintf(L"FAIL line %d: \"%hs\" != \"%hs\"\n", __LINE__,      \
                    a_.c_str(), e_.c_str());                              \
            ++g_failures;                                                 \
        }                                                                 \
    } while (false)

// ---------------------------------------------------------------- 构造数据

static PluginValue makeWeatherRoot() {
    PluginValue root = PluginValue::makeObject();

    PluginValue current = PluginValue::makeObject();
    current.members.emplace_back("temp", PluginValue::makeNumber(23.4));
    current.members.emplace_back("city", PluginValue::makeString("Beijing"));
    current.members.emplace_back("code", PluginValue::makeString("2"));
    root.members.emplace_back("current", std::move(current));

    PluginValue forecast = PluginValue::makeArray();
    for (int index = 0; index < 3; ++index) {
        PluginValue day = PluginValue::makeObject();
        day.members.emplace_back(
            "date", PluginValue::makeString("2026-11-0" + std::to_string(index + 3)));
        day.members.emplace_back("max", PluginValue::makeNumber(20.0 + index));
        forecast.elements.push_back(std::move(day));
    }
    root.members.emplace_back("forecast", std::move(forecast));
    return root;
}

// ---------------------------------------------------------------- 路径

static void testResolvePath() {
    const PluginValue root = makeWeatherRoot();
    const std::vector<std::pair<std::string, std::string>> selectors = {
        {"now", "current"},
        {"days", "forecast"},
    };

    PluginBindingContext context;
    context.root = &root;
    context.selectors = &selectors;

    const PluginValue* temp = pluginResolvePath(context, "now.temp");
    CHECK(temp != nullptr);
    CHECK(temp != nullptr && temp->isNumber());
    CHECK(temp != nullptr && temp->number > 23.39 && temp->number < 23.41);

    const PluginValue* firstDate = pluginResolvePath(context, "days.0.date");
    CHECK(firstDate != nullptr);
    CHECK_STR(firstDate != nullptr ? firstDate->text : std::string(), "2026-11-03");

    // 不经过 select 也能直接访问 root 字段
    const PluginValue* direct = pluginResolvePath(context, "forecast.2.max");
    CHECK(direct != nullptr);
    CHECK(direct != nullptr && direct->number > 21.9);

    CHECK(pluginResolvePath(context, "now.missing") == nullptr);
    CHECK(pluginResolvePath(context, "days.9.date") == nullptr);
}

static void testRepeatLocals() {
    const PluginValue root = makeWeatherRoot();
    const std::vector<std::pair<std::string, std::string>> selectors = {
        {"days", "forecast"},
    };
    const PluginValue* day = root.find("forecast")->at(1);

    std::vector<std::pair<std::string, const PluginValue*>> locals = {
        {"day", day},
    };

    PluginBindingContext context;
    context.root = &root;
    context.selectors = &selectors;
    context.locals = &locals;

    CHECK(pluginResolvePath(context, "day.date") != nullptr);
    CHECK_STR(pluginResolvePath(context, "day.date")->text, "2026-11-04");

    // 局部变量优先于 select 同名
    std::vector<std::pair<std::string, const PluginValue*>> shadow = {
        {"days", day},
    };
    PluginBindingContext shadowContext = context;
    shadowContext.locals = &shadow;
    CHECK(pluginResolvePath(shadowContext, "days.date") != nullptr);
}

// ---------------------------------------------------------------- 模板

static void testTemplate() {
    const PluginValue root = makeWeatherRoot();
    const std::vector<std::pair<std::string, std::string>> selectors = {
        {"now", "current"},
    };
    PluginBindingContext context;
    context.root = &root;
    context.selectors = &selectors;

    CHECK_STR(pluginResolveTemplate("${now.temp}", context), "23.4");
    CHECK_STR(pluginResolveTemplate("${now.temp} C", context), "23.4 C");
    CHECK_STR(pluginResolveTemplate("${now.missing}x", context), "x");
    CHECK_STR(pluginResolveTemplate("no placeholder", context), "no placeholder");
    CHECK_STR(pluginResolveTemplate("", context), "");
}

static void testNumberFormatting() {
    CHECK_STR(PluginValue::makeNumber(20.0).displayString(), "20");
    CHECK_STR(PluginValue::makeNumber(20.04).displayString(), "20");
    CHECK_STR(PluginValue::makeNumber(-3.25).displayString(), "-3.2");
    CHECK_STR(PluginValue::makeNumber(100.0).displayString(), "100");
    CHECK_STR(PluginValue::makeBool(true).displayString(), "true");
    CHECK_STR(PluginValue::makeString("x").displayString(), "x");
    CHECK_STR(PluginValue::makeNull().displayString(), "");
}

static void testNumberResolution() {
    const PluginValue root = makeWeatherRoot();
    const std::vector<std::pair<std::string, std::string>> selectors = {
        {"now", "current"},
    };
    PluginBindingContext context;
    context.root = &root;
    context.selectors = &selectors;

    CHECK(pluginResolveNumber("${now.temp}", context, 0.0) > 23.39);
    CHECK(pluginResolveNumber("42", context, 0.0) > 41.99);
    CHECK(pluginResolveNumber("${now.missing}", context, 7.0) == 7.0);
    CHECK(pluginResolveNumber("abc", context, 7.0) == 7.0);
}

// ---------------------------------------------------------------- 参数与颜色

static void testParamTemplate() {
    const std::vector<std::pair<std::string, std::string>> params = {
        {"city", "Shanghai"},
        {"lang", "zh"},
    };
    CHECK_STR(pluginResolveParamTemplate("https://x/${city}?l=${lang}", params),
              "https://x/Shanghai?l=zh");
    CHECK_STR(pluginResolveParamTemplate("${unknown}", params), "");
    CHECK_STR(pluginResolveParamTemplate("plain", params), "plain");
}

static void testColorParsing() {
    CHECK(pluginParseColor("#112233", 0) == 0xFF112233U);
    CHECK(pluginParseColor("#80112233", 0) == 0x80112233U);
    CHECK(pluginParseColor("112233", 0) == 0U);
    CHECK(pluginParseColor("#zzz", 7U) == 7U);
}

// ---------------------------------------------------------------- main

int wmain() {
    testResolvePath();
    testRepeatLocals();
    testTemplate();
    testNumberFormatting();
    testNumberResolution();
    testParamTemplate();
    testColorParsing();

    if (g_failures != 0) {
        wprintf(L"plugin_binding: %d failure(s)\n", g_failures);
        return 1;
    }
    wprintf(L"plugin_binding: all passed\n");
    return 0;
}
