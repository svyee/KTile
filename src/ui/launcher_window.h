// KTile - UI 层：启动器主窗口
//
// 无边框窗口 + D2D/DWrite 渲染，布局计算委托 core::calculateTileLayout
//（纯算法，已单测）。D2D 资源分设备无关（工厂，长期持有）与设备相关
//（渲染目标，尺寸变化/设备丢失时重建）。D2D 1.0 / DWrite 在 Win7 SP1
// 可用（需 Platform Update，默认已含）。
//
#pragma once

#include "core/data_model.h"
#include "core/plugin_model.h"
#include "core/search.h"
#include "core/tile_layout.h"
#include "platform/win32/icon_loader.h"
#include "platform/win32/outside_click_monitor.h"
#include "platform/win32/svg_path.h"
#include "platform/win32/unique_handles.h"
#include "ui/theme.h"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ktile {
namespace ui {

// 磁贴视觉度量（DIP 单位，96 DPI 下 1 DIP = 1 px）
struct TileMetrics {
    // 单个小格边长（小磁贴 64x64 px 的基准）
    float cellSize = 64.0F;
    // 磁贴之间的间隙
    float cellGap = 6.0F;
    // 0 = 直角磁贴，图标直接铺满整格，不加圆角裁切
    float cornerRadius = 0.0F;
    // 标签字体大小
    float labelFontSizeSmall = 11.0F;
    float labelFontSizeLarge = 13.0F;
    // 内边距
    // 内边距。outerPadding 管左右与顶部基准（顶部实际留白还要减去
    // kHeaderContentTopInset）；bottomPadding 只管底边，与左右不必绑同一数。
    // 高度公式：outerPadding + bottomPadding + headerHeight + 标签条 + 网格。
    float outerPadding = 10.0F;
    float bottomPadding = 6.0F;
    float headerHeight = 36.0F;

    // 单个磁贴的实际像素尺寸（含间隙）
    float tileWidth(std::uint16_t spanColumns) const {
        return static_cast<float>(spanColumns) * cellSize +
               static_cast<float>(spanColumns - 1) * cellGap;
    }
    float tileHeight(std::uint16_t spanRows) const {
        return static_cast<float>(spanRows) * cellSize +
               static_cast<float>(spanRows - 1) * cellGap;
    }
};

// 磁贴主题色（ARGB）
struct TilePalette {
    std::uint32_t background = 0x00000000;  // 0 表示使用默认
    std::uint32_t hoverBackground = 0x00000000;
    std::uint32_t pressedBackground = 0x00000000;
    std::uint32_t labelText = 0x00000000;
    std::uint32_t border = 0x00000000;
    std::uint32_t hoverBorder = 0x00000000;
};

// 标签条度量（DIP）。放在头文件：窗口尺寸计算（launcher_window_behavior.cpp）
// 与绘制/命中（launcher_window.cpp）必须用同一组数字，否则窗口按 A 高度算、
// 标签按 B 高度画，出现永久错位缝。
inline constexpr float kTabBarHeight    = 26.0F;
inline constexpr float kTabBarGapTop    = 4.0F;   // 与标题带的间距
inline constexpr float kTabBarGapBottom = 6.0F;   // 与磁贴网格的间距

// --- 动画支持
// 拖拽动画只插值视觉偏移（渲染位置 − 布局位置），不每帧重跑布局：
// 结束时偏移归零，绘制与静态完全一致，也不会浮点累积错位；布局算法
// 只在落点变化时算一次。SetTimer（约 60fps）驱动，全部用 Win7 可用 API。
struct TileAnimation {
    // 当前视觉偏移（DIP）
    float offsetX = 0.0F;
    float offsetY = 0.0F;
    // 动画起点偏移（本轮动画开始时的偏移值）
    float fromX = 0.0F;
    float fromY = 0.0F;
    // 动画终点偏移
    float toX = 0.0F;
    float toY = 0.0F;
    // 本轮动画起始时刻（GetTickCount64，毫秒）
    std::uint64_t startTick = 0;
    // 时长（毫秒），0 表示无动画
    std::uint32_t durationMs = 0;

    // 本轮动画是否已结束
    bool settled() const noexcept { return durationMs == 0; }

    // 把偏移直接落到终点并结束动画
    void snapToEnd() noexcept {
        offsetX = toX;
        offsetY = toY;
        durationMs = 0;
    }
};

// 缓动函数：ease-out cubic，起步快、收尾稳（Win10 磁贴手感）
inline float easeOutCubic(const float t) noexcept {
    const float clamped = (t < 0.0F) ? 0.0F : ((t > 1.0F) ? 1.0F : t);
    const float inverse = 1.0F - clamped;
    return 1.0F - inverse * inverse * inverse;
}

class LauncherWindow {
public:
    // 启动项目回调
    using LaunchHandler = std::function<void(const core::LaunchItem&)>;
    // 右键菜单请求（由 app 层弹出菜单，返回后由 app 层修改数据）
    using ContextMenuHandler = std::function<void(HWND, std::size_t itemIndex,
                                                  const core::LaunchItem&)>;
    // 磁贴数据变化（拖拽完成、尺寸变更等）
    using DocumentChangedHandler = std::function<void(const core::ItemsDocument&)>;
    // 窗口关闭请求
    using CloseHandler = std::function<void()>;
    // 空白区右键菜单里选了「设置...」
    using SettingsHandler = std::function<void()>;
    // 空白区右键菜单里选了「添加新项目」
    using AddItemHandler = std::function<void()>;
    // 活动标签页变化（app 层用来记住「上次在哪一页」）
    using ActiveTabChangedHandler = std::function<void(std::size_t)>;
    // 用户拖动窗口移动后（参数是新的左上角，物理像素，屏幕坐标）
    using WindowMovedHandler = std::function<void(int x, int y)>;

    // ---- 插件磁贴 ----
    // UI 只负责把数据画出来，插件发现/刷新/缓存在 app 层：UI 不依赖
    // infrastructure / WinHTTP，core 的布局引擎也能单独测试。
    using PluginViewProvider = std::function<core::PluginView(const core::LaunchItem&)>;
    using PluginCatalogProvider =
        std::function<std::vector<core::PluginCatalogEntry>()>;
    using AddPluginHandler = std::function<void(const std::string& pluginId)>;
    // 面板显示 / 隐藏（app 层据此触发懒加载刷新与取消排队）
    using PanelVisibilityHandler = std::function<void()>;

    LauncherWindow();
    ~LauncherWindow();

    LauncherWindow(const LauncherWindow&) = delete;
    LauncherWindow& operator=(const LauncherWindow&) = delete;

