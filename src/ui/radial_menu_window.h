// KTile - UI 层：轮盘菜单窗口
//
// 浅色圆角卡片围成一圈环，中心留空；悬停填充强调色，呼出时 160ms 缩放
// 淡入（SetTimer 驱动）。透明用 32bpp DIB + DCRenderTarget 再
// UpdateLayeredWindow（Win7 下 D2D 分层窗口拿到干净抗锯齿边的标准做法）。
// WS_EX_NOACTIVATE 不抢焦点，Shortcut 注入的按键才能被前台应用接收。
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
    // cancelRadiusDip 是外甩取消半径（DIP），松手时距中心超过它不算命中
    // 任何扇区；传 0 用默认值（kWindowRadiusDip）。用 DIP 不用物理像素：
    // 150% 缩放下 160 物理像素会落进花瓣内部，怎么拖都取消。
    void show(const core::RadialMenu& menu, POINT centerScreen,
              int cancelRadiusDip = 0);

    // 更新光标位置（屏幕坐标），用于高亮扇区
    void updateCursor(POINT screenPoint);

    // 按键释放：判定命中扇区并触发动作，然后隐藏。
    //
    // 没有返回值是有意的：只要轮盘亮过，抬起一律算轮盘的结果（松手即
    // 取消），不补发原生右键，否则后台窗口会凭空弹出上下文菜单。没越过
    // 激活阈值的那次压根没唤出轮盘，由钩子层自己补发。
    void releaseAt(POINT screenPoint);

    void hide();
    bool isVisible() const noexcept;

    void setActionHandler(ActionHandler handler) { actionHandler_ = std::move(handler); }
    void setCloseHandler(CloseHandler handler) { closeHandler_ = std::move(handler); }

    HWND hwnd() const noexcept { return hwnd_; }

    // 把当前菜单渲染到 DIB 并导出 PNG（供 --debug-radial 视觉验证）。
    bool debugExportPng(const wchar_t* fileName);

    // 调试用：把跑马灯进度拨到指定毫秒（--debug-radial-scroll），否则截图
    // 相位不定、无法自动比对。只在 debugExportPng 之前调用有意义。
    void setMarqueePhaseForDebug(float ms) { marqueeMs_ = ms; }

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
    // 优先级高于位图和内置矢量符号：插件作者放图标就是要它生效，
    // 能解析就不能悄悄换成通用符号。
    const platform::win32::SvgGlyph* customGlyphForAction(
        const core::RadialAction& action);

    // 取扇区动作的矢量图标（快捷键 / 内置命令），没有返回 nullptr
    const platform::win32::SvgGlyph* glyphForAction(const core::RadialAction& action);

    // 一行标签在某个扇区里的排版结果
    struct LabelLayout {
        float windowWidth = 0.0F;   // 这个角度下最多能占多宽（DIP）
        float naturalWidth = 0.0F;  // 按 fontStep 排出来实际多宽
        int fontStep = 20;          // 字号（以 0.5 DIP 为单位的整数）
        float overflowDip() const noexcept {
            const float over = naturalWidth - windowWidth;
            return (over > 0.0F) ? over : 0.0F;
        }
    };

    // 算标签排版（带缓存）：放得下就 10 DIP 原样排，差一点缩到 9 DIP
    // （看不出来），还放不下保持 9 DIP 交给横向滚动。不截断加省略号：
    // 「复制文件路…」和「复制文件路径」用户分不出来。
    LabelLayout layoutLabel(const std::string& cacheKey, const std::wstring& text,
                            float windowWidth);

    // 量一段文字的自然宽度（DIP）
    float measureLabelWidth(const std::wstring& text, float fontDip);

    // 某个扇区的标签窗口宽度（带缓存，见 core::radialLabelWindowWidth）
    float labelWindowWidthFor(std::uint8_t index, bool hasIcon);

    // 当前时刻标签该左移多少 DIP（两端口等待 + 往返滑动）
    float marqueeOffsetDip(float overflowDip) const;

    void tickMarquee();
    // 按「这一帧有没有超宽标签」决定跑马灯定时器开/关
    void syncMarqueeTimer();

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
    // 专供 SVG 彩色图层用：textBrush_/accentTextBrush_ 与文字共用，
    // 改颜色会把文字一起染掉。
    platform::win32::ComPtr<ID2D1SolidColorBrush> glyphAccentBrush_;
    platform::win32::ComPtr<ID2D1StrokeStyle> glyphStrokeStyle_;

    std::unordered_map<std::string, platform::win32::ComPtr<ID2D1Bitmap>> iconCache_;
    std::unordered_map<std::wstring, platform::win32::SvgGlyph> glyphCache_;
    // 自定义 SVG 图标缓存（键含文件修改时间，插件改了图标下一帧生效）
    std::unordered_map<std::wstring, platform::win32::SvgGlyph> customGlyphCache_;

    // 标签排版缓存：键 = 扇区下标 + 标签原文 + 是否有图标。
    // 键带下标是因为窗口宽度随角度变，同一个标签在斜下方是另一份排版。
    std::unordered_map<std::string, LabelLayout> labelLayoutCache_;
    // 字号 → IDWriteTextFormat。字号量化成 0.5 DIP 步进，实际只有两三种。
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

    // 跑马灯：只在有超宽标签时开定时器，否则一个定时器都不开，
    // 保持「无输入时全静默」。
    static constexpr UINT_PTR kMarqueeTimerId = 0x52;
    static constexpr UINT kMarqueeTickMs = 30;
    float marqueeMs_ = 0.0F;        // 已经滚了多久
    ULONGLONG marqueeStartTick_ = 0;
    bool marqueeRunning_ = false;
    bool frameHasScroll_ = false;   // 当前这一帧是否有标签需要滚动

    ActionHandler actionHandler_;
    CloseHandler closeHandler_;

    static constexpr const wchar_t* kWindowClassName = L"KTileRadialMenu";

    // 布局（DIP）：8 个 50x48 的圆角卡片挂在半径 97 的圆上，各自沿径向旋转，
    // 相邻卡片对角余量约 5 DIP。高度取 48 是给文字让路：斜位卡片一行水平
    // 文字的宽度由卡片高度和文字带位置决定，40 高、文字带偏下时只剩 24 DIP，
    // 三个字都放不下。
    static constexpr float kHubRadiusDip = 21.0F;
    static constexpr float kCellCenterRadiusDip = 97.0F;
    static constexpr float kCellWidthDip = 50.0F;
    static constexpr float kCellHeightDip = 48.0F;
    static constexpr float kCellCornerDip = 14.0F;
    static constexpr float kIconSizeDip = 17.0F;
    static constexpr float kGlyphStrokeDip = 1.5F;

    // 标签文字（DIP）：卡片内容是一列，图标在上、标签在下，尽量靠卡片中间。
    // 文字带每下移 1 DIP，斜位卡片的宽度就少 2 DIP（见
    // core::radialLabelWindowWidth），再往下就装不下四个汉字。
    static constexpr float kLabelFontDip = 10.0F;
    // 缩字号的底线：只在「差一点点」时用（10 → 8.5 看不出来），再小宁愿滚动
    static constexpr float kLabelMinFontDip = 8.5F;
    static constexpr float kLabelHeightDip = 14.0F;
    // 图标中心 / 标签带中心相对卡片中心的垂直偏移
    static constexpr float kIconCenterOffsetDip = -8.5F;
    static constexpr float kLabelCenterOffsetDip = 8.0F;

    // 跑马灯（DIP/毫秒）：两端各停 700ms，字一上来就滑走等于没写成；
    // 26 DIP/s 约每秒 2.5 个汉字，跟得上读。
    static constexpr float kMarqueeHoldMs = 700.0F;
    static constexpr float kMarqueeSpeedDipPerSec = 26.0F;

    // 调用方/命中测试共用的窗口半径（DIP）：覆盖花瓣外缘 + 阴影
    static constexpr float kWindowRadiusDip = 142.0F;
    // 命中测试用的内/外半径（与花瓣布局对应）
    static constexpr float kInnerRadiusDip = 47.0F;
    static constexpr float kOuterRadiusDip = 153.0F;

    // 实际生效的外甩取消半径（DIP）。下界 core::kMinCancelRadiusDip 保证
    // 不小于花瓣环外缘。
    float cancelRadiusDip_ = kOuterRadiusDip;
};

}  // namespace ui
}  // namespace ktile
