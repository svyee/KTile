// KTile - UI 层：轮盘菜单窗口实现
#include "ui/radial_menu_window.h"

#include "base/text_convert.h"
#include "core/builtin_items.h"
#include "core/lucide_icons.h"
#include "platform/win32/icon_loader.h"
#include "platform/win32/icon_store.h"   // isDecodableImageFile
#include "platform/win32/svg_path.h"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <cmath>
#include <cwctype>
#include <filesystem>
#include <string>

namespace ktile {
namespace ui {

namespace {

constexpr float kPi = 3.14159265358979323846F;
float degToRad(float deg) { return deg * kPi / 180.0F; }

// 给定角度（0=正上，顺时针）和半径，返回圆周上的点（相对中心）
D2D1_POINT_2F pointOnCircle(float angleDeg, float radius) {
    const float rad = degToRad(angleDeg);
    return D2D1::Point2F(radius * std::sin(rad), -radius * std::cos(rad));
}

float easeOutCubic(float t) {
    if (t < 0.0F) t = 0.0F;
    if (t > 1.0F) t = 1.0F;
    const float inv = 1.0F - t;
    return 1.0F - inv * inv * inv;
}

// 扩展名比较（大小写不敏感）。清单里写 ".SVG" 也得认。
bool hasExtensionCi(const std::filesystem::path& path, const wchar_t* ext) {
    const std::wstring actual = path.extension().wstring();
    const std::wstring expected(ext);
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (std::towlower(static_cast<wint_t>(actual[i])) !=
            std::towlower(static_cast<wint_t>(expected[i]))) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- 快捷动作图标
//
// Shortcut 类型（Ctrl+C 这类）没有可提取的文件图标，但恰恰是默认菜单里
// 用得最多的一类。这里内嵌少量 Lucide 形状，让「复制/粘贴/剪切/撤销」
// 这些最常见的组合键也能显示图标；认不出的组合退回键盘图标。
// 形状来自 Lucide（ISC License），24x24 viewBox。
constexpr const wchar_t* kGlyphCopy =
    L"<rect width=\"14\" height=\"14\" x=\"8\" y=\"8\" rx=\"2\" ry=\"2\" />"
    L"<path d=\"M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2\" />";
constexpr const wchar_t* kGlyphPaste =
    L"<rect width=\"8\" height=\"4\" x=\"8\" y=\"2\" rx=\"1\" ry=\"1\" />"
    L"<path d=\"M16 4h2a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2h2\" />";
constexpr const wchar_t* kGlyphCut =
    L"<circle cx=\"6\" cy=\"6\" r=\"3\" /><path d=\"M8.12 8.12 12 12\" />"
    L"<path d=\"M20 4 8.12 15.88\" /><circle cx=\"6\" cy=\"18\" r=\"3\" />"
    L"<path d=\"M14.8 14.8 20 20\" />";
constexpr const wchar_t* kGlyphUndo =
    L"<path d=\"M9 14 4 9l5-5\" />"
    L"<path d=\"M4 9h10.5a5.5 5.5 0 0 1 5.5 5.5v0a5.5 5.5 0 0 1-5.5 5.5H11\" />";
constexpr const wchar_t* kGlyphRedo =
    L"<path d=\"m15 14 5-5-5-5\" />"
    L"<path d=\"M20 9H9.5A5.5 5.5 0 0 0 4 14.5v0A5.5 5.5 0 0 0 9.5 20H13\" />";
constexpr const wchar_t* kGlyphSave =
    L"<path d=\"M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z\" />"
    L"<path d=\"M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7\" />"
    L"<path d=\"M7 3v4a1 1 0 0 0 1 1h7\" />";
constexpr const wchar_t* kGlyphSearch =
    L"<circle cx=\"11\" cy=\"11\" r=\"8\" /><path d=\"m21 21-4.3-4.3\" />";
constexpr const wchar_t* kGlyphKeyboard =
    L"<path d=\"M10 8h.01\" /><path d=\"M12 12h.01\" /><path d=\"M14 8h.01\" />"
    L"<path d=\"M16 12h.01\" /><path d=\"M18 8h.01\" /><path d=\"M6 8h.01\" />"
    L"<path d=\"M7 16h10\" /><path d=\"M8 12h.01\" />"
    L"<rect width=\"20\" height=\"16\" x=\"2\" y=\"4\" rx=\"2\" />";

char asciiUpper(char ch) {
    return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
}

bool hasModifier(const core::RadialAction& action, core::HotkeyModifier modifier) {
    for (const auto& item : action.modifiers) {
        if (item == modifier) return true;
    }
    return false;
}

bool isSingleKey(const std::string& key, const char expected) {
    return key.size() == 1 && asciiUpper(key[0]) == expected;
}

// 快捷动作 → Lucide 形状；认不出的组合键返回键盘图标。
const wchar_t* shortcutGlyphMarkup(const core::RadialAction& action) {
    const bool ctrl = hasModifier(action, core::HotkeyModifier::Control);
    if (ctrl) {
        if (isSingleKey(action.key, 'C')) return kGlyphCopy;
        if (isSingleKey(action.key, 'V')) return kGlyphPaste;
        if (isSingleKey(action.key, 'X')) return kGlyphCut;
        if (isSingleKey(action.key, 'Z')) {
            return hasModifier(action, core::HotkeyModifier::Shift) ? kGlyphRedo
                                                                   : kGlyphUndo;
        }
        if (isSingleKey(action.key, 'Y')) return kGlyphRedo;
        if (isSingleKey(action.key, 'S')) return kGlyphSave;
        if (isSingleKey(action.key, 'F')) return kGlyphSearch;
    }
    return kGlyphKeyboard;
}

}  // namespace

RadialMenuWindow::RadialMenuWindow() = default;

RadialMenuWindow::~RadialMenuWindow() {
    hide();
    discardDeviceResources();
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool RadialMenuWindow::create(HINSTANCE instance) {
    instance_ = instance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &RadialMenuWindow::windowProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClassName;

    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kWindowClassName, L"KTileRadial", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, instance, this);
    return hwnd_ != nullptr;
}

float RadialMenuWindow::dpiScale() const noexcept {
    return static_cast<float>(dpi_) / 96.0F;
}

void RadialMenuWindow::screenToDipOffset(POINT screenPoint,
                                         float& dx, float& dy) const {
    const float scale = dpiScale();
    dx = (static_cast<float>(screenPoint.x - centerScreen_.x)) / scale;
    dy = (static_cast<float>(screenPoint.y - centerScreen_.y)) / scale;
}

void RadialMenuWindow::startAppearAnimation() {
    animScale_ = kAnimStartScale;
    animAlpha_ = kAnimStartAlpha;
    animating_ = true;
    animStartTick_ = GetTickCount64();
    SetTimer(hwnd_, kAnimTimerId, 15, nullptr);
}

void RadialMenuWindow::tickAnimation() {
    const ULONGLONG elapsed = GetTickCount64() - animStartTick_;
    float t = static_cast<float>(elapsed) /
              static_cast<float>(kAnimDurationMs);
    const bool finished = t >= 1.0F;
    const float eased = easeOutCubic(t);
    animScale_ = kAnimStartScale + (1.0F - kAnimStartScale) * eased;
    animAlpha_ = kAnimStartAlpha + (1.0F - kAnimStartAlpha) * eased;
    if (finished) {
        animating_ = false;
        animScale_ = 1.0F;
        animAlpha_ = 1.0F;
        KillTimer(hwnd_, kAnimTimerId);
    }
    render();
}

void RadialMenuWindow::show(const core::RadialMenu& menu, POINT centerScreen,
                            int cancelRadiusDip) {
    menu_ = menu;
    centerScreen_ = centerScreen;
    highlightedValid_ = false;

    // 外甩取消半径：低于卡片环外缘的值没有意义（会切掉卡片自己），夹到下限。
    cancelRadiusDip_ = cancelRadiusDip > 0 ? static_cast<float>(cancelRadiusDip)
                                           : kOuterRadiusDip;
    if (cancelRadiusDip_ < static_cast<float>(core::kMinCancelRadiusDip)) {
        cancelRadiusDip_ = static_cast<float>(core::kMinCancelRadiusDip);
    }

    // 系统 DPI（GetDpiForWindow 是 Win10 1607+，用 Win7 可用的兼容方案）
    HDC screenDc = GetDC(nullptr);
    if (screenDc != nullptr) {
        const int dpiX = GetDeviceCaps(screenDc, LOGPIXELSX);
        ReleaseDC(nullptr, screenDc);
        if (dpiX > 0) dpi_ = static_cast<std::uint32_t>(dpiX);
    }

    const float scale = dpiScale();
    const float sizeDip = kWindowRadiusDip * 2.0F;
    windowSizePx_ = static_cast<int>(std::ceil(sizeDip * scale));
    if (windowSizePx_ < 120) windowSizePx_ = 120;

    // 把窗口夹到屏幕工作区内，避免鼠标在边缘时整圈被裁掉一半。
    // 夹完之后做两件事（参考 StarPie 的边缘处理）：
    // 1. centerScreen_ 同步成实际窗口中心（命中测试的锚点）；
    // 2. SetCursorPos 把光标挪到实际中心——这样「光标相对轮盘中心」
    //    仍是原点，用户拖动时的方向记忆不受夹取影响。若只挪窗口不动
    //    光标，光标会留在屏幕边缘，向左扇区的方向映射全错位。
    int left = centerScreen.x - windowSizePx_ / 2;
    int top = centerScreen.y - windowSizePx_ / 2;
    HMONITOR monitor = MonitorFromPoint(centerScreen, MONITOR_DEFAULTTONEAREST);
    if (monitor != nullptr) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(monitor, &mi) != FALSE) {
            const RECT& work = mi.rcWork;
            const int maxLeft = work.right - windowSizePx_;
            const int maxTop = work.bottom - windowSizePx_;
            if (left < work.left) left = work.left;
            if (left > maxLeft) left = maxLeft;
            if (top < work.top) top = work.top;
            if (top > maxTop) top = maxTop;
        }
    }
    centerScreen_.x = left + windowSizePx_ / 2;
    centerScreen_.y = top + windowSizePx_ / 2;

