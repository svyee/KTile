// KTile - 基础设施层：配置与数据的持久化
//
// 职责：在 core 领域模型与 JSON 文本之间做双向映射，并负责文件读写。
//
// 文件布局（均在 data 目录下）：
//   items.json   —— 磁贴数据（ItemsDocument）
//   config.json  —— 应用配置（ApplicationConfig）
//
// 容错策略：
//   - 文件不存在：返回默认值 + ok=true（首次运行是正常情况）
//   - 文件损坏：返回默认值 + ok=false，并把损坏文件改名备份（*.bak）
//     这样用户的手工编辑出错时不会直接丢失全部数据。
//
#pragma once

#include "core/data_model.h"
#include "infrastructure/json/json_parser.h"

#include <string>

namespace ktile {
namespace infrastructure {

// 读取结果
template <typename T>
struct LoadResult {
    bool ok = false;      // 是否成功从文件读到有效数据
    bool fromFile = false; // 是否真的读到了文件（false = 文件不存在）
    T value;
    std::string error;    // 失败原因（人类可读）
};

// 保存结果
struct SaveResult {
    bool ok = false;
    std::string error;
};

class ConfigStore {
public:
    // ---- items.json ----
    LoadResult<core::ItemsDocument> loadItems(const std::wstring& path) const;
    SaveResult saveItems(const std::wstring& path,
                         const core::ItemsDocument& document) const;

    // ---- config.json ----
    LoadResult<core::ApplicationConfig> loadConfig(const std::wstring& path) const;
    SaveResult saveConfig(const std::wstring& path,
                          const core::ApplicationConfig& config) const;

    // ---- 纯文本映射（便于单测，不碰文件系统）----

    // 序列化
    static std::string serializeItems(const core::ItemsDocument& document);
    static std::string serializeConfig(const core::ApplicationConfig& config);

    // 反序列化。失败时 ok=false，value 保持默认值。
    static LoadResult<core::ItemsDocument> deserializeItems(const std::string& text);
    static LoadResult<core::ApplicationConfig> deserializeConfig(const std::string& text);

private:
    // 读整个文件（UTF-8），失败返回 false
    static bool readFileText(const std::wstring& path, std::string& out);
    // 原子写入：先写 .tmp 再替换，避免写入中途崩溃损坏原文件
    static bool writeFileText(const std::wstring& path, const std::string& text,
                              std::string& error);
    // 把损坏文件改名加时间戳后缀
    static void backupCorruptFile(const std::wstring& path);
};

}  // namespace infrastructure
}  // namespace ktile
