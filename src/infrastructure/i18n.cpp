// KTile - 基础设施层：多语言实现
//
// 文件格式（lang/<code>.json，UTF-8）：
//   {
//     "code": "zh-Hans",
//     "name": "简体中文",
//     "strings": {
//       "settings.title": "设置",
//       "common.ok": "确定"
//     }
//   }
//
// 扁平 key 而不是嵌套对象：查表只做一次哈希，不用逐层 find；
// 漏词时在 JSON 里按字母/前缀排一下就能看出哪一段没翻。
//
#include "infrastructure/i18n.h"

#include "base/text_convert.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <utility>

namespace ktile {
namespace infrastructure {
namespace i18n {
namespace {

namespace fs = std::filesystem;

// 一个语言的整份字典。
//
// 同时存 UTF-8 与 UTF-16：Win32 控件只要宽字符，而 UI 层有些地方
// （图标名、插件名）要 UTF-8。加载时一次转完，查询时零转换。
struct Dictionary {
    std::string code;
    std::string name;
    std::unordered_map<std::string, std::string> utf8;
    std::unordered_map<std::string, std::wstring> wide;

    bool has(const std::string& key) const {
        return utf8.find(key) != utf8.end();
    }
};

struct State {
    std::wstring directory;
    Dictionary current;
    Dictionary fallback;                 // 英语：缺词的兜底
    std::vector<LanguageInfo> languages;  // 语言目录里发现的所有语言
    // 漏词的 key 缓存：tr() 返回引用，必须有个稳定的存储落点
    std::unordered_map<std::string, std::string> missing;
    std::unordered_map<std::string, std::wstring> missingWide;
    std::string empty;
    std::wstring emptyWide;
};

State& state() {
    // 函数内静态：避免全局构造顺序问题（UI 层静态表里也调 tr）
    static State instance;
    return instance;
}

// 读整个文件为 UTF-8 文本。跳过 UTF-8 BOM —— 用户手工编辑语言文件时
// 记事本大概率会加 BOM，而项目里的 JSON 解析器不认它。
bool readTextFile(const fs::path& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    out = std::move(text);
    return true;
}

std::wstring fileNameFor(const std::wstring& directory,
                         const std::string& code) {
    fs::path path(directory);
    path /= fs::u8path(code + ".json");
    return path.wstring();
}

bool loadDictionary(const std::wstring& directory,
                    const std::string& code,
                    Dictionary& out) {
    std::string text;
    if (!readTextFile(fs::path(fileNameFor(directory, code)), text)) {
        return false;
    }
    const json::ParseResult parsed = json::parseJson(text);
    if (!parsed.ok || !parsed.root.isObject()) {
        return false;
    }

    Dictionary dictionary;
    dictionary.code = parsed.root.find("code").asString();
    if (dictionary.code.empty()) {
        dictionary.code = code;   // 文件里忘写 code 就按文件名算
    }
    dictionary.name = parsed.root.find("name").asString();
    if (dictionary.name.empty()) {
        dictionary.name = dictionary.code;
    }

    const json::JsonValue& strings = parsed.root.find("strings");
    if (!strings.isObject()) {
        return false;
    }
    for (const auto& member : strings.members()) {
        const std::string& value = member.second.asString();
        // 空串按「没翻」处理：留空比显示空白更容易发现漏词
        if (value.empty()) {
            continue;
        }
        dictionary.wide.emplace(member.first, base::utf8ToWide(value));
        dictionary.utf8.emplace(member.first, value);
    }
    out = std::move(dictionary);
    return true;
}

void scanLanguages(const std::wstring& directory) {
    State& s = state();
    s.languages.clear();

    std::error_code error;
    fs::directory_iterator it(fs::path(directory), error);
    if (error) {
        return;
    }
    for (const auto& entry : it) {
        if (!entry.is_regular_file(error)) {
            continue;
        }
        const fs::path& path = entry.path();
        if (path.extension() != L".json") {
            continue;
        }
        const std::string code =
            base::wideToUtf8Lossy(path.stem().wstring());
        Dictionary probe;
        if (!loadDictionary(directory, code, probe)) {
            continue;
        }
        s.languages.push_back(LanguageInfo{probe.code, probe.name});
    }

    std::sort(s.languages.begin(), s.languages.end(),
              [](const LanguageInfo& a, const LanguageInfo& b) {
                  return a.code < b.code;
              });
}

}  // namespace

std::string systemLanguageCode() {
    const LANGID language = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(language) == LANG_CHINESE) {
        const WORD sub = SUBLANGID(language);
        // 只有「简体中文」（大陆 / 新加坡）走中文，其余一律英语 ——
        // 繁中的用词与简中差得远，硬套过去比英语还别扭。
        if (sub == SUBLANG_CHINESE_SIMPLIFIED ||
            sub == SUBLANG_CHINESE_SINGAPORE) {
            return kSimplifiedChinese;
        }
    }
    return kFallbackLanguage;
}

std::string resolveLanguage(const std::string& configured) {
    if (configured.empty() || configured == kAutoLanguage) {
        return systemLanguageCode();
    }
    return configured;
}

std::string initialize(const std::wstring& langDirectory,
                       const std::string& configured) {
    State& s = state();
    s.directory = langDirectory;
    s.missing.clear();
    s.missingWide.clear();
    s.current = Dictionary();
    s.fallback = Dictionary();

    scanLanguages(langDirectory);

    // 英语单独留一份：它是回退目标，必须独立于「当前语言」存在
    Dictionary english;
    if (loadDictionary(langDirectory, kFallbackLanguage, english)) {
        s.fallback = english;
    }

    const std::string wanted = resolveLanguage(configured);
    Dictionary chosen;
    if (loadDictionary(langDirectory, wanted, chosen)) {
        s.current = std::move(chosen);
    } else if (!s.fallback.utf8.empty()) {
        // 选了不存在的语言（语言文件被删了）：退回英语并保留配置不动，
        // 用户把文件放回来下次启动就恢复。
        s.current = s.fallback;
    } else if (!s.languages.empty()) {
        if (loadDictionary(langDirectory, s.languages.front().code, chosen)) {
            s.current = std::move(chosen);
        }
    }
    return s.current.code;
}

bool setLanguage(const std::string& code) {
    State& s = state();
    const std::string wanted = resolveLanguage(code);
    if (wanted == s.current.code) {
        return true;
    }
    Dictionary dictionary;
    if (!loadDictionary(s.directory, wanted, dictionary)) {
        return false;
    }
    s.current = std::move(dictionary);
    s.missing.clear();
    s.missingWide.clear();
    return true;
}

const std::string& languageCode() noexcept {
    return state().current.code;
}

const std::vector<LanguageInfo>& languages() noexcept {
    return state().languages;
}

bool isLoaded() noexcept {
    return !state().current.utf8.empty();
}

const std::string& tr(const char* key) {
    if (key == nullptr || *key == '\0') {
        return state().empty;
    }
    State& s = state();
    auto found = s.current.utf8.find(key);
    if (found != s.current.utf8.end()) {
        return found->second;
    }
    found = s.fallback.utf8.find(key);
    if (found != s.fallback.utf8.end()) {
        return found->second;
    }
    auto missed = s.missing.find(key);
    if (missed != s.missing.end()) {
        return missed->second;
    }
    return s.missing.emplace(key, std::string(key)).first->second;
}

const std::wstring& trw(const char* key) {
    if (key == nullptr || *key == '\0') {
        return state().emptyWide;
    }
    State& s = state();
    auto found = s.current.wide.find(key);
    if (found != s.current.wide.end()) {
        return found->second;
    }
    found = s.fallback.wide.find(key);
    if (found != s.fallback.wide.end()) {
        return found->second;
    }
    auto missed = s.missingWide.find(key);
    if (missed != s.missingWide.end()) {
        return missed->second;
    }
    return s.missingWide
        .emplace(key, base::utf8ToWideLossy(std::string(key)))
        .first->second;
}

// 把 {0} / {1} 换成 args 里的对应项。找不到的序号原样保留 ——
// 翻译漏了参数时界面上会露出 "{1}"，比静默吞掉更好排查。
template <typename Char>
std::basic_string<Char> substitute(const std::basic_string<Char>& text,
                                   const std::vector<std::basic_string<Char>>& args) {
    std::basic_string<Char> result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const Char c = text[i];
        if (c == static_cast<Char>('{') && i + 2 < text.size() &&
            text[i + 2] == static_cast<Char>('}')) {
            const Char digit = text[i + 1];
            if (digit >= static_cast<Char>('0') && digit <= static_cast<Char>('9')) {
                const std::size_t index = static_cast<std::size_t>(digit - '0');
                if (index < args.size()) {
                    result += args[index];
                    i += 2;
                    continue;
                }
            }
        }
        result += c;
    }
    return result;
}

std::wstring formatW(const char* key, const std::vector<std::wstring>& args) {
    return substitute<wchar_t>(trw(key), args);
}

std::string formatU(const char* key, const std::vector<std::string>& args) {
    return substitute<char>(tr(key), args);
}

std::string localizedText(const json::JsonValue& value) {
    if (value.isString()) {
        return value.asString();
    }
    if (!value.isObject()) {
        return std::string();
    }
    const json::JsonValue& exact = value.find(languageCode());
    if (exact.isString() && !exact.asString().empty()) {
        return exact.asString();
    }
    const json::JsonValue& fallback = value.find(kFallbackLanguage);
    if (fallback.isString() && !fallback.asString().empty()) {
        return fallback.asString();
    }
    // 两种语言都没写：随便挑一个非空项，总比空名字强
    for (const auto& member : value.members()) {
        if (member.second.isString() && !member.second.asString().empty()) {
            return member.second.asString();
        }
    }
    return std::string();
}

}  // namespace i18n
}  // namespace infrastructure
}  // namespace ktile
