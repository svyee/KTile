#pragma once

// 插件刷新控制器
//
// 生命周期设计（与「面板唤出才刷新」的定位一致）：
//   - 只有一个常驻工作线程，但空闲时不持有网络/进程资源。
//   - 面板 show 时把当前标签页的插件磁贴入队；同一插件（含相同参数）只刷一次。
//   - 命中缓存且未过期时直接画缓存，不入队。
//   - 面板 hide 时清空队列；已在执行的请求无法中断（WinHTTP 同步请求），
//     但结果仍会写回缓存，下次唤出直接命中。
//   - 没有定时器、没有后台轮询。

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/data_model.h"
#include "core/plugin_model.h"
#include "infrastructure/plugin/plugin_store.h"

namespace ktile {
namespace app {

class PluginController {
public:
    using InvalidateHandler = std::function<void()>;

    PluginController() = default;
    ~PluginController();

    PluginController(const PluginController&) = delete;
    PluginController& operator=(const PluginController&) = delete;

    void initialize(const std::wstring& pluginDirectory,
                    const std::wstring& dataDirectory,
                    InvalidateHandler invalidate);
    void shutdown();

    // 扫描 <pluginDirectory>\*\plugin.json，返回是否至少发现一个插件。
    bool discoverPlugins();

    const infrastructure::PluginStore& store() const noexcept { return store_; }
    std::vector<core::PluginCatalogEntry> catalog() const;

    // ---- 插件管理（设置窗口）----
    // 从用户选择的 plugin.json 安装：复制其所在目录到
    // <pluginDirectory>\<id>\，然后重新扫描。
    bool installPlugin(const std::wstring& manifestPath, std::string* error);
    // 删除整个插件目录（只允许删除 pluginDirectory 下的直接子目录）。
    bool removePlugin(const std::string& id, std::string* error);
    // 在资源管理器中打开插件目录；id 为空时打开插件根目录。
    void openPluginFolder(const std::string& id) const;
    const std::wstring& pluginDirectory() const noexcept {
        return pluginDirectory_;
    }

    // UI 线程调用；返回的 PluginView 里的 shared_ptr 在帧内稳定。
    core::PluginView viewFor(const core::LaunchItem& item) const;

    // 面板 show：只处理 Plugin 类型；force=true 时忽略 TTL（点击磁贴手动刷新）。
    // visible=true 表示「面板正开着」的高频刷新：命中清单配置
    // visibleRefreshSeconds 的插件改用这个更短的 TTL。
    void refreshItems(const std::vector<core::LaunchItem>& items, bool force,
                      bool visible = false);
    void requestRefresh(const core::LaunchItem& item, bool force);

    // 当前标签页里最短的 visibleRefreshSeconds（0 = 没有任何插件需要
    // 前台刷新）。UI 定时器按这个值取最小节拍，逐项 TTL 负责各自的节奏。
    int foregroundRefreshSeconds(
        const std::vector<core::LaunchItem>& items) const;

    // 面板 hide：清空尚未开始的请求。
    void cancelPending();

private:
    struct Entry {
        std::shared_ptr<const core::PluginManifest> manifest;
        std::shared_ptr<const core::PluginSnapshot> snapshot;
        bool loading = false;

        Entry() = default;
    };

    struct Job {
        std::string key;
        std::shared_ptr<const core::PluginManifest> manifest;
        std::vector<std::pair<std::string, std::string>> params;

        Job() = default;
    };

    static std::string entryKey(
        const std::string& pluginId,
        const std::vector<std::pair<std::string, std::string>>& params);

    void workerLoop();
    void executeJob(const Job& job);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Job> queue_;
    std::thread worker_;
    bool started_ = false;
    bool stopping_ = false;

    infrastructure::PluginStore store_;
    std::wstring pluginDirectory_;
    std::wstring dataDirectory_;
    InvalidateHandler invalidate_;
    // key → Entry。所有访问都在 mutex_ 下进行（viewFor 也会读，故 mutable）。
    mutable std::map<std::string, Entry> entries_;
};

}  // namespace app
}  // namespace ktile
