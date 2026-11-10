// KTile - UI 层：轮盘菜单窗口
//
// 视觉：浅色圆角卡片围成一圈环（不是实心圆盘），中心留一个空圆。
// 每张卡片里有图标 + 一行标签，悬停时填充强调色并描边。
//
// 动画：呼出时整体从 0.7 倍缩放淡入到 1.0（约 160ms，ease-out），
// 用 SetTimer 驱动，不引入动画库。
//
// 透明方案：32bpp DIB + DCRenderTarget（带 alpha），再 UpdateLayeredWindow。
// 这是 Win7 下 D2D 分层窗口获得干净抗锯齿边缘的标准做法。
//
// 不抢焦点：WS_EX_NOACTIVATE，前台应用保持焦点，Shortcut 动作注入的
// 按键才能被正确窗口接收。
//
#pragma once

#include "core/radial_menu.h"
#include "platform/win32/svg_path.h"
#include "platform/win32/unique_handles.h"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace ktile {
namespace ui {

class RadialMenuWindow {
public:
    // 扇区动作被触发时的回调（参数是扇区索引）
    using ActionHandler = std::function<void(std::uint8_t sectorIndex)>;
    using CloseHandler = std::function<void()>;

    RadialMenuWindow();
    ~RadialMenuWindow();

    RadialMenuWindow(const RadialMenuWindow&) = delete;
    RadialMenuWindow& operator=(const RadialMenuWindow&) = delete;

    bool create(HINSTANCE instance);

    // 显示轮盘，centerScreen 为轮盘中心的屏幕坐标。
    //
    // cancelRadiusDip 是外甩取消半径（DIP）：松手时距中心超过这个半径
    // 就不算命中任何扇区。传 0 表示用默认值（卡片环外缘 kWindowRadiusDip）。
    //
    // 这里用 DIP 而不是物理像素：花瓣布局本身就是 DIP，用物理像素会让
    // 150% 缩放的机器上「取消半径 160」落进花瓣内部，导致怎么拖都取消。
    void show(const core::RadialMenu& menu, POINT centerScreen,
              int cancelRadiusDip = 0);

    // 更新光标位置（屏幕坐标），用于高亮扇区
    void updateCursor(POINT screenPoint);

    // 按键释放：判定命中扇区并触发动作，然后隐藏
    void releaseAt(POINT screenPoint);

    void hide();
    bool isVisible() const noexcept;

    void setActionHandler(ActionHandler handler) { actionHandler_ = std::move(handler); }
    void setCloseHandler(CloseHandler handler) { closeHandler_ = std::move(handler); }

    HWND hwnd() const noexcept { return hwnd_; }

    // 把当前菜单渲染到 DIB 并导出 PNG（供 --debug-radial 视觉验证）。
    bool debugExportPng(const wchar_t* fileName);

private:
    static LRESULT CALLBACK windowProcThunk(HWND hwnd, UINT message,
                                            WPARAM wParam, LPARAM lParam);
    LRESULT windowProc(UINT message, WPARAM wParam, LPARAM lParam);

    bool createDeviceResources();
    void discardDeviceResources();
    void render();
    void drawToDib();
    void paintCell(std::uint8_t index, bool highlighted);
    void paintHub();

    // 动画
    void startAppearAnimation();
    void tickAnimation();

    // 取扇区动作的图标（带缓存），没有图标返回 nullptr
    ID2D1Bitmap* iconForAction(const core::RadialAction& action);

    // 取 action.icon 指定的 SVG 图标（带缓存，按文件修改时间失效）。
    //
    // 优先级高于位图和内置矢量符号：插件作者把图标放在自己目录里，
    // 就是希望「这个按钮长这样」，能解析就必须用它，不能悄悄换成通用符号。
    const platform::win32::SvgGlyph* customGlyphForAction(
        const core::RadialAction& action);

