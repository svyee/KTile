#pragma once

// 插件绑定求值
//
// 绑定语法只有一种：${path}。path 用点号分隔，数组用数字下标：
//   ${now.temp}        select 把 now 映射到 current 后的字段
//   ${days.0.date}     数组第一个元素
//   ${day.icon}        repeat 循环变量
//
// 不引入表达式/函数/条件，是刻意的：声明式清单容易被 AI 生成和校验，
// 也不会把插件变成不可控的脚本引擎。

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/plugin_model.h"

namespace ktile {
namespace core {

struct PluginBindingContext {
    const PluginValue* root = nullptr;
    const std::vector<std::pair<std::string, std::string>>* selectors = nullptr;
    const std::vector<std::pair<std::string, const PluginValue*>>* locals = nullptr;

    PluginBindingContext() = default;
};

// 解析点号路径。找不到返回 nullptr。
const PluginValue* pluginResolvePath(const PluginBindingContext& context,
                                     const std::string& path);

// 替换模板中的全部 ${...}。未知键展开为空串（保持界面稳定，不显示原始占位符）。
std::string pluginResolveTemplate(const std::string& tmpl,
                                  const PluginBindingContext& context);

// 模板 → 数字，失败返回 fallback。
double pluginResolveNumber(const std::string& tmpl,
                           const PluginBindingContext& context,
                           double fallback);

// 仅用磁贴参数替换模板（供 http url / command 参数使用）。
// 语法同样是 ${key}，key 直接取参数名。
std::string pluginResolveParamTemplate(
    const std::string& tmpl,
    const std::vector<std::pair<std::string, std::string>>& params);

// "#RRGGBB" / "#AARRGGBB" → ARGB；失败返回 fallback。
std::uint32_t pluginParseColor(const std::string& text,
                               std::uint32_t fallback) noexcept;

}  // namespace core
}  // namespace ktile
