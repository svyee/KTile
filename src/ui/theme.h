// KTile - 皮肤（配色主题）
//
// 设计取舍：
//
// 1. JSON 只暴露 9 个语义键（windowBackground / accent / text /
//    tileFill / tileHover / tilePressed / tileBorder / tileHoverBorder /
//    backgroundDim），其余约 30 个颜色全部由这几个键**派生**
//    （如「标签未选中文字 = text 的 62% 透明度」「拖入落点框 = accent
//    的 20%」）。手改皮肤文件的人面对 9 个键就够了，40 个键反而没人改得动。
//
// 2. 派生规则与历史配色逐值对齐：default ThemeColors 的每个派生结果
//    都等于重构前 launcher_window.cpp 里的编译期常量 —— 「经典深空」
//    皮肤渲染结果与升级前逐像素一致。
//
// 3. 皮肤文件是 UTF-8 JSON，存 <data>\themes\<显示名>.json，
//    用户可直接手动编辑；config.json 只存皮肤显示名，不内联颜色。
//
// 4. 预置皮肤默认按「深底 + 高对比文字」设计 —— 界面上的文字/描边
//    大量使用 text 的低 alpha 形式，浅色底会糊。另提供一套亮色方案
//    （晨光）：派生色规则完全一样（低 alpha 的深色 = 淡灰），
//    只是方向反过来；标题底衬、跟手磁贴、背景压暗层由 lightSurface()
//    自动切换黑白方向，皮肤 JSON 不需要额外开关。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ktile {
namespace ui {

// 一套完整的皮肤配色。前 9 个字段是「语义键」（可出现在皮肤 JSON 里），
// 其余为派生色（只读视图）。
struct ThemeColors {
    // ---- JSON 语义键 ----
    std::uint32_t windowBackground = 0xFF1E1E1E;
    std::uint32_t accent           = 0xFF4FA3FF;  // 强调色（选中/激活/焦点）
    std::uint32_t text             = 0xFFFFFFFF;  // 文字基准色
    std::uint32_t tileFill         = 0x3C000000;  // 磁贴底（常态）
    std::uint32_t tileHover        = 0x5A000000;  // 磁贴底（悬停）
    std::uint32_t tilePressed      = 0x78000000;  // 磁贴底（按下）
    std::uint32_t tileBorder       = 0x1EFFFFFF;  // 磁贴描边
    std::uint32_t tileHoverBorder  = 0x60FFFFFF;  // 磁贴描边（悬停）
    float backgroundDim            = 0.55F;       // 背景图压暗强度 0~1

    static std::uint32_t withAlphaOf(std::uint32_t color, std::uint32_t alpha) {
        return (alpha << 24) | (color & 0x00FFFFFFU);
    }

    // 背景是否为浅色（亮色皮肤）。
    //
    // 派生色据此决定「往白里混还是往黑里混」：标题底衬、跟手磁贴底色、
    // 背景图压暗层这三处如果写死黑色，亮色皮肤上会出现黑块/深色文字
    // 糊在黑底上。用亮度判定而不是给皮肤 JSON 多加一个 isLight 键 ——
    // 手改皮肤的人只填颜色就已经能表达亮暗，不必再教他一个开关。
    bool lightSurface() const noexcept {
        const float r = static_cast<float>((windowBackground >> 16) & 0xFFU);
        const float g = static_cast<float>((windowBackground >> 8) & 0xFFU);
        const float b = static_cast<float>(windowBackground & 0xFFU);
        // 感知亮度（ITU-R BT.601）。阈值取 0.58：常见的浅灰底
        // (#F4F6F9 ≈ 0.96) 稳稳落在亮色一侧，中灰以下仍算深色。
        return (0.299F * r + 0.587F * g + 0.114F * b) > 148.0F;
    }

    // ---- 派生色 ----
    std::uint32_t labelText() const noexcept { return text; }

    // 磁贴标题底衬（scrim）：把标题从图标内容里「拉」出来。
    // 与磁贴底同色相、alpha 拉高 —— 深色皮肤得到 55%/80% 黑
    // （与历史常量 0x8C000000 / 0xCC000000 逐值一致），
    // 亮色皮肤得到 55%/80% 白，深色标题同样读得清。
    std::uint32_t labelScrim() const noexcept {
        return withAlphaOf(tileFill, 0x8C);
    }
    std::uint32_t labelScrimHover() const noexcept {
        return withAlphaOf(tileFill, 0xCC);
    }

    // 背景图上叠的一层：深色皮肤压黑、亮色皮肤提白（都是为了让
    // 文字与背景图拉开对比）。强度由 backgroundDim 控制。
    std::uint32_t backgroundDimColor() const noexcept {
        return lightSurface() ? 0xFFFFFFFFU : 0xFF000000U;
    }

    // 标签条
    std::uint32_t tabTextIdle() const noexcept { return withAlphaOf(text, 0xA0); }
    std::uint32_t tabTextHover() const noexcept { return withAlphaOf(text, 0xE6); }
    std::uint32_t tabTextActive() const noexcept { return text; }
    std::uint32_t tabFillHover() const noexcept { return withAlphaOf(text, 0x1F); }
    std::uint32_t tabFillActive() const noexcept { return withAlphaOf(accent, 0x2E); }
    std::uint32_t tabUnderline() const noexcept { return accent; }
    std::uint32_t tabBarHairline() const noexcept { return withAlphaOf(text, 0x1F); }
    std::uint32_t tabButtonIdle() const noexcept { return withAlphaOf(text, 0x8C); }
    std::uint32_t tabButtonHover() const noexcept { return text; }
    std::uint32_t tabButtonDisabled() const noexcept { return withAlphaOf(text, 0x33); }
    std::uint32_t tabButtonHoverFill() const noexcept { return withAlphaOf(text, 0x22); }

