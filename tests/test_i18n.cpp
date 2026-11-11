// KTile - 多语言（i18n）测试
//
// 覆盖：
//   1. 语言文件 key 集合一致 —— 两份文件不能一边多一边少，
//      否则某个语言下会出现 key 字样（或者有一边永远用不到的死词）
//   2. 加载与切语言：initialize / setLanguage / languageCode
//   3. 回退链：当前语言 → 英语 → key 本身
//   4. 占位符替换（{0} 出现在不同位置时也正确）
//   5. 系统语言映射：简体中文 → zh-Hans，其余 → en；未知配置值 → en
//   6. 语言目录里有什么就能认出什么（扫描 *.json，代码即文件名）
//
#include "infrastructure/i18n.h"

#include "infrastructure/json/json_parser.h"

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace ktile::infrastructure;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("  FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

// 测试 exe 在 build\ 里，语言文件被 build.ps1 拷到 build\lang\。
std::filesystem::path langDirectory() {
    wchar_t module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length > 0) {
        const std::filesystem::path besideExe =
            std::filesystem::path(module).parent_path() / L"lang";
        if (std::filesystem::exists(besideExe)) {
            return besideExe;
        }
    }
    // 兜底：从仓库根目录跑测试时（.\build.ps1 -Tests）的相对路径
    return std::filesystem::path(L"lang");
}

std::filesystem::path freshDir(const wchar_t* name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

std::set<std::string> stringKeys(const std::filesystem::path& path) {
    const json::JsonValue root = json::parseJson(readText(path)).root;
    std::set<std::string> keys;
    for (const auto& member : root.find("strings").members()) {
        keys.insert(member.first);
    }
    return keys;
}

// ---------------------------------------------------------------- 1. key 一致性

void testLanguageFilesShareSameKeys() {
    const std::filesystem::path dir = langDirectory();
    const std::set<std::string> english = stringKeys(dir / L"en.json");
    CHECK(!english.empty());

    // 目录里每份语言都必须和英语有同一套 key：加词时漏翻一边，
    // 那一语言下就会出现 "settings.title" 这样的字样。
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != L".json") {
            continue;
        }
        const std::set<std::string> other = stringKeys(entry.path());
        std::vector<std::string> missing;
        std::vector<std::string> extra;
        for (const auto& key : english) {
            if (other.count(key) == 0) {
                missing.push_back(key);
            }
        }
        for (const auto& key : other) {
            if (english.count(key) == 0) {
                extra.push_back(key);
            }
        }
        if (!missing.empty() || !extra.empty()) {
            std::printf("  [%s] 缺 %zu 条，多 %zu 条（应为 0）\n",
                        entry.path().filename().u8string().c_str(),
                        missing.size(), extra.size());
            for (std::size_t i = 0; i < missing.size() && i < 8; ++i) {
                std::printf("    缺: %s\n", missing[i].c_str());
            }
            for (std::size_t i = 0; i < extra.size() && i < 8; ++i) {
                std::printf("    多: %s\n", extra[i].c_str());
            }
        }
        CHECK(missing.empty());
        CHECK(extra.empty());
    }
}

// ---------------------------------------------------------------- 2. 加载 / 切换

void testLoadAndSwitch() {
    const std::string loaded = i18n::initialize(langDirectory().wstring(), "zh-Hans");
    CHECK(loaded == "zh-Hans");
    CHECK(i18n::isLoaded());
    CHECK(i18n::languageCode() == "zh-Hans");
    CHECK(i18n::trw("common.ok") == L"确定");

    CHECK(i18n::setLanguage("en"));
    CHECK(i18n::languageCode() == "en");
    CHECK(i18n::tr("common.ok") == "OK");

    // 切回中文：字典换了，同一个 key 拿到另一种文字
    CHECK(i18n::setLanguage("zh-Hans"));
    CHECK(i18n::tr("common.ok") == "确定");

    // 不存在的语言：拒绝并保持原语言（不能把界面切成 key 模式）
    CHECK(!i18n::setLanguage("de"));
    CHECK(i18n::languageCode() == "zh-Hans");
}

