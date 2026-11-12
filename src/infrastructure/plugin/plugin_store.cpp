#include "infrastructure/plugin/plugin_store.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include "base/text_convert.h"
#include "infrastructure/json/json_parser.h"
#include "infrastructure/json/json_writer.h"

namespace ktile {
namespace infrastructure {
namespace {

using core::PluginLayoutNode;
using core::PluginLayoutSpec;
using core::PluginManifest;
using core::PluginMeterSpec;
using core::PluginSlotsSpec;
using core::PluginSourceKind;
using core::PluginSourceSpec;
using core::PluginValue;

// --- 文件

bool readFileText(const std::wstring& path, std::string& out) {
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
    if (size.QuadPart > 16LL * 1024 * 1024) {  // 插件清单/缓存不会这么大
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD totalRead = 0;
    while (totalRead < out.size()) {
        DWORD chunk = 0;
        if (!ReadFile(file, &out[totalRead],
                      static_cast<DWORD>(out.size() - totalRead), &chunk,
                      nullptr) ||
            chunk == 0) {
            CloseHandle(file);
            return false;
        }
        totalRead += chunk;
    }
    CloseHandle(file);

    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

bool writeFileAtomic(const std::wstring& path, const std::string& text,
                     std::string* error) {
    const std::wstring tempPath = path + L".tmp";
    HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = "无法创建临时文件";
        }
        return false;
    }

    std::size_t written = 0;
    while (written < text.size()) {
        DWORD chunk = 0;
        if (!WriteFile(file, text.data() + written,
                       static_cast<DWORD>(text.size() - written), &chunk,
                       nullptr) ||
            chunk == 0) {
            CloseHandle(file);
            DeleteFileW(tempPath.c_str());
            if (error != nullptr) {
                *error = "写入失败";
            }
            return false;
        }
        written += chunk;
    }
    FlushFileBuffers(file);
    CloseHandle(file);

    if (!MoveFileExW(tempPath.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tempPath.c_str());
        if (error != nullptr) {
            *error = "替换缓存文件失败";
        }
        return false;
    }
    return true;
}

// --- JSON → PluginValue

PluginValue toPluginValue(const json::JsonValue& node) {
    switch (node.type()) {
        case json::JsonValue::Type::Null:
            return PluginValue::makeNull();
        case json::JsonValue::Type::Bool:
            return PluginValue::makeBool(node.asBool());
        case json::JsonValue::Type::Number:
            return PluginValue::makeNumber(node.asDouble());
        case json::JsonValue::Type::String:
            return PluginValue::makeString(node.asString());
        case json::JsonValue::Type::Array: {
            PluginValue array = PluginValue::makeArray();
            array.elements.reserve(node.items().size());
            for (const auto& item : node.items()) {
                array.elements.push_back(toPluginValue(item));
            }
            return array;
        }
        case json::JsonValue::Type::Object: {
            PluginValue object = PluginValue::makeObject();
            object.members.reserve(node.members().size());
            for (const auto& member : node.members()) {
                object.members.emplace_back(member.first,
                                            toPluginValue(member.second));
            }
            return object;
        }
    }
    return PluginValue::makeNull();
}

void writePluginValue(json::JsonWriter* writer, const PluginValue& value) {
    switch (value.type) {
        case PluginValue::Type::Null:
            writer->nullValue();
            break;
        case PluginValue::Type::Bool:
            writer->value(value.boolean);
            break;
        case PluginValue::Type::Number:
            writer->value(value.number);
            break;
        case PluginValue::Type::String:
            writer->value(value.text);
            break;
        case PluginValue::Type::Array:
            writer->beginArray();
            for (const PluginValue& item : value.elements) {
                writePluginValue(writer, item);
            }
            writer->endArray();
            break;
        case PluginValue::Type::Object:
            writer->beginObject();
            for (const auto& member : value.members) {
                writer->key(member.first);
                writePluginValue(writer, member.second);
            }
            writer->endObject();
            break;
    }
}

// --- 清单解析

std::string stringOr(const json::JsonValue& value,
                     const std::string& fallback) {
    return value.isString() ? value.asString() : fallback;
}

bool parseSlots(const json::JsonValue& slots, PluginSlotsSpec* out,
                std::string* error) {
    if (!slots.isObject()) {
        if (error != nullptr) {
            *error = "slots 必须是对象";
        }
        return false;
    }
    out->icon = slots.find("icon").asString();
    out->main = slots.find("main").asString();
    out->sub = slots.find("sub").asString();
    out->footer = slots.find("footer").asString();

    if (slots.hasKey("meter")) {
        const json::JsonValue& meter = slots.find("meter");
        if (!meter.isObject()) {
            if (error != nullptr) {
                *error = "meter 必须是对象";
            }
            return false;
        }
        out->hasMeter = true;
        out->meter.value = meter.find("value").asString();
        out->meter.label = meter.find("label").asString();
        out->meter.color = meter.find("color").asString();
        const double maximum = meter.find("max").asDouble(100.0);
        out->meter.maximum = maximum > 0.0 ? maximum : 100.0;
    }
    return true;
}

bool parseLayoutNode(const json::JsonValue& value, int depth,
                     PluginLayoutNode* out, std::string* error) {
    if (depth > 3) {
        if (error != nullptr) {
            *error = "布局嵌套超过 4 层";
        }
        return false;
    }
    if (!value.isObject()) {
        if (error != nullptr) {
            *error = "布局节点必须是对象";
        }
        return false;
    }

    out->weight = static_cast<int>(value.find("weight").asInt(0));

    if (value.hasKey("slots")) {
        out->kind = PluginLayoutNode::Kind::Leaf;
        return parseSlots(value.find("slots"), &out->slots, error);
    }

    out->kind = PluginLayoutNode::Kind::Container;
    const std::string direction = stringOr(value.find("direction"), "column");
    out->horizontal = direction == "row";

    if (value.hasKey("repeat")) {
        const json::JsonValue& repeat = value.find("repeat");
        if (!repeat.isObject()) {
            if (error != nullptr) {
                *error = "repeat 必须是对象";
            }
            return false;
        }
        out->hasRepeat = true;
        out->repeatSource = repeat.find("source").asString();
        out->repeatMap = stringOr(repeat.find("map"), "item");
        int maximum = static_cast<int>(repeat.find("max").asInt(3));
        if (maximum < 1) {
            maximum = 1;
        }
        if (maximum > 4) {
            maximum = 4;
        }
        out->repeatMax = maximum;
        if (out->repeatSource.empty()) {
            if (error != nullptr) {
                *error = "repeat.source 不能为空";
            }
            return false;
        }
    }

    if (!value.hasKey("children")) {
        if (error != nullptr) {
            *error = "容器节点缺少 children";
        }
        return false;
    }
    const json::JsonValue& children = value.find("children");
    if (!children.isArray() || children.items().empty()) {
        if (error != nullptr) {
            *error = "children 必须是非空数组";
        }
        return false;
    }
    if (children.items().size() > 3) {
        if (error != nullptr) {
            *error = "单个容器最多 3 个子节点";
        }
        return false;
    }

    out->children.resize(children.items().size());
    for (std::size_t index = 0; index < children.items().size(); ++index) {
        if (!parseLayoutNode(children.items()[index], depth + 1,
                             &out->children[index], error)) {
            return false;
        }
    }
    return true;
}

bool parseLayoutSpec(const json::JsonValue& value, PluginLayoutSpec* out,
                     std::string* error) {
    return parseLayoutNode(value, 0, &out->root, error);
}

bool parseStringMap(const json::JsonValue& value,
                    std::vector<std::pair<std::string, std::string>>* out) {
    if (!value.isObject()) {
        return false;
    }
    for (const auto& member : value.members()) {
        if (member.second.isString()) {
            out->emplace_back(member.first, member.second.asString());
        }
    }
    return true;
}

bool parseSource(const json::JsonValue& value, PluginSourceSpec* out,
                 std::string* error) {
    if (!value.isObject()) {
        if (error != nullptr) {
            *error = "source 必须是对象";
        }
        return false;
    }

    const std::string type = stringOr(value.find("type"), "builtin");
    if (type == "http") {
        out->kind = PluginSourceKind::Http;
    } else if (type == "command") {
        out->kind = PluginSourceKind::Command;
    } else {
        out->kind = PluginSourceKind::Builtin;
    }

    out->provider = stringOr(value.find("provider"), "system");
    out->url = value.find("url").asString();
    out->method = stringOr(value.find("method"), "GET");
    out->body = value.find("body").asString();
    const int timeout = static_cast<int>(value.find("timeoutSeconds").asInt(8));
    out->timeoutSeconds = timeout < 1 ? 1 : (timeout > 60 ? 60 : timeout);

    out->headers.clear();
    parseStringMap(value.find("headers"), &out->headers);

    out->program = value.find("program").asString();
    out->arguments.clear();
    const json::JsonValue& arguments = value.find("arguments");
    if (arguments.isArray()) {
        for (const auto& argument : arguments.items()) {
            if (argument.isString()) {
                out->arguments.push_back(argument.asString());
            }
        }
    }
    out->outputFile = value.find("outputFile").asString();

    if (out->kind == PluginSourceKind::Http && out->url.empty()) {
        if (error != nullptr) {
            *error = "http 源缺少 url";
        }
        return false;
    }
    if (out->kind == PluginSourceKind::Command && out->program.empty()) {
        if (error != nullptr) {
            *error = "command 源缺少 program";
        }
        return false;
    }
    return true;
}

}  // namespace

// --- PluginStore

const PluginManifest* PluginStore::find(const std::string& id) const noexcept {
    for (const PluginManifest& manifest : manifests_) {
        if (manifest.id == id) {
            return &manifest;
        }
    }
    return nullptr;
}

void PluginStore::discover(const std::wstring& pluginsDirectory,
                           const std::wstring& dataDirectory) {
    pluginsDirectory_ = pluginsDirectory;
    dataDirectory_ = dataDirectory;
    manifests_.clear();

    const std::filesystem::path root(pluginsDirectory_);
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        return;
    }

