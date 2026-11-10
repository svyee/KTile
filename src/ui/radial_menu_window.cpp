// KTile - UI 层：轮盘菜单窗口实现
#include "ui/radial_menu_window.h"

#include "base/text_convert.h"
#include "core/builtin_items.h"
#include "core/lucide_icons.h"
#include "platform/win32/icon_loader.h"
#include "platform/win32/svg_path.h"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <cmath>

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

    // 外甩取消半径：低于花瓣外缘的值没有意义（会切掉花瓣自己），夹到下限。
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
    // 只有"外部资源类"动作有图标；Shortcut / Builtin 没有
    if (action.type == core::RadialActionType::Shortcut ||
        action.type == core::RadialActionType::Builtin) {
        return nullptr;
    }
    if (action.target.empty()) return nullptr;

    const std::string custom = action.icon.value_or(std::string());
    const std::string key = action.target + "|" + custom;
    auto it = iconCache_.find(key);
    if (it != iconCache_.end()) {
        return it->second.get();
    }

    platform::win32::UniqueHIcon icon = platform::win32::loadItemIcon(
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

        hr = dwriteFactory_->CreateTextFormat(
            L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            10.0F, L"zh-CN", labelFormat_.put());
        if (FAILED(hr)) return false;
        labelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        labelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
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
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(1.0F, 1.0F, 1.0F, 0.16F), emptyOutlineBrush_.put());
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
        glyphStrokeStyle_ =
            platform::win32::createRoundStrokeStyle(d2dFactory_.get());
    }

    return true;
}

void RadialMenuWindow::discardDeviceResources() {
    iconCache_.clear();
    glyphCache_.clear();
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
    glyphStrokeStyle_.reset();
    labelFormat_.reset();
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

    // 花瓣矩形（未旋转时位于正上方）
    const float left = centerX - kCellWidthDip * 0.5F;
    const float top = centerY - kCellCenterRadiusDip - kCellHeightDip * 0.5F;
    const D2D1_RECT_F rect = D2D1::RectF(
        left, top, left + kCellWidthDip, top + kCellHeightDip);
    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
        rect, kCellCornerDip, kCellCornerDip);

    // 缩放 + 旋转到对应角度。
    // 高亮只改颜色不放大：放大单元格会让鼠标相对扇区边界的几何关系
    // 跟着变化（本单元格外扩会盖住相邻扇区的判定角），扫过边界时容易
    // 出现「在两个扇区之间来回跳」的抖动。
    const float popScale = animScale_;
    const D2D1_MATRIX_3X2_F base =
        D2D1::Matrix3x2F::Scale(popScale, popScale, center);
    renderTarget_->SetTransform(
        base * D2D1::Matrix3x2F::Rotation(angle, center));

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

    // 图标与文字保持正向（不随花瓣旋转）
    renderTarget_->SetTransform(base);
    const D2D1_POINT_2F pos =
        D2D1::Point2F(centerX + pointOnCircle(angle, kCellCenterRadiusDip).x,
                      centerY + pointOnCircle(angle, kCellCenterRadiusDip).y);

    if (!empty && index < menu_.actions.size()) {
        const core::RadialAction& action = menu_.actions[index];
        ID2D1Bitmap* icon = iconForAction(action);
        const platform::win32::SvgGlyph* glyph =
            (icon == nullptr) ? glyphForAction(action) : nullptr;
        const bool hasIcon = icon != nullptr || glyph != nullptr;
        ID2D1SolidColorBrush* iconBrush =
            highlighted ? accentTextBrush_.get() : textBrush_.get();

        if (hasIcon) {
            const float half = kIconSizeDip * 0.5F;
            const float iconCenterY = pos.y - 6.0F;

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
                renderTarget_->SetTransform(base);
            }
        }

        if (!action.name.empty()) {
            const std::wstring wideName = base::utf8ToWideLossy(action.name);
            const float labelY = hasIcon ? pos.y + 4.0F : pos.y - 9.0F;
            const D2D1_RECT_F textRect = D2D1::RectF(
                pos.x - kCellWidthDip * 0.5F + 3.0F, labelY,
                pos.x + kCellWidthDip * 0.5F - 3.0F, labelY + 16.0F);
            renderTarget_->DrawText(
                wideName.c_str(), static_cast<UINT32>(wideName.size()),
                labelFormat_.get(), textRect,
                highlighted ? accentTextBrush_.get() : textBrush_.get());
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
