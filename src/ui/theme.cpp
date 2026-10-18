// KTile - 皮肤（配色主题）实现
#include "ui/theme.h"

#include "base/text_convert.h"
#include "infrastructure/json/json_parser.h"
#include "infrastructure/json/json_writer.h"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <map>

namespace ktile {
namespace ui {

namespace {

std::wstring g_dataRoot;

std::wstring fileTitle(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::size_t dot = path.find_last_of(L'.');
    const std::size_t begin = (slash == std::wstring::npos) ? 0 : slash + 1;
    std::wstring name = path.substr(begin);
    if (dot != std::wstring::npos && dot > begin) {
        name = path.substr(begin, dot - begin);
    }
    return name;
}

bool readTextFileUtf8(const std::wstring& path, std::string* out) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    // 去 UTF-8 BOM（记事本保存可能带）
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }
    *out = std::move(content);
    return true;
}

bool writeTextFileUtf8(const std::wstring& path, const std::string& content) {
    std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file.write(content.data(),
               static_cast<std::streamsize>(content.size()));
    return file.good();
}

bool ensureDirectory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

int hexDigitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::uint64_t fileFingerprint(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard,
                             &attrs) == FALSE) {
        return 0;
    }
    const std::uint64_t size =
        (static_cast<std::uint64_t>(attrs.nFileSizeHigh) << 32) |
        attrs.nFileSizeLow;
    const std::uint64_t mtime =
        (static_cast<std::uint64_t>(attrs.ftLastWriteTime.dwLowDateTime)) |
        (static_cast<std::uint64_t>(attrs.ftLastWriteTime.dwHighDateTime)
         << 32);
    return size * 0x9E3779B97F4A7C15ULL + mtime;
}

std::string fnv1a64Hex(std::uint64_t hash) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (int i = 15; i >= 0; --i) {
        out += digits[(hash >> (i * 4)) & 0xF];
    }
    return out;
}

}  // namespace

// ================================================================ 颜色文本