    const bool centerShifted =
        centerScreen_.x != centerScreen.x || centerScreen_.y != centerScreen.y;
    if (centerShifted) {
        SetCursorPos(centerScreen_.x, centerScreen_.y);
    }

    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, windowSizePx_, windowSizePx_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    if (!createDeviceResources()) {
        return;
    }

    startAppearAnimation();
    render();
}

void RadialMenuWindow::updateCursor(POINT screenPoint) {
    if (!isVisible()) return;

    float dx = 0, dy = 0;
    screenToDipOffset(screenPoint, dx, dy);

    auto hit = core::hitTestSector(dx, dy, kInnerRadiusDip, cancelRadiusDip_,
                                   menu_.sectorCount);
    std::optional<std::uint8_t> next;
    if (hit.has_value() && *hit < menu_.actions.size() &&
        !menu_.actions[*hit].isEmpty()) {
        next = *hit;
    }

    const bool changed = next.has_value() != highlightedValid_ ||
                         (next.has_value() && *next != highlightedSector_);
    if (changed) {
        highlightedValid_ = next.has_value();
        if (next.has_value()) highlightedSector_ = *next;
        render();
    }
}

void RadialMenuWindow::releaseAt(POINT screenPoint) {
    if (!isVisible()) return;
    updateCursor(screenPoint);
    if (highlightedValid_ && actionHandler_) {
        actionHandler_(highlightedSector_);
    }
    hide();
}

void RadialMenuWindow::hide() {
    if (hwnd_ != nullptr) {
        KillTimer(hwnd_, kAnimTimerId);
        animating_ = false;
        animScale_ = 1.0F;
        if (IsWindowVisible(hwnd_)) {
            ShowWindow(hwnd_, SW_HIDE);
        }
    }
    highlightedValid_ = false;
}

bool RadialMenuWindow::isVisible() const noexcept {
    return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE;
}

// ================================================================ 图标

ID2D1Bitmap* RadialMenuWindow::iconForAction(const core::RadialAction& action) {
    const bool vectorOnly = (action.type == core::RadialActionType::Shortcut ||
                             action.type == core::RadialActionType::Builtin);
    const std::string custom = action.icon.value_or(std::string());

    // 快捷键 / 内置命令本身没有可提取的文件图标，但插件可以塞一张位图给它
    // （图标外置是插件的核心诉求，插件里几乎全是快捷键动作 —— 这里要是
    //   一律返回 nullptr，插件作者放的图标就全部白发）。
    // 只认可解码位图：SVG 走 customGlyphForAction，让 loadIconFromFile 去
    // 处理 .svg 只会拿到「图片文件」的通用空白页图标。
    if (vectorOnly) {
        if (custom.empty()) return nullptr;
        if (!platform::win32::isDecodableImageFile(base::utf8ToWideLossy(custom))) {
            return nullptr;
        }
    } else if (action.target.empty()) {
        return nullptr;
    }

    const std::string key = action.target + "|" + custom;
    auto it = iconCache_.find(key);
    if (it != iconCache_.end()) {
        return it->second.get();
    }

    platform::win32::UniqueHIcon icon =
        vectorOnly
            ? platform::win32::loadIconFromFile(base::utf8ToWideLossy(custom),
                                                platform::win32::IconSizeHint::Small)
            : platform::win32::loadItemIcon(
                  action.target, custom,
                  action.type == core::RadialActionType::Folder,
                  action.type == core::RadialActionType::Url,
                  platform::win32::IconSizeHint::Small);

    if (!icon.valid() || !wicFactory_ || !renderTarget_) {
        iconCache_.emplace(key, platform::win32::ComPtr<ID2D1Bitmap>());
        return nullptr;
    }

    platform::win32::ComPtr<IWICBitmap> wicBitmap;
    if (FAILED(wicFactory_->CreateBitmapFromHICON(icon.get(), wicBitmap.put()))) {
        iconCache_.emplace(key, platform::win32::ComPtr<ID2D1Bitmap>());
        return nullptr;
    }

    platform::win32::ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory_->CreateFormatConverter(converter.put()))) {
        iconCache_.emplace(key, platform::win32::ComPtr<ID2D1Bitmap>());
        return nullptr;
    }
    if (FAILED(converter->Initialize(wicBitmap.get(),
                                     GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0F,
                                     WICBitmapPaletteTypeCustom))) {
        iconCache_.emplace(key, platform::win32::ComPtr<ID2D1Bitmap>());
        return nullptr;
    }

    platform::win32::ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(renderTarget_->CreateBitmapFromWicBitmap(converter.get(), nullptr,
                                                        bitmap.put()))) {
        iconCache_.emplace(key, platform::win32::ComPtr<ID2D1Bitmap>());
        return nullptr;
    }

    auto inserted = iconCache_.emplace(key, std::move(bitmap));
    return inserted.first->second.get();
}

