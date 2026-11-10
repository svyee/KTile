// KTile - 轮盘菜单配置持久化实现
#include "infrastructure/json/radial_store.h"

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

const char* hotkeyModifierToKey(core::HotkeyModifier modifier) {
    switch (modifier) {
        case core::HotkeyModifier::Alt:     return "alt";
        case core::HotkeyModifier::Control: return "control";
        case core::HotkeyModifier::Shift:   return "shift";
        case core::HotkeyModifier::Win:     return "win";
    }
    return "alt";
}

core::HotkeyModifier hotkeyModifierFromKey(const std::string& key,
                                          bool* recognized = nullptr) {
    if (recognized) *recognized = true;
    if (key == "control" || key == "ctrl") return core::HotkeyModifier::Control;
    if (key == "shift") return core::HotkeyModifier::Shift;
    if (key == "win")   return core::HotkeyModifier::Win;
    if (key == "alt")   return core::HotkeyModifier::Alt;
    // 认不出的键名告诉调用方，别默默当成 Alt —— 手写 "ctrl" 曾经会被
    // 理解成 Alt，用户看到的是「抑制键根本没生效」。
    if (recognized) *recognized = false;
    return core::HotkeyModifier::Alt;
}

// 生成默认的 8 扇区全局轮盘（首次运行用）
core::RadialConfig makeDefaultConfig() {
    core::RadialConfig config;
    core::RadialMenu global("default", "全局默认", 8);

    // 预置几个常用动作，其余留空
    global.actions[0].type = core::RadialActionType::Shortcut;
    global.actions[0].name = "复制";
    global.actions[0].modifiers = {core::HotkeyModifier::Control};
    global.actions[0].key = "C";

    global.actions[2].type = core::RadialActionType::Shortcut;
    global.actions[2].name = "粘贴";
    global.actions[2].modifiers = {core::HotkeyModifier::Control};
    global.actions[2].key = "V";

    global.actions[4].type = core::RadialActionType::Shortcut;
    global.actions[4].name = "剪切";
    global.actions[4].modifiers = {core::HotkeyModifier::Control};
    global.actions[4].key = "X";

    global.actions[6].type = core::RadialActionType::Shortcut;
    global.actions[6].name = "撤销";
    global.actions[6].modifiers = {core::HotkeyModifier::Control};
    global.actions[6].key = "Z";

    config.menu = global;
    return config;
}

}  // namespace

// ================================================================ 序列化

std::string RadialStore::serialize(const core::RadialConfig& config) {
    JsonWriter writer;
    writer.beginObject();
    writer.member("schemaVersion",
                  static_cast<std::uint32_t>(config.schemaVersion));

    // trigger
    writer.key("trigger").beginObject();
    writer.member("enabled", config.trigger.enabled);
    writer.member("button", core::radialTriggerButtonKey(config.trigger.button));
    writer.member("modifier", core::radialTriggerModifierKey(config.trigger.modifier));
    writer.member("activationThresholdPx",
                  static_cast<std::uint32_t>(config.trigger.activationThresholdPx));
    writer.member("cancelRadiusPx",
                  static_cast<std::uint32_t>(config.trigger.cancelRadiusPx));
    // 抑制键：空列表不写，保持文件干净（缺字段时读回来就是「不抑制」）
    if (!config.trigger.suppressWhileHeld.empty()) {
        writer.key("suppressWhileHeld").beginArray();
        for (core::HotkeyModifier modifier : config.trigger.suppressWhileHeld) {
            writer.value(hotkeyModifierToKey(modifier));
        }
        writer.endArray();
    }
    writer.endObject();

    // menu —— 只有一份通用轮盘，所以直接写对象而不是数组。
    // 老版本（schemaVersion 2）这里是 menus 数组，读的时候兼容。
    {
        const auto& menu = config.menu;
        writer.key("menu").beginObject();
        writer.member("name", menu.name);
        writer.member("sectorCount", static_cast<std::uint32_t>(menu.sectorCount));

        writer.key("actions").beginArray();
        for (const auto& action : menu.actions) {
            writer.beginObject();
            writer.member("type", core::radialActionTypeKey(action.type));
            writer.member("name", action.name);
            writer.member("target", action.target);

            if (!action.arguments.empty()) {
                writer.key("arguments").beginArray();
                for (const auto& arg : action.arguments) {
                    writer.value(arg);
                }
                writer.endArray();
            }
            if (action.icon.has_value()) {
                writer.member("icon", *action.icon);
            }
            if (!action.modifiers.empty()) {
                writer.key("modifiers").beginArray();
                for (const auto& mod : action.modifiers) {
                    writer.value(hotkeyModifierToKey(mod));
                }
                writer.endArray();
            }
            if (!action.key.empty()) {
                writer.member("key", action.key);
            }
            writer.endObject();
        }
        writer.endArray();  // actions
        writer.endObject();  // menu
    }

    // blacklist：命中即不弹轮盘
    if (!config.blacklist.empty()) {
        writer.key("blacklist").beginArray();
        for (const auto& blocked : config.blacklist) {
            writer.value(blocked);
        }
        writer.endArray();
    }

    writer.endObject();
    return writer.take();
}

