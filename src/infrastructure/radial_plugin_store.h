// KTile - 轮盘插件（slugin）清单发现与解析
//
// 扫描 <exe目录>/slugin/<id>/radial.json 翻译成 core::RadialPlugin；动作里
// 相对插件目录的图标路径解析成绝对路径，渲染层无需区分图标来源。单个插件
// 写坏只跳过它自己，不影响主程序或其他插件。
// 不与磁贴插件的 PluginStore 复用：清单格式、字段、生命周期全不同，且用户
// 要求目录分开（plugin/ 给磁贴，slugin/ 给轮盘）。
//
#pragma once

#include "core/radial_plugin.h"

#include <string>
#include <vector>

namespace ktile {
namespace infrastructure {

class RadialPluginStore {
public:
    // 扫描 root/<id>/radial.json。目录不存在时结果是空（不是错误）。
    void discover(const std::wstring& root);

    const std::vector<core::RadialPlugin>& plugins() const noexcept {
        return plugins_;
    }

    const std::wstring& rootDirectory() const noexcept { return root_; }

private:
    std::wstring root_;
    std::vector<core::RadialPlugin> plugins_;
};

}  // namespace infrastructure
}  // namespace ktile