// ================================================================ 矢量图标
//
// 快捷键（Ctrl+C 等）与内置命令（ktile:lock 等）没有可提取的文件图标，
// 但都可以映射到 Lucide 形状。几何按 24x24 基准建一次，之后靠
// SetTransform 平移到卡片位置 —— 旋转/缩放动画不会让图标模糊。

const platform::win32::SvgGlyph* RadialMenuWindow::customGlyphForAction(
    const core::RadialAction& action) {
    if (!action.icon.has_value() || action.icon->empty() || !d2dFactory_.valid()) {
        return nullptr;
    }

    std::filesystem::path candidate(base::utf8ToWideLossy(*action.icon));
    // 清单可以只写 "icons/copy" 而实际文件是 copy.svg（和磁贴插件同一约定）
    if (!candidate.has_extension()) {
        candidate += L".svg";
    }
    if (!hasExtensionCi(candidate, L".svg")) {
        return nullptr;  // 位图交给 iconForAction
    }

    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(candidate, error);
    if (error) {
        return nullptr;  // 文件不存在
    }
    const std::wstring key =
        candidate.wstring() + L"|" +
        std::to_wstring(static_cast<long long>(stamp.time_since_epoch().count()));

    auto it = customGlyphCache_.find(key);
    if (it == customGlyphCache_.end()) {
        std::wstring markup;
        if (!platform::win32::readSvgMarkupFile(candidate.wstring(), &markup)) {
            return nullptr;
        }
        platform::win32::SvgGlyph glyph;
        // 与内置 Lucide 图标同一约定：24x24 用户单位。放不进 24 框的 SVG
        // （比如自己画的 512 viewBox）会得到一个大到看不见的图形，插件文档
        // 里把这条写清楚了 —— 不做自动缩放，因为无法从标记里可靠取到 viewBox。
        const float scale = kIconSizeDip / 24.0F;
        if (!platform::win32::buildSvgGlyph(d2dFactory_.get(), markup.c_str(),
                                            scale, 0.0F, 0.0F, &glyph)) {
            return nullptr;
        }
        if (glyph.strokeGeometry.get() == nullptr &&
            glyph.fillGeometry.get() == nullptr &&
            glyph.colorLayers.empty()) {
            return nullptr;
        }
        it = customGlyphCache_.emplace(key, std::move(glyph)).first;
    }
    return &it->second;
}