    for (const auto& entry :
         std::filesystem::directory_iterator(root, error)) {
        if (error) {
            break;
        }
        if (!entry.is_directory(error)) {
            continue;
        }
        // 缓存目录虽然也在 plugins 下，但没有 plugin.json，会被下面的
        // exists 检查自然跳过。
        const std::filesystem::path manifestPath = entry.path() / "plugin.json";
        if (!std::filesystem::exists(manifestPath, error)) {
            continue;
        }

        std::string text;
        if (!readFileText(manifestPath.wstring(), text)) {
            continue;
        }

        PluginManifest manifest;
        std::string parseError;
        if (!parsePluginManifest(text, entry.path().wstring(), &manifest,
                                 &parseError)) {
            // 单个坏清单不影响其它插件；错误留在 manifest 里便于日志排查。
            manifest.error = parseError;
            continue;
        }
        manifests_.push_back(std::move(manifest));
    }

    std::sort(manifests_.begin(), manifests_.end(),
              [](const PluginManifest& left, const PluginManifest& right) {
                  return left.id < right.id;
              });
}

bool parsePluginManifest(const std::string& text,
                         const std::wstring& directory,
                         PluginManifest* out,
                         std::string* error) {
    if (out == nullptr) {
        return false;
    }
    *out = PluginManifest();

    json::ParseResult parsed = json::parseJson(text);
    if (!parsed.ok) {
        if (error != nullptr) {
            *error = "清单 JSON 解析失败：" + parsed.error;
        }
        return false;
    }
    const json::JsonValue& root = parsed.root;
    if (!root.isObject()) {
        if (error != nullptr) {
            *error = "清单根节点必须是对象";
        }
        return false;
    }

    out->id = root.find("id").asString();
    if (out->id.empty()) {
        if (error != nullptr) {
            *error = "缺少 id";
        }
        return false;
    }
    out->name = stringOr(root.find("name"), out->id);
    out->description = root.find("description").asString();
    out->icon = root.find("icon").asString();

    int refresh = static_cast<int>(root.find("refreshSeconds").asInt(300));
    if (refresh < 5) {
        refresh = 5;
    }
    if (refresh > 86400) {
        refresh = 86400;
    }
    out->refreshSeconds = refresh;

    // 前台刷新间隔：允许 1 秒起步（系统指标就是按秒看的），上限同 TTL。
    // 缺省 0 表示「只在唤出时刷新」，老清单不需要改也能继续用。
    int visibleRefresh =
        static_cast<int>(root.find("visibleRefreshSeconds").asInt(0));
    if (visibleRefresh < 0) {
        visibleRefresh = 0;
    }
    if (visibleRefresh > 86400) {
        visibleRefresh = 86400;
    }
    out->visibleRefreshSeconds = visibleRefresh;

    if (root.hasKey("select") &&
        !parseStringMap(root.find("select"), &out->selectors)) {
        if (error != nullptr) {
            *error = "select 必须是对象";
        }
        return false;
    }
    if (root.hasKey("icons") &&
        !parseStringMap(root.find("icons"), &out->iconMap)) {
        if (error != nullptr) {
            *error = "icons 必须是对象";
        }
        return false;
    }

    if (!root.hasKey("source")) {
        if (error != nullptr) {
            *error = "缺少 source";
        }
        return false;
    }
    if (!parseSource(root.find("source"), &out->source, error)) {
        return false;
    }

    out->layouts.clear();
    if (root.hasKey("layouts")) {
        const json::JsonValue& layouts = root.find("layouts");
        if (!layouts.isObject()) {
            if (error != nullptr) {
                *error = "layouts 必须是对象";
            }
            return false;
        }
        for (const auto& member : layouts.members()) {
            PluginLayoutSpec spec;
            if (!parseLayoutSpec(member.second, &spec, error)) {
                return false;
            }
            out->layouts.emplace_back(member.first, std::move(spec));
        }
    } else if (root.hasKey("layout")) {
        PluginLayoutSpec spec;
        if (!parseLayoutSpec(root.find("layout"), &spec, error)) {
            return false;
        }
        out->layouts.emplace_back("default", std::move(spec));
    }

    if (out->layouts.empty()) {
        if (error != nullptr) {
            *error = "缺少 layouts / layout";
        }
        return false;
    }

    out->directory = base::wideToUtf8(directory);
    out->valid = true;
    return true;
}

// --- 缓存

std::string pluginCacheFileName(
    const std::string& id,
    const std::vector<std::pair<std::string, std::string>>& params) {
    // id 只保留安全字符，避免插件 id 里的奇怪字符变成非法文件名。
    std::string safeId;
    for (char ch : id) {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        safeId.push_back(safe ? ch : '_');
    }
    if (safeId.empty()) {
        safeId = "plugin";
    }

    // FNV-1a：参数相同的磁贴共享缓存文件。
    std::uint32_t hash = 2166136261u;
    for (char ch : id) {
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
        hash *= 16777619u;
    }
    for (const auto& param : params) {
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>('\x1F'));
        hash *= 16777619u;
        for (char ch : param.first) {
            hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
            hash *= 16777619u;
        }
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>('='));
        hash *= 16777619u;
        for (char ch : param.second) {
            hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
            hash *= 16777619u;
        }
    }

    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08x", hash);
    return safeId + "-" + std::string(hex) + ".json";
}

