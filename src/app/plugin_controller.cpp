#include "app/plugin_controller.h"

#include <Windows.h>
#include <shellapi.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "base/text_convert.h"
#include "core/plugin_binding.h"
#include "infrastructure/i18n.h"
#include "infrastructure/json/json_writer.h"
#include "infrastructure/log/app_log.h"
#include "platform/win32/plugin_http.h"
#include "platform/win32/plugin_providers.h"
#include "platform/win32/plugin_runner.h"

namespace ktile {
namespace app {

PluginController::~PluginController() {
    shutdown();
}

void PluginController::initialize(const std::wstring& pluginDirectory,
                                  const std::wstring& dataDirectory,
                                  InvalidateHandler invalidate) {
    shutdown();
    pluginDirectory_ = pluginDirectory;
    dataDirectory_ = dataDirectory;
    invalidate_ = std::move(invalidate);

    // 插件清单与图标随发布包放在 plugin 目录，主程序只负责扫描；
    // exe 内不内嵌任何插件资源。
    discoverPlugins();

    stopping_ = false;
    started_ = true;
    worker_ = std::thread(&PluginController::workerLoop, this);
}

void PluginController::shutdown() {
    if (!started_) {
        return;
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);
        stopping_ = true;
        queue_.clear();
        condition_.notify_all();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    started_ = false;
    stopping_ = false;
}

bool PluginController::discoverPlugins() {
    if (pluginDirectory_.empty()) {
        return false;
    }
    store_.discover(pluginDirectory_, dataDirectory_);

    std::unique_lock<std::mutex> lock(mutex_);
    entries_.clear();
    return !store_.manifests().empty();
}

std::vector<core::PluginCatalogEntry> PluginController::catalog() const {
    std::vector<core::PluginCatalogEntry> entries;
    for (const core::PluginManifest& manifest : store_.manifests()) {
        entries.emplace_back(manifest.id, manifest.name, manifest.description);
    }
    return entries;
}

namespace {

// 读取小的 UTF-8 文本（清单），去掉 BOM。
bool readUtf8File(const std::filesystem::path& path, std::string* out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    if (text.size() >= 3 && text[0] == '\xEF' && text[1] == '\xBB' &&
        text[2] == '\xBF') {
        text.erase(0, 3);
    }
    *out = std::move(text);
    return true;
}

// 插件 id 会直接当目录名使用，必须挡住 "../" 之类的路径穿越。
bool isSafePluginId(const std::string& id) {
    if (id.empty() || id == "." || id == "..") {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                        c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool PluginController::installPlugin(const std::wstring& manifestPath,
                                     std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (pluginDirectory_.empty()) {
        return fail(infrastructure::i18n::tr("error.pluginDirNotReady"));
    }
    const std::filesystem::path manifest(manifestPath);
    const std::filesystem::path source = manifest.parent_path();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(manifest, ec) ||
        !std::filesystem::is_directory(source, ec)) {
        return fail(infrastructure::i18n::tr("error.pluginManifestInvalid"));
    }

    std::string text;
    if (!readUtf8File(manifest, &text)) {
        return fail(infrastructure::i18n::tr("error.pluginManifestUnreadable"));
    }
    core::PluginManifest parsed;
    std::string parseError;
    if (!infrastructure::parsePluginManifest(text, source.wstring(), &parsed,
                                             &parseError)) {
        return fail(
            infrastructure::i18n::formatU("error.pluginManifestParse",
                                          {parseError}));
    }
    if (!isSafePluginId(parsed.id)) {
        return fail(infrastructure::i18n::tr("error.pluginIdCharset"));
    }

    const std::filesystem::path destination =
        std::filesystem::path(pluginDirectory_) / base::utf8ToWide(parsed.id);
    if (std::filesystem::exists(destination, ec)) {
        return fail(infrastructure::i18n::tr("error.pluginIdExists"));
    }

    std::filesystem::create_directories(pluginDirectory_, ec);
    if (ec) {
        return fail(infrastructure::i18n::tr("error.pluginDirCreate"));
    }
    std::filesystem::copy(source, destination,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
        std::filesystem::remove_all(destination, ec);
        return fail(infrastructure::i18n::tr("error.pluginCopyFailed"));
    }

    discoverPlugins();
    if (invalidate_) {
        invalidate_();
    }
    return true;
}

bool PluginController::removePlugin(const std::string& id,
                                    std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    const core::PluginManifest* manifest = store_.find(id);
    if (manifest == nullptr) {
        return fail(infrastructure::i18n::formatU("error.pluginNotFound", {id}));
    }

    // 只允许删 pluginsDirectory 的直接子目录：防止清单里写出的绝对路径
    // 把用户别处的文件删掉。
    const std::filesystem::path directory(base::utf8ToWide(manifest->directory));
    const std::filesystem::path root(pluginDirectory_);
    if (directory.parent_path() != root) {
        return fail(infrastructure::i18n::tr("error.pluginDirOutside"));
    }

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    if (ec) {
        return fail(
            infrastructure::i18n::formatU("error.pluginDirRemove",
                                          {ec.message()}));
    }

    discoverPlugins();
    if (invalidate_) {
        invalidate_();
    }
    return true;
}

void PluginController::openPluginFolder(const std::string& id) const {
    std::filesystem::path target(pluginDirectory_);
    if (!id.empty()) {
        if (const core::PluginManifest* manifest = store_.find(id)) {
            target = std::filesystem::path(base::utf8ToWide(manifest->directory));
        }
    }
    ShellExecuteW(nullptr, L"open", target.wstring().c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

std::string PluginController::entryKey(
    const std::string& pluginId,
    const std::vector<std::pair<std::string, std::string>>& params) {
    std::string key = pluginId;
    key.push_back('\x1F');
    for (const auto& param : params) {
        key += param.first;
        key.push_back('=');
        key += param.second;
        key.push_back('\x1F');
    }
    return key;
}

core::PluginView PluginController::viewFor(const core::LaunchItem& item) const {
    core::PluginView view;
    if (!item.pluginId.has_value()) {
        view.error = infrastructure::i18n::tr("error.pluginTileNoId");
        return view;
    }

    const std::string key = entryKey(*item.pluginId, item.pluginParams);

    std::unique_lock<std::mutex> lock(mutex_);
    auto entry = entries_.find(key);
    if (entry == entries_.end()) {
        // 尚未 refresh：给出 manifest 供界面先画出布局骨架。
        if (const core::PluginManifest* manifest = store_.find(*item.pluginId)) {
            view.manifest = std::make_shared<const core::PluginManifest>(*manifest);
        } else {
            view.error = infrastructure::i18n::formatU(
                "error.pluginNotFound", {*item.pluginId});
        }
        return view;
    }

    view.manifest = entry->second.manifest;
    view.snapshot = entry->second.snapshot;
    view.loading = entry->second.loading;
    if (view.snapshot) {
        view.hasData = view.snapshot->hasData;
        view.error = view.snapshot->error;
    }
    if (!view.manifest) {
        view.error = infrastructure::i18n::formatU("error.pluginNotFound",
                                                   {*item.pluginId});
    }
    return view;
}

void PluginController::refreshItems(const std::vector<core::LaunchItem>& items,
                                    bool force, bool visible) {
    if (!started_) {
        return;
    }

    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));

    std::deque<Job> pending;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }

        for (const core::LaunchItem& item : items) {
            if (item.type != core::ItemType::Plugin ||
                !item.pluginId.has_value()) {
                continue;
            }
            const std::string& pluginId = *item.pluginId;
            const std::string key = entryKey(pluginId, item.pluginParams);

            auto found = entries_.find(key);
            if (found == entries_.end()) {
                Entry entry;
                if (const core::PluginManifest* manifest = store_.find(pluginId)) {
                    entry.manifest =
                        std::make_shared<const core::PluginManifest>(*manifest);
                }

                // 先读缓存：面板唤出时立刻有内容可画。
                core::PluginSnapshot cached;
                if (entry.manifest &&
                    infrastructure::loadPluginCache(
                        dataDirectory_, pluginId, item.pluginParams, &cached)) {
                    entry.snapshot =
                        std::make_shared<const core::PluginSnapshot>(cached);
                }

                found = entries_.emplace(key, std::move(entry)).first;
            }

            Entry& entry = found->second;
            if (!entry.manifest || entry.loading) {
                continue;
            }

            // 命中 TTL 不重复请求。面板可见时，配置了 visibleRefreshSeconds
            // 的插件改用前台节奏，其余仍按 refreshSeconds，避免一开面板就整页
            // 重刷（HTTP 插件有配额）。
            int ttlSeconds = entry.manifest->refreshSeconds;
            if (visible && entry.manifest->visibleRefreshSeconds > 0) {
                ttlSeconds = entry.manifest->visibleRefreshSeconds;
            }
            if (!force && entry.snapshot && entry.snapshot->hasData &&
                entry.snapshot->updatedAt > 0 &&
                now - entry.snapshot->updatedAt <
                    static_cast<std::int64_t>(ttlSeconds)) {
                continue;
            }

            entry.loading = true;
            Job job;
            job.key = key;
            job.manifest = entry.manifest;
            job.params = item.pluginParams;
            pending.push_back(std::move(job));
        }

        if (!pending.empty()) {
            queue_.insert(queue_.end(),
                          std::make_move_iterator(pending.begin()),
                          std::make_move_iterator(pending.end()));
            condition_.notify_one();
        }
    }
}

int PluginController::foregroundRefreshSeconds(
    const std::vector<core::LaunchItem>& items) const {
    int best = 0;
    for (const core::LaunchItem& item : items) {
        if (item.type != core::ItemType::Plugin ||
            !item.pluginId.has_value()) {
            continue;
        }
        const core::PluginManifest* manifest = store_.find(*item.pluginId);
        if (manifest == nullptr || manifest->visibleRefreshSeconds <= 0) {
            continue;
        }
        if (best == 0 || manifest->visibleRefreshSeconds < best) {
            best = manifest->visibleRefreshSeconds;
        }
    }
    return best;
}

void PluginController::requestRefresh(const core::LaunchItem& item,
                                      bool force) {
    std::vector<core::LaunchItem> items;
    items.push_back(item);
    refreshItems(items, force);
}

void PluginController::cancelPending() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (const Job& job : queue_) {
        auto found = entries_.find(job.key);
        if (found != entries_.end()) {
            found->second.loading = false;
        }
    }
    queue_.clear();
}

void PluginController::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() {
                return stopping_ || !queue_.empty();
            });
            if (stopping_ && queue_.empty()) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        executeJob(job);
    }
}