    // 取扇区动作的矢量图标（快捷键 / 内置命令），没有返回 nullptr
    const platform::win32::SvgGlyph* glyphForAction(const core::RadialAction& action);

    // 标签排版结果：可能被截断的文字 + 字号（以 0.5 DIP 为单位的整数）
    struct LabelFit {
        std::wstring text;
        int fontStep = 20;
    };

    // 把一段标签塞进给定宽度里（带缓存）。
    //
    // 卡片宽度固定，标签长度却由用户 / 插件作者决定：5 个汉字正好 50 DIP，
    // 6 个就超了。于是分三步走：放得下就原样用 → 放不下按比例缩字号（下限
    // kLabelMinFontDip）→ 缩到下限还放不下就截断加省略号。
    //
    // 为什么不截断成一行省略号了事：轮盘一格本来就只放两三个字，能完整显示
    // 就别截；为什么缩到下限就不再缩：8 DIP 在 100% DPI 下只有 11 像素高，
    // 再小等于把字变成灰点，还不如让用户改个短名。
    LabelFit fitLabel(const std::string& cacheKey, const std::wstring& text,
                      float availableWidth);

    // 用量出来的自然宽度（DIP）
    float measureLabelWidth(const std::wstring& text, float fontDip);

    // 创建（或取缓存）指定字号的标签格式
    IDWriteTextFormat* labelFormatForSize(float size);

    float dpiScale() const noexcept;
    void screenToDipOffset(POINT screenPoint, float& dx, float& dy) const;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;

    POINT centerScreen_{};
    int windowSizePx_ = 0;

    core::RadialMenu menu_;
    std::uint8_t highlightedSector_ = 0;
    bool highlightedValid_ = false;

    // D2D / DWrite / WIC
    platform::win32::ComPtr<ID2D1Factory> d2dFactory_;
    platform::win32::ComPtr<ID2D1DCRenderTarget> renderTarget_;
    platform::win32::ComPtr<IDWriteFactory> dwriteFactory_;
    platform::win32::ComPtr<IWICImagingFactory> wicFactory_;

    platform::win32::ComPtr<ID2D1SolidColorBrush> cellBrush_;
    platform::win32::ComPtr<ID2D1SolidColorBrush> emptyCellBrush_;
    platform::win32::ComPtr<ID2D1SolidColorBrush> emptyOutlineBrush_;
    platform::win32::ComPtr<ID2D1SolidColorBrush> highlightBrush_;
    platform::win32::ComPtr<ID2D1SolidColorBrush> borderBrush_;
    platform::win32::ComPtr<ID2D1SolidColorBrush> shadowBrush_;
    platform::win32::ComPtr<ID2D1SolidColorBrush> textBrush_;
    platform::win32::ComPtr<ID2D1SolidColorBrush> accentTextBrush_;
    platform::win32::ComPtr<ID2D1SolidColorBrush> hubBrush_;
    platform::win32::ComPtr<ID2D1SolidColorBrush> hubBorderBrush_;
    // 专供 SVG 彩色图层用的画笔：不能借用 textBrush_/accentTextBrush_，
    // 那两个是文字和图标共用的，改颜色会把下方的文字一起染掉。
    platform::win32::ComPtr<ID2D1SolidColorBrush> glyphAccentBrush_;
    platform::win32::ComPtr<ID2D1StrokeStyle> glyphStrokeStyle_;

    std::unordered_map<std::string, platform::win32::ComPtr<ID2D1Bitmap>> iconCache_;
    std::unordered_map<std::wstring, platform::win32::SvgGlyph> glyphCache_;
    // 自定义 SVG 图标缓存（键含文件修改时间，插件改了图标下一帧生效）
    std::unordered_map<std::wstring, platform::win32::SvgGlyph> customGlyphCache_;

