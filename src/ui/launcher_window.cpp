// KTile - UI 层：启动器主窗口实现
#include "ui/launcher_window.h"

#include "base/text_convert.h"
#include "core/lucide_icons.h"
#include "platform/win32/icon_loader.h"
#include "platform/win32/icon_store.h"
#include "platform/win32/popup_menu.h"
#include "platform/win32/svg_path.h"
#include "platform/win32/tray_icon.h"  // kApplicationIconResourceId（应用图标）

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <commctrl.h>  // SetWindowSubclass（搜索框 Edit 子类化）

#include <wincodec.h>  // WIC：HICON -> 位图

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwctype>

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

// 默认配色已迁移到 ui/theme.h 的 ThemeColors（皮肤系统）。
// 磁贴底色：中性深灰玻璃 —— 对浅色/彩色图标都能保证对比度。
// hover / pressed 在此基础上提亮，反馈明显。

// 标签底衬（scrim）颜色由主题派生（ThemeColors::labelScrim）：

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

// 拖拽判定阈值（DIP）。
//
// 按下后移动超过该距离才升级为拖拽。目的是把「点击启动」与「拖动排序」
// 区分开：没有阈值的话，手抖一两像素就会把启动操作变成排序。
constexpr float kDragThresholdDip = 6.0F;

// 标签跑马灯：33ms 一帧（≈30fps）、每帧 0.8 DIP（约 24 DIP/秒）、
// 滚到两端各停 18 帧（约 600ms）让用户看清头尾。
constexpr UINT kMarqueeIntervalMs = 33;
constexpr float kMarqueeStepDip = 0.8F;
constexpr int kMarqueePauseTicks = 18;

// 落点防抖（毫秒）。
//
// 鼠标停在两格交界附近时，命中测试结果会在相邻磁贴间反复切换，
// 若每次都立刻重排，「挤开」动画会来回抖动。这里要求落点稳定
// 一小段时间后才确认，视觉上更安定。
constexpr std::uint64_t kDropDebounceMs = 80;

// 拖拽视觉反馈配色
//
// 落点占位框画在「被拖项将要占据的格子」上，而不是「光标下那块磁贴」上。
// 这是 Win10 的做法：用户看到的虚线框就是松手后磁贴的真实位置，
// 一目了然且不会误导（拖 2x2 到 1x1 上时框会明显更大）。
//
// 两套颜色各管一件事：
//   - 挪动已有磁贴 → 中性白虚线（「把那块搬到这里」）→ text 派生
//   - 拖入新项目   → 强调色（「这里会长出一块新的」）→ accent 派生
// 同一套几何、同一种虚线节奏，只是颜色不同 —— 学一次就够。
//（颜色改由 ThemeColors 派生，见 paintDropSpotFrame。）
constexpr float kDropTargetDashSpan = 6.0F;              // 虚线段长
constexpr float kDropTargetDashGap  = 4.0F;              // 虚线间隔
constexpr float kDropTargetBorderWidth = 1.6F;

// 跟手磁贴（被拿起的那块）。底色固定深黑（脱离网格感与主题无关），
// 描边跟主题文字走。
constexpr float kLiftedScale = 1.06F;                // 轻微放大
constexpr float kLiftedShadowAlpha = 0.38F;          // 投影强度

// 窗口背景色（深色，完全不透明）。
//
// 曾经写成 0xF0（94% 不透明）——这在「非分层窗口」上没有任何美学效果
//（GDI 呈现会忽略 alpha），却制造了一个极难排查的 bug：
// D2D 的 HWND 渲染目标不保证帧间清屏，每帧用半透明刷子 FillRectangle
// 等于把 94% 深色混合到**上一帧的画面**上，旧内容以 6% 持续透出。
// 平时重绘频繁（悬停/动画每帧都盖）看不出来；一旦某个状态切换后窗口
// 空闲（例如切标签页），残影就一直停在屏幕上，直到下一次重绘才被
// 逐帧稀释 —— 用户看到的就是「约 0.5 秒的旧页图标淡残影」。
constexpr std::uint32_t kWindowBackground = 0xFF1E1E1E;

// ---------------------------------------------------------------- 钉子按钮
//
// 位置与尺寸都在标题带内（见 pinButtonRect），所以钉子永远不会与磁贴重叠。
// 图形用 Lucide 的 pin（core/lucide_icons.h）描边绘制，不依赖字体字形 ——
// Win7 的 Segoe UI Symbol 对图钉字符 U+1F4CC 支持不稳定，用字符会出现
// 方框或空白；同时它与「添加系统项目」里那批图标是同一套线条语言。
constexpr float kPinButtonSize  = 22.0F;   // 命中/绘制方形边长
constexpr float kPinHitPadding  = 4.0F;    // 命中容差（向外扩）
constexpr float kPinBaselineNudge = 0.0F;  // 垂直微调：让图钉视觉重心居中
// Lucide 图形是 24x24：缩到 17.5 落在按钮内，线宽按同一比例换算（≈1.46），
// 和内置项图标的「线宽 = 2/24」保持同一视觉密度。
constexpr float kPinGlyphBox = 17.5F;
constexpr float kPinGlyphScale = kPinGlyphBox / 24.0F;
constexpr float kPinStrokeWidth = 2.0F * kPinGlyphScale;