void PluginController::executeJob(const Job& job) {
    if (!job.manifest) {
        return;
    }

    const core::PluginManifest& manifest = *job.manifest;

    platform::win32::PluginRawResult raw;
    switch (manifest.source.kind) {
        case core::PluginSourceKind::Builtin:
            raw = platform::win32::runBuiltinPluginProvider(
                manifest.source.provider);
            break;

        case core::PluginSourceKind::Http: {
            const std::string url = core::pluginResolveParamTemplate(
                manifest.source.url, job.params);
            const std::string body = core::pluginResolveParamTemplate(
                manifest.source.body, job.params);

            std::vector<std::pair<std::string, std::string>> headers;
            headers.reserve(manifest.source.headers.size());
            for (const auto& header : manifest.source.headers) {
                headers.emplace_back(
                    header.first,
                    core::pluginResolveParamTemplate(header.second,
                                                     job.params));
            }

            raw = platform::win32::httpRequestPlugin(
                url, manifest.source.method, headers, body,
                manifest.source.timeoutSeconds);
            break;
        }

        case core::PluginSourceKind::Command: {
            std::vector<std::string> arguments;
            arguments.reserve(manifest.source.arguments.size());
            for (const std::string& argument : manifest.source.arguments) {
                arguments.push_back(
                    core::pluginResolveParamTemplate(argument, job.params));
            }

            std::string configJson;
            {
                infrastructure::json::JsonWriter writer;
                writer.beginObject();
                for (const auto& param : job.params) {
                    writer.member(param.first, param.second);
                }
                writer.endObject();
                configJson = writer.take();
            }

            raw = platform::win32::runCommandPlugin(
                manifest.source.program, arguments, manifest.directory,
                configJson, manifest.source.outputFile,
                manifest.source.timeoutSeconds);
            break;
        }
    }

    core::PluginSnapshot snapshot;
    snapshot.updatedAt = static_cast<std::int64_t>(std::time(nullptr));

    std::string error;
    if (!raw.ok) {
        error = raw.error.empty()
                    ? infrastructure::i18n::tr("error.pluginDataProviderFailed")
                    : raw.error;
        infrastructure::applog::warn("插件 " + manifest.id + " 刷新失败: " +
                                     error);
    } else {
        core::PluginValue value;
        if (!infrastructure::parsePluginJsonValue(raw.body, &value, &error)) {
            error = infrastructure::i18n::formatU("error.pluginJsonInvalid",
                                                  {error});
            infrastructure::applog::warn("插件 " + manifest.id + " 数据解析失败: " +
                                         error);
        } else {
            snapshot.data = std::move(value);
            snapshot.hasData = true;
        }
    }

    // 刷新失败时保留上一次成功数据，只附带错误信息，避免磁贴闪成空白。
    std::shared_ptr<const core::PluginSnapshot> previous;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto found = entries_.find(job.key);
        if (found != entries_.end()) {
            previous = found->second.snapshot;
        }
    }
    if (!raw.ok || !snapshot.hasData) {
        snapshot.error = error;
        if (previous && previous->hasData) {
            snapshot.data = previous->data;
            snapshot.hasData = true;
            snapshot.updatedAt = previous->updatedAt;
        }
    }

    if (snapshot.hasData && snapshot.error.empty()) {
        infrastructure::savePluginCache(dataDirectory_, manifest.id, job.params,
                                        snapshot);
    }

    auto published = std::make_shared<const core::PluginSnapshot>(std::move(snapshot));
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto found = entries_.find(job.key);
        if (found == entries_.end()) {
            found = entries_.emplace(job.key, Entry()).first;
            found->second.manifest = job.manifest;
        }
        found->second.snapshot = published;
        found->second.loading = false;
    }

    if (invalidate_) {
        invalidate_();
    }
}

}  // namespace app
}  // namespace ktile
