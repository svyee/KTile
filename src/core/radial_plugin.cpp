// KTile - 核心数据模型：轮盘插件实现（纯逻辑，可单测）
#include "core/radial_plugin.h"

namespace ktile {
namespace core {

namespace {

bool containsProcess(const RadialPlugin& plugin,
                     const std::string& processName) noexcept {
    for (const auto& candidate : plugin.processes) {
        if (processNameEquals(candidate, processName)) {
            return true;
        }
    }
    return false;
}

}  // namespace

const RadialPlugin* findPluginForProcess(
    const std::vector<RadialPlugin>& plugins,
    const std::string& processName) noexcept {
    if (processName.empty()) {
        return nullptr;
    }

    const RadialPlugin* best = nullptr;
    for (const auto& plugin : plugins) {
        if (!plugin.valid || !containsProcess(plugin, processName)) {
            continue;
        }
        if (best == nullptr) {
            best = &plugin;
            continue;
        }
        if (plugin.priority > best->priority) {
            best = &plugin;
        } else if (plugin.priority == best->priority && plugin.id < best->id) {
            // 同优先级按 id 字典序，保证结果稳定（不随目录遍历顺序漂移）
            best = &plugin;
        }
    }
    return best;
}

bool validateRadialPlugin(const RadialPlugin& plugin) noexcept {
    if (plugin.id.empty() || plugin.processes.empty()) {
        return false;
    }
    if (plugin.menu.sectorCount < kMinSectors ||
        plugin.menu.sectorCount > kMaxSectors) {
        return false;
    }
    if (plugin.menu.actions.size() != plugin.menu.sectorCount) {
        return false;
    }
    // 空进程名会让「按进程匹配」失去意义，还可能误命中（空串比较）
    for (const auto& process : plugin.processes) {
        if (process.empty()) {
            return false;
        }
    }
    return true;
}

}  // namespace core
}  // namespace ktile
