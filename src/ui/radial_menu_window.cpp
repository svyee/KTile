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
    // 新手势：上一次「碰过扇区」的记录作废，否则会一直算成「真手势」
    gestureHadHighlight_ = false;

    // 换了一份菜单，标签的排版结果（窗口宽度随角度、字号随文字）全部作废
    labelLayoutCache_.clear();
    marqueeMs_ = 0.0F;
    frameHasScroll_ = false;
    // 跑马灯从零开始：用户看到的第一帧必须是标签开头，不能是滚到一半的中段
    if (marqueeRunning_ && hwnd_ != nullptr) {
        KillTimer(hwnd_, kMarqueeTimerId);
    }
    marqueeRunning_ = false;

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
        if (next.has_value()) {
            highlightedSector_ = *next;
            // 有过一帧高亮 = 用户真的进到某个扇区里了。releaseAt 靠它把
            // 「真手势」和「手抖的单击」分开。
            gestureHadHighlight_ = true;
        }
        render();
    }
}

bool RadialMenuWindow::releaseAt(POINT screenPoint) {
    if (!isVisible()) return false;
    updateCursor(screenPoint);
    const bool executed = highlightedValid_ && actionHandler_ != nullptr;
    if (executed) {
        actionHandler_(highlightedSector_);
    }
    // 判「这压根不是手势」：全程一次扇区都没高亮过，松手又落在中心空档。
    // 阈值最小只有 4 像素，一次手抖的单击很容易跨过去，然后原生右键菜单被
    // 吞掉、动作也没执行 —— 用户看到的是「右键按了没反应」。这种情况把点击
    // 还回去（由调用方补发），宁可多一次上下文菜单，也不要丢一次按键。
    float dx = 0.0F;
    float dy = 0.0F;
    screenToDipOffset(screenPoint, dx, dy);
    const bool clickLike = core::releaseLooksLikeClick(
        executed, gestureHadHighlight_, std::sqrt(dx * dx + dy * dy),
        kInnerRadiusDip);
    hide();
    return clickLike;
}

