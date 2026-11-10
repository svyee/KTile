// KTile - 轮盘菜单配置持久化实现
#include "infrastructure/json/radial_store.h"

#include "infrastructure/json/json_writer.h"

#include <Windows.h>

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

core::HotkeyModifier hotkeyModifierFromKey(const std::string& key) {
    if (key == "control") return core::HotkeyModifier::Control;
    if (key == "shift")   return core::HotkeyModifier::Shift;
    if (key == "win")     return core::HotkeyModifier::Win;
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

    config.menus.push_back(global);
    config.profiles.push_back(core::RadialProfile("", "default"));
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
    writer.endObject();

    // menus
    writer.key("menus").beginArray();
    for (const auto& menu : config.menus) {
        writer.beginObject();
        writer.member("id", menu.id);
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
    writer.endArray();  // menus

    // profiles
    writer.key("profiles").beginArray();
    for (const auto& profile : config.profiles) {
        writer.beginObject();
        writer.member("processPattern", profile.processPattern);
        writer.member("menuId", profile.menuId);
        writer.endObject();
    }
    writer.endArray();  // profiles

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
        result.value.trigger.enabled = trigger.find("enabled").asBool(true);
        result.value.trigger.button =
            core::radialTriggerButtonFromKey(trigger.find("button").asString());
        result.value.trigger.modifier =
            core::radialTriggerModifierFromKey(trigger.find("modifier").asString());
        result.value.trigger.activationThresholdPx = static_cast<std::uint16_t>(
            trigger.find("activationThresholdPx").asUint32(8));
        result.value.trigger.cancelRadiusPx = static_cast<std::uint16_t>(
            trigger.find("cancelRadiusPx").asUint32(160));
    }

    // menus
    result.value.menus.clear();
    const JsonValue& menus = root.find("menus");
    if (menus.isArray()) {
        for (std::size_t i = 0; i < menus.items().size(); ++i) {
            const JsonValue& m = menus.at(i);
            if (!m.isObject()) continue;

            core::RadialMenu menu;
            menu.id = m.find("id").asString();
            menu.name = m.find("name").asString();
            menu.sectorCount = static_cast<std::uint8_t>(
                m.find("sectorCount").asUint32(8));
            if (menu.sectorCount < core::kMinSectors) menu.sectorCount = core::kMinSectors;
            if (menu.sectorCount > core::kMaxSectors) menu.sectorCount = core::kMaxSectors;

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
                    menu.actions.push_back(action);
                }
            }
            // 补齐到 sectorCount 个
            while (menu.actions.size() < menu.sectorCount) {
                menu.actions.push_back(core::RadialAction());
            }
            result.value.menus.push_back(menu);
        }
    }

    // profiles
    result.value.profiles.clear();
    const JsonValue& profiles = root.find("profiles");
    if (profiles.isArray()) {
        for (std::size_t i = 0; i < profiles.items().size(); ++i) {
            const JsonValue& p = profiles.at(i);
            if (!p.isObject()) continue;
            result.value.profiles.push_back(core::RadialProfile(
                p.find("processPattern").asString(),
                p.find("menuId").asString()));
        }
    }

    result.ok = core::validateRadialConfig(result.value);
    if (!result.ok) {
        result.error = "轮盘配置校验失败";
        result.value = makeDefaultConfig();
    }
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
