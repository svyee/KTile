// KTile - 轮盘显示名解析
//
// 预置项存文案 key（action.copy），用户与插件填字面量；这里统一成当前语言
// 的显示名。取值即译，不缓存：切语言后下一次绘制就是新语言。
#pragma once

#include "core/radial_menu.h"
#include "infrastructure/i18n.h"

#include <string>

namespace ktile {
namespace ui {

// tr() 返回字典里的引用，必须拷出来。
inline std::string radialActionName(const core::RadialAction& action) {
    if (!action.nameKey.empty()) {
        return std::string(infrastructure::i18n::tr(action.nameKey.c_str()));
    }
    return action.name;
}

inline std::string radialMenuName(const core::RadialMenu& menu) {
    if (!menu.nameKey.empty()) {
        return std::string(infrastructure::i18n::tr(menu.nameKey.c_str()));
    }
    return menu.name;
}

}  // namespace ui
}  // namespace ktile