    // 钉子
    std::uint32_t pinIdle() const noexcept { return withAlphaOf(text, 0x8C); }
    std::uint32_t pinActive() const noexcept { return accent; }
    std::uint32_t pinActiveBackdrop() const noexcept { return withAlphaOf(accent, 0x33); }
    std::uint32_t pinHoverBackdrop() const noexcept { return withAlphaOf(text, 0x22); }

    // 搜索框
    std::uint32_t searchBoxFill() const noexcept { return withAlphaOf(text, 0x1A); }
    std::uint32_t searchBoxActiveFill() const noexcept { return withAlphaOf(text, 0x33); }
    std::uint32_t searchBoxBorder() const noexcept { return withAlphaOf(text, 0x33); }
    std::uint32_t searchBoxActiveBorder() const noexcept { return accent; }
    std::uint32_t searchPlaceholder() const noexcept { return withAlphaOf(text, 0x59); }

    // 拖放反馈
    std::uint32_t dropTargetFill() const noexcept { return withAlphaOf(text, 0x18); }
    std::uint32_t dropTargetBorder() const noexcept { return withAlphaOf(text, 0x99); }
    std::uint32_t dropNewFill() const noexcept { return withAlphaOf(accent, 0x33); }
    std::uint32_t dropNewBorder() const noexcept { return withAlphaOf(accent, 0xCC); }

    // 跟手磁贴 / 键盘选中框
    // 跟手磁贴底色：与磁贴底同色相。深色皮肤沿用历史的 40% 黑
    // （0x66000000），亮色皮肤则提到接近不透明 —— 白色卡片半透明
    // 会像「破了洞」，靠放大与阴影表现脱离网格就够了。
    std::uint32_t liftedFill() const noexcept {
        return withAlphaOf(tileFill, lightSurface() ? 0xF0 : 0x66);
    }
    std::uint32_t liftedBorder() const noexcept { return withAlphaOf(text, 0x99); }
    std::uint32_t selectionFrame() const noexcept { return accent; }
    std::uint32_t accentSubstrate() const noexcept { return withAlphaOf(accent, 0x22); }

    // GDI 合成色：把 over 以不透明度 t 叠在 base 上（结果不透明，
    // 0x00BBGGRR 形式可直接进 RGB 宏 / COLORREF）
    static std::uint32_t blendOver(std::uint32_t base, std::uint32_t over,
                                   float t) {
        if (t < 0.0F) t = 0.0F;
        if (t > 1.0F) t = 1.0F;
        auto mix = [t, base, over](std::uint32_t shift) {
            const float b = static_cast<float>((base >> shift) & 0xFFU);
            const float o = static_cast<float>((over >> shift) & 0xFFU);
            const float m = b * (1.0F - t) + o * t;
            return static_cast<std::uint32_t>(m + 0.5F);
        };
        const std::uint32_t r = mix(16);
        const std::uint32_t g = mix(8);
        const std::uint32_t b = mix(0);
        return 0xFF000000U | (r << 16) | (g << 8) | b;
    }
};

inline std::uint32_t withAlpha(std::uint32_t color, std::uint32_t alpha) {
    return ThemeColors::withAlphaOf(color, alpha);
}

// ---- 颜色文本格式（皮肤 JSON 用）----
// "#RRGGBB" / "#AARRGGBB" / 无 # 前缀均可；parse 失败返回 false
bool parseHexColor(const std::string& text, std::uint32_t* out);
std::string formatHexColor(std::uint32_t argb);

// ---- 目录与文件 ----
// 数据根目录注入（与 icon_store::setIconStoreRoot 同一模式）
void setThemesRoot(const std::wstring& dataDirectory);
std::wstring themesDirectory();       // <data>\themes
std::wstring backgroundsDirectory();  // <data>\themes\backgrounds

struct ThemeEntry {
    std::wstring displayName;  // 来自 JSON 的 name 字段（解析失败用文件名）
    std::wstring path;         // 绝对路径
};

// 枚举 themes 目录下全部 .json（按显示名排序）
std::vector<ThemeEntry> enumerateThemes();

// 读皮肤文件。displayName 可为 nullptr。
bool loadThemeFile(const std::wstring& path, ThemeColors* out,
                   std::wstring* displayName);
// 写皮肤文件（UTF-8、美化缩进，可直接手改）
bool saveThemeFile(const std::wstring& path, const ThemeColors& theme,
                   const std::wstring& displayName);

// 首次启动：把内置预置写进 themes 目录（已存在的文件不覆盖 ——
// 用户改过预设的话，升级不会冲掉他的修改）
void ensurePresetThemesExist();

// 按显示名找皮肤（与 enumerateThemes 的匹配规则一致），找不到返回 false
bool loadThemeByName(const std::wstring& displayName, ThemeColors* out);

// 内置预置（同时用于生成文件与「配置里的皮肤名失效时」的回落）
const std::vector<std::pair<std::wstring, ThemeColors>>& builtinPresets();

// 导入背景图：复制进 backgrounds 目录（哈希命名，内容不变不重复复制），
// 返回相对 data 的存储值（形如 "themes\backgrounds\xxx.jpg"），失败返回空串
std::wstring importBackgroundImage(const std::wstring& sourcePath,
                                   std::wstring* error);

// 把 config 里存的背景图存储值解析成绝对路径（相对 → 相对 data 目录）
std::wstring resolveBackgroundImagePath(const std::wstring& storedPath);

}  // namespace ui
}  // namespace ktile
