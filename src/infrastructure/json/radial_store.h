// KTile - 基础设施层：轮盘菜单配置持久化
//
// 职责：在 core::RadialConfig 与 JSON 文本之间做双向映射，并负责文件读写。
//
// 文件：data/radial.json
//
// 容错策略与 config_store 一致：
//   - 文件不存在：返回默认值 + ok=true（首次运行）
//   - 文件损坏：返回默认值 + ok=false，备份为 *.corrupt
//
#pragma once

#include "core/radial_menu.h"
#include "infrastructure/json/config_store.h"  // LoadResult / SaveResult
#include "infrastructure/json/json_parser.h"

#include <string>

namespace ktile {
namespace infrastructure {

// 解析一个 menu 对象（`{ "name":…, "sectorCount":…, "actions":[…] }`）。
//
// 提成自由函数是因为轮盘插件（slugin/<id>/radial.json）用的是**同一套**
// 扇区/动作格式：用户配置和插件只有外层容器不同，里面的东西一模一样。
// 写两份解析的话，加一个动作字段就得记住改两处 —— 迟早漏一处。
// 解析完保证 actions.size() == sectorCount（不足补空槽位）。
void parseRadialMenuJson(const json::JsonValue& value, core::RadialMenu* out);

class RadialStore {
public:
    LoadResult<core::RadialConfig> load(const std::wstring& path) const;
    SaveResult save(const std::wstring& path, const core::RadialConfig& config) const;

    // 纯文本映射（便于单测）
    static std::string serialize(const core::RadialConfig& config);
    static LoadResult<core::RadialConfig> deserialize(const std::string& text);

private:
    static bool readFileText(const std::wstring& path, std::string& out);
    static bool writeFileText(const std::wstring& path, const std::string& text,
                              std::string& error);
    static void backupCorruptFile(const std::wstring& path);
};

}  // namespace infrastructure
}  // namespace ktile