    // 创建窗口。成功返回 true。
    bool create(HINSTANCE instance,
                const std::wstring& windowTitle,
                core::ItemsDocument document);

    // 数据与配置
    void setDocument(core::ItemsDocument document);
    const core::ItemsDocument& document() const noexcept { return document_; }

    void setMetrics(const TileMetrics& metrics) { metrics_ = metrics; }
    void setPalette(const TilePalette& palette) { palette_ = palette; }

    // 网格尺寸（小格）
    void setGridSize(std::uint16_t columns, std::uint16_t rows) {
        gridColumns_ = columns;
        gridRows_ = rows;
    }
    std::uint16_t gridColumns() const noexcept { return gridColumns_; }
    std::uint16_t gridRows() const noexcept { return gridRows_; }

    // 是否显示磁贴文字标签。关闭时大/纵向磁贴的常显标题也一并隐藏；
    // 再开回来行为自动恢复，不改任何数据。
    void setShowTileLabels(const bool show) {
        showTileLabels_ = show;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    bool showTileLabels() const noexcept { return showTileLabels_; }

    // 图标相对磁贴内框的统一缩放百分比（50~100，默认 90）。
    // 单项可在编辑对话框里覆盖（LaunchItem.iconScalePercent）。
    // 只改绘制矩形，不动磁贴布局与图标位图缓存，无需重算布局。
    void setTileIconScalePercent(const int percent) {
        tileIconScalePercent_ = std::clamp(percent, 50, 100);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // 标题是否默认显示。false 时所有尺寸都改为移入才浮出标题，true（默认）常驻。
    void setAlwaysShowTileLabels(const bool always) {
        alwaysShowTileLabels_ = always;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    bool alwaysShowTileLabels() const noexcept {
        return alwaysShowTileLabels_;
    }

    // 标签栏位置：false = 顶部（标题带下方，默认）；true = 底部（窗口下缘）。
    // 只影响几何（gridOriginY / tabBarRect），不改窗口尺寸与磁贴数据。
    void setTabBarAtBottom(const bool bottom) {
        tabBarAtBottom_ = bottom;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    bool tabBarAtBottom() const noexcept { return tabBarAtBottom_; }

    // 窗口出现位置。只影响下次摆放：show() 与 applyWindowPlacement()
    // 按这个策略算坐标，设置窗口打开期间不会自己乱动。
    void setWindowPlacement(const core::WindowPlacement placement) {
        placement_ = placement;
    }
    core::WindowPlacement windowPlacement() const noexcept {
        return placement_;
    }

    // 「上一次的位置」策略使用的窗口左上角（物理像素，屏幕坐标）。
    void setLastWindowPosition(const int x, const int y, const bool valid) {
        lastWindowPos_.x = x;
        lastWindowPos_.y = y;
        lastWindowPosValid_ = valid;
    }

    // 用户拖完窗口后的回调（app 层把坐标写进 config）
    void setWindowMovedHandler(WindowMovedHandler handler) {
        windowMovedHandler_ = std::move(handler);
    }

    // 按当前放置策略重新摆放窗口：不改可见性、不抢焦点。
    // 设置里刚改完「窗口位置」时调用，用户能立刻看到效果。
    void applyWindowPlacement();

    // 应用皮肤配色（设置窗口切换皮肤时调用）。立即重绘；
    // 搜索框 GDI 画刷随主题重建。
    void setTheme(const ThemeColors& theme);
    const ThemeColors& theme() const noexcept { return theme_; }

    // 设置/清除背景图（传绝对路径；空串 = 清除）。
    // 绘制策略：cover 等比铺满 + 主题 backgroundDim 压暗层。
    void setBackgroundImage(const std::wstring& absolutePath);

    // 主窗口整体不透明度（50~100，100 = 完全不透明）。
    // 走 WS_EX_LAYERED + LWA_ALPHA：系统在合成时统一乘 alpha，内容仍按
    // 不透明绘制，与每帧 Clear 到不透明底的策略不冲突。100% 时撤掉该样式。
    void setWindowOpacityPercent(int percent);
    int windowOpacityPercent() const noexcept { return windowOpacityPercent_; }

    // 磁贴背景不透明度（0~100，-1 = 跟随皮肤 tileFill 自带的 alpha）。
    // 只影响磁贴底色层，与整窗透明度（合成时乘的系数）相互独立。
    void setTileOpacityPercent(int percent);
    int tileOpacityPercent() const noexcept { return tileOpacityPercent_; }
    // 当前实际生效的磁贴背景不透明度（0~100）。跟随皮肤时由 tileFill 的
    // alpha 换算，设置窗口显示的滑块位置才与观感一致。
    int effectiveTileOpacityPercent() const;

    // 所有磁贴的默认背景色（ARGB）。空 = 跟随皮肤。
    // 单项 LaunchItem.tileColor 优先于它；它不套用磁贴背景不透明度。
    void setDefaultTileColor(const std::optional<std::uint32_t>& color) {
        defaultTileColor_ = color;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    const std::optional<std::uint32_t>& defaultTileColor() const noexcept {
        return defaultTileColor_;
    }

    // 丢弃图标缓存。「修改项目」改过目标或图标后必须调用：缓存键是
    // item.id，id 不变，不作废会一直显示旧图标。
    void invalidateIconCache() { clearIconCache(); }

    // 当前布局结果（供设置窗口统计「已占用的行列」）
    const core::TileLayoutResult& layout() const noexcept { return layout_; }

    // 回调设置
    void setLaunchHandler(LaunchHandler handler) { launchHandler_ = std::move(handler); }
    void setContextMenuHandler(ContextMenuHandler handler) {
        contextMenuHandler_ = std::move(handler);
    }
    void setDocumentChangedHandler(DocumentChangedHandler handler) {
        documentChangedHandler_ = std::move(handler);
    }
    void setCloseHandler(CloseHandler handler) { closeHandler_ = std::move(handler); }
    void setSettingsHandler(SettingsHandler handler) {
        settingsHandler_ = std::move(handler);
    }
    void setAboutHandler(SettingsHandler handler) {
        aboutHandler_ = std::move(handler);
    }
    void setAddItemHandler(AddItemHandler handler) {
        addItemHandler_ = std::move(handler);
    }
    void setPluginViewProvider(PluginViewProvider handler) {
        pluginViewProvider_ = std::move(handler);
    }

    // 把插件磁贴渲染进任意 HDC（编辑对话框预览用）。与主窗口同一套
    // 布局与绘制路径，预览即真实效果（含局部彩色图标）。target 是物理
    // 像素矩形；size 决定选哪一档布局（small/wide/tall/large）。
    bool renderPluginPreview(HDC dc, const RECT& target,
                             const core::PluginView& view,
                             core::TileSize size);

    void setPluginCatalogProvider(PluginCatalogProvider handler) {
        pluginCatalogProvider_ = std::move(handler);
    }
    void setAddPluginHandler(AddPluginHandler handler) {
        addPluginHandler_ = std::move(handler);
    }
    void setPanelShownHandler(PanelVisibilityHandler handler) {
        panelShownHandler_ = std::move(handler);
    }
    void setPanelHiddenHandler(PanelVisibilityHandler handler) {
        panelHiddenHandler_ = std::move(handler);
    }
    void setActiveTabChangedHandler(ActiveTabChangedHandler handler) {
        activeTabChangedHandler_ = std::move(handler);
    }

    // ---- 标签页（多页面分标） ----
    // 每页持有独立的磁贴集合（含网格坐标），同一程序可同时出现在两页上，
    // 去重只在同一页内有效（见 app 层的拖入去重）。面板同一时刻只显示
    // activeTabIndex_ 一页。
    std::size_t tabCount() const noexcept { return document_.tabs.size(); }
    std::size_t activeTabIndex() const noexcept { return activeTabIndex_; }
    // 当前页的 id（多用于持久化「上次在哪一页」）。无页时返回空串。
    std::string activeTabId() const;

    // 切换到第 index 页（越界时夹取）。会清掉与旧页下标绑定的瞬态状态
    // （悬停/按下/拖拽/动画/键盘选中），并重算布局与搜索可见性掩码。
    void setActiveTabIndex(std::size_t index);
    // 按 id 恢复上次显示的那一页（启动时用）。返回是否命中。
    bool setActiveTabById(const std::string& tabId);
    // 活动页 ±1（wrap）。供 Ctrl+Tab / Ctrl+Shift+Tab 用。
    void cycleActiveTab(int delta);

    // ---- 标签页数据操作 ----
    // 全部走 core::tab_operations，改完立即通知 documentChangedHandler_
    // 供 app 层落盘。名字为空时自动取名「页面 N」。
    std::size_t createTab(const std::string& rawName);
    bool renameTab(std::size_t index, const std::string& rawName);
    // 删除（连同页内磁贴）。只剩一页时拒绝。
    bool removeTab(std::size_t index);
    // 调整页顺序
    bool moveTab(std::size_t from, std::size_t to);

    // ---- 拖入添加的落点预览 ----
    // 拖文件进来时只画新磁贴将占据的那一格（强调色虚线框）并让被压住的
    // 磁贴做让位预演，松手即落在这一格。铺满内容区加提示条的方案被否决：
    // 提示条挡住磁贴，且个别来源（Directory Opus）放下后补发 DragEnter
    // 却不给 DragLeave，提示条会残留不消失。
    // newItemSize：新磁贴尺寸（决定框大小）；screenPoint：光标屏幕坐标。
    // 只有左键真的按着才点亮：真实拖拽必然按着左键，补发的 DragEnter
    // 到达时按键已松开，据此忽略，残影无从产生。
    void setFileDropTarget(bool active, core::TileSize newItemSize,
                           POINTL screenPoint);
    bool fileDropActive() const noexcept { return fileDropActive_; }
    // 显示/隐藏
    void show();
    void hide();
    void toggle();
    bool isVisible() const noexcept;

    // 刷新布局并重绘
    void refresh();

    // 按当前网格尺寸重算窗口大小并立即应用（不抢焦点、不改可见性）。
    // 只调 recomputeLayout() 的话窗口仍是旧尺寸，放不下的磁贴被裁掉，
    // 看起来像设置没生效。
    void applyGridGeometry();

    // ---- 调试辅助（供 --debug-shot 使用） ----
    // PrintWindow 对 D2D 窗口不可靠（HwndRenderTarget 不实现 WM_PRINT），
    // 屏幕 BitBlt 又要求窗口可见且未遮挡，所以由程序把 D2D 输出渲染到
    // WIC 位图落盘，绕开窗口合成。仅调试路径调用，不影响正常渲染流程。

    // 模拟悬停到指定客户区坐标（<0 表示清除悬停）
    void debugSetHover(int clientX, int clientY);

    // 模拟文件拖到窗口上：点亮落点预览（客户区坐标，<0 表示清除），
    // 供自动化拍下拖入视觉。与真实路径的唯一差别是不检查左键是否按着。
    void debugSetFileDropPreview(int clientX, int clientY);

    // 模拟拖拽状态：source 为被拖动的磁贴坐标，target 为落点坐标
    // （任一为负则清除拖拽状态），用于导出拖拽视觉反馈截图。
    void debugSetDrag(int sourceX, int sourceY, int targetX, int targetY);

    // 把当前窗口内容渲染为 PNG 写入 exe 同级目录，返回是否成功
    bool debugExportPng(const wchar_t* fileName);

    // 窗口句柄
    HWND hwnd() const noexcept { return hwnd_; }

    // 供 app 层调用：修改某个磁贴的尺寸后重新布局
    bool setItemTileSize(std::size_t itemIndex, core::TileSize size);

    // 供 app 层调用：修改某块磁贴的标题显示方式。
    // 与改尺寸不同，这只影响标题画不画，不动位置与布局。
    bool setItemLabelMode(std::size_t itemIndex, core::TileLabelMode mode);

    // 供 app 层调用：把第 fromIndex 项拖到 toIndex 位置（插入式重排）
    // 返回 false 表示索引非法或位置未变化（此时不做任何改动）
    bool moveItem(std::size_t fromIndex, std::size_t toIndex);

    // 供 app 层调用：把第 fromIndex 项放到指定落点（支持放到空白格）
    // 返回 false 表示落点非法、放不下或位置未变化
    bool moveItemToSpot(std::size_t fromIndex, const core::Dropspot& spot);

    // 把若干新项目插到光标所在的格子上，语义与拖动磁贴一致（落点即结果）：
    // 新磁贴占住光标格，压住的已有磁贴按同一套避让规则让位，多项目时
    // 最后一个正好落在光标格。dropPoint 为屏幕坐标（网格外夹到边界）；
    // 不给坐标（自动化/调试入口）时交给布局引擎找第一个空位。
    // 返回 false 表示一个都没能加入（id 冲突/目标为空）。
    bool insertNewItemsAt(std::vector<core::LaunchItem> items,
                          std::optional<POINTL> dropPoint);

    // 供 app 层调用：重新计算布局（数据变化后）
    void recomputeLayout();

    // ---- 钉子（锁定窗口） ----
    //
    // 语义：钉住时点击窗口外不隐藏；松开时点击窗口外自动隐藏。
    // 状态持久化在 config.json 的 appearance.pinned。
    bool isPinned() const noexcept { return pinned_; }
    void setPinned(bool pinned);
    // 状态变化回调（app 层用来落盘）
    void setPinnedChangedHandler(std::function<void(bool)> handler) {
        pinnedChangedHandler_ = std::move(handler);
    }

    // ---- 磁贴锁定（防误拖） ----
    // 锁定时磁贴不可拖拽排序，点击启动不受影响。与钉住是两件事：
    // 钉住管窗口收不收起，锁定管磁贴能不能拖。会话级状态，不落盘。
    bool tilesLocked() const noexcept { return tilesLocked_; }
    void setTilesLocked(bool locked);

    // 供 app 层调用：按当前钉子状态决定是否响应「点击窗口外」
    // 返回 true 表示已隐藏
    bool hideIfUnpinned();

    // ---- 模态交互保护 ----
    // 设置窗口（或文件对话框）打开期间置位：点击窗口外不收起，但不改变
    // 钉子偏好、不落盘。必要性：设置窗口的下拉列表是独立顶层窗口，不在
    // owner 链上，会被外部点击监视判成「点了外面」而收起主窗口。
    void setModalGuard(bool active);
    bool modalGuard() const noexcept { return modalGuard_; }

    // ---- 搜索过滤 ----
    // 查询串为空 = 不过滤。非空时不匹配的磁贴仍在原位，只是显著淡化；
    // 刻意不隐藏加重排：磁贴靠位置形成空间记忆，重排会毁掉方位感。
    void setSearchQuery(const std::string& query);
    const std::string& searchQuery() const noexcept { return searchQuery_; }
    // 清空搜索（等价于 setSearchQuery("")）
    void clearSearch();

    // 当前是否有过滤生效
    bool hasActiveSearch() const noexcept { return !searchQuery_.empty(); }

    // 某块磁贴当前是否匹配搜索（无搜索时恒为 true）。
    // 供绘制与命中测试共用，保证「看得见」与「点得到」一致。
    bool isItemVisible(std::size_t itemIndex) const;

    // ---- 键盘导航 ----
    //
    // 选中的磁贴（方向键移动）。仅在窗口可见时有效。
    void moveSelection(int deltaColumn, int deltaRow);
    void clearSelection();

private:
    // 重新计算 visibleMask_（搜索查询变化或文档变化时调用）
    void rebuildVisibilityMask();

    // 第一个匹配（或全部）磁贴中，按网格顺序的第一个可用索引
    std::optional<std::size_t> firstVisibleItem() const;

    // 把选中的磁贴滚动进视野（本窗口不滚动，仅用于移动后重绘）
    void ensureSelectionVisible();

    // ---- 搜索模式开关（界面状态 + 输入法状态一起切） ----
    // 进入搜索态：置位 searchActive_ 并启用 IME 宿主（中文输入的前提）
    void activateSearch();
    // 退出搜索态：清位并卸下 IME 宿主（避免焦点被隐藏窗口占着）
    void deactivateSearch();

    // ---- 按键处理 ----
    // 返回 LRESULT 是为了让窗口过程的 case 分支能直接 return
    LRESULT handleKeyDown(UINT virtualKey);

public:

    // 当前布局结果（用于调试/测试）
    const core::TileLayoutResult& layoutResult() const noexcept { return layout_; }

private:
    // 窗口过程
    static LRESULT CALLBACK windowProcThunk(HWND hwnd, UINT message,
                                            WPARAM wParam, LPARAM lParam);
    LRESULT windowProc(UINT message, WPARAM wParam, LPARAM lParam);

    // 渲染
    bool createDeviceIndependentResources();
    void discardDeviceResources();
    bool createDeviceResources();
    void render();
    void paintTiles();

    // ---- 活动标签页 ----
    // 所有「当前页」的读写都从这里走，禁止直接写 document_.tabs.front()：
    // 引入活动页后漏改一处就表现为「操作动到了第一页」，极难排查。
    core::Tab* activeTab();
    const core::Tab* activeTab() const;
    // 活动页的磁贴。文档意外为空时返回共享的空表，调用方不必判空
    //（加载器与 removeTab 都保证至少一页）。
    std::vector<core::LaunchItem>& activeItems();
    const std::vector<core::LaunchItem>& activeItems() const;
    // 把 activeTabIndex_ 夹回合法范围（文档变化后调用）
    void clampActiveTab();
    // 切页时清掉与旧页下标绑定的瞬态状态（悬停/按下/拖拽/动画/选中）
    void resetTransientForTabSwitch();

    // ---- 标签条几何 ----
    // 网格内容区的 Y 起点（DIP）。所有磁贴绘制与命中测试都必须用它，
    // 各写各的算式会出现「画在这里、点在那里」。
    float gridOriginY() const;
    // 客户区物理尺寸（已按最大窗口夹取）；跟随网格配置，与窗口位置无关
    void windowSizePixels(int* width, int* height) const;
    // 标签条占据的矩形（客户区 DIP）
    D2D1_RECT_F tabBarRect() const;
    // 当前客户区高度（DIP）。用实际客户区而非配置推算：窗口高度被
    // kMaximumWindowHeightDip 夹住时，底部标签条仍贴住真实下缘。
    float clientHeightDip() const;
    // 标签条右端固定的三个小按钮（＋、◀、▶）
    D2D1_RECT_F newTabButtonRect() const;
    D2D1_RECT_F tabScrollLeftRect() const;
    D2D1_RECT_F tabScrollRightRect() const;
    // 每个标签页的显示矩形（含不可见的：不可见的为空矩形）。
    // 绘制与命中共用，保证「画在哪 = 点在哪」。
    std::vector<D2D1_RECT_F> tabItemRects() const;
    // 命中标签：返回下标，未命中返回 -1
    int hitTestTab(float xDip, float yDip) const;
    // 当前是否处于「标签放不下」的溢出状态
    bool tabBarOverflow() const;
    // 让活动页处于可见范围（切页/新建/删除后调用）
    void ensureActiveTabVisible();

    // ---- 标签条绘制 ----
    void paintTabBar();
    // 标签文字宽度（带缓存：只在页名/页数/DPI 变化时重算）
    const std::vector<float>& tabLabelWidths() const;
    void invalidateTabLabelWidths() { tabLabelWidthsDirty_ = true; }

    // ---- 标签页交互（自绘控件，没有子窗口） ----
    // 命中标签条右端的固定按钮：1 = ＋，2 = ◀，3 = ▶，0 = 未命中
    int hitTestTabBarButton(float xDip, float yDip) const;
    // 标签条上的左键点击（切换页 / 新建页 / 滚动）。返回 true = 已被消费。
    bool handleTabBarClick(float xDip, float yDip);
    // 在标签上右键 → 弹出页面菜单（新建/重命名/删除/左移/右移）
    void showTabContextMenu(std::size_t tabIndex, POINT screenPoint);
    // 「新建标签页」：询问名字后建页并切过去
    void promptCreateTab();
    // 「重命名此页」：询问名字后改名
    void promptRenameTab(std::size_t tabIndex);

    // ---- 钉子 ----
    // 钉子占据的矩形（客户区 DIP）。绘制与命中测试共用同一处几何，
    // 避免「画在这里、点在那里」的错位。
    D2D1_RECT_F pinButtonRect() const;
    // 绘制钉子按钮（含悬停/钉住两种视觉状态）
    void paintPinButton();
    // 命中测试：坐标是否落在钉子上
    bool hitTestPin(float xDip, float yDip) const;

    // ---- 锁定按钮（防误拖磁贴） ----
    // 钉子左侧的小锁。绘制与命中测试共用同一处几何（与钉子同一套路）。
    D2D1_RECT_F lockButtonRect() const;
    void paintLockButton();
    bool hitTestLock(float xDip, float yDip) const;

    // ---- 搜索框 ----
    // 搜索框占据的矩形（标题带内、钉子左侧）。绘制与命中测试共用。
    D2D1_RECT_F searchBoxRect() const;
    // 绘制搜索框外框与占位提示（文字/光标由子 Edit 控件负责）
    void paintSearchBox();
    // 命中测试：坐标是否落在搜索框上
    bool hitTestSearchBox(float xDip, float yDip) const;

    // ---- 搜索框左侧的应用图标 ----
    // 框内的品牌位（嵌在搜索框左端），点击弹「关于」对话框。
    D2D1_RECT_F searchIconRect() const;
    bool hitTestSearchIcon(float xDip, float yDip) const;
    void paintSearchIcon();
    // 应用图标位图（从 exe 资源 101 取，按设备重建；缓存一次反复用）
    ID2D1Bitmap* acquireAppIconBitmap();

    // ---- 搜索框 Edit 控件 ----
    // 文本输入交给系统 Edit 控件，IME 中文/光标/选区全是原生行为
    //（手画方案在中文输入上问题不断，已否决）。面板只画框与占位提示。
    bool createSearchEdit(HINSTANCE instance);
    void ensureSearchEditResources();
    // 从 Edit 控件读回文本并更新过滤查询（EN_CHANGE 驱动）
    void syncSearchFromEdit();
    // Edit 子类过程：把 Esc/回车/上下翻页键转给面板的按键逻辑
    static LRESULT CALLBACK searchEditProc(HWND hwnd, UINT message,
                                           WPARAM wParam, LPARAM lParam,
                                           UINT_PTR subclassId,
                                           DWORD_PTR refData);

    // 绘制单块磁贴。asLifted = true 表示这是被拿起的跟手磁贴
    // （位置跟手光标 + 轻微放大 + 投影）。
    // dimmed = true 表示该磁贴不匹配搜索，需要淡化。
    void paintSingleTile(const core::TilePlacement& placement, bool asLifted,
                         bool dimmed = false);

    // ---- 插件磁贴绘制 ----
    //
    // 布局计算在 core::buildPluginDrawList（纯函数、已单测），这里只把
    // Text/Icon/Meter 指令翻译成 D2D/DWrite 调用。
    void paintPluginTileContent(const core::LaunchItem& item,
                                const D2D1_RECT_F& rect);
    // 无数据 / 加载中 / 出错时的占位（图标 + 一行状态文案）
    void paintPluginPlaceholder(const core::PluginView& view,
                                const D2D1_RECT_F& rect);
    // 按字号复用 DWrite 文本格式（居中 + 不换行）
    IDWriteTextFormat* pluginTextFormat(float fontSize);
    // 用 DWrite 实测文本宽度（供布局引擎做省略号截断）
    float measurePluginText(const std::string& text, float fontSize);
    // 画一枚插件图标。解析顺序：插件目录 .svg（支持局部彩色）→
    // 插件目录图片（png 等）→ fallbackId 重复以上。图标资源完全由插件
    // 自带，都失败时留空（不画方框）。target/brush 显式传入：主窗口与
    // 预览用各自的渲染目标，D2D 1.0 的笔刷不能跨目标复用。
    void drawPluginIcon(const std::string& iconId, const std::string& fallbackId,
                        const std::string& pluginDirectory,
                        ID2D1RenderTarget* target,
                        ID2D1SolidColorBrush* brush,
                        const D2D1_RECT_F& rect, std::uint32_t color);
    bool drawPluginSvgFileIcon(const std::string& storedPath,
                               const std::string& pluginDirectory,
                               ID2D1RenderTarget* target,
                               ID2D1SolidColorBrush* brush,
                               const D2D1_RECT_F& rect, std::uint32_t color);
    bool drawPluginFileIcon(const std::string& storedPath,
                            const std::string& pluginDirectory,
                            ID2D1RenderTarget* target,
                            const D2D1_RECT_F& rect);
    void drawPluginGlyph(const platform::win32::SvgGlyph& glyph,
                         ID2D1RenderTarget* target,
                         ID2D1SolidColorBrush* brush,
                         const D2D1_RECT_F& rect, float size,
                         std::uint32_t color);
    void clearPluginGeometryCache();

    // 绘制选中高亮框（键盘导航用）
    void paintSelectionFrame(const core::TilePlacement& placement);

    // 绘制落点占位框（虚线矩形）。框的位置 = 松手后磁贴的真实位置。
    // addingNew = true 时用强调蓝（拖入添加的新项目），否则用中性白（挪动已有磁贴）。
    void paintDropSpotFrame(const core::Dropspot& spot, bool addingNew);

    // ---- 图标缓存 ----
    // 按 item.id 缓存已转换的 D2D 位图。设备资源重建时需清空。
    ID2D1Bitmap* acquireItemIcon(const core::LaunchItem& item);
    void clearIconCache();

    // 图标缺失时的兜底绘制：白底圆角衬 + 名称首字（保证磁贴可辨识）
    void drawFallbackGlyph(const core::LaunchItem& item,
                           const D2D1_RECT_F& rect);

    // 解析磁贴背景色（自定义色 > 状态色 > 默认色）
    std::uint32_t resolveTileBackground(const core::LaunchItem& item,
                                        bool hovered,
                                        bool pressed) const;

    // 布局
    void updateLayout();
    RECT calculateWindowRect() const;

    // ---- 动画 ----
    //
    // 全部磁贴的动画状态，以 itemIndex 为键。用 map 而不是 vector：
    // 索引在拖拽重排后会整体位移，键值查找比下标访问更能容忍错位。
    std::map<std::size_t, TileAnimation> animations_;

    // 启动一轮偏移动画（from 取当前偏移，to 为传入目标）
    void startOffsetAnimation(std::size_t itemIndex,
                              float toOffsetX, float toOffsetY,
                              std::uint32_t durationMs);

    // 推进所有动画到当前时刻，返回是否仍在动画中
    bool advanceAnimations();

    // 动画结束：把所有偏移归零并停掉定时器
    void settleAnimations();

    // 启动/停止动画定时器（按需，避免空闲时无谓重绘）
    void ensureAnimationTimer();
    void stopAnimationTimer();

    // 取某块磁贴当前的视觉偏移（无动画时为 0）
    void currentOffset(std::size_t itemIndex,
                       float& offsetX, float& offsetY) const;

    // 根据光标位置求出落点（被拖项将占据的格子）。与命中磁贴不同，这里
    // 做矩形贴合加夹取：落在磁贴上贴合到它的格子，落在磁贴之间或右侧
    // 空白带视为新建格子，最后按被拖项跨度夹进网格。没有这一步，拖到
    // 空白处松手会弹回去。
    core::Dropspot resolveDropspot(std::size_t dragIndex, int clientX, int clientY) const;

    // 落点变化 → 更新 dropspotPreview_ 并重绘；压到已有磁贴时播挤开动画
    void updateDropspotPreview();

    // 新磁贴（尚未存在的项）在光标处的落点格
    core::Dropspot resolveNewItemDropspot(core::TileSize newItemSize,
                                          int clientX, int clientY) const;

    // 落点预览的实际执行体：算落点、播让位预演、重绘。
    // setFileDropTarget 与 debugSetFileDropPreview 共用它，
    // 差别只在「要不要检查左键按着」。
    void applyFileDropTarget(bool active, core::TileSize newItemSize,
                             POINTL screenPoint);

    // 给 items 里没有显式坐标的项按当前布局补坐标。避让布局只对有坐标
    // 的项冻结原位，没坐标的会被当成新项挤到落点附近重排，整个面板跟着跳。
    void materializeGridPositions(std::vector<core::LaunchItem>& items) const;

    // 把「谁该挪到哪」翻译成偏移动画（拖动磁贴与拖入新项目共用）。
    // skipIndex 是本次不该被动画的项（跟手磁贴），没有就传 items 数量之外的值。
    void applyDisplacementPreview(
        const std::map<std::size_t,
                       std::pair<std::uint16_t, std::uint16_t>>& targets,
        std::size_t skipIndex);

    // 拖入悬停预览：新磁贴会占哪一格、哪些已有磁贴要让位
    void previewDropForNewItem(core::TileSize newItemSize,
                               const core::Dropspot& spot);

    // 挤开预演：落点压到已有磁贴时，把避让布局翻译成受影响磁贴的
    // 偏移动画（只动视觉，不改 layout_；真正提交在松手时）。
    // 落点是空白时让所有已偏移磁贴回位。
    void previewDisplacement(std::size_t dragIndex, const core::Dropspot& spot);

    // 取消拖拽：所有磁贴回到真实布局位置（偏移归零）
    void animateBackToLayout();

    // 命中测试
    std::optional<std::size_t> hitTestItem(float xDip, float yDip) const;

    // 窗口尺寸/DIP 换算
    float dpiScale() const noexcept;
    void updateDpi();
    // DPI 变化后重算与 DPI 相关的缓存（标签文字宽度）并校正可见范围
    void dpiChangedForLayout();

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;

    core::ItemsDocument document_;
    core::TileLayoutResult layout_;

    TileMetrics metrics_;
    TilePalette palette_;

    // D2D 设备无关资源
    platform::win32::ComPtr<ID2D1Factory> d2dFactory_;
    platform::win32::ComPtr<IDWriteFactory> dwriteFactory_;
    platform::win32::ComPtr<IDWriteTextFormat> labelFormatSmall_;
    // 搜索框专用格式：左对齐 + 垂直居中。
    // 不能复用 labelFormatSmall_（那是磁贴标签用的居中对齐）。
    platform::win32::ComPtr<IDWriteTextFormat> searchFormat_;
    platform::win32::ComPtr<IDWriteTextFormat> labelFormatLarge_;
    // 标签条文字格式（左对齐 + 垂直居中，不换行）。
    // 不能复用磁贴标签格式：那个是居中对齐，画到标签条上名字会飘。
    platform::win32::ComPtr<IDWriteTextFormat> tabFormat_;
    // 兜底首字专用格式（居中对齐，字号按磁贴尺寸动态设置）
    platform::win32::ComPtr<IDWriteTextFormat> fallbackFormat_;

    // D2D 设备相关资源（尺寸变化时重建）
    // hwndTarget_ 持有窗口渲染目标生命周期；renderTarget_ 是当前生效目标，
    // 通常指向 hwndTarget_，调试导出时临时指向 WIC 离屏目标。绘制代码
    // 只依赖 ID2D1RenderTarget 接口，用可切换指针比逐点分支清晰。
    platform::win32::ComPtr<ID2D1HwndRenderTarget> hwndTarget_;
    ID2D1RenderTarget* renderTarget_ = nullptr;
    platform::win32::ComPtr<ID2D1SolidColorBrush> brush_;
    platform::win32::ComPtr<ID2D1SolidColorBrush> textBrush_;
    platform::win32::ComPtr<ID2D1SolidColorBrush> borderBrush_;

    // ---- 标签条状态 ----
    // activeTabIndex_：当前显示的页（与 document_.tabs 同下标）
    // firstVisibleTab_：滚动窗口起始页（页多到放不下时用）
    // hoveredTab_：悬停页（-1 无），只影响视觉
    // tabBarButtonHover_：悬停按钮（1=＋ 2=◀ 3=▶ 0=无）
    // 按下状态不单独记：标签条点击按下即生效，弹起时无需再判定。
    std::size_t activeTabIndex_ = 0;
    std::size_t firstVisibleTab_ = 0;
    int hoveredTab_ = -1;
    int tabBarButtonHover_ = 0;
    // 标签文字宽度缓存（页名/页数/DPI 变化时置脏重算）
    mutable std::vector<float> tabLabelWidths_;
    mutable bool tabLabelWidthsDirty_ = true;

    // 交互状态
    std::optional<std::size_t> hoveredItem_;
    std::optional<std::size_t> pressedItem_;

    // ---- 标签跑马灯（超宽滚动）----
    // 鼠标悬停且标签超出可绘宽度时启动：先停一拍，向左滚到末尾，
    // 停一拍再滚回来（ping-pong）。只挂一块磁贴，悬停离开即停，
    // 不落盘、不参与布局。
    std::optional<std::size_t> marqueeItemIndex_;  // 正在滚动的磁贴（当前页下标）
    float marqueeOffset_ = 0.0F;    // 已滚出的宽度（DIP）
    float marqueeDirection_ = 1.0F; // 1 = 向左滚，-1 = 回滚
    float marqueeOverflow_ = 0.0F;  // 文本超出可用宽度的量（DIP）
    int marqueePause_ = 0;          // 端点停顿剩余 tick
    void stopLabelMarquee();
    void advanceLabelMarquee();

    // ---- 拖拽状态 ----
    // 按下先不动视觉，位移超过 kDragThresholdDip 才升级为拖拽（磁贴浮起
    // 加落点框），短按启动与长拖排序互不干扰。拖拽期间不改任何磁贴位置，
    // 只画落点框，松手才提交（对齐 Win10 开始菜单的行为）。
    bool dragActive_ = false;
    std::optional<std::size_t> dragSource_;   // 被拖动的项
    POINT dragStartPoint_{};                  // 按下时的客户区坐标（物理像素）
    POINT dragCurrentPoint_{};                // 当前光标位置（物理像素）

    // 抓取偏移：按下点相对磁贴左上角的偏移（DIP）。
    // 跟手绘制时用「光标 − 抓取偏移」定位，磁贴不会跳一下。
    float dragGrabOffsetX_ = 0.0F;
    float dragGrabOffsetY_ = 0.0F;

    // 被拖磁贴的尺寸（DIP），用于跟手绘制
    float dragTileWidth_ = 0.0F;
    float dragTileHeight_ = 0.0F;

    // 落点（被拖项将占据的格子）。松手时按它提交重排。
    std::optional<core::Dropspot> dropspotPreview_;

    // 动画定时器 ID
    static constexpr UINT_PTR kAnimationTimerId = 1;
    // 定时器当前是否处于开启状态（避免重复 SetTimer 重置计时）
    bool animationTimerActive_ = false;

    // 图标位图缓存（key = item.id；设备资源重建时清空）
    std::map<std::string, platform::win32::ComPtr<ID2D1Bitmap>> iconCache_;
    // 搜索框左侧的应用图标（exe 资源 101）。与 iconCache_ 一样依附于
    // 渲染目标，设备丢失/重建时置空重取。
    platform::win32::ComPtr<ID2D1Bitmap> appIconBitmap_;

    // ---- 插件绘制缓存 ----
    //
    // pluginTextFormats_：key = 字号（0.1 DIP 精度）。插件布局每帧都会
    // 重新算字号，格式对象必须复用，否则每秒几十次 CreateTextFormat。
    std::map<int, platform::win32::ComPtr<IDWriteTextFormat>> pluginTextFormats_;
    // pluginIconCache_：key = 图标 id + 像素尺寸。几何按目标尺寸直接建，
    // 绘制时只做平移，与钉子/锁图标的做法一致，任意 DPI 都清晰。
    std::map<std::string, platform::win32::SvgGlyph> pluginIconCache_;
    // 插件目录里的图片图标（PNG/JPG/ICO...）：完整路径 → 预乘位图。
    std::map<std::wstring, platform::win32::ComPtr<ID2D1Bitmap>> pluginBitmapCache_;
    platform::win32::ComPtr<ID2D1StrokeStyle> pluginStrokeStyle_;

    // 编辑对话框预览专用的渲染资源：DCRenderTarget 每次绘制临时绑定到
    // 对话框 STATIC 的 HDC。笔刷必须来自这个目标（D2D 1.0 限制），
    // 不能复用主窗口的 borderBrush_。
    platform::win32::ComPtr<ID2D1DCRenderTarget> previewTarget_;
    platform::win32::ComPtr<ID2D1SolidColorBrush> previewBrush_;
    // 预览目标自己的位图缓存（D2D 位图不能跨渲染目标复用）。
    std::map<std::wstring, platform::win32::ComPtr<ID2D1Bitmap>> previewBitmapCache_;

    std::uint32_t dpi_ = 96;

    std::uint16_t gridColumns_ = core::kDefaultGridColumns;
    std::uint16_t gridRows_ = core::kDefaultGridRows;

    // 是否显示磁贴文字标签（设置窗口可控）
    bool showTileLabels_ = true;
    // 图标相对磁贴内框的统一缩放百分比（50~100，默认 90）
    int tileIconScalePercent_ = 90;

    // 标题是否默认显示（false = 仅鼠标移入时浮出）
    bool alwaysShowTileLabels_ = true;

    // 标签栏在窗口底部（false = 顶部，默认；设置窗口可控）
    bool tabBarAtBottom_ = false;

    // 窗口出现位置与「上一次的位置」的坐标（物理像素）
    core::WindowPlacement placement_ = core::WindowPlacement::Mouse;
    POINT lastWindowPos_{};
    bool lastWindowPosValid_ = false;
    WindowMovedHandler windowMovedHandler_;

    // 当前皮肤配色与背景图
    ThemeColors theme_;
    std::wstring backgroundImage_;                 // 绝对路径；空 = 无
    platform::win32::ComPtr<ID2D1Bitmap> backgroundBitmap_;
    void ensureBackgroundBitmap();                 // 设备资源就绪时建位图
    // 背景位图覆盖的目标尺寸（物理像素）：窗口变大超过它时重建，
    // 避免小图被 cover 拉伸变糊。
    int backgroundTargetWidth_ = 0;
    int backgroundTargetHeight_ = 0;

    // 主窗口整体不透明度（50~100，100 = 完全不透明）
    int windowOpacityPercent_ = 100;
    void applyWindowOpacity();

    // 磁贴背景不透明度（0~100；-1 = 跟随皮肤）
    int tileOpacityPercent_ = -1;
    // 全局默认磁贴底色（空 = 跟随皮肤；单项 tileColor 优先）
    std::optional<std::uint32_t> defaultTileColor_;
    // 把主题给的磁贴底色按用户设置换算 alpha。跟随皮肤时原样返回。
    std::uint32_t applyTileOpacityPercent(std::uint32_t baseColor) const;

    // ---- 钉子状态 ----
    // pinned_：是否钉住（钉住 = 点击窗口外不隐藏）
    // pinHovered_：鼠标是否悬停在钉子上（用于画悬停高亮）
    // pinPressed_：鼠标是否正按在钉子上（用于区分「点钉子」与「拖拽」）
    bool pinned_ = false;
    bool pinHovered_ = false;
    bool pinPressed_ = false;
    // 锁定按钮（与钉子同一套三态：悬停/按下/锁定）
    bool tilesLocked_ = false;
    bool lockHovered_ = false;
    bool lockPressed_ = false;

    // 钉子图形（Lucide pin）的 D2D 几何缓存：以按钮左上角为原点、DIP 建
    // 一次，绘制时平移，窗口移动/改列数/换 DPI 都不需重建。线框与实心
    // 两份：FillGeometry 会跳过 HOLLOW 起始的图元，实心版必须单独建。
    platform::win32::ComPtr<ID2D1PathGeometry> pinGeometry_;
    platform::win32::ComPtr<ID2D1PathGeometry> pinFillGeometry_;
    platform::win32::ComPtr<ID2D1StrokeStyle> pinStrokeStyle_;
    void ensurePinGeometry();
    // 锁定按钮的几何：锁定/解锁两枚独立的实心剪影
    // （TurboWarp 的 lock.svg / unlock.svg，12x12 viewBox，直接填充绘制）
    platform::win32::ComPtr<ID2D1PathGeometry> lockClosedGeometry_;
    platform::win32::ComPtr<ID2D1PathGeometry> lockOpenGeometry_;
    void ensureLockGeometry();

    // 模态交互保护（设置窗口打开期间为 true，见 setModalGuard）
    bool modalGuard_ = false;

    // 外部点击监视器：窗口显示期间才需要（隐藏后不必监听）
    platform::win32::OutsideClickMonitor outsideClickMonitor_;

    // ---- 搜索状态 ----
    // 文本输入由子 Edit 控件承担：唤出时 show() 把焦点交给它，WM_KEYDOWN /
    // WM_CHAR / WM_IME_* 由系统直送，IME 中文是原生行为；焦点移走输入
    // 自然停止，无需闸门。
    // searchQuery_：查询串（UTF-8），空表示不过滤。文本来自 Edit 控件
    //（EN_CHANGE 读回），程序侧改动会反向同步。
    std::string searchQuery_;
    // ---- 搜索框 Edit 控件与配套 GDI 资源 ----
    static constexpr int kSearchEditId = 9001;
    static constexpr UINT_PTR kSearchEditSubclassId = 1;
    HWND searchEdit_ = nullptr;
    HFONT searchEditFont_ = nullptr;
    int searchEditFontHeight_ = 0;   // 当前字体的像素高度（DPI 变了就重建）
    HBRUSH searchEditBkBrush_ = nullptr;
    HBRUSH searchEditBkActiveBrush_ = nullptr;
    // 抑制 EN_CHANGE 的往返：程序改控件文本时，通知回来不要再写回去
    bool searchEditSync_ = false;
    // visibleMask_：与「当前 Tab 的 items」一一对应，true 表示匹配。
    // 用掩码而不是过滤后的索引列表：掩码可以直接用 itemIndex 下标访问，
    // 而布局结果里的 placement.itemIndex 正是原索引，不需要映射转换。
    std::vector<bool> visibleMask_;
    // searchActive_：搜索框是否处于「焦点」状态（外框强调蓝 + 底衬加深）
    bool searchActive_ = false;

    // ---- 拖入添加的落点预览 ----
    //
    // fileDropActive_：当前有文件/链接被拖到窗口上（画落点框 + 让位预演）
    // fileDropSpot_  ：新磁贴将占据的格子（由光标位置算出）
    // fileDropSize_  ：新磁贴的尺寸（决定落点框大小）
    bool fileDropActive_ = false;
    core::TileSize fileDropSize_ = core::TileSize::Small;
    std::optional<core::Dropspot> fileDropSpot_;

    // ---- 「外部点击」的延迟判定 ----
    // 从资源管理器拖文件进来时，按下鼠标发生在窗口外：立即收起会让拖放
    // 没机会落进窗口。按下后先探测：很快松开且光标几乎没动按普通点击
    // 收起；仍按着且移动了判定为拖动，保持可见。探测有次数上限，长按
    // 滚动条时窗口最终仍会收起。
    bool outsideProbeActive_ = false;
    POINT outsideProbeDownPoint_{};
    int outsideProbeAttempts_ = 0;
    static constexpr UINT_PTR kOutsideProbeTimerId = 3;
    // 每次探测的间隔
    static constexpr UINT kOutsideProbeIntervalMs = 120;
    // 探测次数上限（120ms × 8 ≈ 1 秒）
    static constexpr int kOutsideProbeMaxAttempts = 8;
    // 判定为「在拖动」所需的位移（物理像素，够区分手抖与真拖）
    static constexpr int kOutsideProbeDragThresholdPx = 10;

    // ---- 拖入落点框的看门狗 ----
    // 个别拖拽来源（Directory Opus）结束拖动时补发 DragEnter 却不给
    // DragLeave，预览会残留。双重兜底：setFileDropTarget 只在左键按着时
    // 点亮（补发那次按键已松开，直接忽略）；点亮期间每 250ms 查左键，
    // 松开即撤。
    static constexpr UINT_PTR kFileDropGuardTimerId = 4;
    static constexpr UINT kFileDropGuardPollMs = 250;

    // ---- 启动后一次性工作集裁剪 ----
    // 初始化摸过的物理页在面板显示前用不上，交给系统回收：隐藏待机时
    // 内存从 ~40MB 降到个位数，唤出时按需换入。用延迟定时器而非直接调：
    // 启动末尾的首次渲染会把页立刻换回来，直接调等于白裁。
    static constexpr UINT_PTR kStartupTrimTimerId = 5;
    static constexpr UINT_PTR kMarqueeTimerId = 6;
    static constexpr UINT kStartupTrimDelayMs = 3000;
    void trimWorkingSet();

    // ---- 键盘导航选中 ----
    // 选中的磁贴（按原 itemIndex）。未选中为空。
    std::optional<std::size_t> selectedItem_;

    // 回调
    LaunchHandler launchHandler_;
    ContextMenuHandler contextMenuHandler_;
    DocumentChangedHandler documentChangedHandler_;
    CloseHandler closeHandler_;
    SettingsHandler settingsHandler_;
    SettingsHandler aboutHandler_;
    AddItemHandler addItemHandler_;
    PluginViewProvider pluginViewProvider_;
    PluginCatalogProvider pluginCatalogProvider_;
    AddPluginHandler addPluginHandler_;
    PanelVisibilityHandler panelShownHandler_;
    PanelVisibilityHandler panelHiddenHandler_;
    std::function<void(bool)> pinnedChangedHandler_;
    ActiveTabChangedHandler activeTabChangedHandler_;
};

}  // namespace ui
}  // namespace ktile