// 锁定按钮（防误拖磁贴）：钉子左侧的小锁，尺寸/线宽/命中容差全部沿用
// 钉子的常量 —— 两个按钮并排，视觉密度必须一致。
constexpr float kLockButtonGap = 4.0F;  // 与钉子的间距

// 标题带内控件（搜索框 / 锁 / 钉子）距标题带顶部的固定留白。
//
// 刻意不用「垂直居中」：headerHeight 特意比控件高，多出来的空间全给下方，
// 这样搜索框上方的窗口边距可以收窄，而它与磁贴之间又能拉开距离。
constexpr float kHeaderContentTopInset = 2.0F;
// 钉子颜色改由 ThemeColors 派生（paintPinButton）。

// ---------------------------------------------------------------- 搜索框
//
// 位置：标题带内、钉子左侧，两者互不重叠（见 searchBoxRect 的右边界）。
// 之所以放在标题带：磁贴从 outerPadding + headerHeight 开始，标题带是
// 唯一恒为空白的区域，搜索框放这里永远不会盖住磁贴。
constexpr float kSearchBoxHeight = 22.0F;   // 与钉子等高的视觉节奏
constexpr float kSearchBoxRadius = 3.0F;    // 轻微圆角，柔和但仍是直角系
constexpr float kSearchBoxRightGap = 10.0F; // 搜索框右边缘与钉子左边缘的间距
// 搜索框颜色改由 ThemeColors 派生（paintSearchBox / WM_CTLCOLOREDIT）。
constexpr float kSearchBoxStrokeWidth = 1.0F;
// 无查询时的占位提示
constexpr const wchar_t* kSearchPlaceholder = L"搜索…";

// 搜索框左端的应用图标（品牌位）。
//
// 尺寸与搜索框（22 DIP）成比例：16 DIP 的图标在框内上下一共留 3 DIP，
// 视觉上「嵌在框里」而不是「贴在框外」。图标右侧留 kSearchIconTextGap，
// 输入文字与占位提示都从这里起排。
constexpr float kSearchIconSize = 16.0F;
constexpr float kSearchIconLeftGap = 4.0F;
constexpr float kSearchIconTextGap = 4.0F;

// 搜索框字号。
//
// 比磁贴小标签（通常 11~12）略大一点：搜索框是交互控件，
// 文字大一些更容易看清输入内容，也与 22px 高的框更协调。
constexpr float kSearchFontSize = 13.0F;

// 不匹配搜索的磁贴淡化到多少（alpha 乘数）
constexpr float kSearchDimOpacity = 0.18F;

// ---------------------------------------------------------------- 标签条
//
// 位置：标题带（搜索框 + 钉子）之下、磁贴网格之上，独占一行。
//
// 为什么单独占一行而不是塞进标题带：标题带只有 28 DIP，已经被搜索框
// （左）与钉子（右）占满；页名是用户自己起的词组（「办公」「游戏」「开发工具」），
// 挤进去只能显示一两个字，等于没显示。
//
// 为什么放在标题带下方而不是窗口最顶部：搜索框与钉子是**面板级**控件
// （作用于整个面板），标签是**内容级**控件（切换网格里的内容）。
// 让内容级的东西紧贴内容，层级关系一眼可见。
// 标签条高度与上下间距定义在 launcher_window.h（kTabBarHeight 等）——
// 窗口尺寸计算也要用，放这里 .cpp 的匿名命名空间会让 behavior.cpp 看不到。

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

// ---------------------------------------------------------------- 选中框
constexpr float kSelectionFrameWidth = 2.0F;
constexpr float kSelectionFrameInset = 1.0F;
// 颜色跟主题强调色走（theme_.selectionFrame()）

// ---------------------------------------------------------------- 磁贴配色
//
// 设计取舍：磁贴底色**不用**高饱和纯色。
//
// 原因：Windows 程序的 exe 图标绝大多数是为「浅色/白底」设计的
// （记事本、命令提示符、任务管理器等都是浅色线条），直接压在
// #1E7145 这类深绿上会糊成一片，反而更看不清。
//
// 因此改为：
//   底色 = 深灰玻璃（保证任意图标都有对比度）
//   标识 = 顶部 4px 强调色条 + 悬停时的强调色描边
//   无图标的磁贴 = 强调色圆角底衬 + 白色首字（此时高饱和是安全的）
//
// 强调色仍按名称哈希分配，保持「一眼可辨」的视觉区分度。

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

// ================================================================ 生命周期

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
    // 注意：这里**故意不加** WS_EX_NOACTIVATE。曾经加过，代价是窗口永远
    // 拿不到焦点，键盘输入只能靠全局钩子 + 隐藏 IME 宿主窗口转发，
    // 于是「在别的程序里打字漏进搜索框」「闸门开关补丁」等问题层出不穷。
    // 现在它是普通的有焦点窗口：唤出时 show() 把焦点拿过来，
    // 失焦时系统自动停止投递按键 —— 行为正确且零维护。
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

