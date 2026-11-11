// KTile - UI 层：内置项的本地化名字
//
// 为什么放在 UI 层而不是 core：
// core 只到 base，不允许依赖 infrastructure（i18n 在 infrastructure）。
// 所以 core 里的 BuiltinItem 保留规范名（中文），显示时才查语言文件。
//
// key 由 id 派生（"lock" → "builtin.lock"），表里不必逐条写 key ——
// 新增内置项只要在语言文件里补一条同名条目，漏补也只是回退到 key，
// 一眼能看出漏在哪儿。
//
#pragma once

#include "base/text_convert.h"
#include "core/builtin_items.h"
#include "infrastructure/i18n.h"

#include <string>

namespace ktile {
namespace ui {

inline std::wstring builtinItemDisplayName(const core::BuiltinItem& item) {
    const std::string key =
        std::string("builtin.") + base::wideToUtf8Lossy(item.id);
    return infrastructure::i18n::trw(key.c_str());
}

inline std::wstring builtinCategoryDisplayName(const wchar_t* canonical) {
    if (canonical == nullptr) {
        return std::wstring();
    }
    const std::wstring name(canonical);
    if (name == L"电源") {
        return infrastructure::i18n::trw("builtin.category.power");
    }
    if (name == L"媒体与音量") {
        return infrastructure::i18n::trw("builtin.category.media");
    }
    if (name == L"系统位置") {
        return infrastructure::i18n::trw("builtin.category.system");
    }
    return name;
}

}  // namespace ui
}  // namespace ktile
