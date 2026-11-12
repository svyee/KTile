// KTile - 核心数据模型：轮盘插件实现（纯逻辑，可单测）
#include "core/radial_plugin.h"

namespace ktile {
namespace core {

namespace {

bool containsProcess(const RadialPlugin& plugin,
                     const std::string& processName,
                     const std::string& windowClass) noexcept {
    for (const auto& candidate : plugin.processes) {
        const std::size_t at = candidate.find('@');
        if (at == std::string::npos) {
            if (processNameEquals(candidate, processName)) {
                return true;
            }
            continue;
        }
        // "进程名@窗口类名"：要求窗口类也一致，用于 explorer.exe 区分
        // 桌面（Progman/WorkerW）与文件窗口（CabinetWClass）。
        // @ 前后留空或没有窗口类信息可比时，条目视为无效。
        if (at == 0 || at + 1 == candidate.size() || windowClass.empty()) {
            continue;
        }
        if (processNameEquals(candidate.substr(0, at), processName) &&
            processNameEquals(candidate.substr(at + 1), windowClass)) {
            return true;
        }
    }
    return false;
}

}  // namespace

const RadialPlugin* findPluginForProcess(
    const std::vector<RadialPlugin>& plugins,
    const std::string& processName,
    const std::string& windowClass) noexcept {
    if (processName.empty()) {
        return nullptr;
    }

    const RadialPlugin* best = nullptr;
    for (const auto& plugin : plugins) {
        if (!plugin.valid || !containsProcess(plugin, processName, windowClass)) {
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