bool parseHexColor(const std::string& text, std::uint32_t* out) {
    if (out == nullptr) {
        return false;
    }
    std::size_t begin = 0;
    if (!text.empty() && text[0] == '#') {
        begin = 1;
    }
    if (text.size() != begin + 6 && text.size() != begin + 8) {
        return false;
    }
    std::uint32_t value = 0;
    for (std::size_t i = begin; i < text.size(); ++i) {
        const int digit = hexDigitValue(text[i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    if (text.size() == begin + 6) {
        value |= 0xFF000000U;  // 6 位形式默认不透明
    }
    *out = value;
    return true;
}

std::string formatHexColor(std::uint32_t argb) {
    static const char* digits = "0123456789ABCDEF";
    std::string out = "#";
    for (int shift = 28; shift >= 0; shift -= 4) {
        out += digits[(argb >> shift) & 0xF];
    }
    return out;
}

// ================================================================ 目录

void setThemesRoot(const std::wstring& dataDirectory) {
    g_dataRoot = dataDirectory;
    while (!g_dataRoot.empty() &&
           (g_dataRoot.back() == L'\\' || g_dataRoot.back() == L'/')) {
        g_dataRoot.pop_back();
    }
}

std::wstring themesDirectory() {
    return g_dataRoot.empty() ? std::wstring() : g_dataRoot + L"\\themes";
}

std::wstring backgroundsDirectory() {
    const std::wstring dir = themesDirectory();
    return dir.empty() ? std::wstring() : dir + L"\\backgrounds";
}

// ================================================================ 读写

bool loadThemeFile(const std::wstring& path, ThemeColors* out,
                   std::wstring* displayName) {
    if (out == nullptr || path.empty()) {
        return false;
    }
    std::string text;
    if (!readTextFileUtf8(path, &text)) {
        return false;
    }
    const infrastructure::json::ParseResult parsed =
        infrastructure::json::parseJson(text);
    if (!parsed.ok || !parsed.root.isObject()) {
        return false;
    }

    ThemeColors theme;  // 先取默认值：缺键 = 用默认，手改文件删键不会炸
    std::uint32_t color = 0;
    if (parseHexColor(parsed.root.find("windowBackground").asString(),
                      &color)) {
        theme.windowBackground = color;
    }
    if (parseHexColor(parsed.root.find("accent").asString(), &color)) {
        theme.accent = color;
    }
    if (parseHexColor(parsed.root.find("text").asString(), &color)) {
        theme.text = color;
    }
    if (parseHexColor(parsed.root.find("tileFill").asString(), &color)) {
        theme.tileFill = color;
    }
    if (parseHexColor(parsed.root.find("tileHover").asString(), &color)) {
        theme.tileHover = color;
    }
    if (parseHexColor(parsed.root.find("tilePressed").asString(), &color)) {
        theme.tilePressed = color;
    }
    if (parseHexColor(parsed.root.find("tileBorder").asString(), &color)) {
        theme.tileBorder = color;
    }
    if (parseHexColor(parsed.root.find("tileHoverBorder").asString(),
                      &color)) {
        theme.tileHoverBorder = color;
    }
    if (parsed.root.find("backgroundDim").isNumber()) {
        double dim = parsed.root.find("backgroundDim").asDouble(0.55);
        if (dim < 0.0) dim = 0.0;
        if (dim > 1.0) dim = 1.0;
        theme.backgroundDim = static_cast<float>(dim);
    }

    *out = theme;
    if (displayName != nullptr) {
        const std::string nameUtf8 = parsed.root.find("name").asString();
        *displayName = nameUtf8.empty()
            ? fileTitle(path)
            : base::utf8ToWide(nameUtf8);
    }
    return true;
}

bool saveThemeFile(const std::wstring& path, const ThemeColors& theme,
                   const std::wstring& displayName) {
    infrastructure::json::JsonWriter writer;
    writer.beginObject();
    writer.member("name", base::wideToUtf8Lossy(displayName));
    writer.member("windowBackground", formatHexColor(theme.windowBackground));
    writer.member("accent", formatHexColor(theme.accent));
    writer.member("text", formatHexColor(theme.text));
    writer.member("tileFill", formatHexColor(theme.tileFill));
    writer.member("tileHover", formatHexColor(theme.tileHover));
    writer.member("tilePressed", formatHexColor(theme.tilePressed));
    writer.member("tileBorder", formatHexColor(theme.tileBorder));
    writer.member("tileHoverBorder", formatHexColor(theme.tileHoverBorder));
    writer.member("backgroundDim",
                  static_cast<double>(theme.backgroundDim));
    writer.endObject();
    return writeTextFileUtf8(path, writer.take() + "\n");
}

// ================================================================ 预置

// 内置皮肤。
//
// 前四套「深底 + 高对比文字」：界面文字/描边大量使用 text 的低 alpha
// 形式，深底才能保证可读性；accent 只做强调，不承担承载文字的职责。
// 第五套「晨光」是亮色方案：浅底 + 深字 + 白卡片磁贴，派生色规则
// 完全一样（低 alpha 的深色 = 淡灰），只是方向反过来。
const std::vector<std::pair<std::wstring, ThemeColors>>& builtinPresets() {
    static const std::vector<std::pair<std::wstring, ThemeColors>> presets = {
        // 经典深空：默认皮肤，与历代版本的观感完全一致（升级无感）
        {L"经典深空", ThemeColors()},
        // 石墨青：冷调近黑底 + 青色强调，对比干净，长时间看不腻
        {L"石墨青",
         [] {
             ThemeColors t;
             t.windowBackground = 0xFF161B22;
             t.accent = 0xFF2DD4BF;
             t.text = 0xFFE6EDF3;
             return t;
         }()},
        // 琥珀暮色：暖棕底 + 琥珀强调，氛围偏暖而饱和度克制
        {L"琥珀暮色",
         [] {
             ThemeColors t;
             t.windowBackground = 0xFF211C16;
             t.accent = 0xFFFFB454;
             t.text = 0xFFFFF3E0;
             return t;
         }()},
        // 幽夜紫：深紫底 + 柔紫强调，现代感强但不刺眼
        {L"幽夜紫",
         [] {
             ThemeColors t;
             t.windowBackground = 0xFF191622;
             t.accent = 0xFFA78BFA;
             t.text = 0xFFF2F0FF;
             return t;
         }()},
        // 晨光：亮色方案。底不用纯白（刺眼且没有层次），浅冷灰打底、
        // 磁贴用半透明白卡片 + 淡描边；强调色压深一档，保证白底上的对比度。
        // 磁贴底从「不透明白」改成 35% 白：不透明白卡片会把背景图整块
        // 盖死，观感生硬 —— 半透明白让背景隐约透出来，与深色皮肤的
        // 「玻璃底」观感对齐。
        {L"晨光",
         [] {
             ThemeColors t;
             t.windowBackground = 0xFFF3F5F9;
             t.accent = 0xFF2B6CE6;
             t.text = 0xFF1B1F24;
             t.tileFill = 0x59FFFFFF;       // 35% 白卡片
             t.tileHover = 0x73FFFFFF;      // 悬停：更亮
             t.tilePressed = 0x8CFFFFFF;    // 按下：再亮
             t.tileBorder = 0x1A000000;     // 10% 黑描边，勾勒卡片边界
             t.tileHoverBorder = 0x38000000;
             return t;
         }()},
    };
    return presets;
}

void ensurePresetThemesExist() {
    const std::wstring dir = themesDirectory();
    if (dir.empty() || !ensureDirectory(dir)) {
        return;
    }
    ensureDirectory(backgroundsDirectory());
    for (const auto& preset : builtinPresets()) {
        const std::wstring path = dir + L"\\" + preset.first + L".json";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            continue;  // 已存在（可能被用户改过）：不覆盖
        }
        saveThemeFile(path, preset.second, preset.first);
    }
}

std::vector<ThemeEntry> enumerateThemes() {
    std::vector<ThemeEntry> entries;
    const std::wstring dir = themesDirectory();
    if (dir.empty()) {
        return entries;
    }
    const std::wstring pattern = dir + L"\\*.json";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return entries;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        ThemeEntry entry;
        entry.path = dir + L"\\" + data.cFileName;
        ThemeColors colors;
        std::wstring name;
        if (loadThemeFile(entry.path, &colors, &name) && !name.empty()) {
            entry.displayName = name;
        } else {
            entry.displayName = fileTitle(entry.path);
        }
        entries.push_back(std::move(entry));
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);

    std::sort(entries.begin(), entries.end(),
              [](const ThemeEntry& a, const ThemeEntry& b) {
                  return a.displayName < b.displayName;
              });
    return entries;
}

bool loadThemeByName(const std::wstring& displayName, ThemeColors* out) {
    if (displayName.empty()) {
        return false;
    }
    for (const ThemeEntry& entry : enumerateThemes()) {
        if (entry.displayName == displayName &&
            loadThemeFile(entry.path, out, nullptr)) {
            return true;
        }
    }
    return false;
}

// ================================================================ 背景图

std::wstring importBackgroundImage(const std::wstring& sourcePath,
                                   std::wstring* error) {
    const std::wstring dir = backgroundsDirectory();
    if (dir.empty() || !ensureDirectory(dir)) {
        if (error != nullptr) {
            *error = L"无法创建背景图目录";
        }
        return std::wstring();
    }

    // 扩展名原样保留（WIC 按内容解码，不挑扩展名，但保留便于用户辨认）
    std::wstring ext;
    const std::size_t dot = sourcePath.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        ext = sourcePath.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    }
    if (ext.empty() || ext == L".") {
        ext = L".png";
    }

    // 哈希 = 路径 + 大小 + 修改时间。内容不变 → 名字不变 → 不重复复制
    const std::string hashSource =
        base::wideToUtf8Lossy(sourcePath) + "|" +
        std::to_string(fileFingerprint(sourcePath));
    std::uint64_t hash = 14695981039346656037ULL;  // FNV offset basis
    for (const char c : hashSource) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL;
    }

    const std::string hashHex = fnv1a64Hex(hash);  // 纯 ASCII，直接加宽
    const std::wstring stored =
        L"themes\\backgrounds\\" + std::wstring(hashHex.begin(),
                                               hashHex.end()) + ext;
    const std::wstring target = g_dataRoot + L"\\" + stored;
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return stored;  // 同一张图已导入过
    }
    if (CopyFileW(sourcePath.c_str(), target.c_str(), TRUE) == FALSE) {
        if (error != nullptr) {
            *error = L"复制背景图失败（文件可能被占用）";
        }
        return std::wstring();
    }
    return stored;
}

std::wstring resolveBackgroundImagePath(const std::wstring& storedPath) {
    if (storedPath.size() >= 2 &&
        (storedPath[1] == L':' || storedPath[0] == L'\\')) {
        return storedPath;  // 已是绝对路径
    }
    return g_dataRoot + L"\\" + storedPath;
}

}  // namespace ui
}  // namespace ktile
