// KTile - 轮盘插件（slugin）清单发现与解析
//
// 职责边界：
//   - 扫描 <exe目录>/slugin/<id>/radial.json，翻译成 core::RadialPlugin。
//   - 把动作里的相对图标路径解析成绝对路径（相对插件目录），这样渲染层不用
//     知道「这个图标来自插件还是用户配置」。
//   - 单个插件写坏了只跳过它自己，绝不影响主程序或其他插件：轮盘是最常用的
//     入口，一个手滑的逗号不该让它整体消失。
//
// 为什么不复用磁贴插件的 PluginStore：两者清单格式、字段、生命周期全都不同，
// 共用一棵目录树 + 一个 store 只会让「这个目录到底是谁的」变成长期困惑。
// 用户也明确要求分开放（plugin/ 给磁贴，slugin/ 给轮盘）。
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
