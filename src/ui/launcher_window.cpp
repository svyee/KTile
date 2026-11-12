// KTile - UI 层：启动器主窗口实现
#include "ui/launcher_window.h"

#include "base/text_convert.h"
#include "core/lucide_icons.h"
#include "core/plugin_layout.h"
#include "platform/win32/icon_loader.h"
#include "platform/win32/icon_store.h"
#include "platform/win32/popup_menu.h"
#include "platform/win32/svg_path.h"
#include "infrastructure/i18n.h"
#include "platform/win32/tray_icon.h"  // kApplicationIconResourceId（应用图标）

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <commctrl.h>  // SetWindowSubclass（搜索框 Edit 子类化）

#include <wincodec.h>  // WIC：HICON -> 位图

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")

namespace ktile {
namespace ui {
namespace {

// 窗口类名
constexpr const wchar_t* kWindowClassName = L"KTile.LauncherWindow";

// 空白区右键菜单的命令 ID（避开磁贴菜单的 3001..3020 段）
constexpr UINT kBlankMenuSettingsId = 3101;
constexpr UINT kBlankMenuAddItemId = 3102;
constexpr UINT kBlankMenuNewTabId = 3103;
constexpr UINT kBlankMenuAboutId = 3104;
// 「添加插件」子菜单命令 ID 段：命令 ID = 3201 + 插件下标。
constexpr UINT kBlankMenuPluginBaseId = 3201;

// 默认配色已迁移到 ui/theme.h 的 ThemeColors（皮肤系统）：磁贴底色为
// 中性深灰玻璃，对浅色/彩色图标都有对比度，hover/pressed 在此基础上提亮。
// 标签底衬（scrim）颜色由主题派生（ThemeColors::labelScrim）。

// 从 ARGB 整数构造 D2D 颜色
D2D1_COLOR_F colorFromArgb(const std::uint32_t argb) {
    D2D1_COLOR_F color;
    color.a = static_cast<float>((argb >> 24) & 0xFFU) / 255.0F;
    color.r = static_cast<float>((argb >> 16) & 0xFFU) / 255.0F;
    color.g = static_cast<float>((argb >> 8) & 0xFFU) / 255.0F;
    color.b = static_cast<float>(argb & 0xFFU) / 255.0F;
    return color;
}

// 搜索框 Edit 控件（GDI）的颜色：底 = 主题背景按比例混入文字色，
// 与 D2D 侧的半透明底衬视觉一致（GDI 画刷不支持 alpha，只能预合成）。
COLORREF searchEditBkColorRef(const ThemeColors& theme, const bool focused) {
    const std::uint32_t c = ThemeColors::blendOver(
        theme.windowBackground, theme.text, focused ? 0.20F : 0.10F);
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

COLORREF searchEditTextColorRef(const ThemeColors& theme) {
    const std::uint32_t c = ThemeColors::blendOver(
        theme.windowBackground, theme.text, 0.91F);
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// 兜底首字的基准字号（绘制时按磁贴尺寸做矩阵缩放）
constexpr float kFallbackBaseFontSize = 24.0F;

// 拖拽判定阈值（DIP）：移动超过该距离才升级为拖拽，否则手抖一两像素
// 就会把点击启动变成排序。
constexpr float kDragThresholdDip = 6.0F;

// 标签跑马灯：33ms 一帧（≈30fps）、每帧 0.8 DIP（约 24 DIP/秒）、
// 滚到两端各停 18 帧（约 600ms）让用户看清头尾。
constexpr UINT kMarqueeIntervalMs = 33;
constexpr float kMarqueeStepDip = 0.8F;
constexpr int kMarqueePauseTicks = 18;

// 落点防抖（毫秒）：光标停在两格交界时命中结果会反复切换，要求落点
// 稳定一小段时间才确认，否则挤开动画来回抖动。
constexpr std::uint64_t kDropDebounceMs = 80;

// 落点占位框画在「被拖项将占据的格子」上而非光标下的磁贴上（Win10 做法）：
// 用户看到的虚线框就是松手后的真实位置。挪动已有磁贴用中性白（text 派生），
// 拖入新项目用强调色（accent 派生），几何与虚线节奏相同，
// 颜色由 ThemeColors 派生（见 paintDropSpotFrame）。
constexpr float kDropTargetDashSpan = 6.0F;              // 虚线段长
constexpr float kDropTargetDashGap  = 4.0F;              // 虚线间隔
constexpr float kDropTargetBorderWidth = 1.6F;

// 跟手磁贴（被拿起的那块）。底色固定深黑（脱离网格感与主题无关），
// 描边跟主题文字走。
constexpr float kLiftedScale = 1.06F;                // 轻微放大
constexpr float kLiftedShadowAlpha = 0.38F;          // 投影强度

// 窗口背景色（深色，完全不透明）。
//
// 必须完全不透明：D2D 的 HWND 渲染目标不保证帧间清屏，每帧用半透明刷子
// FillRectangle 等于把颜色持续混到上一帧画面上，窗口空闲时旧内容会长期
// 透出残影。GDI 呈现也会忽略 alpha，半透明没有任何效果。
constexpr std::uint32_t kWindowBackground = 0xFF1E1E1E;

// --- 钉子按钮
// 图形用 Lucide 的 pin（core/lucide_icons.h）描边绘制，不用字体字形：
// Win7 的 Segoe UI Symbol 对 U+1F4CC 支持不稳定，会画成方框或空白。
constexpr float kPinButtonSize  = 22.0F;   // 命中/绘制方形边长
constexpr float kPinHitPadding  = 4.0F;    // 命中容差（向外扩）
constexpr float kPinBaselineNudge = 0.0F;  // 垂直微调：让图钉视觉重心居中
// Lucide 图形是 24x24：缩到 17.5 落在按钮内，线宽按同一比例换算（≈1.46），
// 和内置项图标的「线宽 = 2/24」保持同一视觉密度。
constexpr float kPinGlyphBox = 17.5F;
constexpr float kPinGlyphScale = kPinGlyphBox / 24.0F;
constexpr float kPinStrokeWidth = 2.0F * kPinGlyphScale;

// 锁定按钮（防误拖磁贴）：钉子左侧的小锁，尺寸/线宽/命中容差全部沿用
// 钉子的常量，两个按钮并排视觉密度必须一致。
constexpr float kLockButtonGap = 4.0F;  // 与钉子的间距

// 标题带内控件（搜索框 / 锁 / 钉子）距标题带顶部的固定留白。不用垂直居中：
// 多出的空间全给下方，搜索框上方边距收窄，与磁贴又能拉开距离。
constexpr float kHeaderContentTopInset = 2.0F;
// 钉子颜色由 ThemeColors 派生（paintPinButton）。

// --- 搜索框
// 放在标题带内、钉子左侧：磁贴从 outerPadding + headerHeight 开始，
// 标题带是唯一恒为空白的区域，搜索框不会盖住磁贴。
constexpr float kSearchBoxHeight = 22.0F;   // 与钉子等高的视觉节奏
constexpr float kSearchBoxRadius = 3.0F;    // 轻微圆角，柔和但仍是直角系
constexpr float kSearchBoxRightGap = 10.0F; // 搜索框右边缘与钉子左边缘的间距
// 搜索框颜色改由 ThemeColors 派生（paintSearchBox / WM_CTLCOLOREDIT）。
constexpr float kSearchBoxStrokeWidth = 1.0F;
// 无查询时的占位提示。文案在语言文件里，所以只能运行时取。
const wchar_t* searchPlaceholder() {
    return infrastructure::i18n::trw("launcher.searchPlaceholder").c_str();
}

// 搜索框左端的应用图标（品牌位）。16 DIP 图标在 22 DIP 框内上下各留
// 3 DIP，视觉上嵌在框里；右侧留 kSearchIconTextGap，输入文字与占位
// 提示从这里起排。
constexpr float kSearchIconSize = 16.0F;
constexpr float kSearchIconLeftGap = 4.0F;
constexpr float kSearchIconTextGap = 4.0F;

// 搜索框字号：比磁贴小标签略大，交互控件文字更易读，与 22px 框协调。
constexpr float kSearchFontSize = 13.0F;

// 不匹配搜索的磁贴淡化到多少（alpha 乘数）
constexpr float kSearchDimOpacity = 0.18F;

// --- 标签条
// 独占一行，位于标题带之下、网格之上：标题带已被搜索框与钉子占满，
// 页名挤不进去；标签是内容级控件，紧贴内容层级更清楚。
// 高度与间距常量在 launcher_window.h（kTabBarHeight 等）：窗口尺寸计算
// 也要用，放 .cpp 的匿名命名空间会让 behavior.cpp 看不到。

// 标签宽度 = 文字宽度 + 左右内边距，再夹到 [min, max]。
// 夹下限：单字页名（「1」「图」）也要有个像样的点击块；
// 夹上限：超长页名不许把整条占掉，让后面的页全被挤出去。
constexpr float kTabMinWidth = 46.0F;
constexpr float kTabMaxWidth = 150.0F;
constexpr float kTabPadX     = 22.0F;
constexpr float kTabGap      = 3.0F;

// 标签条右端的固定按钮：＋（新建）、◀ ▶（页多放不下时滚动）
constexpr float kTabBarButtonSize = 20.0F;
constexpr float kTabBarButtonGap  = 4.0F;

constexpr float kTabFontSize = 12.5F;

// 标签条配色改由 ThemeColors 派生（paintTabBar）。
constexpr float kTabUnderlineHeight = 2.0F;
constexpr float kTabButtonStrokeWidth = 1.4F;

// --- 选中框
constexpr float kSelectionFrameWidth = 2.0F;
constexpr float kSelectionFrameInset = 1.0F;
// 颜色跟主题强调色走（theme_.selectionFrame()）

// --- 磁贴配色
// 底色不用高饱和纯色：exe 图标多为浅色线条设计，压在深绿上会糊成一片。
// 改为深灰玻璃底保证对比度，强调色条与悬停描边做标识；无图标的磁贴用
// 强调色圆角底衬加白色首字，此时高饱和是安全的。强调色按名称哈希分配。

constexpr std::uint32_t kTileAccentPalette[] = {
    0xFF2D7DD2,  // 蓝
    0xFF00A300,  // 绿
    0xFFD24726,  // 橙红
    0xFF7E38B7,  // 紫
    0xFF008299,  // 青
    0xFFD80073,  // 品红
    0xFFA4C400,  // 黄绿
    0xFF603CBA,  // 深紫
    0xFF1E7145,  // 深绿
    0xFFE39B00,  // 琥珀
    0xFF4B3CFA,  // 靛蓝
    0xFFAA1740,  // 绛红
};
constexpr std::size_t kTileAccentCount =
    sizeof(kTileAccentPalette) / sizeof(kTileAccentPalette[0]);

// FNV-1a 哈希（稳定，不依赖运行时随机）
std::uint32_t fnv1aHash(const std::string& text) {
    std::uint32_t hash = 0x811C9DC5U;
    for (const char raw : text) {
        hash ^= static_cast<std::uint8_t>(raw);
        hash *= 0x01000193U;
    }
    return hash;
}

std::uint32_t tileAccentColor(const core::LaunchItem& item) {
    if (item.tileColor.has_value()) {
        return *item.tileColor;
    }
    const std::string seed = !item.name.empty() ? item.name : item.target;
    return kTileAccentPalette[fnv1aHash(seed) % kTileAccentCount];
}

// 在基色上按状态提亮（悬停/按下反馈）
std::uint32_t liftColor(const std::uint32_t base, const std::uint32_t lift) {
    if (lift == 0) {
        return base;
    }
    std::uint32_t result = base & 0xFF000000U;  // 保留 alpha
    for (int channel = 0; channel < 3; ++channel) {
        const int shift = channel * 8;
        const std::uint32_t value = (base >> shift) & 0xFFU;
        const std::uint32_t lifted = std::min<std::uint32_t>(
            0xFFU, value + lift);
        result |= lifted << shift;
    }
    return result;
}

// 图标缺失时的兜底文字：优先取名称首字符。
// 中文等非拉丁名称取第一个字符即可（如「记事本」→「记」），
// 拉丁名称取首字母大写（如 "chrome" → "C"）。
std::wstring fallbackGlyphText(const core::LaunchItem& item) {
    std::wstring source = base::utf8ToWide(
        !item.name.empty() ? item.name : item.target);
    if (source.empty()) {
        return std::wstring();
    }

    // 跳过前导空白
    std::size_t begin = 0;
    while (begin < source.size() &&
           (source[begin] == L' ' || source[begin] == L'\t')) {
        ++begin;
    }
    if (begin >= source.size()) {
        return std::wstring();
    }

    std::wstring glyph(1, source[begin]);
    if (glyph[0] < 0x80) {
        glyph[0] = static_cast<wchar_t>(towupper(glyph[0]));
    }
    return glyph;
}

}  // namespace

// --- 生命周期

LauncherWindow::LauncherWindow() = default;

LauncherWindow::~LauncherWindow() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool LauncherWindow::create(HINSTANCE instance,
                            const std::wstring& windowTitle,
                            core::ItemsDocument document) {
    instance_ = instance;
    document_ = std::move(document);

    // 注册窗口类
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(WNDCLASSEXW);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = &LauncherWindow::windowProcThunk;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // 主窗口图标（Alt-Tab / 标题栏）：资源 101 = src\app\app.rc 里的
    // 应用图标，与托盘 kApplicationIconResourceId 同源。加载失败就让
    // 它是默认图标，不挡启动。
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0,
        LR_DEFAULTSIZE | LR_SHARED));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_SHARED));
    windowClass.hbrBackground = nullptr;  // D2D 负责绘制
    windowClass.lpszClassName = kWindowClassName;

    const ATOM classAtom = RegisterClassExW(&windowClass);
    if (classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    if (!createDeviceIndependentResources()) {
        return false;
    }

    // 计算初始窗口尺寸
    updateLayout();
    const RECT rect = calculateWindowRect();

    // 创建无边框弹出窗口
    // WS_EX_TOOLWINDOW: 不出现在 Alt+Tab
    // WS_EX_TOPMOST:    置顶显示（启动器需要）
    //
    // 故意不加 WS_EX_NOACTIVATE（曾用过）：窗口拿不到焦点，键盘输入只能
    // 靠全局钩子加隐藏 IME 宿主转发，问题层出不穷。现在是普通有焦点窗口，
    // 失焦时系统自动停止投递按键，行为正确且零维护。
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kWindowClassName,
        windowTitle.c_str(),
        // WS_CLIPCHILDREN：绘制子 Edit 控件（搜索框）时，
        // 面板自身的 D2D 重绘不得覆盖它的区域
        WS_POPUP | WS_CLIPCHILDREN,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        this);

    if (hwnd_ == nullptr) {
        return false;
    }

    if (!createSearchEdit(instance)) {
        return false;
    }

    updateDpi();

    // 启动后延迟裁剪工作集（见 kStartupTrimTimerId 的注释）
    SetTimer(hwnd_, kStartupTrimTimerId, kStartupTrimDelayMs, nullptr);
    return true;
}