const platform::win32::SvgGlyph* RadialMenuWindow::glyphForAction(
    const core::RadialAction& action) {
    if (!d2dFactory_ || !renderTarget_) return nullptr;

    const wchar_t* markup = nullptr;
    if (action.type == core::RadialActionType::Shortcut) {
        markup = shortcutGlyphMarkup(action);
    } else if (action.type == core::RadialActionType::Builtin) {
        const std::wstring id =
            core::builtinIdFromTarget(base::utf8ToWideLossy(action.target));
        if (!id.empty()) markup = core::lucideBuiltinMarkup(id);
    }
    if (markup == nullptr) return nullptr;

    std::wstring key(markup);
    auto it = glyphCache_.find(key);
    if (it != glyphCache_.end()) {
        return it->second.strokeGeometry || it->second.fillGeometry
                   ? &it->second
                   : nullptr;
    }

    platform::win32::SvgGlyph glyph;
    const float scale = kIconSizeDip / 24.0F;
    if (!platform::win32::buildSvgGlyph(d2dFactory_.get(), markup, scale,
                                        0.0F, 0.0F, &glyph)) {
        glyphCache_.emplace(std::move(key), platform::win32::SvgGlyph());
        return nullptr;
    }
    auto inserted = glyphCache_.emplace(std::move(key), std::move(glyph));
    return &inserted.first->second;
}

// ================================================================ D2D 资源

IDWriteTextFormat* RadialMenuWindow::labelFormatForSize(float size) {
    // 量化成 0.5 DIP 步进：字号是算出来的浮点数，直接用 float 当 map 键
    // 会因为 8.799999 和 8.800001 各建一份格式。
    int step = static_cast<int>(std::lround(size * 2.0F));
    if (step < static_cast<int>(kLabelMinFontDip * 2.0F)) {
        step = static_cast<int>(kLabelMinFontDip * 2.0F);
    }
    auto it = labelFormats_.find(step);
    if (it != labelFormats_.end()) {
        return it->second.get();
    }

    platform::win32::ComPtr<IDWriteTextFormat> format;
    const HRESULT hr = dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        static_cast<float>(step) / 2.0F, L"zh-CN", format.put());
    if (FAILED(hr)) {
        return nullptr;
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // 不换行：轮盘卡片里的标签必须是一行，折成两行会把图标挤掉
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    IDWriteTextFormat* raw = format.get();
    labelFormats_.emplace(step, std::move(format));
    return raw;
}

float RadialMenuWindow::measureLabelWidth(const std::wstring& text, float fontDip) {
    IDWriteTextFormat* format = labelFormatForSize(fontDip);
    if (format == nullptr || text.empty()) {
        return 0.0F;
    }
    // 让 DirectWrite 自己量：中英混排、全角标点、连字都会让「字符数 x 字号」
    // 这种估算偏得离谱，偏小就溢出、偏大又把字白缩一圈。
    platform::win32::ComPtr<IDWriteTextLayout> layout;
    const HRESULT hr = dwriteFactory_->CreateTextLayout(
        text.c_str(), static_cast<UINT32>(text.size()), format, 4096.0F, 4096.0F,
        layout.put());
    if (FAILED(hr)) {
        return 0.0F;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return 0.0F;
    }
    return metrics.widthIncludingTrailingWhitespace;
}