// ================================================================ 窗口过程

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
            // 窗口宽度变了 → 标签条能放下的页数也变了（网格尺寸由设置
            // 窗口调整时会走到这里），让活动页重新回到视野内。
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
            // 用户拖动标题带结束：记住新位置，供「上一次的位置」策略使用。
            // 只有拖动/系统移动会走这个分支，程序自己 SetWindowPos 不会，
            // 所以不会把「按策略摆好的位置」反过来污染记录。
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
            //
            // 阈值存在的意义：区分「点击启动」与「拖动排序」。若不加阈值，
            // 手抖 1~2px 就会把一次启动变成一次排序，体验很差。
            // 阈值按 DPI 缩放，保证高 DPI 下手感一致。
            // 磁贴锁定时禁止升级为拖拽：按下仍记录（点击启动/键盘同步
            // 不受影响），但位移只会被忽略 —— 想整理布局先解锁。
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
                // 拖拽中：只更新落点虚线框，**不动任何磁贴**。
                // 这是对齐 Win10 的行为 —— 排布变化全部推迟到松手时。
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
            //
            // 这是「拖拽卡死」的兜底出口：捕获丢了以后 WM_LBUTTONUP 不会
            // 再送到本窗口，若不在这里收尾，dragActive_ 会一直为真 ——
            // 表现就是用户说的「松开鼠标后图标还跟着鼠标走」。
            //
            // 这里按「提交」处理而不是取消：捕获丢失是外部原因，
            // 用户本意仍是把磁贴放到虚线框处。lParam 为抢走捕获的窗口句柄，
            // 若就是本窗口（例如我们自己在松手时调 ReleaseCapture），
            // 说明是正常流程，状态已清，不会重复提交。
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
        //
        // 与浏览器一致的手感，且不管页多页少都成立
        //（页少时就是快速切换，页多时切换会把目标页滚进视野）。
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
            //
            // 窗口是无边框的 WS_POPUP，没有系统标题栏。这里把标题带里
            // 搜索框/钉子以外的区域（含搜索框上方那条窄边框）当作标题栏：
            // 交给系统的 HTCAPTION 拖动，比手写 SetCapture + WM_MOUSEMOVE
            // 稳（多显示器、DPI 边界、Aero Snap 都由系统处理）。
            //
            // 搜索框与钉子先各自命中 —— 它们的矩形与这块区域重叠时，
            // 用户点的是控件，不是标题栏。
            if (yDip < metrics_.outerPadding + metrics_.headerHeight &&
                !hitTestPin(xDip, yDip) &&
                !hitTestLock(xDip, yDip) &&
                !hitTestSearchBox(xDip, yDip)) {
                ReleaseCapture();
                SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }

            // ---- 标签条最优先：切页/新建/滚动都不该触发磁贴流程 ----
            // 放在钉子与搜索框之前虽然顺序上无所谓（三者矩形不重叠），
            // 但放前面更符合「越靠近内容的东西越先生效」的直觉。
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

            // 点空白处则退出搜索模式（但不清空已有查询），
            // 让用户能「点别处 → 继续用鼠标操作」而不被搜索框黏住。
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
                //
                // 顺序很重要：ReleaseCapture() 会同步派发 WM_CAPTURECHANGED。
                // 先清掉 dragActive_，那里的兜底分支就不会重复提交；
                // 反之若先 ReleaseCapture，提交会被兜底分支抢先执行，
                // 且随后因状态已清而走不到这里。
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
            // 必须在磁贴命中之前判断：标签条不是磁贴区，
            // 落到下面「空白区菜单」里会让用户找不到改名的入口。
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
                       aboutHandler_) {
                // ---- 空白区右键：面板级操作 ----
                //
                // 托盘图标经常被 Windows 收进溢出区，用户想找设置时
                // 不一定摸得到托盘。主窗口的空白处给一个直接的入口。
                // 「添加新项目」同理放在这里：它是面板级的动作，
                // 不应该要求用户先在某个磁贴上右键再找「新建」。
                POINT screenPoint{};
                screenPoint.x = static_cast<LONG>(x);
                screenPoint.y = static_cast<LONG>(y);
                ClientToScreen(hwnd_, &screenPoint);

                std::vector<platform::win32::PopupMenuEntry> entries;
                entries.push_back(platform::win32::PopupMenuEntry::action(
                    kBlankMenuAddItemId, L"添加新项目..."));
                entries.push_back(platform::win32::PopupMenuEntry::action(
                    kBlankMenuNewTabId, L"新建标签页..."));
                entries.push_back(platform::win32::PopupMenuEntry::separator());
                entries.push_back(platform::win32::PopupMenuEntry::action(
                    kBlankMenuSettingsId, L"设置..."));
                entries.push_back(platform::win32::PopupMenuEntry::action(
                    kBlankMenuAboutId, L"关于 KTile..."));

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
                }
            }
            return 0;
        }

        // ---- 键盘输入 ----
        //
        // 文字输入的主力是子 Edit 控件（searchEdit_，show() 时焦点在那里）。
        // 焦点偶尔停在面板本体（点过磁贴之后）时，这里的兜底把输入
        // 转交给搜索框 —— 打字是搜索意图，顺便把焦点也带过去，
        // IME 中文从下一个字起也走正路。
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
            // 文字增删时糊字），颜色取主题背景按比例混入文字色的合成
            // 结果 —— 与 D2D 侧搜索框底衬（10%/20% text）视觉一致。
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
                // 「窗口外按下的这一次到底是点击还是拖动」的探测。
                //
                //   - 按钮已松开           → 普通点击，收起窗口（与旧行为一致，
                //                            只多了最多一次探测间隔的延迟）
                //   - 还按着且光标已移动    → 判定为在拖东西（很可能是从
                //                            资源管理器拖文件过来），保持可见，
                //                            让 OLE 拖放有机会落在窗口上
                //   - 还按着但没动、超次数  → 长按，按普通点击处理，收起
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
                    // 拖东西呢 —— 不收起，也不继续探测
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
                // 拖入落点框的保险：亮着期间每 250ms 查一次左键状态。
                // 真实拖拽必然按着左键 —— 只要键已松开（无论 OLE 有没有
                // 回报 DragLeave/Drop，个别来源结束后会补发一次 DragEnter
                // 却永远不给 Leave），预览立即撤除。悬停中按键一直按着，
                // 轮询空转，无副作用。
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
            // 推进动画。返回 false 表示全部收敛，可以停表 —— 空闲时
            // 不占定时器、不重绘，对电池友好。
            const bool stillAnimating = advanceAnimations();
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (!stillAnimating) {
                settleAnimations();
                stopAnimationTimer();
            }
            return 0;
        }

        case WM_KILLFOCUS: {
            // 失去焦点。两种收尾，互不干扰：
            //   1. 拖拽中 → 取消本次拖拽、磁贴回位，以免拖拽卡死。
            //      捕获丢失的情况另由 WM_CAPTURECHANGED 负责。
            //   2. 搜索中 → 收掉搜索框的焦点视觉与输入法状态。
            //      按键由系统停止投递（焦点已走），这里收的是残留视觉。
            //      不主动隐藏窗口 —— 收起是外部点击监视的职责。
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
            // 检测到点击了窗口外部（由全局鼠标钩子投递）。
            //
            // 两个前提条件都要满足才收起：
            //   1. 没被钉住 —— 钉住是「常驻面板」，外部点击不收起
            //   2. 不在模态交互中（设置窗口打开）—— 主窗口要保持可见，
            //      皮肤切换才能实时看到效果
            //   3. 不在拖拽中 —— 拖拽时用户可能把光标甩到窗口外一点点，
            //      此时收起会让操作突然中断
            if (dragActive_ || pinPressed_ || modalGuard_) {
                return 0;
            }

            // 钉住时窗口不收起：用户去了别处，焦点随点击自然移走，
            // 输入不会再进搜索框（普通窗口的天然行为），无需记录任何状态。

            // ---- 延迟判定：这一次「按在窗口外」到底是点击还是拖动？----
            //
            // 关键场景：从资源管理器拖文件丢进面板。这次拖动是在
            // **窗口外按下鼠标**开始的 —— 若立刻收起，拖动还没到窗口
            // 上方窗口就没了，拖放功能等于不存在。
            //
            // 区别在于「按下之后还继续动」还是「马上松手」：
            // 所以这里不立即下结论，先记下按下点并启动探测定时器
            // （见 WM_TIMER 里的 kOutsideProbeTimerId 分支）。
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

// ================================================================ D2D 资源

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

    // 文本格式（搜索框）
    //
    // 必须单独建一个，不能复用 labelFormatSmall_：
    // 磁贴标签需要**居中**，而搜索框必须**左对齐**（问题2的「内容居中」）。
    // 同时设置 ParagraphAlignment = CENTER 让文字在框内垂直居中 ——
    // 只设 TextAlignment 的话，文字会贴着框顶，看起来偏上。
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

    // 文本格式（标签条页名）
    //
    // 单独建一个的理由与搜索框相同：磁贴标签格式是居中对齐的，
    // 标签条必须左对齐（文字相对自己的点击块定位，不随宽度变化跳动）。
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

// 把 HICON 转成 D2D 位图。
//
// 走 WIC 的 IWICImagingFactory::CreateBitmapFromHICON（Win7 可用）。
// 失败返回空指针，调用方降级为占位方块。
ID2D1Bitmap* LauncherWindow::acquireItemIcon(const core::LaunchItem& item) {
    if (!renderTarget_) {
        return nullptr;
    }

    // 缓存键 = id + 目标 + 自定义图标。
    //
    // 只用 id 不行：「修改项目」改的正是目标与图标，而 id 不变。
    // 把两者拼进键里之后，改完立刻就是新图标，不必依赖调用方记得作废缓存
    // （invalidateIconCache 仍然保留，用于及时释放旧位图）。
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

    // 源图档位一律取 256px jumbo。
    //
    // 原因：系统 ImageList 的实际档位是 32/48/256，并不随我们请求的
    // 64px 返回 64px（实测 hint=Medium 仍返回 48x48）。磁贴内部尺寸
    // 是 62~132px，用 32/48 源图放大必然模糊。统一取 256 后无论磁贴
    // 多大都是「缩小」，由 D2D 做高质量重采样。
    // （自定义图标是 128x128 PNG，这时 hint 只是给 shell 提取用的参考值。）
    const auto hint = platform::win32::IconSizeHint::Large;

    const std::string iconPath = item.icon.has_value() ? *item.icon : std::string();

    // ---- 用户导入的自定义图片：直接喂像素给 D2D，不走 HICON ----
    //
    // 自定义图片是按原图比例存的（可能是 64x128 这种竖图），而 HICON 的
    // DIB 往返对非正方形图标没有尺寸保证。这里「拿到什么像素就画什么像素」，
    // 顺带省掉一次 HICON 度量与图标解码。
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
        // 路径再试一次，失败则回落到目标自身的图标 —— 磁贴不会变空白。
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
    // 位图必须由窗口渲染目标创建：设备无关的 WIC 位图在 HwndRenderTarget 上
    // 创建后可以在其上绘制。若用离屏目标创建，切回窗口目标会因「位图与渲染
    // 目标不同源」而绘制失败。
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

// ================================================================ 皮肤与背景图

void LauncherWindow::setTheme(const ThemeColors& theme) {
    theme_ = theme;
    // 搜索框 Edit 的 GDI 背景画刷按主题着色、懒创建 ——
    // 删掉旧的，下次 WM_CTLCOLOREDIT 前会按新主题重建
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
//
// 用分层窗口（WS_EX_LAYERED + LWA_ALPHA）而不是给每个填充改 alpha：
//   · 需求是「整个窗口」一起变透明，包括搜索框、标签条、边框这些
//     GDI/D2D 混排的部位 —— 只有系统级合成能做到「一视同仁」；
//   · 内容仍然按不透明绘制，每帧 Clear 到不透明底的铁律不变，
//     alpha 只在窗口与桌面合成时统一相乘，帧间残影问题不受影响。
// 100% 时撤掉 WS_EX_LAYERED：不透明窗口不需要额外合成开销。
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

    // 保留主题里三档底色之间的浓淡差（悬停比常态亮、按下更亮）。
    // 直接统一成同一个 alpha 会把状态反馈抹平 —— 拉到 0% 时连「鼠标
    // 在这块上」都看不出来了。差值以 tileFill 为基准算，换皮肤仍成立。
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

// 把背景图解码成 D2D 位图。渲染目标尚未就绪时直接返回 ——
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
    // 按 1:1 需求解码即可 —— 源壁纸（动辄 3K×2K，解码 25MB）不缩放
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

// ================================================================ 渲染

void LauncherWindow::render() {
    if (!createDeviceResources()) {
        return;
    }

    renderTarget_->BeginDraw();
    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    // 每帧先清成不透明背景。
    //
    // 双保险：即便将来某个填充又用回了半透明色，Clear 也能保证
    // 上一帧的内容不会透过 alpha 混进来（渲染目标帧间内容不保证
    // 被重置，见文件顶部「窗口背景色」处的长注释）。
    renderTarget_->Clear(colorFromArgb(theme_.windowBackground));

    const D2D1_SIZE_F size = renderTarget_->GetSize();

    // ---- 背景图（设置了才画）----
    //
    // cover 等比铺满：缩放取两方向较大者，居中裁掉溢出部分 ——
    // 不变形，任何长宽比的图都好看。画完叠一层压暗层（主题
    // backgroundDim），保证半透明白的文字/描边仍然可读。
    // 残影铁律：即便有背景图，Clear 也不省 —— 位图是完全不透明
    // 绘制倒是安全，但压暗层若改为半透明叠加，就得靠 Clear 兜底。
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

    // 子 Edit 控件保险：万一 D2D 的呈现没有遵守 WS_CLIPCHILDREN
    // （个别驱动路径会把整个窗口表面刷一遍），编辑框的文字会被抹掉。
    // 让它跟着每一帧重绘一次 —— 一个 22px 高的 GDI 编辑框，代价可忽略。
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
    //
    // 只画虚线框，不移动任何磁贴。理由：
    //   Win10 拖拽时并不「预览」最终排布，只给一个明确的落点轮廓，
    //   松手才真正归位。提前把所有受影响磁贴都挪开，用户反而看不清
    //   「这块会去哪儿」，也容易误判成整个面板在乱动。
    //
    // 两种来源用同一套几何：拖动已有磁贴（中性白）与拖入新项目（强调蓝）。
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

// 绘制落点占位框（虚线矩形 + 极淡底衬）。
//
// 几何与磁贴完全一致，所以「框在哪」就等于「松手后在哪」，
// 不存在预先挪动其他磁贴带来的歧义。
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

    // 虚线描边：D2D 1.0 无内建虚线样式，手工按周长铺小段。
    //
    // 顺序：上边 → 右边 → 下边 → 左边（顺时针），这样拐角处线段连续，
    // 不会出现「四个角都是断口」的廉价感。
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

// ================================================================ 标签条

// 网格内容区的 Y 起点（客户区 DIP）。
//
// 全项目只有这一处定义「磁贴从哪开始往下排」—— 绘制、命中测试、
// 拖拽抓取偏移、落点解析全部引用它。此前这段算式散落在 8 处，
// 每加一条标题带相关的东西就要全改一遍，漏一处就变成
// 「看到的位置和点到的地方差一截」。
//
// 标签栏在底部时，网格紧贴标题带开始 —— 标签条占的高度原样
// 挪到窗口下缘，窗口总尺寸不变。
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

// 右端三个固定按钮的位置。
//
// 从右往左排：＋（最右，最常用）→ ▶ → ◀。刻意**始终**为三者留位，
// 不随「是否溢出」动态显隐 —— 动态显隐会让标签可用宽度来回变，
// 用户切页时标签左右乱跳。
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
        // 高度都是 kTabBarHeight - 1：给分隔细线让出 1px。
        // 顶部模式线在标签条底边，标签从上往下铺到线之上；
        // 底部模式线翻到标签条顶边（贴磁贴区），标签从线下往下铺。
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

// 标签文字宽度（带缓存）。
//
// 用 DWrite 的文本布局量一遍文字宽度 —— 系统没有更轻的办法，
// 而标签宽度必须跟着名字走（固定宽度要么截断「开发工具」，
// 要么让「1」占掉半个屏幕）。
// 之所以缓存：每帧重绘都量一遍 N 个文本布局，页多时是白白的开销；
// 页名/页数/DPI 不变时结果也不会变。
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
// 页多到放不下时，切页（Ctrl+Tab / 菜单）必须把目标页滚进视野 ——
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

// 绘制标签条。
//
// 视觉语言与面板一致：不画整条的底色（保持面板通透），只有
// 「活动页 / 悬停页」两块底色 + 一条底部细线。活动页额外压一条
// 强调色下划线 —— 与钉子、选中框同一支强调色，用户一眼能串起来。
void LauncherWindow::paintTabBar() {
    if (!renderTarget_ || !brush_ || !borderBrush_ || !textBrush_ ||
        tabFormat_.get() == nullptr) {
        return;
    }

    const D2D1_RECT_F bar = tabBarRect();
    if (bar.right <= bar.left + 4.0F) {
        return;  // 窗口窄到放不下，宁可不画（负宽度会让 D2D 报错）
    }

    // 刻意不在这里调 ensureActiveTabVisible()：那会让「手动 ◀▶ 翻页」
    // 立刻被拉回活动页，箭头等于失效。活动页的可见性由状态变化点
    // （切页/增删页/改名/DPI 变化）负责维护。
    const std::vector<D2D1_RECT_F> rects = tabItemRects();
    const std::size_t count = document_.tabs.size();
    const bool overflow = tabBarOverflow();

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());

    // ---- 分隔细线 ----
    //
    // 顶部模式画在标签条底边（标签「坐」在线上）；
    // 底部模式翻到标签条顶边（贴着磁贴区，标签「吊」在线下）——
    // 让分隔线永远出现在「标签与内容区之间」，而不是贴着窗口边框。
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
    //
    // 图形全部手绘线条，不用字体字形 —— 与钉子按钮同一考虑：
    // Win7 的字体对这些符号的支持不稳定，缺字形会画成方框。
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

// 标签条上的点击。返回 true 表示这次点击已经被标签条消费掉，
// 调用方（WMLBUTTONDOWN）必须直接返回，不能继续走磁贴的点击/拖拽流程。
//
// 顺序：固定按钮 → 标签 → 条内空白。
// 条内空白也算消费：标签条整行都不该触发「点空白处淡出搜索」之类的
// 面板级动作 —— 那会让用户点标签右侧的空隙时莫名其妙退掉搜索态。
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
        // 页没多到放不下时箭头是灰的 —— 按了也不该动，
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

// ================================================================ 钉子按钮

// 钉子按钮的矩形（客户区 DIP）。
//
// 位置：标题带（headerHeight）内、水平靠右。之所以放在标题带而不是压着
// 磁贴：磁贴从 outerPadding + headerHeight 开始，标题带是唯一不会被
// 磁贴或拖拽跟手绘制的区域，钉子放这里永远不会跟内容打架。
// 绘制与命中测试共用本函数，保证「看到的位置」=「可点的位置」。
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

// 建（或复用）钉子图形的几何。
//
// 几何以按钮左上角为原点、用 DIP 坐标建一次：D2D 绘制时会按渲染目标的
// DPI 再做变换，所以几何本身与 DPI 无关，缓存一次就够。
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

// 绘制钉子按钮。
//
// 视觉：Lucide 的 pin 线框 —— 与「添加系统项目」里那批图标是同一套线条
// 语言；不用字体里的图钉字符（Win7 的 Segoe UI Symbol 对 U+1F4CC 支持不稳，
// 会画成方框或空白）。
// 状态：钉住 = 亮色 + 实心；松开 = 暗色 + 空心；悬停 = 加底衬。
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

// 锁定按钮的两枚图标（来源：TurboWarp scratch-gui 的
// hide-flyout/lock.svg 与 unlock.svg，12x12 viewBox 的实心剪影）。
// 与 Lucide 线框图标不同，这两枚是「填充图形」，直接 FillGeometry 绘制，
// 不描边 —— 所以解析时统一走 forceFilled。
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

// 绘制锁定按钮。
//
// 状态语言与钉子刻意相反方向：钉子「激活=亮」，锁「锁定=亮」。
// 解锁 = 暗色空心（常态，不抢注意力）；锁定 = 亮色实心 + 底衬常显
//（锁定是一种「保持中的状态」，必须一直看得见，不能只靠悬停发现）。
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

// ================================================================ 搜索框

// 搜索框矩形（客户区 DIP）。
//
// 位置：标题带内，锁定按钮左侧，两者之间留 kSearchBoxRightGap 的间距。
// 左边界与内容区左边缘对齐（= outerPadding），与磁贴同一起始线。
// 绘制与命中测试共用本函数，保证「看到的位置」=「可点的位置」。
D2D1_RECT_F LauncherWindow::searchBoxRect() const {
    // 右边界顶到锁定按钮（不再直接顶到钉子）—— 否则两控件重叠
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

// 从 exe 资源取应用图标并转成 D2D 位图。
//
// 源尺寸固定取 64px：图标在界面上最多显示到 16 DIP（150% DPI 下 24 物理
// 像素），64 降采样到 24 比直接取 32 更锐利；再大就浪费显存。
// 位图依附渲染目标，设备重建时由 discardDeviceResources 置空。
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

// 绘制搜索框外框与占位提示。
//
// 文本、光标、选区、退格、IME 中文输入全部由子 Edit 控件负责
// （searchEdit_）——这些系统都给现成的，手画方案在中文输入上问题
// 不断（拼音回显错位、光标不跟随、合成窗口定位），不再重复造轮子。
// 面板只画：圆角外框 + 底衬 + 占位提示（仅当编辑框隐藏时）。
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
    // 画在框内而不是框外：搜索框左边缘就是内容区起始线，标题带左侧
    // 没有多余空间；嵌进框里既不挤占搜索宽度，也形成品牌锚点。
    paintSearchIcon();

    // ---- 占位提示：编辑框隐藏（无内容且无焦点）时才画得出来 ----
    // 查询非空但控件隐藏（如调试预置查询）时不画 —— 那会盖住语义。
    if ((searchEdit_ == nullptr || !IsWindowVisible(searchEdit_)) &&
        searchQuery_.empty()) {
        const float textLeft = iconRect.right + kSearchIconTextGap;
        const float textRight = rect.right - 6.0F;
        if (textRight > textLeft) {
            textBrush_->SetColor(colorFromArgb(theme_.searchPlaceholder()));
            renderTarget_->DrawTextW(
                kSearchPlaceholder,
                static_cast<UINT32>(wcslen(kSearchPlaceholder)),
                searchFormat_.get(),
                D2D1::RectF(textLeft, rect.top, textRight, rect.bottom),
                textBrush_.get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

// ---- 搜索框 Edit 控件 ------------------------------------------------

// 创建子 Edit 控件并挂子类过程。
//
// 键盘导航键（Esc/回车/上下/翻页）在子类过程里转给面板的按键逻辑，
// 其余按键（字符、退格、Home/End 等）留给控件本身 —— 那是编辑框的本职。
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
        //
        // Ctrl+Tab（切页）是面板语义的快捷键，而 Edit 控件不会处理它 ——
        // 不转发的话「搜索框有焦点时按 Ctrl+Tab」会变成往文本里插一个
        // 制表符（或什么都不发生），用户会以为标签快捷键坏了。
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
        // 回车/Esc 的字符形态 —— 按键通路已处理，不能让它进文本
        if (wParam == 0x0D || wParam == 0x1B) {
            return 0;
        }
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

// ================================================================ 选中高亮

// 绘制键盘选中的高亮边框。
//
// 视觉：强调蓝描边 + 极淡内衬。
// 之所以用「描边 + 内衬」而不是纯描边：磁贴本身有底色，纯描边在深色
// 磁贴上不够跳；加一层淡内衬让选中范围形成完整色块，余光也能看见。
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

// 绘制单块磁贴。
//
// asLifted = true 表示这是「被手指拿起来」的那块：
//   - 位置跟手（用拖拽光标 + 抓取偏移，而不是布局坐标）
//   - 轻微放大 + 投影，制造浮起感
// 其余磁贴正常按布局坐标 + 动画偏移绘制。
void LauncherWindow::paintSingleTile(const core::TilePlacement& placement,
                                     const bool asLifted,
                                     const bool dimmed) {
    const auto& items = activeItems();
    const auto& item = items[placement.itemIndex];
    const bool isSmall = (placement.spanColumns == 1 && placement.spanRows == 1);
    const bool isLarge = !isSmall;

    // ---- 淡化：不匹配搜索的磁贴 ----
    //
    // 用 D2D Layer 统一降透明，而不是逐个颜色改 alpha —— 磁贴内部有
    // 底色 / 边框 / 强调条 / 图标 / 文字好几层，逐个改容易漏，
    // 而图层一次生效、代码量最小。
    //
    // PushLayer 必须在 SetTransform 之前，且 PopLayer 要在所有绘制之后 ——
    // 顺序错了会把图层偏移算错或漏 pop（D2D 会累积状态）。
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

    // ---- 浮起投影：先画一层放大的暗色底，形成「影子」----
    //
    // D2D 1.0 没有现成的阴影效果（ID2D1Effect 属 D2D 1.1，Win8+），
    // 这里用「偏移的深色圆角矩形」模拟，成本低且 Win7 可用。
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

    // ---- 标签显示策略 ----
    // 名称一律叠加在图标之上（不挤占图标空间）。
    //
    // 两层开关：
    //   1. 设置里的「显示磁贴文字标签」= 总闸，关掉则一律不画名字
    //   2. 每块磁贴自己的「标题显示方式」——
    //        · 跟随全局设置：由设置里的「默认显示标题」决定
    //        · 始终显示    ：这块磁贴标题常驻
    //        · 移入时显示  ：只有鼠标移入（或被拿起的那块）才浮出
    //      之所以下放到每项，是因为大小磁贴的最优解往往不同：
    //      64x64 上标题会盖住图标，128x128 上留着反而好找。
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
    //
    // 统一规则：标签底衬直接贴住磁贴下边缘（不留缝隙），图标在标签
    // 上方的剩余区域内上下左右居中。所有尺寸都遵守同一规则，混排时
    // 标题基线完全一致，也不会因图标缩放（iconScalePercent < 100）
    // 把标签带离底边。
    constexpr float labelHeight = 18.0F;
    constexpr float iconLabelGap = 3.0F;
    constexpr float iconTopGap = 3.0F;

    // 标签底衬：占据磁贴最下面一整条（绘制时下沿再内缩 1px 给边框）
    const float bottomLabelTop = top + height - labelHeight;

    D2D1_RECT_F iconRect = D2D1::RectF(left, top, left, top);
    float labelTop = bottomLabelTop;

    // 用户导入的自定义图片 → 铺满整块磁贴内框。
    //
    // 自定义图片在导入时按原图比例存（最长边 128），所以「图的比例 = 磁贴
    // 比例」时（竖图配 1x2、横图配 2x1、方图配 1x1 / 2x2）整张图完整铺满；
    // 比例不符时由下面的 cover 取中段填满，不拉伸、不留白。
    //
    // 系统提取的图标（exe / 文件夹 / 网址）天生是方图，仍走下方各尺寸的
    // 方框 —— 否则会被拉成一根竖条，反而认不出。
    //
    // 例外：内置项的矢量图标虽然也存成托管路径（res\builtin-*.png），
    // 但它是程序自绘的方形图 —— 必须跟随图标大小设置缩放（居中），
    // 否则「图标大小」滑块对这些磁贴不生效。
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
    //
    // 每个磁贴可单独覆盖图标相对内框的比例（iconScalePercent，50~100）；
    // 未覆盖（<=0）时用全局统一值。100% = 现状（内框铺满，两侧共留 4px），
    // 缩小后仍按可用区居中。用户导入的整图（fillTile）铺满语义不变。
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
        // 标签位置与 2x2 对齐 —— 同样贴磁贴底边，而不是更靠上。
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
        // Large (2x2)：与其它尺寸同规则 —— 标签贴底，图标在标签
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
        // 源矩形按「覆盖 + 居中裁剪」取：图形宽高比与目标框不一致时，
        // 宁可裁掉两边，也不拉伸变形、更不留白 ——
        // 例如把一张 128x128 的图放进 68x128 的框，取中间竖条铺满。
        // 源图与目标框同为正方形时，这段是恒等变换（不产生任何影响）。
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

        // 图标整体带一点透明度：导入的 PNG 大多自带不透明白底，全不
        // 透明时每块磁贴像贴了张白纸，背景图被完全盖住，观感生硬。
        // 常态降到 80% 让背景隐约透出；悬停 / 按下 / 跟手时恢复全不
        // 透明 —— 既柔和，又顺带成了「这块磁贴有反应」的状态反馈。
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

        // 标签悬浮在图标之上，而图标里可能本身就有亮色像素（Windows
        // 图标大量使用白色），纯白文字会与之糊在一起。因此这里铺一层
        // 半透明黑底衬，把文字从图标内容中「拉」出来，保证任何图标
        // 背景下标题都清晰可读。悬停时底衬加深，形成状态反馈。
        // 全尺寸统一使用同一字号：之前 1x1/1x2 用小号字体，文字在
        // 18px 底衬里垂直位置偏高，与 2x2 的标签基线对不齐。
        // 统一后所有尺寸的标签视觉基线完全一致。
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
// 看清头尾。整窗失效即可 —— 面板小，30fps 全量重绘成本可忽略。
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

    // 格式按「基准字号」创建，这里用变换缩放到位 —— 避免每帧重建文本格式。
    // 基准 24.0F，目标为方块高度的 62%。
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
    // 用户显式指定了磁贴色 —— 尊重用户选择，只叠加状态提亮。
    // 注意：这一档**不套用**全局的「磁贴背景不透明度」—— 那是给
    // 默认玻璃底调的旋钮，套到用户亲手挑的颜色上等于擅自改他的配色。
    //
    // 优先级：单项色 > 设置里的全局默认色 > 皮肤/调色板。
    // 后两档共用同一段提亮逻辑，避免两份「hover 加多少亮」各写一遍。
    const std::optional<std::uint32_t> customColor =
        item.tileColor.has_value() ? item.tileColor : defaultTileColor_;
    if (customColor.has_value()) {
        std::uint32_t lift = 0;
        if (pressed) {
            lift = 0x40U;
        } else if (hovered) {
            lift = 0x20U;
        }
        return liftColor(*customColor, lift);
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