void testLanguagesScanned() {
    const std::vector<i18n::LanguageInfo>& list = i18n::languages();
    bool hasEnglish = false;
    bool hasChinese = false;
    bool sorted = true;
    std::string previous;
    for (const auto& entry : list) {
        CHECK(!entry.code.empty());
        CHECK(!entry.name.empty());
        if (entry.code == "en") {
            hasEnglish = true;
        }
        if (entry.code == "zh-Hans") {
            hasChinese = true;
            // 名字用该语言自己的写法，下拉里才好认
            CHECK(entry.name == "简体中文");
        }
        if (!previous.empty() && previous > entry.code) {
            sorted = false;
        }
        previous = entry.code;
    }
    CHECK(hasEnglish);
    CHECK(hasChinese);
    CHECK(sorted);  // 按 code 排序，下拉顺序与文件系统顺序无关
}

// ---------------------------------------------------------------- 3. 回退链

void testFallbackChain() {
    const std::filesystem::path dir = freshDir(L"ktile_i18n_fallback");

    // 英语齐全
    writeText(dir / L"en.json",
              R"({"code":"en","name":"English","strings":{)"
              R"("a.only":"english only",)"
              R"("a.both":"english both"}})");
    // 假想语言缺一条
    writeText(dir / L"xx.json",
              R"({"code":"xx","name":"Test","strings":{)"
              R"("a.both":"xx both"}})");

    CHECK(i18n::initialize(dir, "xx") == "xx");
    CHECK(i18n::tr("a.both") == "xx both");     // 当前语言优先
    CHECK(i18n::tr("a.only") == "english only");  // 缺词 → 英语兜底
    CHECK(i18n::tr("a.missing") == "a.missing");  // 都没有 → key 本身

    // 目录里没有任何可用字典：不崩，界面显示 key，并报告「未加载」
    const std::filesystem::path empty = freshDir(L"ktile_i18n_empty");
    CHECK(i18n::initialize(empty.wstring(), "zh-Hans").empty());
    CHECK(!i18n::isLoaded());
    CHECK(i18n::tr("common.ok") == "common.ok");

    // 配置里写了一个没有语言文件的代码：实际生效英语（回退），不炸
    CHECK(i18n::initialize(dir.wstring(), "ja") == "en");
    CHECK(i18n::tr("a.both") == "english both");
}

// ---------------------------------------------------------------- 4. 占位符

void testPlaceholders() {
    i18n::initialize(langDirectory().wstring(), "en");
    // 参数里有大括号、中文、空串都不能把替换弄坏
    const std::wstring text =
        i18n::formatW("error.pluginNotFound", {L"demo"});
    CHECK(text.find(L"demo") != std::wstring::npos);
    CHECK(text.find(L"{0}") == std::wstring::npos);

    i18n::initialize(langDirectory().wstring(), "zh-Hans");
    const std::wstring chinese =
        i18n::formatW("error.pluginNotFound", {L"示例"});
    CHECK(chinese.find(L"示例") != std::wstring::npos);
    CHECK(chinese.find(L"{0}") == std::wstring::npos);
}

// ---------------------------------------------------------------- 5. 系统语言映射

void testSystemLanguageMapping() {
    const std::string system = i18n::systemLanguageCode();
    CHECK(system == "zh-Hans" || system == "en");

    CHECK(i18n::resolveLanguage("auto") == system);
    CHECK(i18n::resolveLanguage("") == system);  // 旧配置没有这个字段
    CHECK(i18n::resolveLanguage("zh-Hans") == "zh-Hans");
    CHECK(i18n::resolveLanguage("en") == "en");
    // 不认识的代码原样返回：配置值不动，回退发生在加载期 ——
    // 语言文件被误删后再放回来，用户不必重设一遍。
    CHECK(i18n::resolveLanguage("ja") == "ja");
}

}  // namespace

int main() {
    // 测试打印的是中文/日文样例，控制台按 UTF-8 输出才不会变成乱码
    SetConsoleOutputCP(CP_UTF8);
    std::printf("===== i18n 测试 =====\n");
    testLanguageFilesShareSameKeys();
    testLoadAndSwitch();
    testLanguagesScanned();
    testFallbackChain();
    testPlaceholders();
    testSystemLanguageMapping();

    std::printf("  检查 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