RadialMenuWindow::LabelFit RadialMenuWindow::fitLabel(
    const std::string& cacheKey, const std::wstring& text,
    float availableWidth) {
    const auto cached = labelFitCache_.find(cacheKey);
    if (cached != labelFitCache_.end()) {
        return cached->second;
    }

    const int baseStep = static_cast<int>(std::lround(kLabelFontDip * 2.0F));
    const int minStep = static_cast<int>(std::lround(kLabelMinFontDip * 2.0F));
    LabelFit fit;
    fit.text = text;
    fit.fontStep = baseStep;

    const float natural = measureLabelWidth(text, kLabelFontDip);
    if (availableWidth <= 0.0F || natural <= availableWidth || natural <= 0.0F) {
        labelFitCache_.emplace(cacheKey, fit);
        return fit;   // 放得下，原样用基准字号
    }

    // 第一步：按比例缩字号，缩到下限为止
    const float scaled = kLabelFontDip * availableWidth / natural;
    fit.fontStep = static_cast<int>(std::lround(scaled * 2.0F));
    if (fit.fontStep < minStep) {
        fit.fontStep = minStep;
    }
    if (measureLabelWidth(text, static_cast<float>(fit.fontStep) / 2.0F) <=
        availableWidth) {
        labelFitCache_.emplace(cacheKey, fit);
        return fit;
    }

    // 第二步：缩到下限还放不下 —— 截断加省略号。
    // 宁可少显示几个字，也不许在字中间切一刀：半个汉字看起来像渲染坏了。
    fit.fontStep = minStep;
    const std::wstring ellipsis = L"\u2026";
    if (measureLabelWidth(ellipsis, kLabelMinFontDip) > availableWidth) {
        labelFitCache_.emplace(cacheKey, fit);   // 连省略号都放不下，交给裁剪
        return fit;
    }
    for (std::size_t keep = text.size(); keep > 0; --keep) {
        const std::wstring candidate = text.substr(0, keep - 1) + ellipsis;
        if (measureLabelWidth(candidate, kLabelMinFontDip) <= availableWidth) {
            fit.text = candidate;
            break;
        }
    }
    if (fit.text == text) {
        // 一个字符都装不下：至少留一个字 + 省略号，剩下的让裁剪兜底
        fit.text = text.substr(0, 1) + ellipsis;
    }
    labelFitCache_.emplace(cacheKey, fit);
    return fit;
}

bool RadialMenuWindow::createDeviceResources() {
    if (!d2dFactory_) {
        D2D1_FACTORY_OPTIONS options{};
        HRESULT hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory), &options,
            reinterpret_cast<void**>(d2dFactory_.put()));
        if (FAILED(hr)) return false;
    }

    if (!dwriteFactory_) {
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.put()));
        if (FAILED(hr)) return false;

        // 基准字号必须建得出来，否则后面所有标签都没有格式可用
        if (labelFormatForSize(kLabelFontDip) == nullptr) {
            return false;
        }
    }

    if (!wicFactory_) {
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(wicFactory_.put()));
        if (FAILED(hr)) return false;
    }

    if (memDc_ == nullptr) {
        HDC screenDc = GetDC(nullptr);
        memDc_ = CreateCompatibleDC(screenDc);
        ReleaseDC(nullptr, screenDc);
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = windowSizePx_;
    bmi.bmiHeader.biHeight = -windowSizePx_;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    if (dibBitmap_ != nullptr) {
        SelectObject(memDc_, oldBitmap_);
        DeleteObject(dibBitmap_);
        dibBitmap_ = nullptr;
    }
    dibBitmap_ = CreateDIBSection(memDc_, &bmi, DIB_RGB_COLORS, nullptr, nullptr, 0);
    if (dibBitmap_ == nullptr) return false;
    oldBitmap_ = static_cast<HBITMAP>(SelectObject(memDc_, dibBitmap_));

    if (!renderTarget_) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2dFactory_->CreateDCRenderTarget(&props, renderTarget_.put())))
            return false;
    }

    if (!cellBrush_) {
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(1.0F, 1.0F, 1.0F, 0.97F), cellBrush_.put());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(1.0F, 1.0F, 1.0F, 0.06F), emptyCellBrush_.put());
        // 空槽位的轮廓必须用「深色」而不是白色：
        // 填了动作的卡片是近乎不透明的白卡 + 深色描边，浅壁纸上靠描边显形；
        // 空槽位如果也画白色描边，在浅壁纸上就彻底消失，用户会以为自己只有
        // 4 个扇区（其实是 8 个，另外 4 个只是没配）。白色填充留给深壁纸。
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.06F, 0.09F, 0.16F, 0.11F), emptyOutlineBrush_.put());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.231F, 0.510F, 0.965F, 1.0F), highlightBrush_.put());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.06F, 0.09F, 0.16F, 0.10F), borderBrush_.put());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.16F), shadowBrush_.put());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.12F, 0.16F, 0.22F, 1.0F), textBrush_.put());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(1.0F, 1.0F, 1.0F, 1.0F), accentTextBrush_.put());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.94F, 0.95F, 0.96F, 0.92F), hubBrush_.put());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.80F, 0.82F, 0.86F, 0.85F), hubBorderBrush_.put());
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.12F, 0.16F, 0.22F, 1.0F), glyphAccentBrush_.put());
        glyphStrokeStyle_ =
            platform::win32::createRoundStrokeStyle(d2dFactory_.get());
    }

    return true;
}