// 收回工作集的物理页。隐藏稳态下调给系统，唤出时按需换入。
void LauncherWindow::trimWorkingSet() {
    SetProcessWorkingSetSize(GetCurrentProcess(),
                             static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

// --- 窗口过程

LRESULT CALLBACK LauncherWindow::windowProcThunk(HWND hwnd, UINT message,
                                                  WPARAM wParam, LPARAM lParam) {
    // WM_NCCREATE 时把 this 指针绑到窗口
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<LauncherWindow*>(createStruct->lpCreateParams);
        if (self != nullptr) {
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
    }

    auto* self = reinterpret_cast<LauncherWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        return self->windowProc(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT LauncherWindow::windowProc(const UINT message, const WPARAM wParam,
                                   const LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            render();
            EndPaint(hwnd_, &paint);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;  // 由 D2D 全权绘制，避免闪烁

        case WM_SIZE: {
            if (hwndTarget_.get() != nullptr) {
                const D2D1_SIZE_U size = D2D1::SizeU(
                    LOWORD(lParam), HIWORD(lParam));
                hwndTarget_->Resize(size);
            }
            // 窗口变大超出背景位图的覆盖目标 → 按新尺寸重建，
            // 否则小图会被 cover 拉伸发糊（缩小时不用动，cover 会裁）
            if (backgroundBitmap_.get() != nullptr &&
                (LOWORD(lParam) > backgroundTargetWidth_ ||
                 HIWORD(lParam) > backgroundTargetHeight_)) {
                backgroundBitmap_.reset();
                ensureBackgroundBitmap();
            }
            updateLayout();
            // 宽度变了标签条能放下的页数也变了（设置窗口调网格会走到
            // 这里），让活动页重新回到视野内。
            ensureActiveTabVisible();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED: {
            updateDpi();
            discardDeviceResources();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_EXITSIZEMOVE: {
            // 用户拖动结束：记住新位置，供「上一次的位置」策略使用。
            // 程序自己 SetWindowPos 不触发这条消息，不会污染记录。
            RECT moved{};
            if (GetWindowRect(hwnd_, &moved) != FALSE) {
                lastWindowPos_.x = moved.left;
                lastWindowPos_.y = moved.top;
                lastWindowPosValid_ = true;
                if (windowMovedHandler_) {
                    windowMovedHandler_(moved.left, moved.top);
                }
            }
            return 0;
        }

        case WM_SETCURSOR: {
            // 搜索框左端的应用图标是「可点」的，给手型光标；
            // 其余客户区保持默认箭头（磁贴点击是「选中/启动」，不是链接）。
            if (LOWORD(lParam) == HTCLIENT) {
                POINT point{};
                if (GetCursorPos(&point) != FALSE) {
                    ScreenToClient(hwnd_, &point);
                    const float scale = dpiScale();
                    if (hitTestSearchIcon(point.x / scale,
                                          point.y / scale)) {
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                        return TRUE;
                    }
                }
            }
            break;  // 交给 DefWindowProc 走默认光标
        }

        case WM_MOUSEMOVE: {
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
            const float scale = dpiScale();
            dragCurrentPoint_.x = GET_X_LPARAM(lParam);
            dragCurrentPoint_.y = GET_Y_LPARAM(lParam);

            // ---- 钉子/锁悬停高亮 ----
            // 拖拽中不高亮（此时光标即使扫过标题带也不该有反馈）
            const bool onPin = !dragActive_ &&
                hitTestPin(x / scale, y / scale);
            if (onPin != pinHovered_) {
                pinHovered_ = onPin;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            const bool onLock = !dragActive_ &&
                hitTestLock(x / scale, y / scale);
            if (onLock != lockHovered_) {
                lockHovered_ = onLock;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }

            // ---- 标签条悬停高亮 ----
            // 拖拽中不高亮（此时光标扫过标签条也不该有反馈）。
            // 与钉子同一策略：状态真变了才重绘。
            {
                const int tabUnderCursor =
                    dragActive_ ? -1 : hitTestTab(x / scale, y / scale);
                const int buttonUnderCursor = dragActive_
                    ? 0
                    : hitTestTabBarButton(x / scale, y / scale);
                if (tabUnderCursor != hoveredTab_ ||
                    buttonUnderCursor != tabBarButtonHover_) {
                    hoveredTab_ = tabUnderCursor;
                    tabBarButtonHover_ = buttonUnderCursor;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }

            // ---- 拖拽升级：按下后移动超过阈值才进入拖拽 ----
            // 阈值区分点击启动与拖动排序，按 DPI 缩放保证手感一致。
            // 磁贴锁定时禁止升级：按下仍记录（点击/键盘不受影响），位移忽略。
            if (pressedItem_.has_value() && !dragActive_ && !tilesLocked_) {
                const float threshold = kDragThresholdDip * scale;
                const float dx = static_cast<float>(
                    dragCurrentPoint_.x - dragStartPoint_.x);
                const float dy = static_cast<float>(
                    dragCurrentPoint_.y - dragStartPoint_.y);
                if (std::sqrt(dx * dx + dy * dy) >= threshold) {
                    dragActive_ = true;
                    dragSource_ = pressedItem_;

                    // 记录「抓取偏移」：按下点相对磁贴左上角的距离。
                    // 跟手绘制时用「光标 − 抓取偏移」定位，磁贴不会跳一下。
                    float tileLeft = 0.0F;
                    float tileTop = 0.0F;
                    for (const auto& placement : layout_.placements) {
                        if (placement.itemIndex == *dragSource_) {
                            tileLeft = metrics_.outerPadding +
                                static_cast<float>(placement.column) *
                                    (metrics_.cellSize + metrics_.cellGap);
                            tileTop = gridOriginY() +
                                static_cast<float>(placement.row) *
                                    (metrics_.cellSize + metrics_.cellGap);
                            dragTileWidth_ =
                                metrics_.tileWidth(placement.spanColumns);
                            dragTileHeight_ =
                                metrics_.tileHeight(placement.spanRows);
                            break;
                        }
                    }
                    dragGrabOffsetX_ =
                        static_cast<float>(dragStartPoint_.x) / scale - tileLeft;
                    dragGrabOffsetY_ =
                        static_cast<float>(dragStartPoint_.y) / scale - tileTop;

                    // 拖起来时该磁贴脱离网格，其他磁贴保持原位
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }

            if (dragActive_) {
                // 拖拽中只更新落点虚线框，不动任何磁贴，排布变化推迟到松手
                //（对齐 Win10 行为）。
                updateDropspotPreview();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else {
                auto hit = hitTestItem(x / scale, y / scale);
                if (hit != hoveredItem_) {
                    hoveredItem_ = hit;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }

            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(TRACKMOUSEEVENT);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd_;
            TrackMouseEvent(&track);
            return 0;
        }

        case WM_CAPTURECHANGED: {
            // 捕获被系统或其他窗口抢走（前台切换、弹菜单、显示器切换等）。
            // 拖拽卡死的兜底出口：捕获丢了之后 WM_LBUTTONUP 不会再送达，
            // 不在这里收尾 dragActive_ 会一直为真，磁贴跟着鼠标走。
            // 按「提交」处理：捕获丢失是外部原因，用户本意仍是放到落点框处。
            // lParam 为抢走捕获的窗口，若是本窗口（松手时自己
            // ReleaseCapture）说明是正常流程，状态已清，不会重复提交。
            if (dragActive_ &&
                reinterpret_cast<HWND>(lParam) != hwnd_) {
                const auto source = dragSource_;
                const auto spot = dropspotPreview_;

                dragActive_ = false;
                dragSource_.reset();
                dropspotPreview_.reset();
                pressedItem_.reset();

                if (source.has_value() && spot.has_value() &&
                    !spot->sameAsOrigin) {
                    moveItemToSpot(*source, *spot);
                } else {
                    animations_.clear();
                    stopAnimationTimer();
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            bool needsRepaint = false;
            if (hoveredItem_.has_value()) {
                hoveredItem_.reset();
                needsRepaint = true;
            }
            // 标签条悬停也要一起清：鼠标飞出窗口时 WM_MOUSELEAVE 是
            // 唯一会到达的通知，不清就会留着一块高亮。
            if (hoveredTab_ != -1 || tabBarButtonHover_ != 0) {
                hoveredTab_ = -1;
                tabBarButtonHover_ = 0;
                needsRepaint = true;
            }
            if (needsRepaint) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        // ---- 滚轮：在标签条上滚动 = 上一页/下一页 ----
        // 与浏览器手感一致；页多时切换会把目标页滚进视野。
        case WM_MOUSEWHEEL: {
            POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (ScreenToClient(hwnd_, &cursor) == FALSE) {
                return 0;
            }
            const float scale = dpiScale();
            const float xDip = static_cast<float>(cursor.x) / scale;
            const float yDip = static_cast<float>(cursor.y) / scale;
            const D2D1_RECT_F bar = tabBarRect();
            if (yDip < bar.top || yDip >= bar.bottom || xDip < bar.left ||
                xDip >= bar.right) {
                // 不在标签条上：交给系统（当前没有别的滚动语义）
                return 0;
            }
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            // 一格滚轮 = 切一页。方向与「向上滚 = 前一页」一致。
            cycleActiveTab(delta > 0 ? -1 : 1);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
            const float scale = dpiScale();
            const float xDip = x / scale;
            const float yDip = y / scale;

            // ---- 标题带空白 = 窗口的「标题栏」：按住拖动整块面板 ----
            // 交给系统 HTCAPTION 拖动：多显示器、DPI 边界、Aero Snap 都由
            // 系统处理，比手写 SetCapture + WM_MOUSEMOVE 稳。
            // 搜索框/钉子先各自命中：点在控件上不是标题栏。
            if (yDip < metrics_.outerPadding + metrics_.headerHeight &&
                !hitTestPin(xDip, yDip) &&
                !hitTestLock(xDip, yDip) &&
                !hitTestSearchBox(xDip, yDip)) {
                ReleaseCapture();
                SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }

            // ---- 标签条最优先：切页/新建/滚动不该触发磁贴流程 ----
            if (handleTabBarClick(xDip, yDip)) {
                return 0;
            }

            // ---- 锁定开关优先：命中小锁就不进入磁贴的点击/拖拽流程 ----
            if (hitTestLock(xDip, yDip)) {
                lockPressed_ = true;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            // ---- 钉子优先：命中钉子就不进入磁贴的点击/拖拽流程 ----
            if (hitTestPin(xDip, yDip)) {
                pinPressed_ = true;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            // ---- 搜索框：点它把焦点交给 Edit 控件（键盘输入开始） ----
            // 不设捕获、不置 pressedItem_，它只是个「模式开关」。
            // 框内左端的应用图标例外：它是「关于」按钮，不激活搜索。
            if (hitTestSearchBox(xDip, yDip)) {
                if (hitTestSearchIcon(xDip, yDip)) {
                    if (aboutHandler_) {
                        aboutHandler_();
                    }
                    return 0;
                }
                if (searchEdit_ != nullptr) {
                    ShowWindow(searchEdit_, SW_SHOW);
                    SetFocus(searchEdit_);  // EN_SETFOCUS 里会 activateSearch
                } else {
                    activateSearch();
                }
                return 0;
            }

            // 点空白处退出搜索模式但不清空查询，用户能继续用鼠标操作。
            deactivateSearch();

            pressedItem_ = hitTestItem(xDip, yDip);
            // 点到的磁贴同时成为键盘选中项，鼠标与键盘导航状态保持一致
            if (pressedItem_.has_value() && isItemVisible(*pressedItem_)) {
                selectedItem_ = pressedItem_;
            }
            dragStartPoint_.x = GET_X_LPARAM(lParam);
            dragStartPoint_.y = GET_Y_LPARAM(lParam);
            dragCurrentPoint_ = dragStartPoint_;
            dragActive_ = false;
            dragSource_.reset();
            dropspotPreview_.reset();
            if (pressedItem_.has_value()) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            SetCapture(hwnd_);
            return 0;
        }

        case WM_LBUTTONUP: {
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
            const float scale = dpiScale();
            const float xDip = x / scale;
            const float yDip = y / scale;
            auto hit = hitTestItem(xDip, yDip);

            // ---- 锁：抬起时仍落在锁上才算一次点击 ----
            if (lockPressed_) {
                lockPressed_ = false;
                const bool stillOnLock = hitTestLock(xDip, yDip);
                ReleaseCapture();
                if (stillOnLock) {
                    setTilesLocked(!tilesLocked_);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            // ---- 钉子：抬起时仍落在钉子上才算一次点击 ----
            if (pinPressed_) {
                pinPressed_ = false;
                const bool stillOnPin = hitTestPin(xDip, yDip);
                ReleaseCapture();
                if (stillOnPin) {
                    setPinned(!pinned_);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            if (dragActive_) {
                // ---- 先取落点、清状态，再提交，最后释放捕获 ----
                // ReleaseCapture() 会同步派发 WM_CAPTURECHANGED，先清掉
                // dragActive_ 才不会触发那里的兜底重复提交。
                const auto source = dragSource_;
                const auto spot = dropspotPreview_;

                dragActive_ = false;
                dragSource_.reset();
                dropspotPreview_.reset();
                pressedItem_.reset();
                animations_.clear();
                stopAnimationTimer();

                if (source.has_value() && spot.has_value() && !spot->sameAsOrigin) {
                    moveItemToSpot(*source, *spot);
                }

                ReleaseCapture();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            ReleaseCapture();

            // 非拖拽：按下和抬起在同一磁贴上才算点击
            if (hit.has_value() && pressedItem_.has_value() && *hit == *pressedItem_) {
                const std::size_t index = *hit;
                const auto& items = activeItems();
                if (index < items.size() && launchHandler_) {
                    launchHandler_(items[index]);
                }
            }
            pressedItem_.reset();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_RBUTTONUP: {
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
            const float scale = dpiScale();

            // ---- 标签条右键：页面管理菜单（新建/重命名/删除/调顺序）----
            // 必须在磁贴命中之前判断，否则改名入口落进空白区菜单找不到。
            {
                const int tabIndex = hitTestTab(x / scale, y / scale);
                if (tabIndex >= 0) {
                    POINT screenPoint{};
                    screenPoint.x = static_cast<LONG>(x);
                    screenPoint.y = static_cast<LONG>(y);
                    ClientToScreen(hwnd_, &screenPoint);
                    showTabContextMenu(static_cast<std::size_t>(tabIndex),
                                       screenPoint);
                    return 0;
                }
            }

            auto hit = hitTestItem(x / scale, y / scale);
            if (hit.has_value() && contextMenuHandler_) {
                const std::size_t index = *hit;
                const auto& items = activeItems();
                if (index < items.size()) {
                    // 转为屏幕坐标
                    POINT screenPoint{};
                    screenPoint.x = static_cast<LONG>(x);
                    screenPoint.y = static_cast<LONG>(y);
                    ClientToScreen(hwnd_, &screenPoint);
                    contextMenuHandler_(hwnd_, index, items[index]);
                }
            } else if (settingsHandler_ || addItemHandler_ ||
                       aboutHandler_ || pluginCatalogProvider_) {
                // ---- 空白区右键：面板级操作 ----
                // 托盘图标常被 Windows 收进溢出区，设置与添加项目在空白处
                // 给直接入口。
                POINT screenPoint{};
                screenPoint.x = static_cast<LONG>(x);
                screenPoint.y = static_cast<LONG>(y);
                ClientToScreen(hwnd_, &screenPoint);

                // 插件入口做子菜单：数量随安装增长，平铺会挤占一级菜单。
                std::vector<platform::win32::PopupMenuEntry> pluginEntries;
                std::vector<core::PluginCatalogEntry> catalog;
                if (pluginCatalogProvider_ && addPluginHandler_) {
                    catalog = pluginCatalogProvider_();
                    for (std::size_t index = 0; index < catalog.size(); ++index) {
                        pluginEntries.push_back(platform::win32::PopupMenuEntry::action(
                            kBlankMenuPluginBaseId +
                                static_cast<UINT>(index),
                            base::utf8ToWide(catalog[index].name)));
                    }
                }

                std::vector<platform::win32::PopupMenuEntry> entries;
                entries.push_back(platform::win32::PopupMenuEntry::action(
                    kBlankMenuAddItemId, infrastructure::i18n::trw("launcher.blank.add").c_str()));
                if (!pluginEntries.empty()) {
                    entries.push_back(platform::win32::PopupMenuEntry::parent(
                        infrastructure::i18n::trw("launcher.blank.addPlugin").c_str(), std::move(pluginEntries)));
                }
                entries.push_back(platform::win32::PopupMenuEntry::action(
                    kBlankMenuNewTabId, infrastructure::i18n::trw("launcher.blank.newTab").c_str()));
                entries.push_back(platform::win32::PopupMenuEntry::separator());
                entries.push_back(platform::win32::PopupMenuEntry::action(
                    kBlankMenuSettingsId, infrastructure::i18n::trw("launcher.blank.settings").c_str()));
                entries.push_back(platform::win32::PopupMenuEntry::action(
                    kBlankMenuAboutId, infrastructure::i18n::trw("launcher.blank.about").c_str()));

                const UINT command = platform::win32::showPopupMenu(
                    hwnd_, entries, screenPoint);
                if (command == kBlankMenuSettingsId) {
                    if (settingsHandler_) {
                        settingsHandler_();
                    }
                } else if (command == kBlankMenuAboutId) {
                    if (aboutHandler_) {
                        aboutHandler_();
                    }
                } else if (command == kBlankMenuAddItemId) {
                    if (addItemHandler_) {
                        addItemHandler_();
                    }
                } else if (command == kBlankMenuNewTabId) {
                    promptCreateTab();
                } else if (command >= kBlankMenuPluginBaseId &&
                           addPluginHandler_) {
                    const std::size_t index =
                        static_cast<std::size_t>(command - kBlankMenuPluginBaseId);
                    if (index < catalog.size()) {
                        addPluginHandler_(catalog[index].id);
                    }
                }
            }
            return 0;
        }

        // ---- 键盘输入 ----
        // 焦点偶尔停在面板本体（点过磁贴）时，把输入转交给搜索框：
        // 打字即搜索意图，顺带移焦点，IME 中文从下一个字起走正路。
        case WM_KEYDOWN:
            return handleKeyDown(static_cast<UINT>(wParam));

        case WM_CHAR:
            if (searchEdit_ != nullptr) {
                SetFocus(searchEdit_);
                SendMessageW(searchEdit_, WM_CHAR, wParam, lParam);
            }
            return 0;

        // ---- 搜索框 Edit 控件的通知与配色 ----
        case WM_COMMAND: {
            if (LOWORD(wParam) == kSearchEditId && searchEdit_ != nullptr) {
                switch (HIWORD(wParam)) {
                    case EN_CHANGE:
                        syncSearchFromEdit();
                        break;
                    case EN_SETFOCUS:
                        ShowWindow(searchEdit_, SW_SHOW);
                        activateSearch();
                        break;
                    case EN_KILLFOCUS:
                        if (searchQuery_.empty()) {
                            ShowWindow(searchEdit_, SW_HIDE);
                        }
                        deactivateSearch();
                        break;
                    default:
                        break;
                }
            }
            return 0;
        }

        case WM_CTLCOLOREDIT: {
            // Edit 的底色/文字色在这里给。背景必须不透明（透明底会在
            // 文字增删时糊字），颜色为主题背景混入文字色的合成结果，
            // 与 D2D 侧底衬（10%/20% text）视觉一致。
            if (reinterpret_cast<HWND>(lParam) == searchEdit_ &&
                wParam != 0) {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                SetTextColor(hdc, searchEditTextColorRef(theme_));
                const bool focused = GetFocus() == searchEdit_;
                SetBkColor(hdc, searchEditBkColorRef(theme_, focused));
                return reinterpret_cast<LRESULT>(
                    focused ? searchEditBkActiveBrush_ : searchEditBkBrush_);
            }
            break;
        }

        case WM_TIMER: {
            if (wParam == kMarqueeTimerId) {
                advanceLabelMarquee();
                return 0;
            }
            if (wParam == kStartupTrimTimerId) {
                KillTimer(hwnd_, kStartupTrimTimerId);
                trimWorkingSet();
                return 0;
            }
            if (wParam == kOutsideProbeTimerId) {
                // 探测「窗口外按下是点击还是拖动」：按钮已松开 → 普通点击
                // 收起；仍按着且光标移动 → 拖动（如从资源管理器拖文件），
                // 保持可见让 OLE 拖放落进窗口；仍按着没动且超次数 → 长按收起。
                KillTimer(hwnd_, kOutsideProbeTimerId);
                if (!outsideProbeActive_) {
                    return 0;
                }

                const bool stillDown =
                    (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                if (!stillDown) {
                    outsideProbeActive_ = false;
                    hideIfUnpinned();
                    return 0;
                }

                POINT now{};
                GetCursorPos(&now);
                const int moved =
                    (std::abs(now.x - outsideProbeDownPoint_.x) +
                     std::abs(now.y - outsideProbeDownPoint_.y));
                if (moved >= kOutsideProbeDragThresholdPx) {
                    // 拖东西呢：不收起，也不继续探测
                    outsideProbeActive_ = false;
                    return 0;
                }

                if (outsideProbeAttempts_ >= kOutsideProbeMaxAttempts) {
                    outsideProbeActive_ = false;
                    hideIfUnpinned();
                    return 0;
                }
                ++outsideProbeAttempts_;
                SetTimer(hwnd_, kOutsideProbeTimerId, kOutsideProbeIntervalMs,
                         nullptr);
                return 0;
            }

            if (wParam == kFileDropGuardTimerId) {
                // 拖入落点框的保险：亮着期间每 250ms 查一次左键，键已松开
                // 就撤预览（个别来源结束拖动后补发 DragEnter 却不给 Leave）。
                if (!fileDropActive_) {
                    KillTimer(hwnd_, kFileDropGuardTimerId);
                    return 0;
                }
                if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                    setFileDropTarget(false, fileDropSize_, POINTL{});
                }
                return 0;
            }

            if (wParam != kAnimationTimerId) {
                break;
            }
            // 推进动画，全部收敛则停表：空闲时不占定时器、不重绘。
            const bool stillAnimating = advanceAnimations();
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (!stillAnimating) {
                settleAnimations();
                stopAnimationTimer();
            }
            return 0;
        }

        case WM_KILLFOCUS: {
            // 失去焦点的两种收尾：拖拽中取消拖拽、磁贴回位，以免拖拽卡死
            //（捕获丢失另由 WM_CAPTURECHANGED 负责）；搜索中收掉焦点视觉
            // 与 IME 状态。不主动隐藏窗口，收起是外部点击监视的职责。
            if (dragActive_) {
                dragActive_ = false;
                dragSource_.reset();
                dropspotPreview_.reset();
                pressedItem_.reset();
                animateBackToLayout();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            if (searchActive_ && GetFocus() != searchEdit_) {
                // 焦点去往本窗口之外（去了别的程序）才收搜索视觉；
                // 面板 ↔ 编辑框之间的焦点移动不算失焦。
                deactivateSearch();
            }
            return 0;
        }

        case WM_CLOSE: {
            if (closeHandler_) {
                closeHandler_();
            } else {
                hide();
            }
            return 0;
        }

        case platform::win32::kOutsideClickMessage: {
            // 点击了窗口外部（全局鼠标钩子投递）。拖拽中/按在钉子上/
            // 模态交互中不收起（主窗口保持可见，皮肤切换才能实时预览）；
            // 钉住与否由 hideIfUnpinned 判断。
            if (dragActive_ || pinPressed_ || modalGuard_) {
                return 0;
            }

            // 钉住时不收起：焦点随点击自然移走，输入随之停止，无需额外状态。

            // ---- 延迟判定：按在窗口外是点击还是拖动 ----
            // 从资源管理器拖文件进来时，按下鼠标发生在窗口外，立即收起
            // 会让拖放没机会落到窗口。先记下按下点，交给探测定时器区分
            //（见 WM_TIMER 的 kOutsideProbeTimerId 分支）。
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
                GetCursorPos(&outsideProbeDownPoint_);
                outsideProbeActive_ = true;
                outsideProbeAttempts_ = 0;
                SetTimer(hwnd_, kOutsideProbeTimerId, kOutsideProbeIntervalMs,
                         nullptr);
                return 0;
            }

            hideIfUnpinned();
            return 0;
        }

        case WM_DESTROY:
            outsideClickMonitor_.stop();
            stopAnimationTimer();
            KillTimer(hwnd_, kOutsideProbeTimerId);
            outsideProbeActive_ = false;
            animations_.clear();
            if (searchEditFont_ != nullptr) {
                DeleteObject(searchEditFont_);
                searchEditFont_ = nullptr;
            }
            if (searchEditBkBrush_ != nullptr) {
                DeleteObject(searchEditBkBrush_);
                searchEditBkBrush_ = nullptr;
            }
            if (searchEditBkActiveBrush_ != nullptr) {
                DeleteObject(searchEditBkActiveBrush_);
                searchEditBkActiveBrush_ = nullptr;
            }
            discardDeviceResources();
            hwnd_ = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

// --- D2D 资源

bool LauncherWindow::createDeviceIndependentResources() {
    // D2D 工厂
    D2D1_FACTORY_OPTIONS options{};
    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory),
        &options,
        reinterpret_cast<void**>(d2dFactory_.put()));
    if (FAILED(hr)) {
        return false;
    }

    // DWrite 工厂
    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwriteFactory_.put()));
    if (FAILED(hr)) {
        return false;
    }

    // 文本格式（小磁贴标签）
    hr = dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        metrics_.labelFontSizeSmall,
        L"zh-CN",
        labelFormatSmall_.put());
    if (FAILED(hr)) {
        return false;
    }
    labelFormatSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    labelFormatSmall_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 文本格式（大磁贴标签）
    hr = dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        metrics_.labelFontSizeLarge,
        L"zh-CN",
        labelFormatLarge_.put());
    if (FAILED(hr)) {
        return false;
    }
    labelFormatLarge_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    labelFormatLarge_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 文本格式（图标缺失时的兜底首字，居中对齐、字号动态设置）
    hr = dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        kFallbackBaseFontSize,
        L"zh-CN",
        fallbackFormat_.put());
    if (FAILED(hr)) {
        return false;
    }
    fallbackFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fallbackFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fallbackFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 文本格式（搜索框）：必须单独建，磁贴标签是居中对齐而搜索框要左对齐；
    // ParagraphAlignment = CENTER 让文字在框内垂直居中，只设 TextAlignment
    // 会贴着框顶。
    hr = dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        kSearchFontSize,
        L"zh-CN",
        searchFormat_.put());
    if (FAILED(hr)) {
        return false;
    }
    searchFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    searchFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    searchFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 文本格式（标签条页名）：左对齐，理由同搜索框（磁贴标签格式是居中的）。
    hr = dwriteFactory_->CreateTextFormat(
        L"Microsoft YaHei UI",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        kTabFontSize,
        L"zh-CN",
        tabFormat_.put());
    if (FAILED(hr)) {
        return false;
    }
    tabFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    tabFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tabFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    return true;
}

void LauncherWindow::discardDeviceResources() {
    // 位图依附于渲染目标，必须先清缓存再释放目标
    clearIconCache();
    // 插件图标几何建在 D2D 工厂上，不依附渲染目标；设备丢失时一并清掉，
    // 下次首绘重建只花几十个线段。
    clearPluginGeometryCache();
    appIconBitmap_.reset();
    backgroundBitmap_.reset();
    borderBrush_.reset();
    textBrush_.reset();
    brush_.reset();
    hwndTarget_.reset(); renderTarget_ = nullptr;
}

void LauncherWindow::clearIconCache() {
    iconCache_.clear();
}

// 把 HICON 转成 D2D 位图（WIC CreateBitmapFromHICON，Win7 可用）。
// 失败返回空指针，调用方降级为占位方块。
ID2D1Bitmap* LauncherWindow::acquireItemIcon(const core::LaunchItem& item) {
    if (!renderTarget_) {
        return nullptr;
    }

    // 缓存键 = id + 目标 + 自定义图标：只用 id 不行，「修改项目」改的正是
    // 目标与图标而 id 不变。invalidateIconCache 保留用于及时释放旧位图。
    std::string cacheKey = item.id;
    cacheKey += '\x1F';
    cacheKey += item.target;
    cacheKey += '\x1F';
    cacheKey += item.icon.has_value() ? *item.icon : std::string();

    // 命中缓存直接返回（含「已确认无图标」的空条目）
    const auto cached = iconCache_.find(cacheKey);
    if (cached != iconCache_.end()) {
        return cached->second.get();
    }

    // 源图档位一律取 256px：系统 ImageList 实际档位是 32/48/256，请求
    // 64px 也只返回 48x48，小图放大必糊；统一取 256 后磁贴任意尺寸都是
    // 缩小绘制，由 D2D 高质量重采样。
    const auto hint = platform::win32::IconSizeHint::Large;

    const std::string iconPath = item.icon.has_value() ? *item.icon : std::string();

    // ---- 用户导入的自定义图片：直接喂像素给 D2D，不走 HICON ----
    // 图片按原图比例存（可能是竖图），HICON 的 DIB 往返对非正方形图标
    // 没有尺寸保证。
    if (!iconPath.empty() &&
        platform::win32::isManagedIconPath(base::utf8ToWide(iconPath))) {
        platform::win32::ImagePixels pixels;
        if (platform::win32::decodeImageFile(
                platform::win32::resolveStoredIconPath(
                    base::utf8ToWide(iconPath)),
                &pixels) &&
            pixels.valid()) {
            ID2D1RenderTarget* host =
                (hwndTarget_.get() != nullptr) ? hwndTarget_.get()
                                               : renderTarget_;
            if (host != nullptr) {
                const std::vector<std::uint32_t> premultiplied =
                    platform::win32::premultiplyBgra(pixels);
                platform::win32::ComPtr<ID2D1Bitmap> direct;
                const D2D1_SIZE_U bitmapSize =
                    D2D1::SizeU(static_cast<UINT32>(pixels.width),
                                static_cast<UINT32>(pixels.height));
                const D2D1_BITMAP_PROPERTIES properties =
                    D2D1::BitmapProperties(D2D1::PixelFormat(
                        DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED));
                if (SUCCEEDED(host->CreateBitmap(
                        bitmapSize, premultiplied.data(),
                        static_cast<UINT32>(pixels.width) * 4U, properties,
                        direct.put()))) {
                    auto inserted =
                        iconCache_.emplace(cacheKey, std::move(direct));
                    return inserted.first->second.get();
                }
            }
        }
        // 解码失败（文件被删/被占用）时继续往下走：loadItemIcon 会用同一个
        // 路径再试一次，失败则回落到目标自身的图标，磁贴不会变空白。
    }
    platform::win32::UniqueHIcon icon = platform::win32::loadItemIcon(
        item.target, iconPath,
        item.type == core::ItemType::Folder,
        item.type == core::ItemType::Url,
        hint);

    if (!icon.valid()) {
        // 缓存空条目，避免每次重绘都重复尝试提取
        iconCache_.emplace(cacheKey, platform::win32::ComPtr<ID2D1Bitmap>());
        return nullptr;
    }

    // ---- HICON -> WIC 位图 -> D2D 位图 ----
    platform::win32::ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(wicFactory.put()));
    if (FAILED(hr)) {
        iconCache_.emplace(cacheKey, platform::win32::ComPtr<ID2D1Bitmap>());
        return nullptr;
    }

    platform::win32::ComPtr<IWICBitmap> wicBitmap;
    hr = wicFactory->CreateBitmapFromHICON(icon.get(), wicBitmap.put());
    if (FAILED(hr)) {
        iconCache_.emplace(cacheKey, platform::win32::ComPtr<ID2D1Bitmap>());
        return nullptr;
    }

    // CreateBitmapFromHICON 产出 32bppBGRA（非预乘），而 HwndRenderTarget
    // 只支持 32bppPBGRA（预乘）。必须经 FormatConverter 转换，否则
    // CreateBitmapFromWicBitmap 返回 D2DERR_UNSUPPORTED_PIXEL_FORMAT。
    platform::win32::ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(converter.put());
    if (FAILED(hr)) {
        iconCache_.emplace(cacheKey, platform::win32::ComPtr<ID2D1Bitmap>());
        return nullptr;
    }

    hr = converter->Initialize(
        wicBitmap.get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0F,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        iconCache_.emplace(cacheKey, platform::win32::ComPtr<ID2D1Bitmap>());
        return nullptr;
    }

    platform::win32::ComPtr<ID2D1Bitmap> bitmap;
    // 位图必须由窗口渲染目标创建：用离屏目标创建的话，切回窗口目标会因
    // 「位图与渲染目标不同源」绘制失败。
    ID2D1RenderTarget* bitmapHost =
        (hwndTarget_.get() != nullptr) ? hwndTarget_.get() : renderTarget_;
    if (bitmapHost == nullptr) {
        iconCache_.emplace(cacheKey, platform::win32::ComPtr<ID2D1Bitmap>());
        return nullptr;
    }
    hr = bitmapHost->CreateBitmapFromWicBitmap(converter.get(), nullptr,
                                               bitmap.put());
    if (FAILED(hr)) {
        iconCache_.emplace(cacheKey, platform::win32::ComPtr<ID2D1Bitmap>());
        return nullptr;
    }

    auto inserted = iconCache_.emplace(cacheKey, std::move(bitmap));
    return inserted.first->second.get();
}

bool LauncherWindow::createDeviceResources() {
    if (hwndTarget_) {
        return true;
    }
    if (hwnd_ == nullptr || !d2dFactory_) {
        return false;
    }

    RECT clientRect{};
    GetClientRect(hwnd_, &clientRect);
    const D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(clientRect.right - clientRect.left),
        static_cast<UINT32>(clientRect.bottom - clientRect.top));

    if (size.width == 0 || size.height == 0) {
        return false;
    }

    const HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_, size),
        hwndTarget_.put());
    if (FAILED(hr)) {
        return false;
    }

    // 当前生效的渲染目标 = 窗口目标
    renderTarget_ = hwndTarget_.get();

    // 创建画刷
    if (FAILED(renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White), brush_.put()))) {
        return false;
    }
    if (FAILED(renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White), textBrush_.put()))) {
        return false;
    }
    if (FAILED(renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White), borderBrush_.put()))) {
        return false;
    }

    // 背景图位图跟着渲染目标走（设备丢失后随目标一起重建）
    ensureBackgroundBitmap();

    return true;
}

// --- 皮肤与背景图

void LauncherWindow::setTheme(const ThemeColors& theme) {
    theme_ = theme;
    // GDI 背景画刷按主题着色、懒创建：删掉旧的，下次 WM_CTLCOLOREDIT
    // 前按新主题重建。
    if (searchEditBkBrush_ != nullptr) {
        DeleteObject(searchEditBkBrush_);
        searchEditBkBrush_ = nullptr;
    }
    if (searchEditBkActiveBrush_ != nullptr) {
        DeleteObject(searchEditBkActiveBrush_);
        searchEditBkActiveBrush_ = nullptr;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void LauncherWindow::setBackgroundImage(const std::wstring& absolutePath) {
    backgroundImage_ = absolutePath;
    backgroundBitmap_.reset();
    ensureBackgroundBitmap();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---- 主窗口整体不透明度 ----
// 用分层窗口而非逐个填充改 alpha：GDI/D2D 混排的部位只有系统级合成能
// 统一处理。内容仍按不透明绘制，每帧 Clear 到不透明底，alpha 只在合成
// 时相乘，帧间残影不受影响。100% 时撤掉 WS_EX_LAYERED。
void LauncherWindow::setWindowOpacityPercent(const int percent) {
    int clamped = percent;
    if (clamped < 50) {
        clamped = 50;
    }
    if (clamped > 100) {
        clamped = 100;
    }
    windowOpacityPercent_ = clamped;
    applyWindowOpacity();
}

void LauncherWindow::applyWindowOpacity() {
    if (hwnd_ == nullptr) {
        return;
    }

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (windowOpacityPercent_ >= 100) {
        if ((exStyle & WS_EX_LAYERED) != 0) {
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
        }
        return;
    }

    if ((exStyle & WS_EX_LAYERED) == 0) {
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
    }
    // 0~255 的 alpha 用整数算，避免 100 → 254 这类浮点误差
    const BYTE alpha =
        static_cast<BYTE>((static_cast<int>(windowOpacityPercent_) * 255) / 100);
    SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA);
}

// ---- 磁贴背景不透明度 ----
//
// 只改「磁贴底」这一层的 alpha，其余（图标、标题、边框）不受影响。
// -1 表示跟随皮肤：皮肤文件里 tileFill 的 alpha 就是设计值，不动它。
void LauncherWindow::setTileOpacityPercent(const int percent) {
    int clamped = percent;
    if (clamped < -1) {
        clamped = -1;
    }
    if (clamped > 100) {
        clamped = 100;
    }
    if (clamped == tileOpacityPercent_) {
        return;
    }
    tileOpacityPercent_ = clamped;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// 当前实际生效的磁贴背景不透明度：显式值直接用，跟随皮肤时把皮肤
// tileFill 的 alpha 换算成百分比（设置窗口据此显示滑块的真实位置）。
int LauncherWindow::effectiveTileOpacityPercent() const {
    if (tileOpacityPercent_ >= 0) {
        return tileOpacityPercent_;
    }
    const std::uint32_t alpha = (theme_.tileFill >> 24) & 0xFFU;
    return static_cast<int>(alpha * 100U / 255U);
}

std::uint32_t LauncherWindow::applyTileOpacityPercent(
    const std::uint32_t color) const {
    if (tileOpacityPercent_ < 0) {
        return color;  // 跟随皮肤
    }
    const std::uint32_t targetAlpha =
        static_cast<std::uint32_t>(tileOpacityPercent_) * 255U / 100U;

    // 保留三档底色间的浓淡差（悬停亮于常态、按下更亮）：统一 alpha 会把
    // 状态反馈抹平。差值以 tileFill 为基准算，换皮肤仍成立。
    const std::int32_t fillAlpha = static_cast<std::int32_t>(
        (theme_.tileFill >> 24) & 0xFFU);
    const std::int32_t ownAlpha =
        static_cast<std::int32_t>((color >> 24) & 0xFFU);
    std::int32_t next =
        static_cast<std::int32_t>(targetAlpha) + (ownAlpha - fillAlpha);
    if (next < 0) {
        next = 0;
    }
    if (next > 255) {
        next = 255;
    }
    return (static_cast<std::uint32_t>(next) << 24) | (color & 0x00FFFFFFU);
}

// 把背景图解码成 D2D 位图。渲染目标尚未就绪时直接返回，
// createDeviceResources 会在设备资源建好后再次调用。
// 解码失败（文件被删/损坏）时不报错：路径保留，下次设备重建时
// 再试一次；期间窗口用纯色背景，磁贴不受影响。
void LauncherWindow::ensureBackgroundBitmap() {
    if (backgroundImage_.empty() || backgroundBitmap_.get() != nullptr) {
        return;
    }
    ID2D1RenderTarget* host = (hwndTarget_.get() != nullptr)
                                  ? hwndTarget_.get()
                                  : renderTarget_;
    if (host == nullptr) {
        return;
    }

    // 目标尺寸 = 窗口客户区物理像素。cover 绘制只需要位图「盖住」窗口，
    // 按 1:1 需求解码即可：源壁纸（动辄 3K×2K，解码 25MB）不缩放
    // 直接建位图的话，内存是窗口需求的几十倍，常驻应用受不了。
    const D2D1_SIZE_F dip = host->GetSize();
    float dpiX = 96.0F;
    float dpiY = 96.0F;
    host->GetDpi(&dpiX, &dpiY);
    const UINT targetWidth = (std::max)(1U, static_cast<UINT>(
                                                 dip.width * dpiX / 96.0F + 0.5F));
    const UINT targetHeight = (std::max)(1U, static_cast<UINT>(
                                                  dip.height * dpiY / 96.0F + 0.5F));

    platform::win32::ImagePixels pixels;
    if (!platform::win32::decodeImageFileCover(
            backgroundImage_, targetWidth, targetHeight, &pixels) ||
        !pixels.valid()) {
        return;
    }
    backgroundTargetWidth_ = pixels.width;
    backgroundTargetHeight_ = pixels.height;
    const std::vector<std::uint32_t> premultiplied =
        platform::win32::premultiplyBgra(pixels);
    const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED));
    host->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(pixels.width),
                    static_cast<UINT32>(pixels.height)),
        premultiplied.data(),
        static_cast<UINT32>(pixels.width) * 4U,
        properties,
        backgroundBitmap_.put());
}

// --- 渲染

void LauncherWindow::render() {
    if (!createDeviceResources()) {
        return;
    }

    renderTarget_->BeginDraw();
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    // 每帧先清成不透明背景：渲染目标帧间内容不保证被重置，半透明填充
    // 会把上一帧混进来（见 kWindowBackground 处的说明）。
    renderTarget_->Clear(colorFromArgb(theme_.windowBackground));

    const D2D1_SIZE_F size = renderTarget_->GetSize();

    // ---- 背景图（设置了才画）----
    // cover 等比铺满后叠一层压暗层（backgroundDim），保证浅色文字与描边
    // 可读。压暗层是半透明叠加，Clear 每帧不能省。
    if (backgroundBitmap_.get() != nullptr) {
        const D2D1_SIZE_F bmp = backgroundBitmap_->GetSize();
        if (bmp.width > 0.5F && bmp.height > 0.5F) {
            const float scale = std::max(size.width / bmp.width,
                                         size.height / bmp.height);
            const float drawWidth = bmp.width * scale;
            const float drawHeight = bmp.height * scale;
            const float left = (size.width - drawWidth) / 2.0F;
            const float top = (size.height - drawHeight) / 2.0F;
            renderTarget_->DrawBitmap(
                backgroundBitmap_.get(),
                D2D1::RectF(left, top, left + drawWidth, top + drawHeight));

            if (theme_.backgroundDim > 0.001F) {
                const std::uint8_t dimAlpha = static_cast<std::uint8_t>(
                    theme_.backgroundDim * 255.0F + 0.5F);
                brush_->SetColor(colorFromArgb(
                    ThemeColors::withAlphaOf(theme_.backgroundDimColor(),
                                             dimAlpha)));
                renderTarget_->FillRectangle(
                    D2D1::RectF(0.0F, 0.0F, size.width, size.height),
                    brush_.get());
            }
        }
    }

    // 背景（无背景图时的保底填充；有背景图时位图已覆盖全窗口）
    if (backgroundBitmap_.get() == nullptr) {
        brush_->SetColor(colorFromArgb(theme_.windowBackground));
        renderTarget_->FillRectangle(
            D2D1::RectF(0.0F, 0.0F, size.width, size.height), brush_.get());
    }

    // 磁贴
    paintTiles();

    const HRESULT hr = renderTarget_->EndDraw();
    // D2DERR_RECREATE_TARGET 表示设备丢失，需要重建资源
    if (hr == D2DERR_RECREATE_TARGET) {
        discardDeviceResources();
    }

    // 子 Edit 控件保险：个别驱动路径的 D2D 呈现不遵守 WS_CLIPCHILDREN，
    // 会抹掉编辑框文字。每帧让它跟着重绘一次，代价可忽略。
    if (searchEdit_ != nullptr && IsWindowVisible(searchEdit_)) {
        InvalidateRect(searchEdit_, nullptr, TRUE);
    }
}

void LauncherWindow::paintTiles() {
    if (!renderTarget_ || document_.tabs.empty()) {
        return;
    }

    const auto& items = activeItems();

    // D2D 使用 DIP 坐标，渲染目标已按 DPI 缩放，这里直接用 DIP
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    // 被拖动的磁贴：跟手绘制，必须最后画（盖在最上层），
    // 所以这里先从常规循环里排除。
    std::optional<std::size_t> liftedIndex;
    if (dragActive_ && dragSource_.has_value()) {
        liftedIndex = dragSource_;
    }

    for (const auto& placement : layout_.placements) {
        if (placement.itemIndex >= items.size()) {
            continue;
        }

        // 跳过未能放置的项（容量不足时其坐标为占位值）
        const bool overflowed = std::find(
            layout_.overflowed.begin(), layout_.overflowed.end(),
            placement.itemIndex) != layout_.overflowed.end();
        if (overflowed) {
            continue;
        }

        // 跟手磁贴稍后单独绘制
        if (liftedIndex.has_value() && *liftedIndex == placement.itemIndex) {
            continue;
        }

        paintSingleTile(placement, /*asLifted=*/false,
                        /*dimmed=*/!isItemVisible(placement.itemIndex));
    }

    // ---- 键盘选中高亮框 ----
    // 画在磁贴之上、落点框之下：它是「焦点」提示，不该被磁贴的内容挡住。
    if (selectedItem_.has_value() && !dragActive_ &&
        isItemVisible(*selectedItem_)) {
        for (const auto& placement : layout_.placements) {
            if (placement.itemIndex != *selectedItem_) {
                continue;
            }
            const bool overflowed = std::find(
                layout_.overflowed.begin(), layout_.overflowed.end(),
                placement.itemIndex) != layout_.overflowed.end();
            if (!overflowed) {
                paintSelectionFrame(placement);
            }
            break;
        }
    }

    // ---- 落点占位框：松手后磁贴将占据的格子 ----
    // 只画虚线框不移动磁贴：提前挪开受影响磁贴反而看不清这块会去哪儿。
    // 拖动已有磁贴（中性白）与拖入新项目（强调蓝）用同一套几何。
    if (dragActive_ && dropspotPreview_.has_value() && !items.empty()) {
        paintDropSpotFrame(*dropspotPreview_, /*addingNew=*/false);
    }
    if (fileDropActive_ && fileDropSpot_.has_value()) {
        paintDropSpotFrame(*fileDropSpot_, /*addingNew=*/true);
    }

    // ---- 跟手磁贴：最上层单独绘制 ----
    if (liftedIndex.has_value() && *liftedIndex < items.size()) {
        for (const auto& placement : layout_.placements) {
            if (placement.itemIndex != *liftedIndex) {
                continue;
            }
            paintSingleTile(placement, /*asLifted=*/true);
            break;
        }
    }

    // ---- 钉子按钮：最后绘制，保证压在所有内容之上 ----
    //
    // 钉子位于标题带，本就与磁贴不重叠；放在最后是防止将来标题带高度
    // 调小或磁贴越界时被遮住（宁可钉子盖住内容，也不能让用户点不到）。
    paintPinButton();
    paintLockButton();

    // ---- 搜索框：与钉子同在标题带，紧随其后绘制 ----
    // 两者矩形互不重叠（搜索框右边界 = 钉子左边界 − 间距），
    // 但搜索框内容更长，放最后便于将来调整标题带布局时不被压住。
    paintSearchBox();

    // ---- 标签条：标题带之下的独立一行 ----
    // 与磁贴不重叠（网格起点 = gridOriginY()），但放在磁贴之后绘制，
    // 万一将来调小间距也不会被跟手磁贴盖住。
    paintTabBar();
}

// 绘制落点占位框（虚线矩形 + 极淡底衬）。几何与磁贴完全一致，
// 框在哪就等于松手后在哪。
void LauncherWindow::paintDropSpotFrame(const core::Dropspot& spot,
                                       const bool addingNew) {
    if (!renderTarget_ || !brush_) {
        return;
    }

    const float step = metrics_.cellSize + metrics_.cellGap;
    const float left =
        metrics_.outerPadding + static_cast<float>(spot.column) * step;
    const float top = gridOriginY() + static_cast<float>(spot.row) * step;
    const float width = metrics_.tileWidth(spot.spanColumns);
    const float height = metrics_.tileHeight(spot.spanRows);

    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
        D2D1::RectF(left, top, left + width, top + height),
        metrics_.cornerRadius,
        metrics_.cornerRadius);

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    brush_->SetColor(colorFromArgb(addingNew ? theme_.dropNewFill()
                                             : theme_.dropTargetFill()));
    renderTarget_->FillRoundedRectangle(rounded, brush_.get());

    // 虚线描边：D2D 1.0 无内建虚线样式，按周长顺时针手工铺小段，
    // 拐角处线段连续不断口。
    borderBrush_->SetColor(
        colorFromArgb(addingNew ? theme_.dropNewBorder()
                                : theme_.dropTargetBorder()));
    ID2D1Brush* dashBrush = borderBrush_.get();

    const float right = left + width;
    const float bottom = top + height;
    const float dash = kDropTargetDashSpan;
    const float gap = kDropTargetDashGap;

    auto drawDashes = [&](const float x0, const float y0,
                          const float x1, const float y1) {
        float dx = x1 - x0;
        float dy = y1 - y0;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length <= 0.0F) {
            return;
        }
        dx /= length;
        dy /= length;

        float travelled = 0.0F;
        while (travelled < length) {
            const float segmentEnd = (travelled + dash < length)
                ? travelled + dash
                : length;
            renderTarget_->DrawLine(
                D2D1::Point2F(x0 + dx * travelled, y0 + dy * travelled),
                D2D1::Point2F(x0 + dx * segmentEnd, y0 + dy * segmentEnd),
                dashBrush, kDropTargetBorderWidth);
            travelled = segmentEnd + gap;
        }
    };

    drawDashes(left, top, right, top);
    drawDashes(right, top, right, bottom);
    drawDashes(right, bottom, left, bottom);
    drawDashes(left, bottom, left, top);

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
}

// --- 标签条

// 网格内容区的 Y 起点（客户区 DIP），全项目唯一出处：绘制、命中测试、
// 拖拽抓取偏移、落点解析都引用它，散落多处会出现画与点的错位。
// 标签栏在底部时网格紧贴标题带开始，标签条高度挪到窗口下缘，总尺寸不变。
float LauncherWindow::gridOriginY() const {
    if (tabBarAtBottom_) {
        return metrics_.outerPadding + metrics_.headerHeight;
    }
    return metrics_.outerPadding + metrics_.headerHeight + kTabBarGapTop +
           kTabBarHeight + kTabBarGapBottom;
}

// 当前客户区高度（DIP）。取实际客户区而非按配置推算：
// 窗口高度被 kMaximumWindowHeightDip 夹住时两者会不一致，
// 按实际值画才能保证标签条贴着真实的窗口下缘。
float LauncherWindow::clientHeightDip() const {
    RECT client{};
    if (hwnd_ == nullptr || GetClientRect(hwnd_, &client) == FALSE) {
        return 0.0F;
    }
    const float scale = dpiScale();
    if (scale <= 0.0F) {
        return 0.0F;
    }
    return static_cast<float>(client.bottom - client.top) / scale;
}

// 标签条矩形。左边界与内容区左对齐，右边界与钉子右边缘对齐
//（= 内容区右边缘），这样标签条与网格、搜索框、钉子共用同一条视觉边线。
//
// 顶部模式：紧跟标题带；底部模式：贴窗口下缘（留出 gapBottom 的
// 呼吸空间，与顶部模式下方的间距对称）。
D2D1_RECT_F LauncherWindow::tabBarRect() const {
    const float left = metrics_.outerPadding;
    const float right = pinButtonRect().right;
    float top = metrics_.outerPadding + metrics_.headerHeight + kTabBarGapTop;
    if (tabBarAtBottom_) {
        top = clientHeightDip() - metrics_.bottomPadding -
              kTabBarGapBottom - kTabBarHeight;
        // 客户区取不到（极端情况）时别把标签条画出窗口外
        if (top < metrics_.outerPadding) {
            top = metrics_.outerPadding;
        }
    }
    return D2D1::RectF(left, top, right > left ? right : left + 1.0F,
                       top + kTabBarHeight);
}

// 右端三个固定按钮，从右往左：＋、▶、◀。始终留位、不随溢出动态显隐，
// 否则标签可用宽度来回变，切页时标签左右跳。
D2D1_RECT_F LauncherWindow::newTabButtonRect() const {
    const D2D1_RECT_F bar = tabBarRect();
    const float size = kTabBarButtonSize;
    const float top = bar.top + (kTabBarHeight - size) / 2.0F;
    const float left = bar.right - size;
    return D2D1::RectF(left, top, left + size, top + size);
}

D2D1_RECT_F LauncherWindow::tabScrollRightRect() const {
    const D2D1_RECT_F plus = newTabButtonRect();
    const float size = kTabBarButtonSize;
    const float right = plus.left - kTabBarButtonGap;
    return D2D1::RectF(right - size, plus.top, right, plus.bottom);
}

D2D1_RECT_F LauncherWindow::tabScrollLeftRect() const {
    const D2D1_RECT_F rightButton = tabScrollRightRect();
    const float size = kTabBarButtonSize;
    const float right = rightButton.left - kTabBarButtonGap;
    return D2D1::RectF(right - size, rightButton.top, right,
                       rightButton.bottom);
}

// 是否处于「标签放不下」的状态。
// 只按「所有标签 + 间距」与「去掉右端三个按钮后的可用宽度」比较，
// 不依赖 firstVisibleTab_，因此不会与可见性判断互相递归。
bool LauncherWindow::tabBarOverflow() const {
    const std::vector<float>& widths = tabLabelWidths();
    if (widths.empty()) {
        return false;
    }
    float total = 0.0F;
    for (const float width : widths) {
        total += width + kTabGap;
    }
    const D2D1_RECT_F bar = tabBarRect();
    // 右端三个按钮 + 各自右侧的间距（最后一个间距把标签与按钮分开）
    const float reserved =
        kTabBarButtonSize * 3.0F + kTabBarButtonGap * 3.0F;
    const float available = (bar.right - bar.left) - reserved;
    return total > available;
}

// 每个标签页的显示矩形。
// 返回的 vector 与 document_.tabs 等长；放不下的那一页为空矩形
//（left == right），绘制与命中都直接跳过。
std::vector<D2D1_RECT_F> LauncherWindow::tabItemRects() const {
    const std::size_t count = document_.tabs.size();
    std::vector<D2D1_RECT_F> rects(count, D2D1::RectF(0, 0, 0, 0));
    if (count == 0) {
        return rects;
    }

    const std::vector<float>& widths = tabLabelWidths();
    const D2D1_RECT_F bar = tabBarRect();
    const float rightLimit = tabScrollLeftRect().left - kTabBarButtonGap;

    float x = bar.left;
    // 从滚动窗口起点开始铺，直到用尽宽度
    std::size_t index = (firstVisibleTab_ < count) ? firstVisibleTab_ : 0;
    for (; index < count; ++index) {
        const float width = widths[index];
        if (x + width > rightLimit) {
            break;  // 这一页放不下：连同它后面的页一起留到滚动后再显示
        }
        // 高度 kTabBarHeight - 1，给分隔细线让出 1px；底部模式 rect.top
        // 已让开细线，标签铺在线下。
        rects[index] = tabBarAtBottom_
            ? D2D1::RectF(x, bar.top + 1.0F, x + width, bar.bottom)
            : D2D1::RectF(x, bar.top, x + width,
                          bar.top + kTabBarHeight - 1.0F);
        x += width + kTabGap;
    }
    return rects;
}

// 命中标签：返回下标，未命中返回 -1。
int LauncherWindow::hitTestTab(const float xDip, const float yDip) const {
    const std::vector<D2D1_RECT_F> rects = tabItemRects();
    for (std::size_t index = 0; index < rects.size(); ++index) {
        const D2D1_RECT_F& rect = rects[index];
        if (rect.right <= rect.left) {
            continue;
        }
        if (xDip >= rect.left && xDip < rect.right && yDip >= rect.top &&
            yDip < rect.bottom) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

// 命中标签条右端固定按钮：1 = ＋，2 = ◀，3 = ▶，0 = 未命中。
int LauncherWindow::hitTestTabBarButton(const float xDip,
                                        const float yDip) const {
    const D2D1_RECT_F bar = tabBarRect();
    if (yDip < bar.top || yDip >= bar.bottom) {
        return 0;
    }

    auto inside = [&](const D2D1_RECT_F& rect) {
        return xDip >= rect.left && xDip < rect.right;
    };
    if (inside(newTabButtonRect())) {
        return 1;
    }
    if (inside(tabScrollLeftRect())) {
        return 2;
    }
    if (inside(tabScrollRightRect())) {
        return 3;
    }
    return 0;
}

// 标签文字宽度（带缓存）。宽度必须跟着页名走，用 DWrite 文本布局实测；
// 页名/页数/DPI 不变时缓存结果，否则每帧量 N 个文本布局是白白的开销。
const std::vector<float>& LauncherWindow::tabLabelWidths() const {
    const std::size_t count = document_.tabs.size();
    if (!tabLabelWidthsDirty_ && tabLabelWidths_.size() == count) {
        return tabLabelWidths_;
    }

    tabLabelWidths_.assign(count, kTabMinWidth);
    tabLabelWidthsDirty_ = false;

    if (dwriteFactory_.get() == nullptr || tabFormat_.get() == nullptr) {
        return tabLabelWidths_;  // 工厂还没建好：先用最小宽度占位
    }

    for (std::size_t index = 0; index < count; ++index) {
        const std::wstring label =
            base::utf8ToWideLossy(document_.tabs[index].name);
        float textWidth = 0.0F;
        platform::win32::ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                label.c_str(), static_cast<UINT32>(label.size()),
                tabFormat_.get(), 4096.0F, kTabBarHeight, layout.put()))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                textWidth = metrics.widthIncludingTrailingWhitespace;
            }
        }
        float width = textWidth + kTabPadX;
        width = (std::max)(width, kTabMinWidth);
        width = (std::min)(width, kTabMaxWidth);
        tabLabelWidths_[index] = width;
    }
    return tabLabelWidths_;
}

