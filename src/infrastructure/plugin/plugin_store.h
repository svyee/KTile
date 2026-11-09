#pragma once

// 插件清单与缓存持久化
//
// 职责边界：
//   - 发现 <exe目录>/plugin/<id>/plugin.json，把 JSON 翻译成 core::PluginManifest。
//   - 缓存插件返回的数据（<data>/plugins/cache/<id>-<hash>.json），供面板
//     唤出时先画旧数据、再异步刷新。
//   - 这里可以依赖 Win32 / JSON；core 保持纯逻辑。

#include <string>
#include <utility>
#include <vector>

#include "core/plugin_model.h"

namespace ktile {
namespace infrastructure {

class PluginStore {
public:
    // 扫描 <pluginsDirectory>\*\plugin.json。
    // 失败不抛异常，单个插件损坏只会从清单里消失。
    // dataDirectory 只用于定位缓存，不参与清单发现。
    void discover(const std::wstring& pluginsDirectory,
                  const std::wstring& dataDirectory);

    const std::vector<core::PluginManifest>& manifests() const noexcept {
        return manifests_;
    }

    const core::PluginManifest* find(const std::string& id) const noexcept;

    const std::wstring& pluginsDirectory() const noexcept {
        return pluginsDirectory_;
    }

    const std::wstring& dataDirectory() const noexcept {
        return dataDirectory_;
    }

private:
    std::wstring dataDirectory_;
    std::wstring pluginsDirectory_;
    std::vector<core::PluginManifest> manifests_;
};

// JSON 文本 → PluginValue（供 http / command / builtin 的统一解析路径使用）
bool parsePluginJsonValue(const std::string& text,
                          core::PluginValue* out,
                          std::string* error);

// 单条 manifest 的解析（独立出来便于单测）。
bool parsePluginManifest(const std::string& text,
                         const std::wstring& directory,
                         core::PluginManifest* out,
                         std::string* error);

// 缓存文件路径（不含目录）。key 由 id + 参数决定。
std::string pluginCacheFileName(
    const std::string& id,
    const std::vector<std::pair<std::string, std::string>>& params);

bool loadPluginCache(const std::wstring& dataDirectory,
                     const std::string& id,
                     const std::vector<std::pair<std::string, std::string>>& params,
                     core::PluginSnapshot* out);

bool savePluginCache(const std::wstring& dataDirectory,
                     const std::string& id,
                     const std::vector<std::pair<std::string, std::string>>& params,
                     const core::PluginSnapshot& snapshot);

}  // namespace infrastructure
}  // namespace ktile