    // 标签排版缓存：标签原文 → 排版结果（截断后的文字 + 字号）。
    // 菜单内容在一次显示期间不会变，缓存到窗口生命周期即可；
    // 万一以后卡片宽度变成可配置，这个键要跟着带上宽度。
    std::unordered_map<std::string, LabelFit> labelFitCache_;
    // 字号 → IDWriteTextFormat。字号已量化成 0.5 DIP 步进，实际同时存在的
    // 也就两三种。
    std::unordered_map<int, platform::win32::ComPtr<IDWriteTextFormat>> labelFormats_;

    // 离屏 DIB
    HDC memDc_ = nullptr;
    HBITMAP dibBitmap_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;

    std::uint32_t dpi_ = 96;

    // 出现动画
    static constexpr UINT_PTR kAnimTimerId = 0x51;
    static constexpr ULONGLONG kAnimDurationMs = 160;
    static constexpr float kAnimStartScale = 0.70F;
    static constexpr float kAnimStartAlpha = 0.35F;
    float animScale_ = 1.0F;
    float animAlpha_ = 1.0F;
    bool animating_ = false;
    ULONGLONG animStartTick_ = 0;

    ActionHandler actionHandler_;
    CloseHandler closeHandler_;

    static constexpr const wchar_t* kWindowClassName = L"KTileRadialMenu";

    // 布局（DIP）
    //
    // 这些数值是「花瓣环」的基准：8 个 62x40 的圆角卡片挂在半径 97 的
    // 圆上，相邻卡片中心弧长约 74 DIP。宽度不能再窄：卡片里要放得下
    // 5 个汉字（10 DIP 字号 = 50 DIP），50 DIP 宽的卡片装不下，字会顶出
    // 圆角；「新建文件夹」这种 5 字标签在真实配置里很常见。
    //
    // 62 宽在 8 扇区下也不会互相压住：相邻卡片永远相差 45°，横向
    // 68.6 DIP（> 62）或纵向 68.6 DIP（> 40），总有一个方向是错开的。
    static constexpr float kHubRadiusDip = 21.0F;
    static constexpr float kCellCenterRadiusDip = 97.0F;
    static constexpr float kCellWidthDip = 62.0F;
    static constexpr float kCellHeightDip = 40.0F;
    static constexpr float kCellCornerDip = 12.0F;
    static constexpr float kIconSizeDip = 19.0F;
    static constexpr float kGlyphStrokeDip = 1.6F;

    // 标签文字（DIP）
    //
    // 基准 10 DIP；标签比可用宽度还长时按比例缩，缩到 8 DIP 为止——再小在
    // 100% DPI 下只剩 11 像素高，宁可让用户把标签改短。
    // 文字框左右各留 4 DIP、高 14 DIP：上边贴着图标、下边距离卡片下沿 6 DIP。
    // 之前文字框底边正好压在卡片下沿上，12 DIP 的圆角会把最后几个字啃掉一块，
    // 看起来像「字跑到按钮外面去了」。
    static constexpr float kLabelFontDip = 10.0F;
    static constexpr float kLabelMinFontDip = 8.0F;
    static constexpr float kLabelSidePaddingDip = 4.0F;
    static constexpr float kLabelHeightDip = 14.0F;
    // 文字框相对卡片中心的垂直偏移（有图标时）
    static constexpr float kLabelTopOffsetDip = 3.0F;
    // 调用方/命中测试共用的窗口半径（DIP）：覆盖花瓣外缘 + 阴影
    static constexpr float kWindowRadiusDip = 142.0F;
    // 命中测试用的内/外半径（与花瓣布局对应）
    static constexpr float kInnerRadiusDip = 47.0F;
    static constexpr float kOuterRadiusDip = 153.0F;

    // 实际生效的外甩取消半径（DIP）。下界用 core::kMinCancelRadiusDip，
    // 保证不会小于花瓣环外缘；见 show() 的注释。
    float cancelRadiusDip_ = kOuterRadiusDip;
};

}  // namespace ui
}  // namespace ktile
