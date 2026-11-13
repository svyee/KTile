// KTile - 基础设施层：配置与数据的持久化实现
#include "infrastructure/json/config_store.h"

#include "base/text_convert.h"
#include "infrastructure/json/json_writer.h"

#include <Windows.h>

#include <algorithm>
#include <vector>

namespace ktile {
namespace infrastructure {
namespace {

using json::JsonWriter;
using json::JsonValue;
using json::parseJson;

// ---- 枚举 <-> 字符串（持久化用的稳定键名）----

const char* itemTypeToKey(const core::ItemType type) {
    return core::itemTypeKey(type);
}

const char* hotkeyModifierToKey(const core::HotkeyModifier modifier) {
    switch (modifier) {
        case core::HotkeyModifier::Alt:     return "alt";
        case core::HotkeyModifier::Control: return "control";
        case core::HotkeyModifier::Shift:   return "shift";
        case core::HotkeyModifier::Win:     return "win";
    }
    return "alt";
}

core::HotkeyModifier hotkeyModifierFromKey(const std::string& key) {
    if (key == "control") return core::HotkeyModifier::Control;
    if (key == "shift")   return core::HotkeyModifier::Shift;
    if (key == "win")     return core::HotkeyModifier::Win;
    return core::HotkeyModifier::Alt;
}

// 可选的 uint32 颜色：0 表示「无」会被误判（透明黑是合法值），
// 因此用 has_value 显式控制 null 与否。
void writeOptionalUint32(JsonWriter& writer, const char* name,
                         const std::optional<std::uint32_t>& value) {
    writer.key(name);
    if (value.has_value()) {
        writer.value(*value);
    } else {
        writer.nullValue();
    }
}

void writeOptionalString(JsonWriter& writer, const char* name,
                         const std::optional<std::string>& value) {
    writer.key(name);
    if (value.has_value()) {
        writer.value(*value);
    } else {
        writer.nullValue();
    }
}

void writeOptionalUint16(JsonWriter& writer, const char* name,
                         const std::optional<std::uint16_t>& value) {
    writer.key(name);
    if (value.has_value()) {
        writer.value(static_cast<std::uint32_t>(*value));
    } else {
        writer.nullValue();
    }
}

// 把文件内容按 UTF-8 解码后当作 JSON 文本传入解析
std::string toUtf8(const std::string& raw) {
    return raw;  // 文件本身即 UTF-8，直接使用
}

}  // namespace

// --- 序列化

std::string ConfigStore::serializeItems(const core::ItemsDocument& document) {
    JsonWriter writer;
    writer.beginObject();
    writer.member("schemaVersion",
                  static_cast<std::uint32_t>(document.schemaVersion));

    writer.key("tabs").beginArray();
    for (const auto& tab : document.tabs) {
        writer.beginObject();
        writer.member("id", tab.id);
        writer.member("name", tab.name);

        writer.key("items").beginArray();
        for (const auto& item : tab.items) {
            writer.beginObject();
            writer.member("id", item.id);
            writer.member("type", itemTypeToKey(item.type));
            writer.member("name", item.name);
            writer.member("target", item.target);

            // 插件磁贴：pluginId + 参数表（参数会替换 manifest 里的
            // ${param}，例如天气插件的城市名）。非插件项不写，保持 JSON 干净。
            if (item.type == core::ItemType::Plugin) {
                writeOptionalString(writer, "pluginId", item.pluginId);
                writer.key("pluginParams").beginObject();
                for (const auto& entry : item.pluginParams) {
                    writer.member(entry.first, entry.second);
                }
                writer.endObject();
            }

            writer.key("arguments").beginArray();
            for (const auto& argument : item.arguments) {
                writer.value(argument);
            }
            writer.endArray();

            writeOptionalString(writer, "workingDirectory", item.workingDirectory);
            writeOptionalString(writer, "icon", item.icon);

            writer.member("runAsAdministrator", item.runAsAdministrator);

                    // ---- 磁贴 ----
                    writer.member("tileSize", core::tileSizeToInt(item.tileSize));
                    writeOptionalUint32(writer, "tileColor", item.tileColor);
                    writer.member("labelMode",
                                  core::tileLabelModeKey(item.labelMode));
                    // 0 = 跟随全局（写出去保持键集合稳定，便于手改对照）
                    writer.member("iconScalePercent", item.iconScalePercent);
                    writeOptionalUint16(writer, "gridColumn", item.gridColumn);
                    writeOptionalUint16(writer, "gridRow", item.gridRow);

            // ---- 统计 ----
            writer.member("launchCount",
                          static_cast<std::uint64_t>(item.launchCount));
            writeOptionalString(writer, "lastLaunchedAt", item.lastLaunchedAt);

            writer.endObject();
        }
        writer.endArray();

        writer.endObject();
    }
    writer.endArray();

    writer.endObject();
    return writer.take();
}

std::string ConfigStore::serializeConfig(const core::ApplicationConfig& config) {
    JsonWriter writer;
    writer.beginObject();
    writer.member("schemaVersion",
                  static_cast<std::uint32_t>(config.schemaVersion));
    writer.member("loggingEnabled", config.loggingEnabled);
    writer.member("language", config.language);

    // ---- appearance ----
    writer.key("appearance").beginObject();
    writer.member("gridColumns",
                  static_cast<std::uint32_t>(config.appearance.gridColumns));
    writer.member("gridRows",
                  static_cast<std::uint32_t>(config.appearance.gridRows));
    writer.member("defaultTileSize",
                  core::tileSizeToInt(config.appearance.defaultTileSize));
    writer.member("themeName", config.appearance.themeName);
    writer.member("backgroundImage", config.appearance.backgroundImage);
    writer.member("showTileLabels", config.appearance.showTileLabels);
    writer.member("alwaysShowTileLabels",
                  config.appearance.alwaysShowTileLabels);
    writer.member("tabBarAtBottom", config.appearance.tabBarAtBottom);
    writer.member("windowPlacement",
                  core::windowPlacementKey(config.appearance.windowPlacement));
    writer.member("lastWindowX", config.appearance.lastWindowX);
    writer.member("lastWindowY", config.appearance.lastWindowY);
    writer.member("lastWindowPositionValid",
                  config.appearance.lastWindowPositionValid);
    writer.member("pinned", config.appearance.pinned);
    writer.member("windowOpacityPercent",
                  config.appearance.windowOpacityPercent);
    writer.member("tileOpacityPercent", config.appearance.tileOpacityPercent);
    writeOptionalUint32(writer, "defaultTileColor",
                        config.appearance.defaultTileColor);
    writer.member("iconScalePercent", config.appearance.iconScalePercent);
    writeOptionalUint32(writer, "accentColor", config.appearance.accentColor);
    // 上次所在页（存 id）。空串代表还没记录，照样写出保持键集合稳定，
    // 便于手改对照。
    writer.member("activeTabId", config.appearance.activeTabId);
    writer.endObject();

    // ---- activation ----
    writer.key("activation").beginObject();
    writer.member("startWithWindows", config.activation.startWithWindows);
    writer.member("screenEdgeEnabled", config.activation.screenEdgeEnabled);

    // ---- 跟随启动 ----
    // 即使为空也写出：键集合稳定（手改时可照着补），清空在文件里也看得见。
    writer.key("followLaunch").beginArray();
    for (const auto& entry : config.activation.followLaunch) {
        writer.beginObject();
        writer.member("name", entry.name);
        writer.member("target", entry.target);
        // 来源与启动方式只在有内容时写：手动条目写四条恒为默认值的键并不
        // 对。反过来，手改给手动条目写了 arguments 也能存住：读取走同一段
        // 代码，写出时不区别对待。
        if (!entry.itemId.empty()) {
            writer.member("itemId", entry.itemId);
        }
        if (!entry.arguments.empty()) {
            writer.key("arguments").beginArray();
            for (const std::string& argument : entry.arguments) {
                writer.value(argument);
            }
            writer.endArray();
        }
        if (entry.workingDirectory.has_value() &&
            !entry.workingDirectory->empty()) {
            writer.member("workingDirectory", *entry.workingDirectory);
        }
        if (entry.runAsAdministrator) {
            writer.member("runAsAdministrator", true);
        }
        writer.endObject();
    }
    writer.endArray();

    writer.key("hotkey").beginObject();
    writer.member("enabled", config.activation.hotkey.enabled);
    writer.key("modifiers").beginArray();
    for (const auto modifier : config.activation.hotkey.modifiers) {
        writer.value(hotkeyModifierToKey(modifier));
    }
    writer.endArray();
    writer.member("key", config.activation.hotkey.key);
    // 手势（doubleCtrl / winKey / none）。缺键 = none，老配置行为不变。
    writer.member("gesture",
                  core::hotkeyGestureKey(config.activation.hotkey.gesture));
    writer.endObject();  // hotkey

    writer.key("topmostHotkey").beginObject();
    writer.member("enabled", config.activation.topmostHotkey.enabled);
    writer.key("modifiers").beginArray();
    for (const auto modifier : config.activation.topmostHotkey.modifiers) {
        writer.value(hotkeyModifierToKey(modifier));
    }
    writer.endArray();
    writer.member("key", config.activation.topmostHotkey.key);
    writer.endObject();  // topmostHotkey

    writer.endObject();  // activation

    writer.endObject();
    return writer.take();
}

// --- 反序列化

LoadResult<core::ItemsDocument>
ConfigStore::deserializeItems(const std::string& text) {
    LoadResult<core::ItemsDocument> result;
    result.value = core::ItemsDocument();
    // 失败路径也保证有默认 Tab，调用方可无条件假定 tabs 非空。
    result.value.ensureDefaultTab();

    const auto parsed = parseJson(text);
    if (!parsed.ok) {
        result.error = "JSON 解析失败（第 " + std::to_string(parsed.errorLine) +
                       " 行）：" + parsed.error;
        return result;
    }
    if (!parsed.root.isObject()) {
        result.error = "根节点不是对象";
        return result;
    }

    // 解析成功后重建：先清空默认 Tab，由文件内容决定
    result.value.tabs.clear();

    const JsonValue& root = parsed.root;

    // 读成功就盖成当前版本号（与 radial_store 同一策略）。没有迁移函数，
    // 也不需要：字段缺失走默认值、认不出的枚举回落、越界值夹取，老文件
    // 本来就能读。让版本号停在旧值只会失去「文件按哪版结构解释」的判断
    // 依据，下次保存又原样写回，版本号永远不动。
    result.value.schemaVersion = core::kCurrentSchemaVersion;

    const JsonValue& tabs = root.find("tabs");
    if (tabs.isArray()) {
        for (std::size_t i = 0; i < tabs.size(); ++i) {
            const JsonValue& tabNode = tabs.at(i);
            if (!tabNode.isObject()) {
                continue;
            }

            core::Tab tab;
            tab.id = tabNode.find("id").asString();
            tab.name = tabNode.find("name").asString();
            if (tab.id.empty() && tab.name.empty()) {
                continue;  // 完全空的 Tab 丢弃
            }

            const JsonValue& items = tabNode.find("items");
            if (items.isArray()) {
                for (std::size_t j = 0; j < items.size(); ++j) {
                    const JsonValue& itemNode = items.at(j);
                    if (!itemNode.isObject()) {
                        continue;
                    }

                    core::LaunchItem item;
                    item.id = itemNode.find("id").asString();
                    item.name = itemNode.find("name").asString();
                    item.target = itemNode.find("target").asString();

                    bool recognized = false;
                    item.type = core::itemTypeFromKey(
                        itemNode.find("type").asString(), &recognized);
                    (void)recognized;  // 未知类型已回退 Application

                    const JsonValue& arguments = itemNode.find("arguments");
                    if (arguments.isArray()) {
                        for (std::size_t k = 0; k < arguments.size(); ++k) {
                            item.arguments.push_back(arguments.at(k).asString());
                        }
                    }

                    const JsonValue& workingDir = itemNode.find("workingDirectory");
                    if (workingDir.isString()) {
                        item.workingDirectory = workingDir.asString();
                    }
                    const JsonValue& icon = itemNode.find("icon");
                    if (icon.isString()) {
                        item.icon = icon.asString();
                    }

                    // 插件磁贴（缺失键 = 普通项目，老数据不受影响）
                    const JsonValue& pluginId = itemNode.find("pluginId");
                    if (pluginId.isString() && !pluginId.asString().empty()) {
                        item.pluginId = pluginId.asString();
                    }
                    const JsonValue& pluginParams =
                        itemNode.find("pluginParams");
                    if (pluginParams.isObject()) {
                        for (const auto& entry : pluginParams.members()) {
                            if (entry.second.isString()) {
                                item.setPluginParam(entry.first,
                                                    entry.second.asString());
                            }
                        }
                    }

                    item.runAsAdministrator =
                        itemNode.find("runAsAdministrator").asBool(false);

                    item.tileSize = core::tileSizeFromInt(
                        static_cast<int>(itemNode.find("tileSize").asInt(0)));

                    const JsonValue& tileColor = itemNode.find("tileColor");
                    if (tileColor.isNumber()) {
                        item.tileColor = tileColor.asUint32(0);
                    }

                    // 缺失 → 跟随全局（老数据天然落在这一支，行为不变）
                    const JsonValue& labelMode = itemNode.find("labelMode");
                    if (labelMode.isString()) {
                        item.labelMode =
                            core::tileLabelModeFromKey(labelMode.asString());
                    }

                    // 图标缩放（缺失/非法 → 0 = 跟随全局统一设置）
                    const JsonValue& iconScale =
                        itemNode.find("iconScalePercent");
                    item.iconScalePercent =
                        iconScale.isNumber() ? iconScale.asInt(0) : 0;
                    if (item.iconScalePercent != 0) {
                        item.iconScalePercent = std::clamp(
                            item.iconScalePercent, 50, 100);
                    }

                    const JsonValue& column = itemNode.find("gridColumn");
                    if (column.isNumber()) {
                        item.gridColumn = static_cast<std::uint16_t>(
                            column.asUint32(0));
                    }
                    const JsonValue& row = itemNode.find("gridRow");
                    if (row.isNumber()) {
                        item.gridRow = static_cast<std::uint16_t>(
                            row.asUint32(0));
                    }

                    item.launchCount = static_cast<std::uint64_t>(
                        itemNode.find("launchCount").asInt(0));

                    const JsonValue& lastLaunched =
                        itemNode.find("lastLaunchedAt");
                    if (lastLaunched.isString()) {
                        item.lastLaunchedAt = lastLaunched.asString();
                    }

                    // 丢弃完全无意义的条目（无 target 且无 name）
                    if (item.target.empty() && item.name.empty()) {
                        continue;
                    }

                    tab.items.push_back(std::move(item));
                }
            }

            result.value.tabs.push_back(std::move(tab));
        }
    }

    // 至少要有一个 Tab，便于后续代码假定 tabs 非空
    result.value.ensureDefaultTab();

    result.ok = true;
    result.fromFile = true;
    return result;
}

LoadResult<core::ApplicationConfig>
ConfigStore::deserializeConfig(const std::string& text) {
    LoadResult<core::ApplicationConfig> result;
    result.value = core::ApplicationConfig();

    const auto parsed = parseJson(text);
    if (!parsed.ok) {
        result.error = "JSON 解析失败（第 " + std::to_string(parsed.errorLine) +
                       " 行）：" + parsed.error;
        return result;
    }
    if (!parsed.root.isObject()) {
        result.error = "根节点不是对象";
        return result;
    }

    const JsonValue& root = parsed.root;
    // 读成功就盖成当前版本号：字段缺失走默认值、枚举回落、越界夹取，老
    // 文件本来就能读，不需要迁移函数。让版本号停在旧值只会失去「按哪版
    // 结构解释」的判断依据，下次保存又原样写回，那它永远不动。
    result.value.schemaVersion = core::kCurrentSchemaVersion;
    result.value.loggingEnabled = root.find("loggingEnabled").asBool(true);
    result.value.language = root.find("language").asString();
    if (result.value.language.empty()) {
        // 老配置文件没有这一项：按「跟随系统」处理，与首次运行一致
        result.value.language = "auto";
    }

    const JsonValue& appearance = root.find("appearance");
    if (appearance.isObject()) {
        auto& config = result.value.appearance;

        const std::uint32_t columns = appearance.find("gridColumns").asUint32(
            core::kDefaultGridColumns);
        const std::uint32_t rows = appearance.find("gridRows").asUint32(
            core::kDefaultGridRows);
        config.gridColumns = static_cast<std::uint16_t>(
            columns < core::kMinimumGridColumns ? core::kMinimumGridColumns
            : (columns > core::kMaximumGridColumns ? core::kMaximumGridColumns
                                                   : columns));
        config.gridRows = static_cast<std::uint16_t>(
            rows < core::kMinimumGridRows ? core::kMinimumGridRows
            : (rows > core::kMaximumGridRows ? core::kMaximumGridRows : rows));

        config.defaultTileSize = core::tileSizeFromInt(static_cast<int>(
            appearance.find("defaultTileSize").asInt(0)));

        // 皮肤与背景图（老配置没有这两项 → 空 = 默认配色、无背景图）。
        // 旧版本的 backdrop / opacityPercent 键只是存储、从未参与渲染，
        // 已移除；读到也不影响（解析器按 key 取值，多余键被忽略）。
        config.themeName = appearance.find("themeName").asString();
        config.backgroundImage =
            appearance.find("backgroundImage").asString();

        config.showTileLabels = appearance.find("showTileLabels").asBool(true);
        // 老配置没有这一项，默认 true（标题常驻），与产品默认一致
        config.alwaysShowTileLabels =
            appearance.find("alwaysShowTileLabels").asBool(true);
        // 老配置没有这一项，默认 false（标签栏在顶部），行为不变
        config.tabBarAtBottom =
            appearance.find("tabBarAtBottom").asBool(false);

        // 窗口放置策略（老配置没有 → 跟随鼠标，这是产品默认）
        config.windowPlacement = core::windowPlacementFromKey(
            appearance.find("windowPlacement").asString());
        config.lastWindowX =
            static_cast<std::int32_t>(appearance.find("lastWindowX").asInt(0));
        config.lastWindowY =
            static_cast<std::int32_t>(appearance.find("lastWindowY").asInt(0));
        config.lastWindowPositionValid =
            appearance.find("lastWindowPositionValid").asBool(false);
        config.pinned = appearance.find("pinned").asBool(false);

        // 不透明度（老配置没有 → 100% 不透明 + 磁贴跟随皮肤，观感不变）。
        // 读入后仍夹一次范围：手改 JSON 写成 20 或 999 都不该让窗口消失。
        {
            const int windowOpacity = appearance.find("windowOpacityPercent")
                                          .asInt(100);
            config.windowOpacityPercent =
                windowOpacity < 50 ? 50 : (windowOpacity > 100 ? 100
                                                               : windowOpacity);
            const int tileOpacity = appearance.find("tileOpacityPercent").asInt(-1);
            if (tileOpacity < 0) {
                config.tileOpacityPercent = -1;  // 跟随皮肤
            } else {
                config.tileOpacityPercent =
                    tileOpacity > 100 ? 100 : tileOpacity;
            }

            // 图标缩放（老配置没有 → 90%。读入后仍夹一次范围）
            const int iconScale =
                appearance.find("iconScalePercent").asInt(90);
            config.iconScalePercent =
                iconScale < 50 ? 50 : (iconScale > 100 ? 100 : iconScale);
        }

        // 全局默认磁贴底色（老配置没有 → 空 = 跟随皮肤，行为不变）
        {
            const JsonValue& defaultTileColor =
                appearance.find("defaultTileColor");
            if (defaultTileColor.isNumber()) {
                config.defaultTileColor = defaultTileColor.asUint32(0);
            }
        }

        const JsonValue& accent = appearance.find("accentColor");
        if (accent.isNumber()) {
            config.accentColor = accent.asUint32(0);
        }

        // 上次所在页（老配置里没有这一项 → 空串 → 停在第一页）
        config.activeTabId = appearance.find("activeTabId").asString();
    }

    const JsonValue& activation = root.find("activation");
    if (activation.isObject()) {
        auto& config = result.value.activation;
        config.startWithWindows = activation.find("startWithWindows").asBool(false);
        config.screenEdgeEnabled = activation.find("screenEdgeEnabled").asBool(false);

        // 跟随启动：缺键 = 空表（v2 及更老的配置都走这里）。
        // 只收有 target 的条目，并夹到上限；手改配置写坏了不至于每次
        // 启动都去拉几十个进程。
        const JsonValue& follow = activation.find("followLaunch");
        if (follow.isArray()) {
            config.followLaunch.clear();
            for (std::size_t i = 0; i < follow.size(); ++i) {
                if (config.followLaunch.size() >=
                    core::kMaximumFollowLaunchItems) {
                    break;
                }
                const JsonValue& node = follow.at(i);
                if (!node.isObject()) {
                    continue;
                }
                core::FollowLaunchEntry entry;
                entry.name = node.find("name").asString();
                entry.target = node.find("target").asString();
                if (entry.target.empty()) {
                    continue;  // 没目标的行没有意义，别带进内存
                }
                if (entry.name.size() > core::kMaximumNameLength) {
                    entry.name.resize(core::kMaximumNameLength);
                }
                if (entry.target.size() > core::kMaximumTargetLength) {
                    entry.target.resize(core::kMaximumTargetLength);
                }

                // v4 新增。没有 itemId = 手动添加的老条目，只认 target，
                // 行为与 v3 完全一致。
                entry.itemId = node.find("itemId").asString();
                if (entry.itemId.size() > core::kMaximumIdLength) {
                    entry.itemId.resize(core::kMaximumIdLength);
                }
                const JsonValue& arguments = node.find("arguments");
                if (arguments.isArray()) {
                    for (std::size_t a = 0; a < arguments.size(); ++a) {
                        entry.arguments.push_back(arguments.at(a).asString());
                    }
                }
                const std::string workingDirectory =
                    node.find("workingDirectory").asString();
                if (!workingDirectory.empty()) {
                    entry.workingDirectory =
                        workingDirectory.size() > core::kMaximumTargetLength
                            ? workingDirectory.substr(0,
                                                      core::kMaximumTargetLength)
                            : workingDirectory;
                }
                entry.runAsAdministrator =
                    node.find("runAsAdministrator").asBool(false);

                config.followLaunch.push_back(std::move(entry));
            }
        }

        const JsonValue& hotkey = activation.find("hotkey");
        if (hotkey.isObject()) {
            config.hotkey.enabled = hotkey.find("enabled").asBool(true);

            const JsonValue& modifiers = hotkey.find("modifiers");
            if (modifiers.isArray() && modifiers.size() > 0) {
                config.hotkey.modifiers.clear();
                for (std::size_t i = 0; i < modifiers.size(); ++i) {
                    config.hotkey.modifiers.push_back(
                        hotkeyModifierFromKey(modifiers.at(i).asString()));
                }
            }

            const std::string key = hotkey.find("key").asString();
            if (!key.empty()) {
                config.hotkey.key = key;
            }

            // 手势缺键 = none（用 modifiers + key），保证老配置零迁移
            config.hotkey.gesture = core::hotkeyGestureFromKey(
                hotkey.find("gesture").asString());
        }

        // 置顶快捷键。两代旧格式都要认：更早是 bool 开关
        //（topmostHotkeyEnabled），上一版是预设字符串（"f7" / "none" /
        // "ctrlF7" / "altF7"），当前是 {enabled, modifiers, key} 对象。
        const JsonValue& topmost = activation.find("topmostHotkey");
        if (topmost.isObject()) {
            config.topmostHotkey.enabled = topmost.find("enabled").asBool(true);
            const JsonValue& modifiers = topmost.find("modifiers");
            if (modifiers.isArray()) {
                config.topmostHotkey.modifiers.clear();
                for (std::size_t i = 0; i < modifiers.size(); ++i) {
                    config.topmostHotkey.modifiers.push_back(
                        hotkeyModifierFromKey(modifiers.at(i).asString()));
                }
            }
            const std::string key = topmost.find("key").asString();
            if (!key.empty()) {
                config.topmostHotkey.key = key;
            }
        } else if (topmost.isString()) {
            const std::string preset = topmost.asString();
            config.topmostHotkey = core::defaultTopmostHotkey();
            if (preset == "none") {
                config.topmostHotkey.enabled = false;
            } else if (preset == "ctrlF7") {
                config.topmostHotkey.modifiers = {core::HotkeyModifier::Control};
            } else if (preset == "altF7") {
                config.topmostHotkey.modifiers = {core::HotkeyModifier::Alt};
            }
        } else if (!activation.find("topmostHotkeyEnabled").asBool(true)) {
            config.topmostHotkey = core::defaultTopmostHotkey();
            config.topmostHotkey.enabled = false;
        }
    }

    result.ok = true;
    result.fromFile = true;
    return result;
}

// --- 文件读写

bool ConfigStore::readFileText(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        CloseHandle(file);
        out.clear();
        return size.QuadPart == 0;  // 空文件算成功读取（内容为空串）
    }
    if (size.QuadPart > 64LL * 1024 * 1024) {  // 64MB 上限，防异常文件
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD totalRead = 0;
    while (totalRead < out.size()) {
        DWORD chunk = 0;
        const DWORD toRead = static_cast<DWORD>(
            (out.size() - totalRead) > 0xFFFFFFFFu
                ? 0xFFFFFFFFu
                : (out.size() - totalRead));
        if (!ReadFile(file, &out[totalRead], toRead, &chunk, nullptr) ||
            chunk == 0) {
            CloseHandle(file);
            return false;
        }
        totalRead += chunk;
    }

    CloseHandle(file);

    // 去掉 UTF-8 BOM（若存在）
    if (out.size() >= 3 &&
        static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

bool ConfigStore::writeFileText(const std::wstring& path,
                                const std::string& text,
                                std::string& error) {
    // 原子写：先写临时文件，再 MoveFileEx 替换
    const std::wstring tempPath = path + L".tmp";

    HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "无法创建临时文件（错误码 " +
                std::to_string(GetLastError()) + "）";
        return false;
    }

    std::size_t written = 0;
    while (written < text.size()) {
        DWORD chunk = 0;
        const DWORD toWrite = static_cast<DWORD>(
            (text.size() - written) > 0xFFFFFFFFu
                ? 0xFFFFFFFFu
                : (text.size() - written));
        if (!WriteFile(file, text.data() + written, toWrite, &chunk, nullptr) ||
            chunk == 0) {
            CloseHandle(file);
            DeleteFileW(tempPath.c_str());
            error = "写入失败（错误码 " + std::to_string(GetLastError()) + "）";
            return false;
        }
        written += chunk;
    }

    // 刷盘后关闭，保证数据真正落盘再替换
    FlushFileBuffers(file);
    CloseHandle(file);

    if (!MoveFileExW(tempPath.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tempPath.c_str());
        error = "替换目标文件失败（错误码 " +
                std::to_string(GetLastError()) + "）";
        return false;
    }
    return true;
}

void ConfigStore::backupCorruptFile(const std::wstring& path) {
    const std::wstring backup = path + L".corrupt";
    // 已存在旧备份就删掉，只保留最近一次
    DeleteFileW(backup.c_str());
    MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
}

LoadResult<core::ItemsDocument>
ConfigStore::loadItems(const std::wstring& path) const {
    std::string text;
    if (!readFileText(path, text)) {
        // 文件不存在：首次运行，返回默认值并视为成功
        LoadResult<core::ItemsDocument> result;
        result.ok = true;
        result.fromFile = false;
        result.value.ensureDefaultTab();
        return result;
    }

    auto result = deserializeItems(text);
    if (!result.ok) {
        backupCorruptFile(path);
    }
    return result;
}

SaveResult ConfigStore::saveItems(const std::wstring& path,
                                  const core::ItemsDocument& document) const {
    SaveResult result;
    const std::string text = serializeItems(document);

    std::string error;
    if (!writeFileText(path, text, error)) {
        result.error = error;
        return result;
    }
    result.ok = true;
    return result;
}

LoadResult<core::ApplicationConfig>
ConfigStore::loadConfig(const std::wstring& path) const {
    std::string text;
    if (!readFileText(path, text)) {
        LoadResult<core::ApplicationConfig> result;
        result.ok = true;
        result.fromFile = false;
        result.value = core::ApplicationConfig();
        return result;
    }

    auto result = deserializeConfig(text);
    if (!result.ok) {
        backupCorruptFile(path);
    }
    return result;
}

SaveResult ConfigStore::saveConfig(const std::wstring& path,
                                   const core::ApplicationConfig& config) const {
    SaveResult result;
    const std::string text = serializeConfig(config);

    std::string error;
    if (!writeFileText(path, text, error)) {
        result.error = error;
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace infrastructure
}  // namespace ktile
