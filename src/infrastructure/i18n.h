// KTile - 基础设施层：多语言（i18n）
//
// 设计取舍：
//
// 1. 文案放外部文件，不编进 exe。
//    <exe>\lang\<code>.json 每份一个语言，加语言 = 加一个文件，不改代码、
//    不用重新编译。这与 plugin/、slugin/ 的「资源随 exe 走」保持一致。
//
// 2. 缺词不炸。
//    回退链：当前语言 → 英语 → key 本身。key 漏译时界面上会出现
//    "settings.title" 这种字样 —— 丑，但一眼能看出漏在哪儿，比空白好排查。
//
// 3. 语言代码用 BCP 47 子集。
//    目前只有 "zh-Hans" 与 "en"。代码即文件名（lang/zh-Hans.json），
//    不做大小写/别名归一 —— 少一层隐式约定，加语言时不容易踩坑。
//
// 4. 运行时可变。
//    设置里改语言后立刻 setLanguage()，新创建的界面直接是新手语言；
//    已经画好的窗口（设置对话框自身）下次打开时生效。
//
#pragma once

#include "infrastructure/json/json_parser.h"

#include <string>
#include <vector>

namespace ktile {
namespace infrastructure {
namespace i18n {

// 配置里存这个值 = 跟随系统语言（首次运行的默认值）
inline constexpr const char* kAutoLanguage = "auto";
// 兜底语言：任何语言缺词都先找它
inline constexpr const char* kFallbackLanguage = "en";
// 系统语言是简体中文时用的代码
inline constexpr const char* kSimplifiedChinese = "zh-Hans";

struct LanguageInfo {
    // 语言代码（= 文件名，不含扩展名）
    std::string code;
    // 该语言自己的写法（"简体中文" / "English"），下拉里直接显示
    std::string name;
};

// 扫描 langDirectory 下的 *.json 并按 configured 选定语言。
//
// configured 传 kAutoLanguage 时按系统语言（简体中文 → zh-Hans，其余 → en）。
// 返回实际生效的语言代码；一份字典都加载不到时返回空串（此时 tr() 返回 key）。
std::string initialize(const std::wstring& langDirectory,
                       const std::string& configured);

// 运行时切换。文件缺失/解析失败返回 false，并保持原语言不变。
bool setLanguage(const std::string& code);

// 由配置值算出生效语言代码（"auto" → 系统语言）。
std::string resolveLanguage(const std::string& configured);

const std::string& languageCode() noexcept;
const std::vector<LanguageInfo>& languages() noexcept;
// 是否真的加载到了字典（false = 语言目录缺失，界面会显示 key）
bool isLoaded() noexcept;

// 翻译。未命中按回退链处理，返回的引用在下次 setLanguage 之前有效。
const std::string& tr(const char* key);
const std::wstring& trw(const char* key);

// 带占位符的翻译：文案里写 {0} / {1}，按 args 顺序替换。
//
// 为什么不用 printf 风格：语序因语言而异（"「{0}」不能为空" 与
// "\"{0}\" cannot be empty"），只有命名占位符能让译者自己调整位置。
std::wstring formatW(const char* key, const std::vector<std::wstring>& args);
std::string formatU(const char* key, const std::vector<std::string>& args);

// 兼容老写法的「多语言字段」：
//   "名称"                                单一字符串，原样返回
//   { "zh-Hans": "关闭标签", "en": "Close tab" }   按当前语言挑，缺项回退英语
std::string localizedText(const json::JsonValue& value);

// 系统界面语言 → 语言代码。简体中文（含新加坡）→ zh-Hans，其余 → en。
std::string systemLanguageCode();

}  // namespace i18n
}  // namespace infrastructure
}  // namespace ktile