bool loadPluginCache(const std::wstring& dataDirectory,
                     const std::string& id,
                     const std::vector<std::pair<std::string, std::string>>& params,
                     core::PluginSnapshot* out) {
    if (out == nullptr) {
        return false;
    }
    *out = core::PluginSnapshot();

    const std::wstring cacheDirectory = dataDirectory + L"\\plugins\\cache";
    const std::wstring path =
        cacheDirectory + L"\\" + base::utf8ToWide(pluginCacheFileName(id, params));

    std::string text;
    if (!readFileText(path, text)) {
        return false;
    }

    json::ParseResult parsed = json::parseJson(text);
    if (!parsed.ok || !parsed.root.isObject()) {
        return false;
    }

    out->updatedAt = parsed.root.find("updatedAt").asInt(0);
    out->data = toPluginValue(parsed.root.find("data"));
    out->hasData = !out->data.isNull();
    out->error.clear();
    return out->hasData;
}

bool savePluginCache(const std::wstring& dataDirectory,
                     const std::string& id,
                     const std::vector<std::pair<std::string, std::string>>& params,
                     const core::PluginSnapshot& snapshot) {
    if (!snapshot.hasData) {
        return false;
    }

    const std::wstring cacheDirectory = dataDirectory + L"\\plugins\\cache";
    std::error_code error;
    std::filesystem::create_directories(cacheDirectory, error);
    if (error) {
        return false;
    }

    json::JsonWriter writer;
    writer.beginObject();
    writer.member("updatedAt", snapshot.updatedAt);
    writer.key("data");
    writePluginValue(&writer, snapshot.data);
    writer.endObject();

    const std::wstring path =
        cacheDirectory + L"\\" + base::utf8ToWide(pluginCacheFileName(id, params));
    return writeFileAtomic(path, writer.take(), nullptr);
}

bool parsePluginJsonValue(const std::string& text,
                          core::PluginValue* out,
                          std::string* error) {
    if (out == nullptr) {
        return false;
    }
    json::ParseResult parsed = json::parseJson(text);
    if (!parsed.ok) {
        if (error != nullptr) {
            *error = parsed.error;
        }
        return false;
    }
    *out = toPluginValue(parsed.root);
    return true;
}

}  // namespace infrastructure
}  // namespace ktile