void RadialMenuWindow::discardDeviceResources() {
    iconCache_.clear();
    glyphCache_.clear();
    customGlyphCache_.clear();
    cellBrush_.reset();
    emptyCellBrush_.reset();
    emptyOutlineBrush_.reset();
    highlightBrush_.reset();
    borderBrush_.reset();
    shadowBrush_.reset();
    textBrush_.reset();
    accentTextBrush_.reset();
    hubBrush_.reset();
    hubBorderBrush_.reset();
    glyphAccentBrush_.reset();
    glyphStrokeStyle_.reset();
    // 排版缓存与格式缓存一起清：格式来自旧的 DWrite 工厂，工厂都重建了，
    // 留着的旧结果虽然还能用，但换机器/换字体后量出来的宽度会不一样
    labelFormats_.clear();
    labelFitCache_.clear();
    wicFactory_.reset();
    renderTarget_.reset();
    dwriteFactory_.reset();
    d2dFactory_.reset();

    if (oldBitmap_ != nullptr && memDc_ != nullptr) {
        SelectObject(memDc_, oldBitmap_);
        oldBitmap_ = nullptr;
    }
    if (dibBitmap_ != nullptr) {
        DeleteObject(dibBitmap_);
        dibBitmap_ = nullptr;
    }
    if (memDc_ != nullptr) {
        DeleteDC(memDc_);
        memDc_ = nullptr;
    }
}

// ================================================================ 绘制