// ================================================================ 反序列化

LoadResult<core::RadialConfig> RadialStore::deserialize(const std::string& text) {
    LoadResult<core::RadialConfig> result;
    result.value = makeDefaultConfig();

    auto parsed = parseJson(text);
    if (!parsed.ok || !parsed.root.isObject()) {
        result.ok = false;
        result.error = parsed.error.empty() ? "JSON 解析失败" : parsed.error;
        return result;
    }

    const JsonValue& root = parsed.root;
    result.value.schemaVersion = root.find("schemaVersion").asUint32(1);

    // trigger
    const JsonValue& trigger = root.find("trigger");
    if (trigger.isObject()) {
        const core::RadialTriggerConfig defaults;
        result.value.trigger.enabled = trigger.find("enabled").asBool(true);
        result.value.trigger.button =
            core::radialTriggerButtonFromKey(trigger.find("button").asString());

        // modifier 缺失（而不是写了个认不出的值）时回落到默认的 Ctrl。
        // 手写 trigger 忘了这一项的用户想要的多半是「别动我的右键」，
        // 而 fromKey 对空串给的是 None = 纯右键拖动，会把右键整个吞掉。
        const std::string modifierKey = trigger.find("modifier").asString();
        result.value.trigger.modifier =
            modifierKey.empty() ? defaults.modifier
                                : core::radialTriggerModifierFromKey(modifierKey);
        result.value.trigger.activationThresholdPx = static_cast<std::uint16_t>(
            trigger.find("activationThresholdPx").asUint32(8));
        result.value.trigger.cancelRadiusPx = static_cast<std::uint16_t>(
            trigger.find("cancelRadiusPx").asUint32(160));

        // 手改过的离谱值夹回可用区间，别让不合法的配置流到 UI 和窗口里。
        // （阈值 0 会让「按下即呼出」，卡死右键；取消半径小于花瓣外缘
        //   会把花瓣自己判成已甩出，看起来像半个轮盘坏了。）
        if (result.value.trigger.activationThresholdPx < 1) {
            result.value.trigger.activationThresholdPx = 1;
        }
        if (result.value.trigger.cancelRadiusPx < core::kMinCancelRadiusDip) {
            result.value.trigger.cancelRadiusPx = core::kMinCancelRadiusDip;
        }
        if (result.value.trigger.cancelRadiusPx > core::kMaxCancelRadiusDip) {
            result.value.trigger.cancelRadiusPx = core::kMaxCancelRadiusDip;
        }

        // 抑制键：认不出的名字跳过、重复的去重、与触发修饰键撞车的剔除。
        //
        // 撞车那一项选择「静默丢掉抑制项」而不是「整份配置打回默认」：
        // 用户手写 "alt" 抑制 + Alt 触发，本意多半是「换个触发键」，丢掉
        // 抑制至少还能用；整份回默认会把辛苦配的扇区全清掉。
        const JsonValue& suppressed = trigger.find("suppressWhileHeld");
        if (suppressed.isArray()) {
            for (std::size_t i = 0; i < suppressed.items().size(); ++i) {
                const std::string key = suppressed.at(i).asString();
                if (key.empty()) continue;
                bool recognized = false;
                const core::HotkeyModifier modifier =
                    hotkeyModifierFromKey(key, &recognized);
                if (!recognized) continue;
                if (core::modifierIsTriggerModifier(
                        modifier, result.value.trigger.modifier)) {
                    continue;
                }
                auto& held = result.value.trigger.suppressWhileHeld;
                if (std::find(held.begin(), held.end(), modifier) ==
                    held.end()) {
                    held.push_back(modifier);
                }
            }
        }
    }

    // ---- 解析一个 menu 对象（新旧两种格式共用这段逻辑） ----
    auto parseMenuObject = [](const JsonValue& m, core::RadialMenu* out) {
        out->name = m.find("name").asString();
        out->sectorCount = static_cast<std::uint8_t>(
            m.find("sectorCount").asUint32(8));
        if (out->sectorCount < core::kMinSectors) {
            out->sectorCount = core::kMinSectors;
        }
        if (out->sectorCount > core::kMaxSectors) {
            out->sectorCount = core::kMaxSectors;
        }

        out->actions.clear();
        const JsonValue& actions = m.find("actions");
        if (actions.isArray()) {
            for (std::size_t j = 0; j < actions.items().size(); ++j) {
                const JsonValue& a = actions.at(j);
                if (!a.isObject()) continue;

                core::RadialAction action;
                bool recognized = false;
                action.type = core::radialActionTypeFromKey(
                    a.find("type").asString(), &recognized);
                action.name = a.find("name").asString();
                action.target = a.find("target").asString();

                const JsonValue& args = a.find("arguments");
                if (args.isArray()) {
                    for (std::size_t k = 0; k < args.items().size(); ++k) {
                        action.arguments.push_back(args.at(k).asString());
                    }
                }
                if (a.hasKey("icon") && !a.find("icon").isNull()) {
                    action.icon = a.find("icon").asString();
                }
                const JsonValue& mods = a.find("modifiers");
                if (mods.isArray()) {
                    for (std::size_t k = 0; k < mods.items().size(); ++k) {
                        action.modifiers.push_back(
                            hotkeyModifierFromKey(mods.at(k).asString()));
                    }
                }
                if (a.hasKey("key")) {
                    action.key = a.find("key").asString();
                }
                out->actions.push_back(action);
            }
        }
        // 补齐到 sectorCount 个
        while (out->actions.size() < out->sectorCount) {
            out->actions.push_back(core::RadialAction());
        }
    };

    // ---- menu：新格式是单个对象，老格式是 menus 数组 ----
    const JsonValue& menuValue = root.find("menu");
    if (menuValue.isObject()) {
        parseMenuObject(menuValue, &result.value.menu);
    } else {
        // schemaVersion <= 2 的文件：menus 里可能有多份方案，优先取 id 为
        // "default" 的那份（老版本的全局默认），没有就取第一份。
        // profiles（按应用白名单）整段丢弃 —— 按应用差异化已经交给 slugin
        // 插件目录，用户侧只留一份通用轮盘。
        const JsonValue& menus = root.find("menus");
        if (menus.isArray()) {
            const JsonValue* chosen = nullptr;
            for (std::size_t i = 0; i < menus.items().size(); ++i) {
                const JsonValue& m = menus.at(i);
                if (!m.isObject()) continue;
                if (chosen == nullptr) chosen = &m;
                if (m.find("id").asString() == "default") {
                    chosen = &m;
                    break;
                }
            }
            if (chosen != nullptr) {
                parseMenuObject(*chosen, &result.value.menu);
            }
        }
    }

    // ---- blacklist ----
    result.value.blacklist.clear();
    const JsonValue& blacklist = root.find("blacklist");
    if (blacklist.isArray()) {
        for (std::size_t i = 0; i < blacklist.items().size(); ++i) {
            std::string name = blacklist.at(i).asString();
            // 从文档/聊天里复制来的进程名常带首尾空白，先削掉
            const std::size_t first = name.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) continue;
            const std::size_t last = name.find_last_not_of(" \t\r\n");
            name = name.substr(first, last - first + 1);
            if (name.empty()) continue;
            // 复用 isProcessBlocked 做大小写不敏感去重（它已经是这个语义）
            if (result.value.isProcessBlocked(name)) continue;
            result.value.blacklist.push_back(name);
        }
    }

    result.ok = core::validateRadialConfig(result.value);
    if (!result.ok) {
        result.error = "轮盘配置校验失败";
        result.value = makeDefaultConfig();
        return result;
    }

    // 读成功就统一盖成当前版本号。
    //
    // 这里没有「迁移函数」这一类东西：字段缺失走默认值、认不出的枚举值回落到
    // 安全项、越界值上面已经夹过，所以老文件本来就能读。既然如此，让 schemaVersion
    // 一直停在文件里的旧值没有意义（下次保存又会原样写回去，版本号永远不动），
    // 反而会让以后真正的破坏性变更失去判断依据。
    result.value.schemaVersion = core::kRadialSchemaVersion;
    return result;
}