void RadialMenuWindow::hide() {
    if (hwnd_ != nullptr) {
        KillTimer(hwnd_, kAnimTimerId);
        KillTimer(hwnd_, kMarqueeTimerId);
        marqueeRunning_ = false;
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

RadialMenuWindow::LabelLayout RadialMenuWindow::layoutLabel(
    const std::string& cacheKey, const std::wstring& text,
    float windowWidth) {
    const auto cached = labelLayoutCache_.find(cacheKey);
    if (cached != labelLayoutCache_.end()) {
        return cached->second;
    }

    const int baseStep = static_cast<int>(std::lround(kLabelFontDip * 2.0F));
    const int minStep = static_cast<int>(std::lround(kLabelMinFontDip * 2.0F));
    LabelLayout layout;
    layout.windowWidth = windowWidth;
    layout.fontStep = baseStep;
    layout.naturalWidth = measureLabelWidth(text, kLabelFontDip);

    // 差一点点：逐个字号往下试，缩到放得下为止。
    //
    // 只试到 kLabelMinFontDip（8.5 DIP）：这个幅度（10 → 8.5）在轮盘上几乎
    // 看不出来，却能把「10 DIP 差半个字」这类最常见的情况整整齐齐收进卡片，
    // 不必让每一格都滚动。
    //
    // 为什么不按比例一次算好字号：算出来的字号落地时会被量化到 0.5 DIP，
    // 差一点点就可能仍然超出（47.5 > 47），于是「算过了却还要滚」，白缩一圈。
    if (windowWidth > 0.0F && layout.naturalWidth > windowWidth) {
        for (int step = baseStep - 1; step >= minStep; --step) {
            const float fontDip = static_cast<float>(step) / 2.0F;
            const float measured = measureLabelWidth(text, fontDip);
            if (measured <= windowWidth) {
                layout.fontStep = step;
                layout.naturalWidth = measured;
                break;
            }
        }
        // 缩到下限也放不下：那就用最小字号去滚，而不是拿 10 DIP 去滚 ——
        // 字号小的那份要滚的距离更短，看到的字更多。
        if (layout.fontStep == baseStep) {
            const float fontDip = static_cast<float>(minStep) / 2.0F;
            const float measured = measureLabelWidth(text, fontDip);
            if (measured > 0.0F) {
                layout.fontStep = minStep;
                layout.naturalWidth = measured;
            }
        }
    }

    labelLayoutCache_.emplace(cacheKey, layout);
    return layout;
}

float RadialMenuWindow::labelWindowWidthFor(std::uint8_t index, bool hasIcon) {
    // 窗口宽度由角度决定：花瓣斜过来之后，短边挡在文字带前面。正上方最宽
    // （47 DIP），斜下方 135°/315° 最窄（33 DIP）。没有图标时文字带回到卡片
    // 中间，能宽出来不少。
    const float sectorWidth = 360.0F / static_cast<float>(menu_.sectorCount);
    const float angle = static_cast<float>(index) * sectorWidth;
    const float bandCenter =
        hasIcon ? kLabelCenterOffsetDip : 0.0F;
    return core::radialLabelWindowWidth(
        kCellWidthDip, kCellHeightDip, kCellCornerDip, angle, bandCenter,
        kLabelHeightDip);
}

float RadialMenuWindow::marqueeOffsetDip(float overflowDip) const {
    if (!(overflowDip > 0.0F)) return 0.0F;

    const float moveMs = overflowDip / kMarqueeSpeedDipPerSec * 1000.0F;
    const float cycle = 2.0F * (kMarqueeHoldMs + moveMs);
    float t = std::fmod(marqueeMs_, cycle);
    if (t < 0.0F) t += cycle;

    // 往返：停 → 左移到尾 → 停 → 回开头。两端都留停顿，第一段尤其重要：
    // 轮盘是「看一眼就选」的界面，字一上来就滑走等于没写成。
    if (t < kMarqueeHoldMs) return 0.0F;
    t -= kMarqueeHoldMs;
    if (t < moveMs) {
        const float k = t / moveMs;
        return overflowDip * (k * k * (3.0F - 2.0F * k));   // smoothstep
    }
    t -= moveMs;
    if (t < kMarqueeHoldMs) return overflowDip;
    t -= kMarqueeHoldMs;
    const float k = t / moveMs;
    return overflowDip * (1.0F - k * k * (3.0F - 2.0F * k));
}

void RadialMenuWindow::tickMarquee() {
    marqueeMs_ = static_cast<float>(GetTickCount64() - marqueeStartTick_);
    render();
}

void RadialMenuWindow::syncMarqueeTimer() {
    if (hwnd_ == nullptr) return;
    if (frameHasScroll_ && !marqueeRunning_) {
        marqueeRunning_ = true;
        // 从 0 起算（show() 里也清过）：停留期从第一帧开始，用户看到的一定
        // 是标签开头，而不是滚到一半的中段。
        marqueeStartTick_ = GetTickCount64();
        SetTimer(hwnd_, kMarqueeTimerId, kMarqueeTickMs, nullptr);
    } else if (!frameHasScroll_ && marqueeRunning_) {
        marqueeRunning_ = false;
        KillTimer(hwnd_, kMarqueeTimerId);
    }
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
    labelLayoutCache_.clear();
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

    // 卡片：挂在半径 kCellCenterRadiusDip 的圆上，并像花瓣一样**沿径向旋转**
    // —— 正上方那张正着，右侧的朝右倒，下方那张整个倒过来。这个倾斜是外观的
    // 主体，去掉之后就成了普通的一圈方块。
    //
    // 旋转的代价是卡片里的水平文字能占的宽度随角度变化（见 labelWindowWidthFor）：
    // 50x46 的卡片转到 45° 时只剩 38 DIP，转到 135° 只剩 33 DIP。所以文字宽度
    // 必须按角度算、放不下就滚动，不能像以前那样按卡片宽排 —— 那正是「字顶出
    // 按钮」的根源。
    const D2D1_POINT_2F pos =
        D2D1::Point2F(centerX + pointOnCircle(angle, kCellCenterRadiusDip).x,
                      centerY + pointOnCircle(angle, kCellCenterRadiusDip).y);

    // 卡片矩形先在「正上方」摆好，再整体绕菜单中心转到自己的角度 ——
    // 不能直接摆在 pos 上再转，那会转两次（pos 已经是转过的位置）。
    const float cellCenterY = centerY - kCellCenterRadiusDip;
    const D2D1_RECT_F rect = D2D1::RectF(
        centerX - kCellWidthDip * 0.5F, cellCenterY - kCellHeightDip * 0.5F,
        centerX + kCellWidthDip * 0.5F, cellCenterY + kCellHeightDip * 0.5F);
    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
        rect, kCellCornerDip, kCellCornerDip);

    // 高亮只改颜色不放大：放大单元格会让鼠标相对扇区边界的几何关系
    // 跟着变化（本单元格外扩会盖住相邻扇区的判定角），扫过边界时容易
    // 出现「在两个扇区之间来回跳」的抖动。
    const float popScale = animScale_;
    const D2D1_MATRIX_3X2_F base =
        D2D1::Matrix3x2F::Scale(popScale, popScale, center);
    const D2D1_MATRIX_3X2_F rotated =
        base * D2D1::Matrix3x2F::Rotation(angle, center);

    // 伪阴影（D2D 1.0 没有模糊效果，用偏移的半透明圆角矩形近似）。
    // 偏移写在旋转**之后**（矩阵右乘 Translation）：光是同一个方向来的，
    // 阴影就该永远朝屏幕下方，不能跟着花瓣各倒各的。
    const D2D1_ROUNDED_RECT shadow = D2D1::RoundedRect(rect, kCellCornerDip,
                                                        kCellCornerDip);
    if (!empty) {
        renderTarget_->SetTransform(rotated *
                                    D2D1::Matrix3x2F::Translation(0.0F, 2.0F));
        renderTarget_->FillRoundedRectangle(shadow, shadowBrush_.get());
    }
    renderTarget_->SetTransform(rotated);

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

    // 图标和文字保持正向（不随花瓣旋转）：旋转过的中英文认起来要费脑，
    // 而轮盘是「一眼扫过去」的界面。倾斜感由卡片提供就够了。
    renderTarget_->SetTransform(base);

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
            // 图标中心在卡片中心上方 9 DIP：19 DIP 的图标占 -18.5~+0.5，
            // 标签带占 +2~+16，两者留 1.5 DIP 的间隙，整组在 46 高的卡片里
            // 上下各余 6.5 / 7 DIP，看起来是居中的。
            const float iconCenterY = pos.y + kIconCenterOffsetDip;

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
            // 标签带的宽度不是卡片宽度：卡片斜着，能容下多宽由角度决定。
            // 没有图标时文字带回到卡片中间，能宽出十来 DIP。
            const float bandCenter =
                hasIcon ? kLabelCenterOffsetDip : 0.0F;
            const float windowWidth = labelWindowWidthFor(index, hasIcon);

            if (windowWidth > 1.0F) {
                // 缓存键带扇区下标：同一个「复制路径」在正上方能放下、
                // 在斜下方要滚动，窗口宽度不同就是两份排版。
                std::string cacheKey = std::to_string(index);
                cacheKey += hasIcon ? "|1|" : "|0|";
                cacheKey += action.name;

                const LabelLayout layout =
                    layoutLabel(cacheKey, wideName, windowWidth);
                const float overflow = layout.overflowDip();

                float offset = 0.0F;
                if (overflow > 0.5F) {
                    // 记下来交给 render()：只要有任何一格在滚，就把跑马灯
                    // 定时器开起来；全都不滚就一个定时器都不留。
                    frameHasScroll_ = true;
                    offset = marqueeOffsetDip(overflow);
                }

                const float winLeft = pos.x - windowWidth * 0.5F;
                const float winTop =
                    pos.y + bandCenter - kLabelHeightDip * 0.5F;
                const D2D1_RECT_F window = D2D1::RectF(
                    winLeft, winTop, winLeft + windowWidth,
                    winTop + kLabelHeightDip);

                // 硬裁剪。这个窗口是「按卡片形状算出来的、整条都在卡片里」的
                // 区域，所以裁到它之外的字一定在卡片外 —— 无论字号、滚动相位、
                // 中文标点怎么折腾，都不可能再出现「字跑到按钮外面」。
                //
                // 先把变换复位再推裁剪框：不依赖「PushAxisAlignedClip 会不会
                // 套用当前变换」这个各版本说法不一的细节。坐标仍是 DIP ——
                // 渲染目标已经 SetDpi，DIP→像素由它负责，这里不要乘缩放。
                renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
                renderTarget_->PushAxisAlignedClip(
                    window, D2D1_ANTIALIAS_MODE_ALIASED);
                renderTarget_->SetTransform(base);

                // 排版框至少和窗口一样宽，文字才会在窗口里居中；
                // 超宽时框就是文字的自然宽度，从窗口左沿开始往外排。
                const float drawWidth = (layout.naturalWidth > windowWidth)
                                            ? layout.naturalWidth
                                            : windowWidth;
                const D2D1_RECT_F textRect = D2D1::RectF(
                    window.left - offset, window.top,
                    window.left - offset + drawWidth, window.bottom);
                renderTarget_->DrawText(
                    wideName.c_str(), static_cast<UINT32>(wideName.size()),
                    labelFormatForSize(
                        static_cast<float>(layout.fontStep) / 2.0F),
                    textRect,
                    highlighted ? accentTextBrush_.get() : textBrush_.get());

                renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
                renderTarget_->PopAxisAlignedClip();
            }
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
    // 绘制过程会顺手告诉我们「这一帧有没有超宽标签」，据此决定跑马灯
    // 定时器要不要继续跑 —— 放在 drawToDib 之后，就不必在 show() 里
    // 额外量一遍文字（那还得把图标先解析一次才能知道文字带在哪）。
    syncMarqueeTimer();

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

    // 每帧重置：这一帧画完就知道「有没有标签需要滚动」，
    // render() 据此决定跑马灯定时器开不开。
    frameHasScroll_ = false;

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
            if (wParam == kMarqueeTimerId) {
                tickMarquee();
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