void RadialMenuWindow::paintCell(std::uint8_t index, bool highlighted) {
    const float sectorWidth = 360.0F / static_cast<float>(menu_.sectorCount);
    const float angle = static_cast<float>(index) * sectorWidth;

    const float scale = dpiScale();
    const float centerX = static_cast<float>(windowSizePx_) / 2.0F / scale;
    const float centerY = centerX;
    const D2D1_POINT_2F center = D2D1::Point2F(centerX, centerY);

    const bool empty = index >= menu_.actions.size() ||
                       menu_.actions[index].isEmpty();

    // 卡片矩形：以扇区中心 pos 为中心的 62x40 圆角卡片
    // 卡片位置：挂在半径 kCellCenterRadiusDip 的圆上。卡片本身**不随角度
    // 旋转**（以前是像花瓣一样沿径向歪着的）。
    //
    // 为什么放弃旋转：卡片里要放一行水平文字，而图标和文字都不跟着转。一个
    // 62x40 的矩形转过 45° 之后，能容下的居中水平文字宽度只剩三十几 DIP——
    // 四个汉字（40 DIP）就会从斜边探出去，看着就是「字跑到按钮外面」。
    // 改成轴对齐之后，8 个卡片长得一模一样、标签也在同一高度，辨识起来反而
    // 更省力；「环」的感觉靠位置维持，不靠歪着的卡片。
    // 相邻卡片也不会打挤：45° 方向上是 68.6 DIP 的横向错开（> 62），横向那一对
    // 则是 68.6 DIP 的纵向错开（> 40），总有一个方向是让开的。
    const D2D1_POINT_2F pos =
        D2D1::Point2F(centerX + pointOnCircle(angle, kCellCenterRadiusDip).x,
                      centerY + pointOnCircle(angle, kCellCenterRadiusDip).y);

    const float left = pos.x - kCellWidthDip * 0.5F;
    const float top = pos.y - kCellHeightDip * 0.5F;
    const D2D1_RECT_F rect = D2D1::RectF(
        left, top, left + kCellWidthDip, top + kCellHeightDip);
    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
        rect, kCellCornerDip, kCellCornerDip);

    // 高亮只改颜色不放大：放大单元格会让鼠标相对扇区边界的几何关系
    // 跟着变化（本单元格外扩会盖住相邻扇区的判定角），扫过边界时容易
    // 出现「在两个扇区之间来回跳」的抖动。
    const float popScale = animScale_;
    const D2D1_MATRIX_3X2_F base =
        D2D1::Matrix3x2F::Scale(popScale, popScale, center);
    renderTarget_->SetTransform(base);

    // 伪阴影（D2D 1.0 没有模糊效果，用偏移的半透明圆角矩形近似）
    const D2D1_ROUNDED_RECT shadow = D2D1::RoundedRect(
        D2D1::RectF(rect.left, rect.top + 2.0F, rect.right, rect.bottom + 2.0F),
        kCellCornerDip, kCellCornerDip);
    if (!empty) {
        renderTarget_->FillRoundedRectangle(shadow, shadowBrush_.get());
    }

    if (empty) {
        // 空槽位只画一个很淡的轮廓：环的"骨架"要看得见，但不能抢戏，
        // 也不能像实心灰块那样堆成一团。
        renderTarget_->FillRoundedRectangle(rounded, emptyCellBrush_.get());
        renderTarget_->DrawRoundedRectangle(rounded, emptyOutlineBrush_.get(),
                                            1.0F);
    } else {
        renderTarget_->FillRoundedRectangle(
            rounded, highlighted ? highlightBrush_.get() : cellBrush_.get());
        renderTarget_->DrawRoundedRectangle(
            rounded, highlighted ? highlightBrush_.get() : borderBrush_.get(),
            highlighted ? 2.0F : 1.0F);
    }

    if (!empty && index < menu_.actions.size()) {
        const core::RadialAction& action = menu_.actions[index];
        // 图标解析顺序：动作自带的 SVG（插件目录里的）→ 可提取的位图/文件图标
        // → 内置 Lucide 符号。自带的排第一，是因为插件作者放图标就是要它生效。
        const platform::win32::SvgGlyph* glyph = customGlyphForAction(action);
        ID2D1Bitmap* icon =
            (glyph != nullptr) ? nullptr : iconForAction(action);
        if (glyph == nullptr && icon == nullptr) {
            glyph = glyphForAction(action);
        }
        const bool hasIcon = icon != nullptr || glyph != nullptr;
        ID2D1SolidColorBrush* iconBrush =
            highlighted ? accentTextBrush_.get() : textBrush_.get();

        if (hasIcon) {
            const float half = kIconSizeDip * 0.5F;
            // 图标中心比卡片中心稍高，给下方文字留位：19 DIP 的图标占
            // -16.5~+2.5，文字框从 +3 开始，两者刚好不打架。
            const float iconCenterY = pos.y - 7.0F;

            if (icon != nullptr) {
                const D2D1_RECT_F dest = D2D1::RectF(
                    pos.x - half, iconCenterY - half,
                    pos.x + half, iconCenterY + half);
                renderTarget_->DrawBitmap(icon, dest, 1.0F,
                                          D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                // 几何按 24x24 建立、已经缩放到 kIconSizeDip。
                // 用弧长质心（光学中心）居中：Lucide 图标在 24 框里的留白
                // 不固定，剪刀左重右轻、撤销箭头尾重头轻这类形状如果按
                // 包围盒或 20x20 框居中，视觉重心会偏到一边，和下方文字对不齐。
                float anchorX, anchorY;
                if (glyph->hasCentroid) {
                    anchorX = glyph->centroid.x;
                    anchorY = glyph->centroid.y;
                } else {
                    anchorX = (glyph->bounds.left + glyph->bounds.right) * 0.5F;
                    anchorY = (glyph->bounds.top + glyph->bounds.bottom) * 0.5F;
                }
                const bool anchorValid =
                    (glyph->hasCentroid) ||
                    (glyph->bounds.right > glyph->bounds.left &&
                     glyph->bounds.bottom > glyph->bounds.top);
                const float tx = anchorValid ? pos.x - anchorX : pos.x - half;
                const float ty = anchorValid ? iconCenterY - anchorY : iconCenterY - half;
                // 注意矩阵顺序：D2D 行向量约定下 A * B 是「先 A 后 B」。
                // 必须先平移（把锚点送到 pos）再绕菜单中心缩放，写成
                // base * Translation 会让平移量逃过缩放——高亮 popScale=1.06
                // 时图标被甩到左上约 9 DIP（出现动画期间也会暂时偏移）。
                renderTarget_->SetTransform(
                    D2D1::Matrix3x2F::Translation(tx, ty) * base);
                if (glyph->strokeGeometry) {
                    renderTarget_->DrawGeometry(glyph->strokeGeometry.get(),
                                                iconBrush, kGlyphStrokeDip,
                                                glyphStrokeStyle_.get());
                }
                if (glyph->fillGeometry) {
                    renderTarget_->FillGeometry(glyph->fillGeometry.get(),
                                                iconBrush);
                }
                // 显式带颜色的图层（插件 SVG 里写 stroke="#e5484d" 之类）：
                // 保留它自己的 RGB，只继承当前图标的透明度 —— 高亮反色时
                // 彩色图标不该忽明忽暗地跳成别的颜色。
                if (!glyph->colorLayers.empty() && glyphAccentBrush_.valid()) {
                    const D2D1_COLOR_F base = iconBrush->GetColor();
                    for (const platform::win32::SvgColorLayer& layer :
                         glyph->colorLayers) {
                        if (!layer.geometry || layer.color == 0) continue;
                        const std::uint32_t rgb = layer.color & 0x00FFFFFFU;
                        const float alpha =
                            static_cast<float>((layer.color >> 24) & 0xFFU) /
                            255.0F * base.a;
                        glyphAccentBrush_->SetColor(D2D1::ColorF(
                            static_cast<std::uint32_t>(rgb), alpha));
                        renderTarget_->DrawGeometry(layer.geometry.get(),
                                                    glyphAccentBrush_.get(),
                                                    kGlyphStrokeDip,
                                                    glyphStrokeStyle_.get());
                    }
                }
                renderTarget_->SetTransform(base);
            }
        }

        if (!action.name.empty()) {
            const std::wstring wideName = base::utf8ToWideLossy(action.name);
            // 有图标：文字框落在图标下方（顶边距卡片中心 3 DIP，下沿离卡片
            // 下沿还有 40/2-3-14 = 3 DIP，圆角啃不到字）。
            // 没图标：文字框垂直居中。
            const float labelTop = hasIcon ? pos.y + kLabelTopOffsetDip
                                           : pos.y - kLabelHeightDip * 0.5F;
            const float labelWidth = kCellWidthDip - kLabelSidePaddingDip * 2.0F;
            const D2D1_RECT_F textRect = D2D1::RectF(
                pos.x - kCellWidthDip * 0.5F + kLabelSidePaddingDip, labelTop,
                pos.x + kCellWidthDip * 0.5F - kLabelSidePaddingDip,
                labelTop + kLabelHeightDip);
            const LabelFit fit = fitLabel(action.name, wideName, labelWidth);
            // 兜底裁剪：字号与截断已经处理过绝大多数情况，但「极短卡片 + 全角
            // 标点」这类边角仍可能超出去，宁可切掉也不许画到卡片外——
            // 「字跑到按钮外面」是最难解释的一类观感问题。
            renderTarget_->DrawText(
                fit.text.c_str(), static_cast<UINT32>(fit.text.size()),
                labelFormatForSize(static_cast<float>(fit.fontStep) / 2.0F),
                textRect,
                highlighted ? accentTextBrush_.get() : textBrush_.get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
}

void RadialMenuWindow::paintHub() {
    const float scale = dpiScale();
    const float centerX = static_cast<float>(windowSizePx_) / 2.0F / scale;
    const float centerY = centerX;
    const D2D1_POINT_2F center = D2D1::Point2F(centerX, centerY);

    const D2D1_MATRIX_3X2_F base =
        D2D1::Matrix3x2F::Scale(animScale_, animScale_, center);
    renderTarget_->SetTransform(base);

    renderTarget_->FillEllipse(
        D2D1::Ellipse(center, kHubRadiusDip, kHubRadiusDip), hubBrush_.get());
    renderTarget_->DrawEllipse(
        D2D1::Ellipse(center, kHubRadiusDip, kHubRadiusDip),
        hubBorderBrush_.get(), 1.2F);

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
}

void RadialMenuWindow::render() {
    if (!renderTarget_) return;

    drawToDib();

    POINT srcPos = {0, 0};
    SIZE wndSize = {windowSizePx_, windowSizePx_};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    // 出现动画期间整体淡入：SourceConstantAlpha 与逐像素 alpha 相乘，
    // 不需要在绘制时给每个图元乘透明度。
    blend.SourceConstantAlpha =
        static_cast<BYTE>(animAlpha_ * 255.0F + 0.5F);
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd_, nullptr, nullptr, &wndSize, memDc_, &srcPos, 0,
                        &blend, ULW_ALPHA);
}

// 把菜单内容画进离屏 DIB。render() 在此基础上做 UpdateLayeredWindow；
// debugExportPng() 在此基础上直接存盘。这样两套输出共用同一条绘制路径，
// 调试截图就是屏幕上看到的内容。
void RadialMenuWindow::drawToDib() {
    renderTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));

    RECT rect = {0, 0, windowSizePx_, windowSizePx_};
    renderTarget_->BindDC(memDc_, &rect);

    renderTarget_->BeginDraw();
    renderTarget_->Clear(D2D1::ColorF(0, 0, 0, 0));

    for (std::uint8_t i = 0; i < menu_.sectorCount; ++i) {
        paintCell(i, highlightedValid_ && highlightedSector_ == i);
    }
    paintHub();

    const HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discardDeviceResources();
    }
}