// ================================================================ 文件 I/O

bool RadialStore::readFileText(const std::wstring& path, std::string& out) {
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
        return size.QuadPart == 0;
    }
    if (size.QuadPart > 64LL * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD totalRead = 0;
    while (totalRead < out.size()) {
        DWORD chunk = 0;
        const DWORD toRead = static_cast<DWORD>(
            (out.size() - totalRead) > 0xFFFFFFFFu
                ? 0xFFFFFFFFu : (out.size() - totalRead));
        if (!ReadFile(file, &out[totalRead], toRead, &chunk, nullptr) ||
            chunk == 0) {
            CloseHandle(file);
            return false;
        }
        totalRead += chunk;
    }
    CloseHandle(file);
    if (out.size() >= 3 &&
        static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

bool RadialStore::writeFileText(const std::wstring& path, const std::string& text,
                                std::string& error) {
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
                ? 0xFFFFFFFFu : (text.size() - written));
        if (!WriteFile(file, text.data() + written, toWrite, &chunk, nullptr) ||
            chunk == 0) {
            CloseHandle(file);
            DeleteFileW(tempPath.c_str());
            error = "写入失败（错误码 " + std::to_string(GetLastError()) + "）";
            return false;
        }
        written += chunk;
    }
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

void RadialStore::backupCorruptFile(const std::wstring& path) {
    const std::wstring backup = path + L".corrupt";
    DeleteFileW(backup.c_str());
    MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
}

LoadResult<core::RadialConfig> RadialStore::load(const std::wstring& path) const {
    std::string text;
    if (!readFileText(path, text)) {
        LoadResult<core::RadialConfig> result;
        result.ok = true;
        result.fromFile = false;
        result.value = makeDefaultConfig();
        return result;
    }
    auto result = deserialize(text);
    if (!result.ok) {
        backupCorruptFile(path);
    }
    result.fromFile = true;
    return result;
}

SaveResult RadialStore::save(const std::wstring& path,
                             const core::RadialConfig& config) const {
    SaveResult result;
    const std::string text = serialize(config);
    if (!writeFileText(path, text, result.error)) {
        result.ok = false;
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace infrastructure
}  // namespace ktile
