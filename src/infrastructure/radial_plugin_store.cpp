// KTile - 轮盘插件（slugin）清单发现与解析实现
#include "infrastructure/radial_plugin_store.h"

#include "base/text_convert.h"
#include "infrastructure/i18n.h"
#include "infrastructure/json/json_parser.h"
#include "infrastructure/json/radial_store.h"  // parseRadialMenuJson

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ktile {
namespace infrastructure {

namespace {

namespace fs = std::filesystem;

// 把清单里的相对图标路径解析成绝对路径（相对插件目录）。
// 必须在加载期定死：用户配置的图标相对 data 目录、插件的相对插件目录，
// 渲染层如果还要区分两者，一旦混了就是图标全丢且无人报错。
void resolveIconPath(core::RadialAction* action, const fs::path& directory) {
    if (action == nullptr || !action->icon.has_value()) {
        return;
    }
    const std::string& raw = *action->icon;
    if (raw.empty()) {
        action->icon.reset();
        return;
    }
    const fs::path candidate = base::utf8ToWide(raw);
    if (candidate.is_absolute()) {
        return;  // 作者写了绝对路径，尊重它
    }
    const fs::path resolved = directory / candidate;
    *action->icon = base::wideToUtf8Lossy(resolved.wstring());
}

bool readTextFile(const fs::path& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return true;
}

void loadOne(const fs::path& directory, const std::string& id,
             core::RadialPlugin* plugin) {
    plugin->id = id;
    plugin->directory = base::wideToUtf8Lossy(directory.wstring());

    const fs::path manifest = directory / L"radial.json";
    std::string text;
    if (!readTextFile(manifest, text)) {
        plugin->error = i18n::tr("error.radialManifestRead");
        return;
    }

    const auto parsed = json::parseJson(text);
    if (!parsed.ok || !parsed.root.isObject()) {
        plugin->error = i18n::tr("error.radialManifestNotObject");
        return;
    }
    const json::JsonValue& root = parsed.root;

    // 版本比本程序新：字段语义可能已变，宁可整份不加载也不猜。反之老插件
    // 缺字段走默认值，不拦。
    const std::uint32_t schema = root.find("schemaVersion").asUint32(
        core::kRadialPluginSchemaVersion);
    if (schema > core::kRadialPluginSchemaVersion) {
        plugin->error = i18n::formatU("error.radialManifestTooNew",
                                      {std::to_string(schema)});
        return;
    }

    plugin->name = root.find("name").asString();
    if (plugin->name.empty()) {
        plugin->name = id;  // 没写显示名就用目录名，总比空着强
    }
    plugin->priority = static_cast<int>(root.find("priority").asInt(0));

    const json::JsonValue& processes = root.find("processes");
    if (processes.isArray()) {
        for (std::size_t i = 0; i < processes.items().size(); ++i) {
            const std::string name = processes.at(i).asString();
            if (!name.empty()) {
                plugin->processes.push_back(name);
            }
        }
    }

    // 扇区与动作：和用户配置共用同一套解析（字段格式完全一致）
    const json::JsonValue& menu = root.find("menu");
    if (menu.isObject()) {
        parseRadialMenuJson(menu, &plugin->menu);
    }

    for (auto& action : plugin->menu.actions) {
        resolveIconPath(&action, directory);
    }

    plugin->valid = core::validateRadialPlugin(*plugin);
    if (!plugin->valid && plugin->error.empty()) {
        if (plugin->processes.empty()) {
            plugin->error = i18n::tr("error.radialProcessesEmpty");
        } else {
            plugin->error = i18n::tr("error.radialMenuInvalid");
        }
    }
}

}  // namespace

void RadialPluginStore::discover(const std::wstring& root) {
    root_ = root;
    plugins_.clear();

    std::error_code error;
    const fs::path rootPath(root_);
    if (!fs::is_directory(rootPath, error)) {
        return;  // 没有 slugin 目录是常态，不是错误
    }

    std::vector<fs::path> directories;
    for (const auto& entry : fs::directory_iterator(rootPath, error)) {
        if (entry.is_directory(error)) {
            directories.push_back(entry.path());
        }
    }
    // 目录遍历顺序在不同文件系统上不一致，先排序，让「同优先级按 id 取名」
    // 这类规则有稳定输入。
    std::sort(directories.begin(), directories.end());

    for (const auto& directory : directories) {
        const std::string id = base::wideToUtf8Lossy(directory.filename().wstring());
        if (id.empty()) {
            continue;
        }
        // 只认带 radial.json 的目录：别人的图标目录、备份目录都会被跳过
        if (!fs::is_regular_file(directory / L"radial.json", error)) {
            continue;
        }
        core::RadialPlugin plugin;
        loadOne(directory, id, &plugin);
        plugins_.push_back(std::move(plugin));
    }
}

}  // namespace infrastructure
}  // namespace ktile