bool RadialMenuWindow::debugExportPng(const wchar_t* fileName) {
    if (memDc_ == nullptr || dibBitmap_ == nullptr) return false;

    // 强制以最终态（无动画）画一帧，保证导出的是用户实际看到的样子。
    animScale_ = 1.0F;
    animAlpha_ = 1.0F;
    drawToDib();

    platform::win32::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) return false;

    platform::win32::ComPtr<IWICBitmap> wicBitmap;
    hr = factory->CreateBitmapFromHBITMAP(dibBitmap_, nullptr,
                                          WICBitmapUsePremultipliedAlpha,
                                          wicBitmap.put());
    if (FAILED(hr)) return false;

    platform::win32::ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put()))) return false;
    if (FAILED(stream->InitializeFromFilename(fileName, GENERIC_WRITE))) return false;

    platform::win32::ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                      encoder.put()))) return false;
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;

    platform::win32::ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(frame.put(), nullptr))) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    if (FAILED(frame->SetSize(static_cast<UINT>(windowSizePx_),
                              static_cast<UINT>(windowSizePx_)))) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    if (format != GUID_WICPixelFormat32bppPBGRA &&
        format != GUID_WICPixelFormat32bppBGRA) {
        return false;
    }

    if (FAILED(frame->WriteSource(wicBitmap.get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;
    return true;
}

// ================================================================ 窗口过程

LRESULT CALLBACK RadialMenuWindow::windowProcThunk(HWND hwnd, UINT message,
                                                   WPARAM wParam, LPARAM lParam) {
    RadialMenuWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<RadialMenuWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<RadialMenuWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self == nullptr) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->windowProc(message, wParam, lParam);
}

LRESULT RadialMenuWindow::windowProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_TIMER:
            if (wParam == kAnimTimerId) {
                tickAnimation();
                return 0;
            }
            break;
        case WM_DPICHANGED: {
            dpi_ = static_cast<std::uint32_t>(LOWORD(wParam));
            return 0;
        }
        case WM_DESTROY:
            if (closeHandler_) closeHandler_();
            return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace ui
}  // namespace ktile