// 让活动页处于可见范围。
// 页多到放不下时，切页（Ctrl+Tab / 菜单）必须把目标页滚进视野，
// 否则「切过去了但看不见」会让人以为没生效。
void LauncherWindow::ensureActiveTabVisible() {
    const std::size_t count = document_.tabs.size();
    if (count == 0) {
        firstVisibleTab_ = 0;
        return;
    }
    if (firstVisibleTab_ >= count) {
        firstVisibleTab_ = count - 1;
    }
    if (activeTabIndex_ < firstVisibleTab_) {
        firstVisibleTab_ = activeTabIndex_;
    }

    // 从滚动窗口起点往后铺，直到活动页也铺进去
    const std::vector<float>& widths = tabLabelWidths();
    const D2D1_RECT_F bar = tabBarRect();
    const float rightLimit = tabScrollLeftRect().left - kTabBarButtonGap;

    float x = bar.left;
    for (std::size_t index = firstVisibleTab_; index < count; ++index) {
        if (x + widths[index] > rightLimit) {
            // 活动页放不下：把它作为新的起点（保证它一定在视野内）
            if (index <= activeTabIndex_) {
                firstVisibleTab_ = activeTabIndex_;
            }
            break;
        }
        x += widths[index] + kTabGap;
    }
}

// 绘制标签条：不画整条底色，只画活动/悬停页底色加一条分隔细线；活动页
// 压强调色下划线，与钉子、选中框同一支强调色。
void LauncherWindow::paintTabBar() {
    if (!renderTarget_ || !brush_ || !borderBrush_ || !textBrush_ ||
        tabFormat_.get() == nullptr) {
        return;
    }

    const D2D1_RECT_F bar = tabBarRect();
    if (bar.right <= bar.left + 4.0F) {
        return;  // 窗口窄到放不下，宁可不画（负宽度会让 D2D 报错）
    }

    // 这里不调 ensureActiveTabVisible()：手动 ◀▶ 翻页会被立刻拉回活动页，
    // 箭头等于失效。活动页可见性由切页/增删页/改名/DPI 变化点维护。
    const std::vector<D2D1_RECT_F> rects = tabItemRects();
    const std::size_t count = document_.tabs.size();
    const bool overflow = tabBarOverflow();

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    // ---- 分隔细线 ----
    // 顶部模式画在标签条底边，底部模式翻到顶边：分隔线永远在标签与
    // 内容区之间，不贴窗口边框。
    brush_->SetColor(colorFromArgb(theme_.tabBarHairline()));
    if (tabBarAtBottom_) {
        renderTarget_->FillRectangle(
            D2D1::RectF(bar.left, bar.top, bar.right, bar.top + 1.0F),
            brush_.get());
    } else {
        renderTarget_->FillRectangle(
            D2D1::RectF(bar.left, bar.bottom - 1.0F, bar.right, bar.bottom),
            brush_.get());
    }

    // ---- 各标签页 ----
    for (std::size_t index = 0; index < count; ++index) {
        const D2D1_RECT_F& rect = rects[index];
        if (rect.right <= rect.left) {
            continue;
        }

        const bool active = (index == activeTabIndex_);
        const bool hovered =
            (!active && hoveredTab_ >= 0 &&
             static_cast<std::size_t>(hoveredTab_) == index);

        if (active || hovered) {
            // 底部模式 rect.top 已让开 1px 细线，背景从线下开始
            const D2D1_ROUNDED_RECT back = D2D1::RoundedRect(
                tabBarAtBottom_
                    ? D2D1::RectF(rect.left, rect.top + 1.0F, rect.right,
                                  rect.bottom)
                    : D2D1::RectF(rect.left, rect.top, rect.right,
                                  rect.bottom - 1.0F),
                3.0F, 3.0F);
            brush_->SetColor(
                colorFromArgb(active ? theme_.tabFillActive()
                                     : theme_.tabFillHover()));
            renderTarget_->FillRoundedRectangle(back, brush_.get());
        }

        if (active) {
            // 活动页强调条：顶部模式压在底边细线上方；
            // 底部模式翻到顶边、紧贴细线下方（同样「贴着内容区一侧」）。
            borderBrush_->SetColor(colorFromArgb(theme_.tabUnderline()));
            renderTarget_->FillRectangle(
                tabBarAtBottom_
                    ? D2D1::RectF(rect.left, rect.top, rect.right,
                                  rect.top + kTabUnderlineHeight)
                    : D2D1::RectF(rect.left,
                                  rect.bottom - kTabUnderlineHeight,
                                  rect.right, rect.bottom),
                borderBrush_.get());
        }

        const std::wstring label =
            base::utf8ToWideLossy(document_.tabs[index].name);
        textBrush_->SetColor(colorFromArgb(
            active ? theme_.tabTextActive()
                   : (hovered ? theme_.tabTextHover() : theme_.tabTextIdle())));
        // 左右各留 kTabPadX/2 的内边距；超长名字由 CLIP 截断，
        // 不会溢出到邻页上（宽度已夹过上限，这里只是保险）。
        renderTarget_->DrawTextW(
            label.c_str(), static_cast<UINT32>(label.size()),
            tabFormat_.get(),
            tabBarAtBottom_
                ? D2D1::RectF(rect.left + kTabPadX / 2.0F, rect.top + 1.0F,
                              rect.right - kTabPadX / 2.0F, rect.bottom)
                : D2D1::RectF(rect.left + kTabPadX / 2.0F, rect.top,
                              rect.right - kTabPadX / 2.0F,
                              rect.bottom - 1.0F),
            textBrush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // ---- 右端固定按钮：＋ / ◀ / ▶ ----
    // 手绘线条不用字体字形：Win7 缺字形会画成方框（与钉子同一考虑）。
    auto drawButtonBackdrop = [&](const D2D1_RECT_F& rect, const bool hovered) {
        if (!hovered) {
            return;
        }
        const D2D1_ROUNDED_RECT back = D2D1::RoundedRect(rect, 3.0F, 3.0F);
        brush_->SetColor(colorFromArgb(theme_.tabButtonHoverFill()));
        renderTarget_->FillRoundedRectangle(back, brush_.get());
    };

    auto buttonColor = [&](const int id, const bool enabled) {
        if (!enabled) {
            return theme_.tabButtonDisabled();
        }
        return (tabBarButtonHover_ == id) ? theme_.tabButtonHover()
                                          : theme_.tabButtonIdle();
    };

    // ＋
    {
        const D2D1_RECT_F rect = newTabButtonRect();
        const bool hovered = (tabBarButtonHover_ == 1);
        drawButtonBackdrop(rect, hovered);
        const float cx = (rect.left + rect.right) / 2.0F;
        const float cy = (rect.top + rect.bottom) / 2.0F;
        const float arm = kTabBarButtonSize * 0.28F;
        borderBrush_->SetColor(colorFromArgb(buttonColor(1, true)));
        renderTarget_->DrawLine(D2D1::Point2F(cx - arm, cy),
                                D2D1::Point2F(cx + arm, cy),
                                borderBrush_.get(), kTabButtonStrokeWidth);
        renderTarget_->DrawLine(D2D1::Point2F(cx, cy - arm),
                                D2D1::Point2F(cx, cy + arm),
                                borderBrush_.get(), kTabButtonStrokeWidth);
    }

    // ◀ / ▶：只有真正溢出时才亮起来（不溢出时按了也没东西可滚）
    auto drawArrow = [&](const D2D1_RECT_F& rect, const bool pointsLeft,
                         const bool enabled, const int id) {
        const bool hovered = enabled && (tabBarButtonHover_ == id);
        drawButtonBackdrop(rect, hovered);
        const float cx = (rect.left + rect.right) / 2.0F;
        const float cy = (rect.top + rect.bottom) / 2.0F;
        const float arm = kTabBarButtonSize * 0.22F;
        const float tip = pointsLeft ? cx - arm : cx + arm;
        const float tail = pointsLeft ? cx + arm : cx - arm;
        borderBrush_->SetColor(colorFromArgb(buttonColor(id, enabled)));
        renderTarget_->DrawLine(D2D1::Point2F(tail, cy - arm),
                                D2D1::Point2F(tip, cy),
                                borderBrush_.get(), kTabButtonStrokeWidth);
        renderTarget_->DrawLine(D2D1::Point2F(tip, cy),
                                D2D1::Point2F(tail, cy + arm),
                                borderBrush_.get(), kTabButtonStrokeWidth);
    };
    drawArrow(tabScrollLeftRect(), true, overflow, 2);
    drawArrow(tabScrollRightRect(), false, overflow, 3);

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
}

// 标签条上的点击。返回 true 表示已被消费，调用方必须直接返回，不能继续
// 走磁贴的点击/拖拽流程。顺序：固定按钮 → 标签 → 条内空白；条内空白也
// 算消费，否则点标签右侧的空隙会误退搜索态。
bool LauncherWindow::handleTabBarClick(const float xDip, const float yDip) {
    const D2D1_RECT_F bar = tabBarRect();
    if (yDip < bar.top || yDip >= bar.bottom || xDip < bar.left) {
        return false;  // 不在标签条上
    }

    const int button = hitTestTabBarButton(xDip, yDip);
    if (button == 1) {
        promptCreateTab();
        return true;
    }
    if (button == 2 || button == 3) {
        // 页没多到放不下时箭头是灰的：按了也不该动，
        // 只是把这次点击吃掉（免得穿透到下面的磁贴）。
        if (!tabBarOverflow()) {
            return true;
        }
        const std::size_t count = document_.tabs.size();
        if (button == 2 && firstVisibleTab_ > 0) {
            --firstVisibleTab_;
        } else if (button == 3 && firstVisibleTab_ + 1 < count) {
            ++firstVisibleTab_;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    const int tab = hitTestTab(xDip, yDip);
    if (tab >= 0) {
        setActiveTabIndex(static_cast<std::size_t>(tab));
    }
    return true;
}

// --- 钉子按钮

// 钉子按钮的矩形（客户区 DIP）。放在标题带内水平靠右：标题带是唯一
// 不会被磁贴或跟手绘制占用的区域。绘制与命中测试共用本函数，
// 看到的位置即可点的位置。
D2D1_RECT_F LauncherWindow::pinButtonRect() const {
    const float size = kPinButtonSize;
    // 右边缘对齐内容区右边缘（与最后一列磁贴的右边界一致）
    const float right = metrics_.outerPadding +
        static_cast<float>(gridColumns_) * metrics_.cellSize +
        static_cast<float>(gridColumns_ > 0 ? gridColumns_ - 1 : 0) *
            metrics_.cellGap;
    const float left = right - size;
    // 与搜索框顶边对齐（两者等高），用固定的上留白而不是在标题带里居中
    const float top = metrics_.outerPadding + kHeaderContentTopInset +
        kPinBaselineNudge;
    return D2D1::RectF(left, top, left + size, top + size);
}

bool LauncherWindow::hitTestPin(const float xDip, const float yDip) const {
    const D2D1_RECT_F rect = pinButtonRect();
    // 命中区域比视觉稍大一点，鼠标不必像素级瞄准（但不侵入相邻磁贴）
    const float pad = kPinHitPadding;
    return xDip >= rect.left - pad && xDip < rect.right + pad &&
           yDip >= rect.top - pad && yDip < rect.bottom + pad;
}

// 建（或复用）钉子图形的几何。以按钮左上角为原点、DIP 建一次，绘制时
// 再按渲染目标 DPI 变换，几何本身与 DPI 无关。
void LauncherWindow::ensurePinGeometry() {
    if (pinGeometry_ || !d2dFactory_.valid()) {
        return;
    }
    const float offset = (kPinButtonSize - kPinGlyphBox) / 2.0F;
    platform::win32::SvgGlyph outline;
    if (!platform::win32::buildSvgGlyph(d2dFactory_.get(),
                                        core::lucidePinMarkup(), kPinGlyphScale,
                                        offset, offset, &outline)) {
        return;
    }
    // 实心版单独建：D2D 的 FillGeometry 跳过 HOLLOW 起始的图元
    platform::win32::SvgGlyph solid;
    platform::win32::buildSvgGlyph(d2dFactory_.get(), core::lucidePinMarkup(),
                                   kPinGlyphScale, offset, offset, &solid,
                                   true);
    pinGeometry_ = outline.strokeGeometry;
    pinFillGeometry_ = solid.fillGeometry;
    pinStrokeStyle_ = platform::win32::createRoundStrokeStyle(d2dFactory_.get());
}

// 绘制钉子按钮：Lucide pin 线框，不用字体字符（Win7 对 U+1F4CC 支持不稳）。
// 钉住 = 亮色实心，松开 = 暗色空心，悬停 = 加底衬。
void LauncherWindow::paintPinButton() {
    if (!renderTarget_ || !brush_ || !borderBrush_) {
        return;
    }

    const D2D1_RECT_F rect = pinButtonRect();
    ensurePinGeometry();

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    // ---- 悬停/钉住的底衬 ----
    // 松开且未悬停时不画底衬，保持面板干净。
    if (pinned_ || pinHovered_) {
        const D2D1_ROUNDED_RECT back = D2D1::RoundedRect(
            D2D1::RectF(rect.left - 1.0F, rect.top - 1.0F,
                        rect.right + 1.0F, rect.bottom + 1.0F),
            3.0F, 3.0F);
        brush_->SetColor(colorFromArgb(
            pinned_ ? theme_.pinActiveBackdrop() : theme_.pinHoverBackdrop()));
        renderTarget_->FillRoundedRectangle(back, brush_.get());
    }

    // ---- 图钉图形 ----
    if (pinGeometry_) {
        borderBrush_->SetColor(
            colorFromArgb(pinned_ ? theme_.pinActive() : theme_.pinIdle()));
        // 几何以按钮左上角为原点，绘制时平移过去（与按钮位置解耦）
        renderTarget_->SetTransform(
            D2D1::Matrix3x2F::Translation(rect.left, rect.top));
        if (pinned_) {
            // 钉住时把针身填实：不靠颜色也一眼可辨（色盲友好）
            if (pinFillGeometry_) {
                renderTarget_->FillGeometry(pinFillGeometry_.get(),
                                            borderBrush_.get(), nullptr);
            }
            renderTarget_->DrawGeometry(pinGeometry_.get(), borderBrush_.get(),
                                        kPinStrokeWidth, pinStrokeStyle_.get());
        } else {
            renderTarget_->DrawGeometry(pinGeometry_.get(), borderBrush_.get(),
                                        kPinStrokeWidth, pinStrokeStyle_.get());
        }
    }

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
}

D2D1_RECT_F LauncherWindow::lockButtonRect() const {
    // 钉子左侧相邻：尺寸与垂直位置完全对齐钉子（同一套常量）
    const D2D1_RECT_F pinRect = pinButtonRect();
    const float right = pinRect.left - kLockButtonGap;
    const float left = right - kPinButtonSize;
    const float top = pinRect.top;
    return D2D1::RectF(left, top, left + kPinButtonSize, top + kPinButtonSize);
}

bool LauncherWindow::hitTestLock(const float xDip, const float yDip) const {
    const D2D1_RECT_F rect = lockButtonRect();
    const float pad = kPinHitPadding;
    return xDip >= rect.left - pad && xDip < rect.right + pad &&
           yDip >= rect.top - pad && yDip < rect.bottom + pad;
}

// 锁定按钮的两枚图标（TurboWarp scratch-gui 的 lock.svg / unlock.svg，
// 12x12 viewBox 实心剪影），直接 FillGeometry 填充不描边，
// 解析时统一走 forceFilled。
constexpr const wchar_t* kLockClosedMarkup = LR"SVG(<path d="M3.5 3.5 C3.5 2.119 4.619 1 6 1 C7.381 1 8.5 2.119 8.5 3.5 L8.5 4.5 C9.328 4.5 10 5.172 10 6 L10 9.5 C10 10.328 9.328 11 8.5 11 L3.5 11 C2.672 11 2 10.328 2 9.5 L2 6 C2 5.172 2.672 4.5 3.5 4.5 Z M6 2 C5.172 2 4.5 2.672 4.5 3.5 L4.5 4.5 L7.5 4.5 L7.5 3.5 C7.5 2.672 6.828 2 6 2 Z M6.092 6.5 C5.791 6.5 5.546 6.745 5.546 7.046 L5.546 8.299 C5.546 8.601 5.791 8.845 6.092 8.845 C6.394 8.845 6.639 8.601 6.639 8.299 L6.639 7.046 C6.639 6.745 6.394 6.5 6.092 6.5 Z" />)SVG";
constexpr const wchar_t* kLockOpenMarkup = LR"SVG(<path d="M6 6.5 C5.688 6.496 5.407 6.687 5.296 6.978 C5.184 7.27 5.266 7.599 5.5 7.805 L5.5 8.5 C5.5 8.776 5.724 9 6 9 C6.276 9 6.5 8.776 6.5 8.5 L6.5 7.805 C6.734 7.599 6.816 7.27 6.704 6.978 C6.593 6.687 6.312 6.496 6 6.5 Z M8.5 4.5 L4.5 4.5 L4.5 3.5 C4.499 2.893 4.863 2.345 5.424 2.111 C5.984 1.878 6.63 2.006 7.06 2.435 C7.248 2.627 7.382 2.865 7.45 3.125 C7.519 3.393 7.792 3.554 8.06 3.485 C8.328 3.416 8.489 3.143 8.42 2.875 C8.306 2.442 8.08 2.047 7.765 1.73 C7.049 1.017 5.975 0.804 5.042 1.191 C4.109 1.579 3.5 2.49 3.5 3.5 L3.5 4.5 C2.672 4.5 2 5.172 2 6 L2 9.5 C2 10.328 2.672 11 3.5 11 L8.5 11 C9.328 11 10 10.328 10 9.5 L10 6 C10 5.172 9.328 4.5 8.5 4.5 Z M9 9.5 C9 9.776 8.776 10 8.5 10 L3.5 10 C3.224 10 3 9.776 3 9.5 L3 6 C3 5.724 3.224 5.5 3.5 5.5 L8.5 5.5 C8.776 5.5 9 5.724 9 6 Z" />)SVG";

void LauncherWindow::ensureLockGeometry() {
    if (lockClosedGeometry_ || !d2dFactory_.valid()) {
        return;
    }
    const float offset = (kPinButtonSize - kPinGlyphBox) / 2.0F;
    // 源 viewBox 是 12x12（Lucide 是 24x24），缩放系数按源尺寸算
    const float scale = kPinGlyphBox / 12.0F;
    platform::win32::SvgGlyph closed;
    if (!platform::win32::buildSvgGlyph(d2dFactory_.get(), kLockClosedMarkup,
                                        scale, offset, offset, &closed,
                                        true)) {
        return;
    }
    platform::win32::SvgGlyph open;
    if (!platform::win32::buildSvgGlyph(d2dFactory_.get(), kLockOpenMarkup,
                                        scale, offset, offset, &open, true)) {
        return;
    }
    lockClosedGeometry_ = closed.fillGeometry;
    lockOpenGeometry_ = open.fillGeometry;
}

// 绘制锁定按钮：解锁 = 暗色空心（常态不抢注意力），锁定 = 亮色实心加
// 常显底衬（锁定状态必须一直可见，不能只靠悬停发现）。
void LauncherWindow::paintLockButton() {
    if (!renderTarget_ || !brush_ || !borderBrush_) {
        return;
    }

    const D2D1_RECT_F rect = lockButtonRect();
    ensureLockGeometry();

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    if (tilesLocked_ || lockHovered_) {
        const D2D1_ROUNDED_RECT back = D2D1::RoundedRect(
            D2D1::RectF(rect.left - 1.0F, rect.top - 1.0F,
                        rect.right + 1.0F, rect.bottom + 1.0F),
            3.0F, 3.0F);
        brush_->SetColor(colorFromArgb(
            tilesLocked_ ? theme_.pinActiveBackdrop()
                         : theme_.pinHoverBackdrop()));
        renderTarget_->FillRoundedRectangle(back, brush_.get());
    }

    // 两枚图标本身是实心剪影（锁定 = 闭合锁 / 解锁 = 开口锁），
    // 直接按状态选一个填充绘制，不再区分描边/实心两套几何。
    ID2D1PathGeometry* glyph = tilesLocked_ ? lockClosedGeometry_.get()
                                            : lockOpenGeometry_.get();
    if (glyph != nullptr) {
        borderBrush_->SetColor(colorFromArgb(
            tilesLocked_ ? theme_.pinActive() : theme_.pinIdle()));
        renderTarget_->SetTransform(
            D2D1::Matrix3x2F::Translation(rect.left, rect.top));
        renderTarget_->FillGeometry(glyph, borderBrush_.get(), nullptr);
    }

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
}

// --- 搜索框

// 搜索框矩形（客户区 DIP）：标题带内、锁定按钮左侧，左边界与内容区左
// 边缘对齐。绘制与命中测试共用本函数。
D2D1_RECT_F LauncherWindow::searchBoxRect() const {
    // 右边界顶到锁定按钮（不再直接顶到钉子）：否则两控件重叠
    const D2D1_RECT_F lockRect = lockButtonRect();
    const float right = lockRect.left - kSearchBoxRightGap;
    const float left = metrics_.outerPadding;
    const float top = metrics_.outerPadding + kHeaderContentTopInset;
    return D2D1::RectF(left, top, right, top + kSearchBoxHeight);
}

bool LauncherWindow::hitTestSearchBox(const float xDip,
                                      const float yDip) const {
    const D2D1_RECT_F rect = searchBoxRect();
    return xDip >= rect.left && xDip < rect.right &&
           yDip >= rect.top && yDip < rect.bottom;
}

// 搜索框内左端的应用图标矩形（客户区 DIP，与搜索框垂直居中）。
D2D1_RECT_F LauncherWindow::searchIconRect() const {
    const D2D1_RECT_F box = searchBoxRect();
    const float left = box.left + kSearchIconLeftGap;
    const float top =
        box.top + (kSearchBoxHeight - kSearchIconSize) / 2.0F;
    return D2D1::RectF(left, top, left + kSearchIconSize,
                       top + kSearchIconSize);
}

bool LauncherWindow::hitTestSearchIcon(const float xDip,
                                       const float yDip) const {
    const D2D1_RECT_F rect = searchIconRect();
    return xDip >= rect.left && xDip < rect.right &&
           yDip >= rect.top && yDip < rect.bottom;
}

// 从 exe 资源取应用图标并转成 D2D 位图。源尺寸 64px：界面最多显示
// 16 DIP，64 降采样比 32 更锐利。位图依附渲染目标，设备重建时置空。
ID2D1Bitmap* LauncherWindow::acquireAppIconBitmap() {
    if (appIconBitmap_) {
        return appIconBitmap_.get();
    }
    ID2D1RenderTarget* host =
        (hwndTarget_.get() != nullptr) ? hwndTarget_.get() : renderTarget_;
    if (host == nullptr || instance_ == nullptr) {
        return nullptr;
    }

    const HICON icon = static_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(
                       platform::win32::kApplicationIconResourceId),
        IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR));
    if (icon == nullptr) {
        return nullptr;
    }

    platform::win32::ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(wicFactory.put()));
    if (SUCCEEDED(hr)) {
        platform::win32::ComPtr<IWICBitmap> wicBitmap;
        hr = wicFactory->CreateBitmapFromHICON(icon, wicBitmap.put());
        if (SUCCEEDED(hr)) {
            // 与磁贴图标同一条链路：HICON 出来的是非预乘 BGRA，
            // HwndRenderTarget 只认预乘，必须转一次。
            platform::win32::ComPtr<IWICFormatConverter> converter;
            hr = wicFactory->CreateFormatConverter(converter.put());
            if (SUCCEEDED(hr)) {
                hr = converter->Initialize(
                    wicBitmap.get(), GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.0F,
                    WICBitmapPaletteTypeCustom);
            }
            if (SUCCEEDED(hr)) {
                hr = host->CreateBitmapFromWicBitmap(
                    converter.get(), nullptr, appIconBitmap_.put());
            }
        }
    }
    DestroyIcon(icon);
    return appIconBitmap_.get();
}

void LauncherWindow::paintSearchIcon() {
    if (!renderTarget_) {
        return;
    }
    // 标题带太窄（窗口被拉到极窄）时图标会顶到框右壁，干脆不画
    const D2D1_RECT_F box = searchBoxRect();
    if (box.right < searchIconRect().right + kSearchIconTextGap + 4.0F) {
        return;
    }
    ID2D1Bitmap* bitmap = acquireAppIconBitmap();
    if (bitmap == nullptr) {
        return;
    }
    renderTarget_->DrawBitmap(bitmap, searchIconRect(), 1.0F,
                              D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

// 绘制搜索框外框与占位提示。文本/光标/IME 全部由子 Edit 控件负责
//（手画方案在中文输入上问题不断，已否决）；占位提示仅在编辑框隐藏时画。
void LauncherWindow::paintSearchBox() {
    if (!renderTarget_ || !brush_ || !borderBrush_ || !textBrush_ ||
        searchFormat_.get() == nullptr) {
        return;
    }

    const D2D1_RECT_F rect = searchBoxRect();
    if (rect.right <= rect.left + 8.0F) {
        return;  // 标题带太窄，放不下就不画（避免负宽度引起 D2D 报错）
    }

    ensureSearchEditResources();
    const float scale = dpiScale();

    // ---- 编辑框就位（框内留出描边宽度与左侧图标位）----
    const D2D1_RECT_F iconRect = searchIconRect();
    const float editLeft = iconRect.right + kSearchIconTextGap;
    if (searchEdit_ != nullptr) {
        const int ex = static_cast<int>(std::lround(editLeft * scale));
        const int ey = static_cast<int>(
            std::lround((rect.top + 1.5F) * scale));
        const int ew = static_cast<int>(std::lround(
            (rect.right - 1.5F - editLeft) * scale));
        const int eh = static_cast<int>(
            std::lround((rect.bottom - rect.top - 3.0F) * scale));
        RECT cur{};
        GetWindowRect(searchEdit_, &cur);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&cur), 2);
        if (cur.left != ex || cur.top != ey ||
            cur.right - cur.left != ew || cur.bottom - cur.top != eh) {
            MoveWindow(searchEdit_, ex, ey, ew, eh, TRUE);
        }
    }

    const bool focused =
        searchEdit_ != nullptr && GetFocus() == searchEdit_;

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    // ---- 底衬 ----
    const D2D1_ROUNDED_RECT back = D2D1::RoundedRect(
        rect, kSearchBoxRadius, kSearchBoxRadius);
    brush_->SetColor(colorFromArgb(
        focused ? theme_.searchBoxActiveFill() : theme_.searchBoxFill()));
    renderTarget_->FillRoundedRectangle(back, brush_.get());

    // ---- 描边（聚焦时用强调蓝，明确「键盘输入会进这里」） ----
    borderBrush_->SetColor(colorFromArgb(
        focused ? theme_.searchBoxActiveBorder() : theme_.searchBoxBorder()));
    renderTarget_->DrawRoundedRectangle(back, borderBrush_.get(),
                                        kSearchBoxStrokeWidth);

    // ---- 左端应用图标（点击 = 关于）----
    // 画在框内：标题带左侧没有多余空间，也形成品牌锚点。
    paintSearchIcon();

    // ---- 占位提示：编辑框隐藏（无内容且无焦点）时才画得出来 ----
    // 查询非空但控件隐藏（如调试预置查询）时不画：那会盖住语义。
    if ((searchEdit_ == nullptr || !IsWindowVisible(searchEdit_)) &&
        searchQuery_.empty()) {
        const float textLeft = iconRect.right + kSearchIconTextGap;
        const float textRight = rect.right - 6.0F;
        if (textRight > textLeft) {
            textBrush_->SetColor(colorFromArgb(theme_.searchPlaceholder()));
            const wchar_t* placeholder = searchPlaceholder();
            renderTarget_->DrawTextW(
                placeholder,
                static_cast<UINT32>(wcslen(placeholder)),
                searchFormat_.get(),
                D2D1::RectF(textLeft, rect.top, textRight, rect.bottom),
                textBrush_.get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

// --- 搜索框 Edit 控件

// 创建子 Edit 控件并挂子类过程。
//
// 键盘导航键（Esc/回车/上下/翻页）在子类过程里转给面板的按键逻辑，
// 其余按键（字符、退格、Home/End 等）留给控件本身，那是编辑框的本职。
bool LauncherWindow::createSearchEdit(HINSTANCE instance) {
    searchEdit_ = CreateWindowExW(
        0, L"EDIT", nullptr,
        WS_CHILD | ES_AUTOHSCROLL | ES_NOHIDESEL,
        0, 0, 10, 10, hwnd_,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(kSearchEditId)),
        instance, nullptr);
    if (searchEdit_ == nullptr) {
        return false;
    }
    SendMessageW(searchEdit_, EM_SETMARGINS,
                 EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(1, 1));
    if (!SetWindowSubclass(searchEdit_, searchEditProc,
                           kSearchEditSubclassId,
                           reinterpret_cast<DWORD_PTR>(this))) {
        return false;
    }
    return true;
}

// Edit 的字体与背景刷。DPI 变了字体跟着重建；刷子颜色是搜索框底衬
// （10%/20% 白）叠在深色窗口背景上的合成结果，必须与面板画的外框一致。
void LauncherWindow::ensureSearchEditResources() {
    const int fontHeight =
        -static_cast<int>(std::lround(kSearchFontSize * dpiScale()));
    if (searchEditFont_ == nullptr ||
        searchEditFontHeight_ != fontHeight) {
        HFONT font = CreateFontW(
            fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");
        if (font != nullptr) {
            if (searchEditFont_ != nullptr) {
                DeleteObject(searchEditFont_);
            }
            searchEditFont_ = font;
            searchEditFontHeight_ = fontHeight;
            if (searchEdit_ != nullptr) {
                SendMessageW(searchEdit_, WM_SETFONT,
                             reinterpret_cast<WPARAM>(searchEditFont_),
                             TRUE);
            }
        }
    }
    if (searchEditBkBrush_ == nullptr) {
        searchEditBkBrush_ = CreateSolidBrush(
            searchEditBkColorRef(theme_, false));
    }
    if (searchEditBkActiveBrush_ == nullptr) {
        searchEditBkActiveBrush_ = CreateSolidBrush(
            searchEditBkColorRef(theme_, true));
    }
}

// EN_CHANGE 驱动：从 Edit 控件读回文本，更新过滤查询。
// searchEditSync_ 挡住反向同步（setSearchQuery 会把文本写回控件），
// 否则两边互相触发没有尽头。
void LauncherWindow::syncSearchFromEdit() {
    if (searchEditSync_ || searchEdit_ == nullptr) {
        return;
    }
    const int len = GetWindowTextLengthW(searchEdit_);
    std::wstring text(static_cast<std::size_t>(len) + 1, L'\0');
    GetWindowTextW(searchEdit_, &text[0], len + 1);
    text.resize(static_cast<std::size_t>(len));
    searchEditSync_ = true;
    setSearchQuery(base::wideToUtf8Lossy(text));
    searchEditSync_ = false;
}

LRESULT CALLBACK LauncherWindow::searchEditProc(HWND hwnd, UINT message,
                                                WPARAM wParam,
                                                LPARAM lParam,
                                                UINT_PTR /*subclassId*/,
                                                DWORD_PTR refData) {
    auto* self = reinterpret_cast<LauncherWindow*>(refData);
    if (message == WM_KEYDOWN) {
        // ---- Ctrl 组合键转给面板 ----
        // Edit 控件不会处理 Ctrl+Tab（切页快捷键），不转发会被当成插
        // 制表符或无效，用户以为标签快捷键坏了。
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (wParam == VK_TAB ||
             (wParam >= '1' && wParam <= '9'))) {
            if (self != nullptr) {
                self->handleKeyDown(static_cast<UINT>(wParam));
            }
            return 0;
        }

        switch (wParam) {
            case VK_ESCAPE:
            case VK_RETURN:
            case VK_UP:
            case VK_DOWN:
            case VK_PRIOR:
            case VK_NEXT:
                // 面板语义的按键：磁贴导航 / 回车启动 / Esc 清搜索或关窗
                if (self != nullptr) {
                    self->handleKeyDown(static_cast<UINT>(wParam));
                }
                return 0;
            default:
                break;
        }
    } else if (message == WM_CHAR) {
        // 回车/Esc 的字符形态：按键通路已处理，不能让它进文本
        if (wParam == 0x0D || wParam == 0x1B) {
            return 0;
        }
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

// --- 选中高亮

// 绘制键盘选中的高亮边框：强调色描边加极淡内衬。纯描边在深色磁贴上
// 不够跳，色块余光也能看见。
void LauncherWindow::paintSelectionFrame(const core::TilePlacement& placement) {
    if (!renderTarget_ || !brush_ || !borderBrush_) {
        return;
    }

    float left = metrics_.outerPadding +
        static_cast<float>(placement.column) * (metrics_.cellSize + metrics_.cellGap);
    float top = gridOriginY() +
        static_cast<float>(placement.row) * (metrics_.cellSize + metrics_.cellGap);

    // 叠加动画偏移，让选中框跟着被挤开的磁贴一起动
    float offsetX = 0.0F;
    float offsetY = 0.0F;
    currentOffset(placement.itemIndex, offsetX, offsetY);
    left += offsetX;
    top += offsetY;

    const float width = metrics_.tileWidth(placement.spanColumns);
    const float height = metrics_.tileHeight(placement.spanRows);

    const float inset = kSelectionFrameInset;
    const D2D1_ROUNDED_RECT frame = D2D1::RoundedRect(
        D2D1::RectF(left - inset, top - inset,
                    left + width + inset, top + height + inset),
        metrics_.cornerRadius + inset,
        metrics_.cornerRadius + inset);

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    brush_->SetColor(colorFromArgb(theme_.accentSubstrate()));
    renderTarget_->FillRoundedRectangle(frame, brush_.get());

    borderBrush_->SetColor(colorFromArgb(theme_.selectionFrame()));
    renderTarget_->DrawRoundedRectangle(frame, borderBrush_.get(),
                                        kSelectionFrameWidth);
}

// 绘制单块磁贴。asLifted = true 表示被拖起的那块：位置跟手（光标加
// 抓取偏移）、轻微放大加投影；其余按布局坐标加动画偏移绘制。
void LauncherWindow::paintSingleTile(const core::TilePlacement& placement,
                                     const bool asLifted,
                                     const bool dimmed) {
    const auto& items = activeItems();
    const auto& item = items[placement.itemIndex];
    const bool isSmall = (placement.spanColumns == 1 && placement.spanRows == 1);
    const bool isLarge = !isSmall;

    // ---- 淡化：不匹配搜索的磁贴 ----
    // 用 D2D Layer 统一降透明：磁贴内有多层颜色，逐个改 alpha 容易漏。
    // PushLayer 必须在 SetTransform 之前，PopLayer 在所有绘制之后，
    // 顺序错了会算错图层偏移或漏 pop。
    bool layerPushed = false;
    if (dimmed && !asLifted) {
        D2D1_LAYER_PARAMETERS layer{};
        layer.contentBounds = D2D1::InfiniteRect();
        layer.opacity = kSearchDimOpacity;
        renderTarget_->PushLayer(layer, nullptr);
        layerPushed = true;
    }

    // ---- 几何计算 ----
    float left = metrics_.outerPadding +
        static_cast<float>(placement.column) * (metrics_.cellSize + metrics_.cellGap);
    float top = gridOriginY() +
        static_cast<float>(placement.row) * (metrics_.cellSize + metrics_.cellGap);

    const float width = metrics_.tileWidth(placement.spanColumns);
    const float height = metrics_.tileHeight(placement.spanRows);

    float scale = 1.0F;
    float shadowAlpha = 0.0F;

    if (asLifted) {
        // 跟手：位置 = 光标（换算成 DIP）− 抓取偏移
        const float scaleFactor = dpiScale();
        const float cursorX =
            static_cast<float>(dragCurrentPoint_.x) / scaleFactor;
        const float cursorY =
            static_cast<float>(dragCurrentPoint_.y) / scaleFactor;
        left = cursorX - dragGrabOffsetX_;
        top = cursorY - dragGrabOffsetY_;

        // 轻微放大：明确「这块浮起来了」。绕中心的缩放由下文的矩阵完成。
        scale = kLiftedScale;
        shadowAlpha = kLiftedShadowAlpha;
    } else {
        // 普通磁贴：叠加动画偏移（挤开 / 回位）
        float offsetX = 0.0F;
        float offsetY = 0.0F;
        currentOffset(placement.itemIndex, offsetX, offsetY);
        left += offsetX;
        top += offsetY;
    }

    // ---- 浮起投影 ----
    // D2D 1.0 没有阴影效果（ID2D1Effect 是 Win8+），用偏移的深色圆角
    // 矩形模拟，Win7 可用。
    if (shadowAlpha > 0.0F) {
        const float spread = 3.0F;
        const D2D1_ROUNDED_RECT shadow = D2D1::RoundedRect(
            D2D1::RectF(left - spread + 2.0F, top - spread + 3.0F,
                        left + width + spread + 2.0F, top + height + spread + 3.0F),
            metrics_.cornerRadius,
            metrics_.cornerRadius);
        brush_->SetColor(D2D1::ColorF(0.0F, 0.0F, 0.0F, shadowAlpha));
        renderTarget_->FillRoundedRectangle(shadow, brush_.get());
    }

    // ---- 缩放矩阵：以磁贴中心为锚点放大 ----
    if (scale != 1.0F) {
        const float centerX = left + width / 2.0F;
        const float centerY = top + height / 2.0F;
        renderTarget_->SetTransform(
            D2D1::Matrix3x2F::Scale(scale, scale,
                                    D2D1::Point2F(centerX, centerY)));
    } else {
        renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
        D2D1::RectF(left, top, left + width, top + height),
        metrics_.cornerRadius,
        metrics_.cornerRadius);

    // ---- 背景色（自定义色 > 状态色 > 默认色） ----
    const bool hovered = hoveredItem_.has_value() &&
                         *hoveredItem_ == placement.itemIndex;
    const bool pressed = pressedItem_.has_value() &&
                         *pressedItem_ == placement.itemIndex;

    std::uint32_t background = resolveTileBackground(item, hovered, pressed);
    if (asLifted) {
        // 跟手磁贴：加深底色，强调它已脱离网格
        background = theme_.liftedFill();
    }

    brush_->SetColor(colorFromArgb(background));
    renderTarget_->FillRoundedRectangle(rounded, brush_.get());

    // ---- 边框 ----
    std::uint32_t borderColor = (palette_.border != 0)
        ? palette_.border
        : theme_.tileBorder;
    float borderWidth = 1.0F;
    if (asLifted) {
        borderColor = theme_.liftedBorder();
        borderWidth = 2.0F;
    } else if (hovered) {
        borderColor = (palette_.hoverBorder != 0)
            ? palette_.hoverBorder
            : theme_.tileHoverBorder;
    }
    borderBrush_->SetColor(colorFromArgb(borderColor));
    renderTarget_->DrawRoundedRectangle(rounded, borderBrush_.get(),
                                        borderWidth);

    // ---- 插件磁贴：内容区完全交给插件布局树 ----
    // 不画图标加名称标签那一套：布局树给出图标、文本与进度条，
    // 名称只是菜单里的标识，不占磁贴空间。
    if (item.type == core::ItemType::Plugin) {
        paintPluginTileContent(item, D2D1::RectF(left, top,
                                                 left + width, top + height));
        renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
        if (layerPushed) {
            renderTarget_->PopLayer();
        }
        return;
    }

    // ---- 标签显示策略 ----
    // 名称一律叠加在图标之上。总闸是「显示磁贴文字标签」，其下每块磁贴
    // 可选始终显示/移入显示/跟随全局：大小磁贴最优解不同，64x64 上标题
    // 盖图标，128x128 上留着好找。
    const bool isWide = (placement.spanColumns > 1 && placement.spanRows == 1);
    const bool isTall = (placement.spanColumns == 1 && placement.spanRows > 1);

    bool showLabel = !item.name.empty();

    if (!showTileLabels_) {
        showLabel = false;
    } else {
        bool alwaysShow = alwaysShowTileLabels_;
        switch (item.labelMode) {
            case core::TileLabelMode::Always:
                alwaysShow = true;
                break;
            case core::TileLabelMode::OnHover:
                alwaysShow = false;
                break;
            case core::TileLabelMode::FollowGlobal:
            default:
                break;
        }
        if (!alwaysShow) {
            showLabel = showLabel && (hovered || asLifted);
        }
    }

    // ---- 几何统一度量 ----
    // 统一规则：标签底衬贴住磁贴下边缘，图标在剩余区域内居中。混排时
    // 标题基线一致，图标缩放不会把标签带离底边。
    constexpr float labelHeight = 18.0F;
    constexpr float iconLabelGap = 3.0F;
    constexpr float iconTopGap = 3.0F;

    // 标签底衬：占据磁贴最下面一整条（绘制时下沿再内缩 1px 给边框）
    const float bottomLabelTop = top + height - labelHeight;

    D2D1_RECT_F iconRect = D2D1::RectF(left, top, left, top);
    float labelTop = bottomLabelTop;

    // 用户导入的自定义图片铺满整块磁贴内框：按原图比例存，比例不符时由
    // cover 取中段填满，不拉伸不留白。系统提取的图标是方图，走下方方框，
    // 否则会被拉成竖条。内置项图标也存成托管路径但属方图，需跟随图标
    // 大小设置缩放（builtinIcon 例外），否则「图标大小」滑块不生效。
    const bool builtinIcon =
        item.icon.has_value() &&
        [icon = base::utf8ToWide(*item.icon)] {
            const std::size_t slash = icon.find_last_of(L"\\/");
            const std::wstring filename = (slash == std::wstring::npos)
                                              ? icon
                                              : icon.substr(slash + 1);
            return filename.rfind(L"builtin-", 0) == 0;
        }();
    const bool fillTile =
        item.icon.has_value() &&
        platform::win32::isManagedIconPath(base::utf8ToWide(*item.icon)) &&
        !builtinIcon;

    // ---- 图标缩放系数 ----
    // 单项可覆盖（iconScalePercent，50~100），未覆盖时用全局值；
    // 100% 即内框铺满留 4px，缩小后仍居中。fillTile 铺满语义不变。
    int iconScaleValue = item.iconScalePercent > 0
        ? item.iconScalePercent
        : tileIconScalePercent_;
    iconScaleValue = std::clamp(iconScaleValue, 50, 100);
    const float iconFactor = static_cast<float>(iconScaleValue) / 100.0F;

    if (fillTile) {
        iconRect = D2D1::RectF(left + 2.0F, top + 2.0F,
                               left + width - 2.0F, top + height - 2.0F);
        labelTop = bottomLabelTop;
    } else if (isWide) {
        // 横向磁贴 (2x1)：图标上下居中，标签悬停时贴底浮出。
        const float base = std::min(height - iconTopGap * 2.0F, width - 8.0F);
        const float side = std::max(12.0F, base * iconFactor);
        const float iconLeft = left + (width - side) / 2.0F;
        const float iconTopPos = top + (height - side) / 2.0F;
        iconRect = D2D1::RectF(iconLeft, iconTopPos,
                               iconLeft + side, iconTopPos + side);
        labelTop = bottomLabelTop;
    } else if (isSmall) {
        // 小磁贴 (1x1)：图标铺满内框，标签悬浮其上（悬停才显示）。
        // 标签位置与 2x2 对齐：同样贴磁贴底边，而不是更靠上。
        const float base = std::min(width, height) - 4.0F;
        const float side = std::max(12.0F, base * iconFactor);
        // 缩小后居中：100% 时 (width-side)/2 == 2，与旧的固定 +2 一致
        const float iconLeft = left + (width - side) / 2.0F;
        const float iconTopPos = top + (height - side) / 2.0F;
        iconRect = D2D1::RectF(iconLeft, iconTopPos,
                               iconLeft + side, iconTopPos + side);
        labelTop = bottomLabelTop;
    } else if (isTall) {
        // 纵向磁贴 (1x2)：图标上下居中、横向居中，
        // 标签常驻贴底（与 2x2 基线一致）。
        const float base = std::min(width - 8.0F,
                     height - labelHeight - iconLabelGap - iconTopGap);
        const float side = std::max(12.0F, base * iconFactor);
        const float iconLeft = left + (width - side) / 2.0F;
        const float iconTopPos = top + iconTopGap +
            (height - iconTopGap - labelHeight -
             iconLabelGap - side) / 2.0F;
        iconRect = D2D1::RectF(iconLeft, iconTopPos,
                               iconLeft + side, iconTopPos + side);
        labelTop = bottomLabelTop;
    } else {
        // Large (2x2)：与其它尺寸同规则，标签贴底，图标在标签
        // 上方的剩余区域里上下左右居中。图标缩到 50% 时标签仍在
        // 底边不动，只是居中区域里的图标变小。
        const float maxIconHeight =
            height - iconTopGap - iconLabelGap - labelHeight;
        const float base = std::min(width - 8.0F, maxIconHeight);
        const float side = std::max(12.0F, base * iconFactor);
        const float iconTopPos = top + iconTopGap +
            (maxIconHeight - side) / 2.0F;
        iconRect = D2D1::RectF(left + (width - side) / 2.0F, iconTopPos,
                               left + (width + side) / 2.0F,
                               iconTopPos + side);
        labelTop = bottomLabelTop;
    }

    ID2D1Bitmap* iconBitmap = acquireItemIcon(item);
    if (iconBitmap != nullptr) {
        // 源矩形按覆盖加居中裁剪取：宽高比不符时裁掉两边而非拉伸留白；
        // 正方形源图配正方形目标框时为恒等变换。
        D2D1_RECT_F sourceRect = D2D1::RectF();
        const D2D1_SIZE_U sourceSize = iconBitmap->GetPixelSize();
        const float destinationWidth = iconRect.right - iconRect.left;
        const float destinationHeight = iconRect.bottom - iconRect.top;
        if (sourceSize.width > 0 && sourceSize.height > 0 &&
            destinationWidth > 0.0F && destinationHeight > 0.0F) {
            const float sourceWidth = static_cast<float>(sourceSize.width);
            const float sourceHeight = static_cast<float>(sourceSize.height);
            const float scale = (std::max)(
                destinationWidth / sourceWidth,
                destinationHeight / sourceHeight);
            float cropWidth = (std::min)(destinationWidth / scale, sourceWidth);
            float cropHeight =
                (std::min)(destinationHeight / scale, sourceHeight);
            const float cropLeft = (sourceWidth - cropWidth) * 0.5F;
            const float cropTop = (sourceHeight - cropHeight) * 0.5F;
            sourceRect = D2D1::RectF(cropLeft, cropTop, cropLeft + cropWidth,
                                     cropTop + cropHeight);
        }

        // 常态图标降到 70% 透明度：导入的 PNG 多带不透明白底，全不透明
        // 会盖住背景图；悬停/按下/跟手时恢复全不透明，兼作状态反馈。
        // 磁贴底色（tileFill）本来就是半透明玻璃，这里只管图标位图。
        const float iconOpacity =
            (hovered || pressed || asLifted) ? 1.0F : 0.70F;

        renderTarget_->DrawBitmap(
            iconBitmap,
            iconRect,
            iconOpacity,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
            sourceRect);
    } else {
        // 兜底：白底圆角底衬 + 首字母大字（保证磁贴一眼可辨）
        drawFallbackGlyph(item, iconRect);
    }

    // ---- 文字标签 ----
    // 全尺寸统一走底部标签条：紧贴图标下边缘（2x1 图标上下居中时，
    // 标签依然贴磁贴底边，与 2x2 基线一致）。
    if (showLabel) {
        const std::wstring label = base::utf8ToWide(item.name);
        textBrush_->SetColor(colorFromArgb(
            palette_.labelText != 0 ? palette_.labelText
                                    : theme_.labelText()));

        // 标签底衬：半透明黑衬底把白字从亮色图标里拉出来，悬停加深作
        // 状态反馈。全尺寸统一字号：1x1/1x2 用小号曾与 2x2 基线对不齐。
        IDWriteTextFormat* format = labelFormatLarge_.get();

        const float scrimBottom =
            std::min(labelTop + labelHeight, top + height - 1.0F);

        brush_->SetColor(colorFromArgb(
            (hovered || asLifted) ? theme_.labelScrimHover()
                                  : theme_.labelScrim()));
        renderTarget_->FillRectangle(
            D2D1::RectF(left + 1.0F, labelTop,
                        left + width - 1.0F, scrimBottom),
            brush_.get());

        // ---- 超宽滚动（跑马灯）----
        // 先测完整文本宽度：放得下就照旧；放不下且正在悬停，这块磁贴
        // 就接管跑马灯（同一时刻只有一块在滚）。测量只在悬停期间发生，
        // 每帧一次 CreateTextLayout 与标签条测宽同一成本，可接受。
        const float textLeft = left + 4.0F;
        const float textRight = left + width - 4.0F;
        const float textAvailable = std::max(1.0F, textRight - textLeft);

        float fullTextWidth = 0.0F;
        if (dwriteFactory_.get() != nullptr) {
            platform::win32::ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                    label.c_str(), static_cast<UINT32>(label.size()), format,
                    4096.0F, labelHeight, layout.put()))) {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                    fullTextWidth = metrics.widthIncludingTrailingWhitespace;
                }
            }
        }
        const bool overflow = fullTextWidth > textAvailable + 0.5F;

        float drawOffset = 0.0F;
        if (marqueeItemIndex_.has_value() &&
            *marqueeItemIndex_ == placement.itemIndex) {
            if (!hovered || !overflow) {
                stopLabelMarquee();  // 悬停离开 / 文本变短：停
            } else {
                drawOffset = marqueeOffset_;
            }
        } else if (hovered && overflow) {
            // 接管跑马灯：从自然位置起步，先停一拍再往左滚
            marqueeItemIndex_ = placement.itemIndex;
            marqueeOverflow_ = fullTextWidth - textAvailable;
            marqueeOffset_ = 0.0F;
            marqueeDirection_ = 1.0F;
            marqueePause_ = kMarqueePauseTicks;
            SetTimer(hwnd_, kMarqueeTimerId, kMarqueeIntervalMs, nullptr);
        }

        if (drawOffset > 0.0F) {
            // 滚动中：裁剪到标签文字区，文本矩形随偏移左移
            renderTarget_->PushAxisAlignedClip(
                D2D1::RectF(textLeft, labelTop, textRight, scrimBottom),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            renderTarget_->DrawTextW(
                label.c_str(),
                static_cast<UINT32>(label.size()),
                format,
                D2D1::RectF(textLeft - drawOffset, labelTop,
                            textLeft - drawOffset + fullTextWidth + 8.0F,
                            scrimBottom),
                textBrush_.get(),
                D2D1_DRAW_TEXT_OPTIONS_NONE);
            renderTarget_->PopAxisAlignedClip();
        } else {
            renderTarget_->DrawTextW(
                label.c_str(),
                static_cast<UINT32>(label.size()),
                format,
                D2D1::RectF(textLeft, labelTop, textRight, scrimBottom),
                textBrush_.get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    // 还原变换，避免影响下一块磁贴
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    // 结束淡化图层（必须在所有绘制完成之后）
    if (layerPushed) {
        renderTarget_->PopLayer();
    }
}

// --- 插件磁贴
// 把 app 层的 PluginView 翻译成 core::PluginDrawList 再逐条画出。
// 排版不放 UI：各插件的差异必须由 manifest 描述，且 core 逻辑可脱离
// D2D 单测（tests/test_plugin_layout.cpp）。
void LauncherWindow::paintPluginTileContent(const core::LaunchItem& item,
                                            const D2D1_RECT_F& rect) {
    if (renderTarget_ == nullptr || !brush_.valid() ||
        !textBrush_.valid()) {
        return;
    }

    core::PluginView view;
    if (pluginViewProvider_) {
        view = pluginViewProvider_(item);
    }

    // 没有 manifest 或没有可画的数据：统一走占位视图。
    // 这里不能退化成「普通磁贴」画法：插件磁贴没有可执行目标，
    // 画个空图标加名字会让用户以为点了没反应。
    if (!view.manifest || !view.hasData || !view.snapshot) {
        paintPluginPlaceholder(view, rect);
        return;
    }

    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    const int tileWidth = static_cast<int>(std::lround(width));
    const int tileHeight = static_cast<int>(std::lround(height));

    const core::PluginLayoutSpec* spec =
        core::selectPluginLayout(*view.manifest, tileWidth, tileHeight);
    if (spec == nullptr) {
        paintPluginPlaceholder(view, rect);
        return;
    }

    auto measure = [this](const std::string& text, float fontSize) {
        return measurePluginText(text, fontSize);
    };
    const core::PluginDrawList list = core::buildPluginDrawList(
        *view.manifest, view.snapshot->data, spec->root, rect.left, rect.top,
        width, height, measure);

    for (const core::PluginDrawCommand& command : list.commands) {
        switch (command.kind) {
            case core::PluginDrawKind::Text: {
                IDWriteTextFormat* format = pluginTextFormat(command.fontSize);
                if (format == nullptr) {
                    break;
                }
                std::uint32_t color = theme_.text;
                if (command.role == core::PluginTextRole::Sub) {
                    color = withAlpha(theme_.text, 0xCC);
                } else if (command.role == core::PluginTextRole::Footer) {
                    color = withAlpha(theme_.text, 0x99);
                }
                const std::wstring text = base::utf8ToWide(command.text);
                textBrush_->SetColor(colorFromArgb(color));
                renderTarget_->DrawTextW(
                    text.c_str(), static_cast<UINT32>(text.size()), format,
                    D2D1::RectF(command.left, command.top, command.right,
                                command.bottom),
                    textBrush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                break;
            }
            case core::PluginDrawKind::Icon: {
                drawPluginIcon(command.icon, command.iconFallback,
                               view.manifest->directory, renderTarget_,
                               borderBrush_.get(),
                               D2D1::RectF(command.left, command.top,
                                           command.right, command.bottom),
                               withAlpha(theme_.text, 0xE6));
                break;
            }
            case core::PluginDrawKind::Meter: {
                const float meterHeight = command.bottom - command.top;
                if (meterHeight <= 0.0F) {
                    break;
                }
                const float radius = meterHeight * 0.5F;
                const D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
                    D2D1::RectF(command.left, command.top, command.right,
                                command.bottom),
                    radius, radius);
                brush_->SetColor(colorFromArgb(withAlpha(theme_.text, 0x26)));
                renderTarget_->FillRoundedRectangle(track, brush_.get());

                const float fraction = std::clamp(command.fraction, 0.0F, 1.0F);
                if (fraction > 0.0F) {
                    const float fillRight = command.left +
                        (command.right - command.left) * fraction;
                    const D2D1_ROUNDED_RECT fill = D2D1::RoundedRect(
                        D2D1::RectF(command.left, command.top,
                                    std::max(command.left + meterHeight,
                                             fillRight),
                                    command.bottom),
                        radius, radius);
                    brush_->SetColor(colorFromArgb(
                        command.color != 0 ? command.color : theme_.accent));
                    renderTarget_->FillRoundedRectangle(fill, brush_.get());
                }
                break;
            }
            default:
                break;
        }
    }
}

void LauncherWindow::paintPluginPlaceholder(const core::PluginView& view,
                                            const D2D1_RECT_F& rect) {
    if (renderTarget_ == nullptr || !textBrush_.valid()) {
        return;
    }

    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    if (width <= 0.0F || height <= 0.0F) {
        return;
    }

    // 图标：用插件自己的缺省图标；插件不存在时没有图标可画（只在文字里说明）。
    const std::string iconId =
        view.manifest ? view.manifest->icon : std::string();

    const float iconSize = std::clamp(std::min(width, height) * 0.40F,
                                      16.0F, 44.0F);
    const float textSize = std::clamp(std::min(width, height) * 0.14F,
                                      8.0F, 11.0F);
    const float textHeight = textSize * 1.4F;
    const float gap = 4.0F;
    const float blockHeight = iconSize + gap + textHeight;
    const float iconTop = rect.top + (height - blockHeight) * 0.5F;
    const float iconLeft = rect.left + (width - iconSize) * 0.5F;

    std::string label;
    std::uint32_t labelColor = withAlpha(theme_.text, 0x99);
    if (!view.manifest) {
        label = infrastructure::i18n::tr("launcher.pluginNotFound");
        labelColor = withAlpha(theme_.text, 0x99);
    } else if (view.loading) {
        label = infrastructure::i18n::tr("launcher.loading");
    } else if (!view.error.empty()) {
        label = infrastructure::i18n::tr("launcher.dataUnavailable");
        labelColor = withAlpha(theme_.text, 0x99);
    } else {
        label = infrastructure::i18n::tr("launcher.noData");
    }

    drawPluginIcon(iconId, std::string(),
                   view.manifest ? view.manifest->directory : std::string(),
                   renderTarget_, borderBrush_.get(),
                   D2D1::RectF(iconLeft, iconTop, iconLeft + iconSize,
                               iconTop + iconSize),
                   withAlpha(theme_.text, 0x66));

    IDWriteTextFormat* format = pluginTextFormat(textSize);
    if (format == nullptr) {
        return;
    }
    // 没有图标可画时把文字放到磁贴正中，避免留下上半屏空白。
    const float textTop = iconId.empty()
                              ? rect.top + (height - textHeight) * 0.5F
                              : iconTop + iconSize + gap;
    const std::wstring text = base::utf8ToWide(label);
    textBrush_->SetColor(colorFromArgb(labelColor));
    renderTarget_->DrawTextW(
        text.c_str(), static_cast<UINT32>(text.size()), format,
        D2D1::RectF(rect.left, textTop, rect.right, textTop + textHeight),
        textBrush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// 编辑对话框预览：与主窗口完全相同的绘制路径，预览即真实效果；
// 差别只在渲染资源：DCRenderTarget 加自己的笔刷（D2D 1.0 笔刷不能跨目标）。
bool LauncherWindow::renderPluginPreview(HDC dc, const RECT& target,
                                         const core::PluginView& view,
                                         const core::TileSize size) {
    if (dc == nullptr || !d2dFactory_.valid() || !dwriteFactory_.valid()) {
        return false;
    }
    // 对话框的磁贴矩形是物理像素，而布局/字体是 DIP：先把渲染目标
    // 固定到屏幕 DPI，再把像素尺寸换算成 DIP，预览与真实磁贴才同比例。
    const int deviceDpi = GetDeviceCaps(dc, LOGPIXELSX);
    const float dpi = (deviceDpi > 0) ? static_cast<float>(deviceDpi) : 96.0F;
    const float dpiScale = dpi / 96.0F;

    const float width = static_cast<float>(target.right - target.left) / dpiScale;
    const float height = static_cast<float>(target.bottom - target.top) / dpiScale;
    if (width < 4.0F || height < 4.0F) {
        return false;
    }

    if (!previewTarget_.valid()) {
        D2D1_RENDER_TARGET_PROPERTIES properties =
            D2D1::RenderTargetProperties();
        properties.dpiX = dpi;
        properties.dpiY = dpi;
        properties.pixelFormat = D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        if (FAILED(d2dFactory_->CreateDCRenderTarget(&properties,
                                                     previewTarget_.put()))) {
            return false;
        }
        if (FAILED(previewTarget_->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::White), previewBrush_.put()))) {
            previewTarget_.reset();
            return false;
        }
    }
    // 每次绘制都同步 DPI：用户可能把窗口拖到另一块不同缩放的屏幕。
    previewTarget_->SetDpi(dpi, dpi);

    // 绑定的矩形就是绘制原点：内容坐标从 (0,0) 起（单位为 DIP）。
    const RECT bindRect = target;
    if (FAILED(previewTarget_->BindDC(dc, &bindRect))) {
        return false;
    }

    previewTarget_->BeginDraw();
    previewTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    // 用逻辑尺寸选布局：对话框会把大磁贴等比缩小放进预览区，
    // 缩小后的像素尺寸不能拿来判断 small/large。
    const int logicalWidth = 64 * core::tileSpanColumns(size);
    const int logicalHeight = 64 * core::tileSpanRows(size);

    const core::PluginLayoutSpec* spec = nullptr;
    if (view.manifest) {
        spec = core::selectPluginLayout(*view.manifest, logicalWidth,
                                       logicalHeight);
    }

    const bool drawable =
        view.manifest && view.hasData && view.snapshot && spec != nullptr;
    if (!drawable) {
        // ---- 占位：插件图标 + 一行状态（与 paintPluginPlaceholder 同观感）----
        const std::string iconId =
            view.manifest ? view.manifest->icon : std::string();
        const float iconSize = std::clamp(std::min(width, height) * 0.40F,
                                          16.0F, 44.0F);
        const float textSize = std::clamp(std::min(width, height) * 0.14F,
                                          8.0F, 11.0F);
        const float textHeight = textSize * 1.4F;
        const float gap = 4.0F;
        const float blockHeight = iconSize + gap + textHeight;
        const float iconTop = (height - blockHeight) * 0.5F;
        const float iconLeft = (width - iconSize) * 0.5F;

        std::string label = infrastructure::i18n::tr("launcher.noData");
        if (!view.manifest) {
            label = infrastructure::i18n::tr("launcher.pluginNotFound");
        } else if (view.loading) {
            label = infrastructure::i18n::tr("launcher.loading");
        } else if (!view.error.empty()) {
            label = infrastructure::i18n::tr("launcher.dataUnavailable");
        }

        drawPluginIcon(iconId, std::string(),
                       view.manifest ? view.manifest->directory : std::string(),
                       previewTarget_.get(), previewBrush_.get(),
                       D2D1::RectF(iconLeft, iconTop, iconLeft + iconSize,
                                   iconTop + iconSize),
                       withAlpha(theme_.text, 0x66));

        IDWriteTextFormat* format = pluginTextFormat(textSize);
        if (format != nullptr) {
            const float textTop = iconId.empty()
                                      ? (height - textHeight) * 0.5F
                                      : iconTop + iconSize + gap;
            const std::wstring text = base::utf8ToWide(label);
            previewBrush_->SetColor(colorFromArgb(withAlpha(theme_.text, 0x99)));
            previewTarget_->DrawTextW(
                text.c_str(), static_cast<UINT32>(text.size()), format,
                D2D1::RectF(0.0F, textTop, width, textTop + textHeight),
                previewBrush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    } else {
        auto measure = [this](const std::string& text, float fontSize) {
            return measurePluginText(text, fontSize);
        };
        const core::PluginDrawList list = core::buildPluginDrawList(
            *view.manifest, view.snapshot->data, spec->root, 0.0F, 0.0F,
            width, height, measure);

        for (const core::PluginDrawCommand& command : list.commands) {
            switch (command.kind) {
                case core::PluginDrawKind::Text: {
                    IDWriteTextFormat* format = pluginTextFormat(command.fontSize);
                    if (format == nullptr) {
                        break;
                    }
                    std::uint32_t color = theme_.text;
                    if (command.role == core::PluginTextRole::Sub) {
                        color = withAlpha(theme_.text, 0xCC);
                    } else if (command.role == core::PluginTextRole::Footer) {
                        color = withAlpha(theme_.text, 0x99);
                    }
                    const std::wstring text = base::utf8ToWide(command.text);
                    previewBrush_->SetColor(colorFromArgb(color));
                    previewTarget_->DrawTextW(
                        text.c_str(), static_cast<UINT32>(text.size()), format,
                        D2D1::RectF(command.left, command.top, command.right,
                                    command.bottom),
                        previewBrush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    break;
                }
                case core::PluginDrawKind::Icon: {
                    drawPluginIcon(command.icon, command.iconFallback,
                                   view.manifest->directory,
                                   previewTarget_.get(), previewBrush_.get(),
                                   D2D1::RectF(command.left, command.top,
                                               command.right, command.bottom),
                                   withAlpha(theme_.text, 0xE6));
                    break;
                }
                case core::PluginDrawKind::Meter: {
                    const float meterHeight = command.bottom - command.top;
                    if (meterHeight <= 0.0F) {
                        break;
                    }
                    const float radius = meterHeight * 0.5F;
                    const D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
                        D2D1::RectF(command.left, command.top, command.right,
                                    command.bottom),
                        radius, radius);
                    previewBrush_->SetColor(
                        colorFromArgb(withAlpha(theme_.text, 0x26)));
                    previewTarget_->FillRoundedRectangle(track,
                                                         previewBrush_.get());

                    const float fraction = std::clamp(command.fraction, 0.0F, 1.0F);
                    if (fraction > 0.0F) {
                        const float fillRight = command.left +
                            (command.right - command.left) * fraction;
                        const D2D1_ROUNDED_RECT fill = D2D1::RoundedRect(
                            D2D1::RectF(command.left, command.top,
                                        std::max(command.left + meterHeight,
                                                 fillRight),
                                        command.bottom),
                            radius, radius);
                        previewBrush_->SetColor(colorFromArgb(
                            command.color != 0 ? command.color : theme_.accent));
                        previewTarget_->FillRoundedRectangle(fill,
                                                             previewBrush_.get());
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    previewTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
    return SUCCEEDED(previewTarget_->EndDraw());
}

IDWriteTextFormat* LauncherWindow::pluginTextFormat(const float fontSize) {
    if (!dwriteFactory_.valid()) {
        return nullptr;
    }
    const float clamped = std::clamp(fontSize, 7.0F, 32.0F);
    const int key = static_cast<int>(clamped * 10.0F + 0.5F);
    const auto cached = pluginTextFormats_.find(key);
    if (cached != pluginTextFormats_.end()) {
        return cached->second.get();
    }

    platform::win32::ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwriteFactory_->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, clamped,
            L"zh-CN", format.put()))) {
        return nullptr;
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    auto inserted = pluginTextFormats_.emplace(key, format);
    return inserted.first->second.get();
}

float LauncherWindow::measurePluginText(const std::string& text,
                                        const float fontSize) {
    if (text.empty() || !dwriteFactory_.valid()) {
        return 0.0F;
    }
    IDWriteTextFormat* format = pluginTextFormat(fontSize);
    if (format == nullptr) {
        return 0.0F;
    }
    const std::wstring wide = base::utf8ToWide(text);
    platform::win32::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwriteFactory_->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()), format, 4096.0F,
            fontSize * 1.6F, layout.put()))) {
        return 0.0F;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return 0.0F;
    }
    return metrics.widthIncludingTrailingWhitespace;
}

namespace {

// 相对路径里出现 ".." 一律拒绝：插件清单可能来自第三方，
// 不允许它把图标路径指到插件目录之外。
bool hasParentTraversal(const std::filesystem::path& path) {
    for (const std::filesystem::path& part : path) {
        if (part == L"..") {
            return true;
        }
    }
    return false;
}

// 扩展名比较（大小写不敏感）
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

}  // namespace

void LauncherWindow::drawPluginIcon(const std::string& iconId,
                                    const std::string& fallbackId,
                                    const std::string& pluginDirectory,
                                    ID2D1RenderTarget* target,
                                    ID2D1SolidColorBrush* brush,
                                    const D2D1_RECT_F& rect,
                                    const std::uint32_t color) {
    if (target == nullptr || brush == nullptr) {
        return;
    }
    if (iconId.empty() && fallbackId.empty()) {
        return;
    }

    // 解析顺序：插件目录里的 .svg（支持局部彩色）→ 插件目录里的图片。
    // 图标资源跟着插件走：主程序不必、也不会背任何插件的图标。
    auto tryIcon = [&](const std::string& id) -> bool {
        if (id.empty()) {
            return false;
        }
        if (drawPluginSvgFileIcon(id, pluginDirectory, target, brush, rect,
                                  color)) {
            return true;
        }
        return drawPluginFileIcon(id, pluginDirectory, target, rect);
    };

    if (tryIcon(iconId)) {
        return;
    }
    // 数据值（例如天气 code "149"）不在 iconMap 里，或映射到了不存在的
    // 图标：退回 manifest.icon，避免磁贴留下一个空槽。
    if (fallbackId != iconId) {
        tryIcon(fallbackId);
    }
}

bool LauncherWindow::drawPluginSvgFileIcon(const std::string& storedPath,
                                           const std::string& pluginDirectory,
                                           ID2D1RenderTarget* target,
                                           ID2D1SolidColorBrush* brush,
                                           const D2D1_RECT_F& rect,
                                           const std::uint32_t color) {
    if (target == nullptr || !d2dFactory_.valid() || brush == nullptr ||
        storedPath.empty()) {
        return false;
    }

    std::filesystem::path candidate(base::utf8ToWide(storedPath));
    const bool relative = !candidate.is_absolute();
    if (relative && pluginDirectory.empty()) {
        return false;
    }
    if (hasParentTraversal(candidate)) {
        return false;
    }
    // 清单可以只写 "icons/cloud-sun" 而实际文件是 cloud-sun.svg；
    // 写出别的扩展名（如 .png）则不归这里管，交给图片路径。
    if (!candidate.has_extension()) {
        candidate += L".svg";
    }
    if (!hasExtensionCi(candidate, L".svg")) {
        return false;
    }

    const std::filesystem::path full =
        relative
            ? (std::filesystem::path(base::utf8ToWide(pluginDirectory)) /
               candidate)
            : candidate;

    const float size = std::min(rect.right - rect.left, rect.bottom - rect.top);
    if (size < 4.0F) {
        return false;
    }

    // 缓存键带上文件修改时间：用户替换插件目录里的 SVG 后，
    // 下一帧就能自动失效，不必重启。
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(full, ec);
    if (ec) {
        return false;  // 文件不存在
    }
    const int sizeKey = static_cast<int>(size * 2.0F + 0.5F);
    const std::string key =
        base::wideToUtf8(full.wstring()) + "|" + std::to_string(sizeKey) + "|" +
        std::to_string(
            static_cast<long long>(stamp.time_since_epoch().count()));

    auto cached = pluginIconCache_.find(key);
    if (cached == pluginIconCache_.end()) {
        std::wstring markup;
        if (!platform::win32::readSvgMarkupFile(full.wstring(), &markup)) {
            return false;
        }
        platform::win32::SvgGlyph glyph;
        // 约定 SVG 使用 24x24 用户单位（与 Lucide 一致）。
        const float scale = size / 24.0F;
        if (!platform::win32::buildSvgGlyph(d2dFactory_.get(), markup.c_str(),
                                            scale, 0.0F, 0.0F, &glyph)) {
            return false;
        }
        auto inserted = pluginIconCache_.emplace(key, std::move(glyph));
        if (!inserted.second) {
            return false;
        }
        cached = inserted.first;
    }

    drawPluginGlyph(cached->second, target, brush, rect, size, color);
    return true;
}

// 把解析好的几何画到 rect：填充/描边沿用调用方颜色，
// 显式颜色图层（如太阳的黄色）保留自身 RGB、只继承调用方的透明度。
void LauncherWindow::drawPluginGlyph(const platform::win32::SvgGlyph& glyph,
                                     ID2D1RenderTarget* target,
                                     ID2D1SolidColorBrush* brush,
                                     const D2D1_RECT_F& rect,
                                     const float size,
                                     const std::uint32_t color) {
    if (target == nullptr || brush == nullptr) {
        return;
    }
    if (!pluginStrokeStyle_.valid()) {
        pluginStrokeStyle_ =
            platform::win32::createRoundStrokeStyle(d2dFactory_.get());
    }

    const float strokeWidth = 2.0F * (size / 24.0F);
    target->SetTransform(
        D2D1::Matrix3x2F::Translation(rect.left, rect.top));

    brush->SetColor(colorFromArgb(color));
    if (glyph.fillGeometry.get() != nullptr) {
        target->FillGeometry(glyph.fillGeometry.get(), brush, nullptr);
    }
    if (glyph.strokeGeometry.get() != nullptr) {
        target->DrawGeometry(glyph.strokeGeometry.get(), brush, strokeWidth,
                             pluginStrokeStyle_.get());
    }
    for (const platform::win32::SvgColorLayer& layer : glyph.colorLayers) {
        if (layer.geometry.get() == nullptr || layer.color == 0) {
            continue;
        }
        const std::uint32_t layerColor =
            (layer.color & 0x00FFFFFFU) | (color & 0xFF000000U);
        brush->SetColor(colorFromArgb(layerColor));
        target->DrawGeometry(layer.geometry.get(), brush, strokeWidth,
                             pluginStrokeStyle_.get());
    }

    target->SetTransform(D2D1::Matrix3x2F::Identity());
}

bool LauncherWindow::drawPluginFileIcon(const std::string& storedPath,
                                        const std::string& pluginDirectory,
                                        ID2D1RenderTarget* target,
                                        const D2D1_RECT_F& rect) {
    if (target == nullptr || storedPath.empty()) {
        return false;
    }

    std::filesystem::path candidate(base::utf8ToWide(storedPath));
    if (!candidate.is_absolute()) {
        if (pluginDirectory.empty()) {
            return false;
        }
        candidate = std::filesystem::path(base::utf8ToWide(pluginDirectory)) /
                    candidate;
    }
    // 允许 manifest 只写 "sun" 而实际文件是 sun.png。
    if (!candidate.has_extension()) {
        candidate += L".png";
    }
    if (!platform::win32::isDecodableImageFile(candidate.wstring())) {
        return false;
    }

    // 位图依附于创建它的渲染目标：主窗口与预览各自一份缓存。
    auto& cache = (target == renderTarget_) ? pluginBitmapCache_
                                            : previewBitmapCache_;
    const std::wstring key = candidate.wstring();
    const auto cached = cache.find(key);
    if (cached == cache.end()) {
        platform::win32::ImagePixels pixels;
        platform::win32::ComPtr<ID2D1Bitmap> bitmap;
        if (platform::win32::decodeImageFile(key, &pixels) &&
            pixels.valid()) {
            const std::vector<std::uint32_t> premultiplied =
                platform::win32::premultiplyBgra(pixels);
            const D2D1_BITMAP_PROPERTIES properties =
                D2D1::BitmapProperties(D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_PREMULTIPLIED));
            target->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(pixels.width),
                            static_cast<UINT32>(pixels.height)),
                premultiplied.data(),
                static_cast<UINT32>(pixels.width) * 4U, properties,
                bitmap.put());
        }
        // 失败也写空条目：磁贴会一直显示缺图，但不必每帧重新读盘解码。
        auto inserted = cache.emplace(key, std::move(bitmap));
        if (!inserted.second || inserted.first->second.get() == nullptr) {
            return false;
        }
    }

    ID2D1Bitmap* bitmap = cache.find(key)->second.get();
    if (bitmap == nullptr) {
        return false;
    }

    const D2D1_SIZE_F size = bitmap->GetSize();
    if (size.width <= 0.0F || size.height <= 0.0F) {
        return false;
    }
    const float boxWidth = rect.right - rect.left;
    const float boxHeight = rect.bottom - rect.top;
    const float scale = std::min(boxWidth / size.width, boxHeight / size.height);
    const float width = size.width * scale;
    const float height = size.height * scale;
    const float left = rect.left + (boxWidth - width) * 0.5F;
    const float top = rect.top + (boxHeight - height) * 0.5F;
    target->DrawBitmap(
        bitmap, D2D1::RectF(left, top, left + width, top + height), 1.0F,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    return true;
}

void LauncherWindow::clearPluginGeometryCache() {
    pluginIconCache_.clear();
    pluginBitmapCache_.clear();
    previewBitmapCache_.clear();
    pluginStrokeStyle_.reset();
}

// 停掉跑马灯：清状态 + 杀定时器。悬停离开、文本变短、磁贴被
// 删除/重排时都会走到（paint 每帧自检，无需在各处散落清理）。
void LauncherWindow::stopLabelMarquee() {
    if (!marqueeItemIndex_.has_value()) {
        return;
    }
    marqueeItemIndex_.reset();
    marqueeOffset_ = 0.0F;
    marqueeDirection_ = 1.0F;
    marqueeOverflow_ = 0.0F;
    marqueePause_ = 0;
    KillTimer(hwnd_, kMarqueeTimerId);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// 跑马灯一帧：ping-pong 滚动，端点各停 kMarqueePauseTicks 帧让用户
// 看清头尾。整窗失效即可：面板小，30fps 全量重绘成本可忽略。
void LauncherWindow::advanceLabelMarquee() {
    if (!marqueeItemIndex_.has_value()) {
        KillTimer(hwnd_, kMarqueeTimerId);
        return;
    }
    if (marqueePause_ > 0) {
        --marqueePause_;
    } else {
        marqueeOffset_ += marqueeDirection_ * kMarqueeStepDip;
        if (marqueeOffset_ >= marqueeOverflow_) {
            marqueeOffset_ = marqueeOverflow_;
            marqueeDirection_ = -1.0F;
            marqueePause_ = kMarqueePauseTicks;
        } else if (marqueeOffset_ <= 0.0F) {
            marqueeOffset_ = 0.0F;
            marqueeDirection_ = 1.0F;
            marqueePause_ = kMarqueePauseTicks;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// 图标缺失时的兜底绘制：居中首字（CJK 取第一个汉字/字符）
void LauncherWindow::drawFallbackGlyph(const core::LaunchItem& item,
                                       const D2D1_RECT_F& rect) {
    const float side = std::min(rect.right - rect.left, rect.bottom - rect.top);
    if (side < 8.0F) {
        return;
    }

    // 图标缺失：用「强调色底衬 + 白色首字」填补，这块区域是纯色背景，
    // 高饱和强调色在这里是安全的（不涉及第三方图标的配色冲突）。
    const float radius = std::max(2.0F, side * 0.12F);
    brush_->SetColor(colorFromArgb(tileAccentColor(item)));
    renderTarget_->FillRoundedRectangle(
        D2D1::RoundedRect(rect, radius, radius), brush_.get());

    const std::wstring glyph = fallbackGlyphText(item);
    if (glyph.empty() || fallbackFormat_.get() == nullptr) {
        return;
    }

    // 格式按基准字号创建，用变换缩放到位，避免每帧重建文本格式。
    const float scale = (side * 0.62F) / kFallbackBaseFontSize;
    const float cx = (rect.left + rect.right) * 0.5F;
    const float cy = (rect.top + rect.bottom) * 0.5F;

    renderTarget_->SetTransform(
        D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(cx, cy)));

    textBrush_->SetColor(D2D1::ColorF(1.0F, 1.0F, 1.0F, 1.0F));
    renderTarget_->DrawTextW(
        glyph.c_str(),
        static_cast<UINT32>(glyph.size()),
        fallbackFormat_.get(),
        rect,
        textBrush_.get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP);

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
}

// 解析磁贴背景色：自定义色优先，否则用中性灰玻璃 + 状态提亮
std::uint32_t LauncherWindow::resolveTileBackground(const core::LaunchItem& item,
                                                    const bool hovered,
                                                    const bool pressed) const {
    // 优先级：单项色 > 全局默认色 > 皮肤/调色板。自定义色同样套用全局
    // 磁贴背景不透明度，否则换底色后透明度旋钮失灵；皮肤三档底色的
    // 浓淡差仍由 applyTileOpacityPercent 保留。
    const std::optional<std::uint32_t> customColor =
        item.tileColor.has_value() ? item.tileColor : defaultTileColor_;
    if (customColor.has_value()) {
        std::uint32_t lift = 0;
        if (pressed) {
            lift = 0x40U;
        } else if (hovered) {
            lift = 0x20U;
        }
        const std::uint32_t lifted = liftColor(*customColor, lift);
        if (tileOpacityPercent_ < 0) {
            return lifted;  // 跟随皮肤：自定义色保持不透明原样
        }
        // 不走 applyTileOpacityPercent：它以皮肤 tileFill 的 alpha 为基准
        // 保留差值，自定义色基准是 0xFF，套上去会凭空偏移，直接替换 alpha。
        const std::uint32_t targetAlpha =
            static_cast<std::uint32_t>(tileOpacityPercent_) * 255U / 100U;
        return (lifted & 0x00FFFFFFU) | (targetAlpha << 24);
    }

    // 默认：中性深灰玻璃。图标多为浅色设计，中性底能保证对比度。
    std::uint32_t color = palette_.background != 0
                              ? palette_.background
                              : theme_.tileFill;
    if (pressed) {
        color = palette_.pressedBackground != 0
                    ? palette_.pressedBackground
                    : theme_.tilePressed;
    } else if (hovered) {
        color = palette_.hoverBackground != 0
                    ? palette_.hoverBackground
                    : theme_.tileHover;
    }
    return applyTileOpacityPercent(color);
}

}  // namespace ui
}  // namespace ktile
