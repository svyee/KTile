// KTile - UI 层：启动器窗口 —— 布局、命中测试、显示控制
//
// 本文件与 launcher_window.cpp 共同构成 LauncherWindow 实现。
// 拆分目的：渲染逻辑与窗口管理逻辑分离，便于各自维护。
//
#include "ui/launcher_window.h"

#include "base/text_convert.h"
#include "core/item_operations.h"
#include "core/tab_operations.h"
#include "platform/win32/popup_menu.h"
#include "ui/text_input_dialog.h"

#include <Windows.h>
#include <imm.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "imm32.lib")

namespace ktile {
namespace ui {
namespace {

// 标签页右键菜单的命令 ID
//（避开磁贴菜单 3001..3032、空白区菜单 3101..3102）
constexpr UINT kTabMenuNew = 3201;
constexpr UINT kTabMenuRename = 3202;
constexpr UINT kTabMenuRemove = 3203;
constexpr UINT kTabMenuMoveLeft = 3204;
constexpr UINT kTabMenuMoveRight = 3205;

// 窗口最大允许尺寸（防止超大网格导致窗口超出屏幕）
constexpr float kMaximumWindowWidthDip = 1600.0F;
constexpr float kMaximumWindowHeightDip = 1400.0F;

// 屏幕可用工作区（多显示器时取窗口所在显示器）
RECT workAreaForWindow(HWND hwnd) {
    RECT workArea{};
    if (hwnd != nullptr) {
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(MONITORINFO);
        if (GetMonitorInfoW(monitor, &info) != FALSE) {
            return info.rcWork;
        }
    }
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    return workArea;
}

}  // namespace

// ================================================================ DPI

float LauncherWindow::dpiScale() const noexcept {
    return static_cast<float>(dpi_) / 96.0F;
}

void LauncherWindow::updateDpi() {
    if (hwnd_ == nullptr) {
        return;
    }
    // GetDpiForWindow 是 Win10 1607+ API，Win7 上不可用。
    // 使用 GetDeviceCaps 兼容方案。
    HDC dc = GetDC(hwnd_);
    if (dc != nullptr) {
        const int dpiX = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(hwnd_, dc);
        if (dpiX > 0) {
            dpi_ = static_cast<std::uint32_t>(dpiX);
            dpiChangedForLayout();
            return;
        }
    }
    dpi_ = 96;
    dpiChangedForLayout();
}

// DPI 变化后的布局善后。
//
// 标签宽度是按 DIP 文字宽度算的，字号随 DPI 换算成物理像素后
// 文字实际占的 DIP 宽度也会变（不同 DPI 的字体度量并不线性），
// 所以缓存必须作废重算；重算后可见页数可能变化，再让活动页回到视野内。
void LauncherWindow::dpiChangedForLayout() {
    invalidateTabLabelWidths();
    ensureActiveTabVisible();
}

// ================================================================ 布局

void LauncherWindow::updateLayout() {
    const auto& items = activeItems();

    core::TileLayoutRequest request;
    request.columns = gridColumns_;
    request.rows = gridRows_;
    request.items = &items;
    request.preserveExplicitPositions = true;

    layout_ = core::calculateTileLayout(request);

    // 文档或布局变化后，可见性掩码必须跟着重建 —— 否则会因为下标错位
    // 把「匹配」标到别的磁贴上（增删磁贴后最容易出现）。
    rebuildVisibilityMask();

    // 选中项若已不存在（被删除）或已被过滤掉，主动清掉，避免画出野框
    if (selectedItem_.has_value() &&
        (*selectedItem_ >= items.size() || !isItemVisible(*selectedItem_))) {
        selectedItem_.reset();
    }
}

// 客户区物理尺寸（已按「最大窗口」夹取）。
//
// 只由网格配置决定，与「窗口放哪」无关 —— 所以尺寸计算单独拆出来，
// 定位逻辑（calculateWindowRect）与「只改尺寸不动位置」的
// applyGridGeometry 共用同一份数字。
void LauncherWindow::windowSizePixels(int* width, int* height) const {
    // 窗口客户区 = 完整网格尺寸（gridColumns_ x gridRows_），固定不变。
    //
    // 刻意不按「已用行列」收缩：早期实现会根据布局实际占用收缩窗口，
    // 结果是拖动磁贴到边缘时窗口跟着变大变小，视觉上很不安定，
    // 也容易让用户误以为磁贴被挤出去了。
    const std::uint16_t usedColumns = std::max<std::uint16_t>(gridColumns_, 1);
    const std::uint16_t usedRows = std::max<std::uint16_t>(gridRows_, 1);

    // 客户区尺寸（DIP）
    const float clientWidth = metrics_.outerPadding * 2.0F +
        static_cast<float>(usedColumns) * metrics_.cellSize +
        static_cast<float>(usedColumns > 0 ? usedColumns - 1 : 0) * metrics_.cellGap;

    const float clientHeight = metrics_.outerPadding + metrics_.bottomPadding +
        metrics_.headerHeight +
        // 标签条也占一行高度：窗口高度 = 上内边距 + 标题带 + 标签条 + 网格
        // + 下内边距。漏掉这一段的话磁贴会被窗口下边缘裁掉。
        kTabBarGapTop + kTabBarHeight + kTabBarGapBottom +
        static_cast<float>(usedRows) * metrics_.cellSize +
        static_cast<float>(usedRows > 0 ? usedRows - 1 : 0) * metrics_.cellGap;

    const float scale = dpiScale();
    int pixelWidth = static_cast<int>(std::lround(clientWidth * scale));
    int pixelHeight = static_cast<int>(std::lround(clientHeight * scale));

    const int maxWidth =
        static_cast<int>(std::lround(kMaximumWindowWidthDip * scale));
    const int maxHeight =
        static_cast<int>(std::lround(kMaximumWindowHeightDip * scale));
    pixelWidth = std::min(pixelWidth, maxWidth);
    pixelHeight = std::min(pixelHeight, maxHeight);

    if (width != nullptr) {
        *width = pixelWidth;
    }
    if (height != nullptr) {
        *height = pixelHeight;
    }
}

// 按「窗口出现位置」策略算显示矩形。
//
// 所有策略都以**鼠标所在显示器**的工作区为准：用户可能在第二块屏上
// 按热键唤出，窗口跑到主屏角落会显得莫名其妙。
RECT LauncherWindow::calculateWindowRect() const {
    int windowWidth = 0;
    int windowHeight = 0;
    windowSizePixels(&windowWidth, &windowHeight);

    POINT cursor{};
    const bool haveCursor = GetCursorPos(&cursor) != FALSE;

    HMONITOR monitor = nullptr;
    if (haveCursor) {
        monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    }
    if (monitor == nullptr) {
        monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    }
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    RECT work = workAreaForWindow(hwnd_);
    if (monitor != nullptr &&
        GetMonitorInfoW(monitor, &monitorInfo) != FALSE) {
        work = monitorInfo.rcWork;
    }

    // 角落策略留一点边距，别让窗口贴着屏幕边框（视觉上像被裁掉）
    const int gap = static_cast<int>(std::lround(8.0F * dpiScale()));

    int left = work.left;
    int top = work.top;
    switch (placement_) {
        case core::WindowPlacement::Mouse:
        case core::WindowPlacement::Last:
            if (placement_ == core::WindowPlacement::Last &&
                lastWindowPosValid_) {
                left = lastWindowPos_.x;
                top = lastWindowPos_.y;
            } else if (haveCursor) {
                // 跟随鼠标：窗口左上角就在光标处，鼠标自然落在搜索框上，
                // 唤出即可直接打字。
                left = cursor.x;
                top = cursor.y;
            } else {
                left = work.left + ((work.right - work.left) - windowWidth) / 2;
                top = work.top + ((work.bottom - work.top) - windowHeight) / 2;
            }
            break;
        case core::WindowPlacement::LeftTop:
            left = work.left + gap;
            top = work.top + gap;
            break;
        case core::WindowPlacement::RightTop:
            left = work.right - windowWidth - gap;
            top = work.top + gap;
            break;
        case core::WindowPlacement::LeftBottom:
            left = work.left + gap;
            top = work.bottom - windowHeight - gap;
            break;
        case core::WindowPlacement::RightBottom:
            left = work.right - windowWidth - gap;
            top = work.bottom - windowHeight - gap;
            break;
        case core::WindowPlacement::Center:
            left = work.left + ((work.right - work.left) - windowWidth) / 2;
            top = work.top + ((work.bottom - work.top) - windowHeight) / 2;
            break;
    }

    // 夹进工作区：鼠标贴着屏幕边角唤出时，窗口只可能露出一部分；
    // 「上一次的位置」也可能来自已拔掉的显示器。
    if (left + windowWidth > work.right) {
        left = work.right - windowWidth;
    }
    if (top + windowHeight > work.bottom) {
        top = work.bottom - windowHeight;
    }
    if (left < work.left) {
        left = work.left;
    }
    if (top < work.top) {
        top = work.top;
    }

    RECT rect{};
    rect.left = left;
    rect.top = top;
    rect.right = left + windowWidth;
    rect.bottom = top + windowHeight;
    return rect;
}

// ================================================================ 命中测试

std::optional<std::size_t> LauncherWindow::hitTestItem(const float xDip,
                                                       const float yDip) const {
    if (document_.tabs.empty()) {
        return std::nullopt;
    }

    for (const auto& placement : layout_.placements) {
        const bool overflowed = std::find(
            layout_.overflowed.begin(), layout_.overflowed.end(),
            placement.itemIndex) != layout_.overflowed.end();
        if (overflowed) {
            continue;
        }

        // 搜索过滤中：被淡化的磁贴不可点。
        // 否则会出现「看不见的东西还能点开」的诡异体验 ——
        // 淡化的是视觉，但命中测试必须与视觉一致。
        if (!isItemVisible(placement.itemIndex)) {
            continue;
        }

        const float left = metrics_.outerPadding +
            static_cast<float>(placement.column) * (metrics_.cellSize + metrics_.cellGap);
        const float top = gridOriginY() +
            static_cast<float>(placement.row) * (metrics_.cellSize + metrics_.cellGap);
        const float width = metrics_.tileWidth(placement.spanColumns);
        const float height = metrics_.tileHeight(placement.spanRows);

        if (xDip >= left && xDip < left + width &&
            yDip >= top && yDip < top + height) {
            return placement.itemIndex;
        }
    }
    return std::nullopt;
}

// ================================================================ 数据与配置

void LauncherWindow::setDocument(core::ItemsDocument document) {
    document_ = std::move(document);
    // 新文档的页数/页名可能与旧的不同：标签宽度缓存与活动页下标
    // 都要按新文档重新校准（app 层在「添加/编辑/删除」后都会换上
    // 一份新文档，漏了这两步就会缓出错位）。
    invalidateTabLabelWidths();
    document_.ensureDefaultTab();
    clampActiveTab();
    refresh();
}

bool LauncherWindow::setItemTileSize(const std::size_t itemIndex,
                                     const core::TileSize size) {
    if (document_.tabs.empty()) {
        return false;
    }
    auto& items = activeItems();
    if (itemIndex >= items.size()) {
        return false;
    }
    if (items[itemIndex].tileSize == size) {
        return true;
    }

    items[itemIndex].tileSize = size;
    // 尺寸变化后显式坐标可能不再合法，清除以免布局引擎困惑
    items[itemIndex].clearGridPosition();

    recomputeLayout();

    if (documentChangedHandler_) {
        documentChangedHandler_(document_);
    }
    return true;
}

bool LauncherWindow::setItemLabelMode(const std::size_t itemIndex,
                                      const core::TileLabelMode mode) {
    if (document_.tabs.empty()) {
        return false;
    }
    auto& items = activeItems();
    if (itemIndex >= items.size()) {
        return false;
    }
    if (items[itemIndex].labelMode == mode) {
        return true;  // 没变，幂等返回，调用方无需特殊处理
    }

    items[itemIndex].labelMode = mode;

    // 只影响标题画不画 —— 不 clearGridPosition，也不重排布局，
    // 否则用户切一下标题显示方式，磁贴会突然跳位。
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (documentChangedHandler_) {
        documentChangedHandler_(document_);
    }
    return true;
}

void LauncherWindow::recomputeLayout() {
    updateLayout();
    // 窗口尺寸固定为网格尺寸（见 calculateWindowRect），因此重排布局
    // 不需要改窗口大小 —— 只重绘即可。这里刻意不调 SetWindowPos：
    // 拖拽落定时窗口绝不能跟着变化，否则会看到窗口「弹」一下。
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ================================================================ 标签页

core::Tab* LauncherWindow::activeTab() {
    if (document_.tabs.empty()) {
        return nullptr;
    }
    clampActiveTab();
    return &document_.tabs[activeTabIndex_];
}

const core::Tab* LauncherWindow::activeTab() const {
    if (document_.tabs.empty()) {
        return nullptr;
    }
    const std::size_t index = (activeTabIndex_ < document_.tabs.size())
                                  ? activeTabIndex_
                                  : document_.tabs.size() - 1;
    return &document_.tabs[index];
}

// 活动页的磁贴（可写）。
//
// 这里调 ensureDefaultTab：本类早期多个入口（如「添加新项目」）假定
// 「一定有一页」，而确保这个不变量在这里做最省事 —— 调用方不必
// 每处都写一遍判空与兜底。
std::vector<core::LaunchItem>& LauncherWindow::activeItems() {
    document_.ensureDefaultTab();
    clampActiveTab();
    return document_.tabs[activeTabIndex_].items;
}

const std::vector<core::LaunchItem>& LauncherWindow::activeItems() const {
    // 文档意外为空时返回共享空表：调用方不必到处判空。
    // 加载器与 core::removeTab 都保证「至少一页」，这里只是兜底。
    static const std::vector<core::LaunchItem> kEmptyItems;
    const core::Tab* tab = activeTab();
    return (tab != nullptr) ? tab->items : kEmptyItems;
}

// 把 activeTabIndex_ / firstVisibleTab_ 夹回合法范围。
// 删页、换文档（setDocument）之后必须调用，否则会索引到不存在的页。
void LauncherWindow::clampActiveTab() {
    if (document_.tabs.empty()) {
        activeTabIndex_ = 0;
        firstVisibleTab_ = 0;
        return;
    }
    if (activeTabIndex_ >= document_.tabs.size()) {
        activeTabIndex_ = document_.tabs.size() - 1;
    }
    if (firstVisibleTab_ >= document_.tabs.size()) {
        firstVisibleTab_ = document_.tabs.size() - 1;
    }
}

std::string LauncherWindow::activeTabId() const {
    const core::Tab* tab = activeTab();
    return (tab != nullptr) ? tab->id : std::string();
}

// 切页时清掉「与旧页下标绑定」的瞬态状态。
//
// animations_ / hoveredItem_ / selectedItem_ 全是**按下标索引**的：
// 换页后同一个下标指向的是另一块磁贴，不清掉就会出现
// 「刚切过去，某块磁贴自己抖一下」或者「键盘选中框框住了不相干的磁贴」。
void LauncherWindow::resetTransientForTabSwitch() {
    hoveredItem_.reset();
    pressedItem_.reset();
    selectedItem_.reset();
    dragActive_ = false;
    dragSource_.reset();
    dropspotPreview_.reset();
    fileDropActive_ = false;
    fileDropSpot_.reset();
    animations_.clear();
    stopAnimationTimer();
}

void LauncherWindow::setActiveTabIndex(const std::size_t index) {
    if (document_.tabs.empty()) {
        return;
    }
    const std::size_t target = (index < document_.tabs.size())
                                   ? index
                                   : document_.tabs.size() - 1;
    if (target == activeTabIndex_) {
        return;  // 点了当前页：什么都不做（不重排、不重绘、不落盘）
    }

    activeTabIndex_ = target;
    resetTransientForTabSwitch();
    ensureActiveTabVisible();
    // 新页的磁贴集合不同 → 布局与搜索可见性掩码都要重算。
    // 搜索查询刻意保留：用户搜到一半切页，多半就是想找另一页里的东西，
    // 清空反而要重打一遍。
    recomputeLayout();

    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // 注意：切页**不算数据变更**，所以不通知 documentChangedHandler_
    //（那是 items.json 的落盘信号，而 items 一个字节都没变）。
    // 只通知「活动页变了」，由 app 层记住位置。
    if (activeTabChangedHandler_) {
        activeTabChangedHandler_(activeTabIndex_);
    }
}

bool LauncherWindow::setActiveTabById(const std::string& tabId) {
    if (tabId.empty()) {
        return false;
    }
    const std::size_t index = document_.findTabIndex(tabId);
    if (index >= document_.tabs.size()) {
        return false;  // 上次那页已被删除：停在第一页即可
    }
    setActiveTabIndex(index);
    return true;
}

void LauncherWindow::cycleActiveTab(const int delta) {
    const std::size_t count = document_.tabs.size();
    if (count < 2 || delta == 0) {
        return;
    }
    const long long step = delta;
    long long target = static_cast<long long>(activeTabIndex_) + step;
    target %= static_cast<long long>(count);
    if (target < 0) {
        target += static_cast<long long>(count);
    }
    setActiveTabIndex(static_cast<std::size_t>(target));
}

std::size_t LauncherWindow::createTab(const std::string& rawName) {
    const std::size_t index = core::addTab(document_, rawName);
    invalidateTabLabelWidths();
    // 新建的页立刻切过去：用户刚建它就是为了往里面放东西，
    // 建完还停在旧页会让他以为「没建成」。
    setActiveTabIndex(index);
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (documentChangedHandler_) {
        documentChangedHandler_(document_);
    }
    return index;
}

bool LauncherWindow::renameTab(const std::size_t index,
                               const std::string& rawName) {
    if (!core::renameTab(document_, index, rawName)) {
        return false;
    }
    // 页名变了 → 标签宽度也变了，必须作废缓存，否则新名字会被
    // 按旧宽度截断（或留一大片空白）。
    invalidateTabLabelWidths();
    ensureActiveTabVisible();
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (documentChangedHandler_) {
        documentChangedHandler_(document_);
    }
    return true;
}

bool LauncherWindow::removeTab(const std::size_t index) {
    if (!core::removeTab(document_, index)) {
        return false;
    }
    invalidateTabLabelWidths();
    // 删的若是当前页，activeTabIndex_ 会落在新的末页上（clampActiveTab），
    // 但绑在旧下标上的瞬态状态必须清掉 —— 不然后面那块磁贴会莫名抖一下。
    resetTransientForTabSwitch();
    clampActiveTab();
    ensureActiveTabVisible();
    recomputeLayout();
    if (documentChangedHandler_) {
        documentChangedHandler_(document_);
    }
    return true;
}

bool LauncherWindow::moveTab(const std::size_t from, const std::size_t to) {
    if (!core::moveTab(document_, from, to)) {
        return false;
    }
    // 活动页必须跟着**页本身**走，而不是留在原来的下标上：
    // 用户左移当前页之后，看到的应该还是同一页内容。
    if (activeTabIndex_ == from) {
        activeTabIndex_ = to;
    } else if (from < activeTabIndex_ && to >= activeTabIndex_) {
        --activeTabIndex_;
    } else if (from > activeTabIndex_ && to <= activeTabIndex_) {
        ++activeTabIndex_;
    }
    invalidateTabLabelWidths();
    ensureActiveTabVisible();
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (documentChangedHandler_) {
        documentChangedHandler_(document_);
    }
    return true;
}

void LauncherWindow::promptCreateTab() {
    std::wstring name;
    if (!showTextInputDialog(hwnd_, L"新建标签页", L"页面名称：",
                             std::wstring(), &name)) {
        return;  // 用户取消
    }
    (void)createTab(base::wideToUtf8Lossy(name));
}

void LauncherWindow::promptRenameTab(const std::size_t tabIndex) {
    if (tabIndex >= document_.tabs.size()) {
        return;
    }
    const std::wstring current =
        base::utf8ToWideLossy(document_.tabs[tabIndex].name);
    std::wstring name;
    if (!showTextInputDialog(hwnd_, L"重命名标签页", L"页面名称：", current,
                             &name)) {
        return;
    }
    (void)renameTab(tabIndex, base::wideToUtf8Lossy(name));
}

// 标签上右键的页面菜单。
//
// 页面管理全部收敛到这一处：新建、改名、删除、调顺序。之所以不放进
// 设置窗口 —— 那是「全局偏好」，而页面是「当前这块面板的内容结构」，
// 在页面上右键改它，路径最短。
void LauncherWindow::showTabContextMenu(const std::size_t tabIndex,
                                        const POINT screenPoint) {
    if (tabIndex >= document_.tabs.size()) {
        return;
    }
    const std::size_t count = document_.tabs.size();

    std::vector<platform::win32::PopupMenuEntry> entries;
    entries.push_back(platform::win32::PopupMenuEntry::action(
        kTabMenuNew, L"新建标签页..."));
    entries.push_back(platform::win32::PopupMenuEntry::action(
        kTabMenuRename, L"重命名此页..."));
    entries.push_back(platform::win32::PopupMenuEntry::separator());

    // 只剩一页时不允许删：删除按钮灰掉并说明原因（比点了没反应清楚）
    const bool canRemove = count > 1;
    std::wstring removeText = L"删除此页";
    if (canRemove) {
        const std::size_t itemCount = document_.tabs[tabIndex].items.size();
        if (itemCount > 0) {
            removeText += L"（含 " + std::to_wstring(itemCount) + L" 个项目）";
        }
    } else {
        removeText = L"删除此页（至少要保留一页）";
    }
    entries.push_back(platform::win32::PopupMenuEntry::action(
        kTabMenuRemove, removeText, canRemove));

    entries.push_back(platform::win32::PopupMenuEntry::separator());
    entries.push_back(platform::win32::PopupMenuEntry::action(
        kTabMenuMoveLeft, L"左移", tabIndex > 0));
    entries.push_back(platform::win32::PopupMenuEntry::action(
        kTabMenuMoveRight, L"右移", tabIndex + 1 < count));

    const UINT command = platform::win32::showPopupMenu(hwnd_, entries,
                                                       screenPoint);
    switch (command) {
        case kTabMenuNew:
            promptCreateTab();
            break;
        case kTabMenuRename:
            promptRenameTab(tabIndex);
            break;
        case kTabMenuRemove: {
            if (!canRemove) {
                break;
            }
            // 删页会连带删掉页内所有磁贴 —— 必须确认。确认框里带上
            // 页名与项目数，用户才知道自己在删什么。
            const std::wstring pageName =
                base::utf8ToWideLossy(document_.tabs[tabIndex].name);
            std::wstring message = L"确定删除标签页「" + pageName + L"」吗？";
            const std::size_t itemCount = document_.tabs[tabIndex].items.size();
            if (itemCount > 0) {
                message += L"\n\n该页里的 " + std::to_wstring(itemCount) +
                           L" 个项目也会一并删除。";
            } else {
                message += L"\n\n该页是空的。";
            }
            if (MessageBoxW(hwnd_, message.c_str(), L"KTile",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) ==
                IDYES) {
                (void)removeTab(tabIndex);
            }
            break;
        }
        case kTabMenuMoveLeft:
            (void)moveTab(tabIndex, tabIndex - 1);
            break;
        case kTabMenuMoveRight:
            (void)moveTab(tabIndex, tabIndex + 1);
            break;
        default:
            break;
    }
}

// ================================================================ 搜索过滤

void LauncherWindow::setSearchQuery(const std::string& query) {
    const std::string normalized = core::normalizeSearchQuery(query);
    if (searchQuery_ == normalized && !visibleMask_.empty()) {
        return;  // 没变化，避免无谓重绘
    }

    searchQuery_ = normalized;
    rebuildVisibilityMask();

    // 查询变化后，原选中项可能已被过滤掉 —— 自动落到第一个可见项，
    // 这样「输入后再按回车」总能启动一个合理的目标。
    if (hasActiveSearch()) {
        selectedItem_ = firstVisibleItem();
    }
    activateSearch();

    // 反向同步到 Edit 控件：处理的情形是「程序主动改查询」
    // （调试预置查询、clearSearch）。文本相同就不动 ——
    // 否则每次正常键入都会重置控件的光标位置。
    if (searchEdit_ != nullptr && !searchEditSync_) {
        const std::wstring want = base::utf8ToWideLossy(searchQuery_);
        const int len = GetWindowTextLengthW(searchEdit_);
        std::wstring current(static_cast<std::size_t>(len) + 1, L'\0');
        GetWindowTextW(searchEdit_, &current[0], len + 1);
        current.resize(static_cast<std::size_t>(len));
        if (current != want) {
            searchEditSync_ = true;
            SetWindowTextW(searchEdit_, want.c_str());
            searchEditSync_ = false;
        }
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void LauncherWindow::clearSearch() {
    // 先清控件文本（EN_CHANGE 会被 searchEditSync_ 挡住，不会绕回来）
    searchEditSync_ = true;
    if (searchEdit_ != nullptr) {
        SetWindowTextW(searchEdit_, L"");
    }
    searchEditSync_ = false;
    searchQuery_.clear();
    rebuildVisibilityMask();
    selectedItem_.reset();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void LauncherWindow::rebuildVisibilityMask() {
    const auto& items = activeItems();

    const auto matches = core::searchItems(items, searchQuery_);

    // 掩码与 items 一一对应。用 matches 里出现过的下标置 true。
    visibleMask_.assign(items.size(), false);
    for (const auto& match : matches) {
        if (match.itemIndex < visibleMask_.size()) {
            visibleMask_[match.itemIndex] = true;
        }
    }
}

bool LauncherWindow::isItemVisible(const std::size_t itemIndex) const {
    // 无搜索时全部可见（含掩码尚未建立的情况）
    if (!hasActiveSearch()) {
        return true;
    }
    if (itemIndex >= visibleMask_.size()) {
        return true;  // 越界视为可见，交给布局/绘制环节的正常判断处理
    }
    return visibleMask_[itemIndex];
}

std::optional<std::size_t> LauncherWindow::firstVisibleItem() const {
    if (document_.tabs.empty()) {
        return std::nullopt;
    }
    const auto& items = activeItems();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (isItemVisible(i)) {
            return i;
        }
    }
    return std::nullopt;
}

// ================================================================ 键盘导航

void LauncherWindow::moveSelection(const int deltaColumn, const int deltaRow) {
    if (document_.tabs.empty()) {
        return;
    }
    const auto& items = activeItems();
    if (items.empty()) {
        return;
    }

    // 没有任何可见项时不做任何事（全被过滤掉）
    if (!firstVisibleItem().has_value()) {
        selectedItem_.reset();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // 尚无选中：第一次按方向键就选中第一个可见项
    if (!selectedItem_.has_value() || *selectedItem_ >= items.size() ||
        !isItemVisible(*selectedItem_)) {
        selectedItem_ = firstVisibleItem();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // 按网格坐标移动：用布局结果求出当前项的行列，再加 delta 找最近项。
    //
    // 为什么不用「数组下标 ±1」：磁贴是二维排布，且大磁贴占多格，
    // 数组相邻不等于视觉相邻。按坐标算才符合直觉。
    const core::TilePlacement* current = nullptr;
    for (const auto& placement : layout_.placements) {
        if (placement.itemIndex == *selectedItem_) {
            current = &placement;
            break;
        }
    }
    if (current == nullptr) {
        return;
    }

    const int targetColumn =
        static_cast<int>(current->column) + static_cast<int>(deltaColumn);
    const int targetRow =
        static_cast<int>(current->row) + static_cast<int>(deltaRow);

    // 在可见项里找「与目标格距离最近」的一块。
    // 距离用列差 + 行差；方向键只走一格，所以最优解通常就是正对着的那块。
    // 若该方向没有磁贴（空格），则找该方向上第一个可见磁贴。
    std::optional<std::size_t> best;
    long bestDistance = 0;

    for (const auto& placement : layout_.placements) {
        if (!isItemVisible(placement.itemIndex)) {
            continue;
        }
        if (placement.itemIndex == *selectedItem_) {
            continue;
        }

        const long dc = static_cast<long>(placement.column) -
                        static_cast<long>(targetColumn);
        const long dr = static_cast<long>(placement.row) - static_cast<long>(targetRow);

        // 方向过滤：向右移动时只考虑目标格右边的，避免「原地打转」
        if (deltaColumn > 0 && dc < 0) continue;
        if (deltaColumn < 0 && dc > 0) continue;
        if (deltaRow > 0 && dr < 0) continue;
        if (deltaRow < 0 && dr > 0) continue;
        // 垂直移动时不希望横向漂移太多；给出一个宽松上限
        if (deltaRow != 0 && deltaColumn == 0 && std::labs(dc) > 1) continue;
        if (deltaColumn != 0 && deltaRow == 0 && std::labs(dr) > 1) continue;

        const long distance = std::labs(dc) + std::labs(dr);
        if (!best.has_value() || distance < bestDistance) {
            best = placement.itemIndex;
            bestDistance = distance;
        }
    }

    if (best.has_value()) {
        selectedItem_ = best;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void LauncherWindow::clearSelection() {
    selectedItem_.reset();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void LauncherWindow::ensureSelectionVisible() {
    // 本窗口不滚动，选中项恒在可见区域内。保留此钩子是为了
    // 将来若引入分页/滚动时不必改调用点。
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ================================================================ 按键处理
//
// 窗口有焦点时 WM_KEYDOWN 由系统直送。抽成独立方法是为了让
// 键盘导航与搜索框的编辑键互不干扰（文本编辑由 Edit 控件原生处理）。

LRESULT LauncherWindow::handleKeyDown(const UINT virtualKey) {
    if (virtualKey == VK_ESCAPE) {
        // 拖拽中按 ESC = 取消拖拽，被挤开的磁贴回位
        if (dragActive_) {
            dragActive_ = false;
            dragSource_.reset();
            dropspotPreview_.reset();
            pressedItem_.reset();
            animateBackToLayout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        // 搜索中按 ESC = 先清空搜索（不清空就直接关窗会让人措手不及）
        if (hasActiveSearch() || searchActive_) {
            clearSearch();
            deactivateSearch();
            selectedItem_.reset();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        hide();
        return 0;
    }

    // ---- 搜索模式下的编辑键 ----
    // （退格/删除等文本编辑由搜索框的 Edit 控件原生处理，这里不再管）

    // ---- 标签页快捷键 ----
    //
    // Ctrl+Tab / Ctrl+Shift+Tab：循环切页（与浏览器一致）。
    // Ctrl+1..9：直接跳到第 N 页。
    // 这两组键在搜索框有焦点时也要生效 —— 那里由 searchEditProc 转发
    // 过来（Edit 控件本身不处理 Ctrl+Tab）。
    const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrlDown && virtualKey == VK_TAB) {
        const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        cycleActiveTab(shiftDown ? -1 : 1);
        return 0;
    }
    if (ctrlDown && virtualKey >= '1' && virtualKey <= '9') {
        const std::size_t index =
            static_cast<std::size_t>(virtualKey - '1');
        if (index < document_.tabs.size()) {
            setActiveTabIndex(index);
        }
        return 0;
    }

    // ---- 方向键导航 ----
    // 搜索状态下方向键同样有效：让「输入 → 方向键选 → 回车启动」
    // 成为一条无需碰鼠标的完整链路。
    switch (virtualKey) {
        case VK_LEFT:
            moveSelection(-1, 0);
            return 0;
        case VK_RIGHT:
            moveSelection(1, 0);
            return 0;
        case VK_UP:
            moveSelection(0, -1);
            return 0;
        case VK_DOWN:
            moveSelection(0, 1);
            return 0;
        case VK_RETURN: {
            // 回车启动当前选中项（无选中则启动第一个可见项）
            if (document_.tabs.empty()) {
                return 0;
            }
            const auto& items = activeItems();
            std::size_t index = 0;
            bool haveTarget = false;
            if (selectedItem_.has_value() && *selectedItem_ < items.size() &&
                isItemVisible(*selectedItem_)) {
                index = *selectedItem_;
                haveTarget = true;
            } else if (const auto first = firstVisibleItem()) {
                index = *first;
                haveTarget = true;
            }
            if (haveTarget && launchHandler_) {
                launchHandler_(items[index]);
            }
            return 0;
        }
        default:
            break;
    }
    return 0;
}


bool LauncherWindow::moveItemToSpot(const std::size_t fromIndex,
                                    const core::Dropspot& spot) {
    if (document_.tabs.empty()) {
        return false;
    }
    auto& items = activeItems();
    if (fromIndex >= items.size()) {
        return false;
    }

    // 落点即结果：把避让式布局算出的排布固化成显式坐标。
    //
    // 之所以不用「从数组里抽出来再插进某个序号」（早期做法）：
    //   1. 那样等于告诉引擎「重新流式排布」，空位会被推到右下角，
    //      用户想在中途留个洞是不可能的
    //   2. 序号位移会让整排磁贴跟着挪，视觉上像整个面板都在动
    // 改成显式坐标后，只有真正让位的磁贴挪动，其余原地不动。
    //
    // 先把「当前屏幕上就是这么摆的」写进数据：避让布局只对**有坐标**的项
    // 做「冻结原位、冲突才让位」，没有坐标的项会被当成新来的统统挤到落点
    // 附近重排 —— 整个面板会跟着跳。补齐坐标后，「只动该动的」才成立。
    materializeGridPositions(items);

    core::AvoidanceRequest request;
    request.columns = gridColumns_;
    request.rows = gridRows_;
    request.items = &items;
    request.dragIndex = fromIndex;
    request.targetColumn = spot.column;
    request.targetRow = spot.row;
    request.dragSpanColumns = spot.spanColumns;
    request.dragSpanRows = spot.spanRows;

    const core::TileLayoutResult committed =
        core::calculateAvoidanceLayout(request);
    if (!committed.complete()) {
        return false;
    }

    bool changed = false;
    for (const auto& placement : committed.placements) {
        if (placement.itemIndex >= items.size()) {
            continue;
        }
        const auto& item = items[placement.itemIndex];
        if (!item.hasGridPosition() ||
            *item.gridColumn != placement.column ||
            *item.gridRow != placement.row) {
            changed = true;
            items[placement.itemIndex].setGridPosition(placement.column,
                                                       placement.row);
        }
    }

    animations_.clear();
    stopAnimationTimer();
    recomputeLayout();

    if (!changed) {
        return false;
    }

    // 通知外层持久化：排布是用户可见的状态，必须立刻落盘，
    // 否则崩溃或强制退出会丢失排序结果。
    if (documentChangedHandler_) {
        documentChangedHandler_(document_);
    }
    return true;
}

// ---------------------------------------------------------------- 新项目安放

void LauncherWindow::materializeGridPositions(
    std::vector<core::LaunchItem>& items) const {
    for (const auto& placement : layout_.placements) {
        if (placement.itemIndex >= items.size()) {
            continue;
        }
        // 屏幕上的位置就是它的坐标 —— 只补没有显式坐标的那些，
        // 已有坐标的项尊重数据本身（那才是「用户上一次摆在这儿」）。
        if (!items[placement.itemIndex].hasGridPosition()) {
            items[placement.itemIndex].setGridPosition(placement.column,
                                                       placement.row);
        }
    }
}

bool LauncherWindow::insertNewItemsAt(std::vector<core::LaunchItem> newItems,
                                      const std::optional<POINTL> dropPoint) {
    if (newItems.empty()) {
        return false;
    }
    document_.ensureDefaultTab();
    if (document_.tabs.empty()) {
        return false;
    }
    auto& items = activeItems();

    // 已有磁贴先固化成「现在就摆在这儿」的显式坐标，
    // 后面的避让才会只挤该挤的那几块。
    materializeGridPositions(items);

    // 光标位置换算成客户区坐标：落点解析与预览框共用同一套几何，
    // 预览在哪，松手就落在哪。
    POINT cursor{0, 0};
    const bool hasDropPoint = dropPoint.has_value();
    if (hasDropPoint) {
        cursor.x = dropPoint->x;
        cursor.y = dropPoint->y;
        if (hwnd_ != nullptr && ScreenToClient(hwnd_, &cursor) == FALSE) {
            cursor.x = dropPoint->x;
            cursor.y = dropPoint->y;
        }
    }

    std::size_t added = 0;
    for (core::LaunchItem& item : newItems) {
        std::size_t index = 0;
        // 加进**当前活动页**：拖到哪一页上就加进哪一页，
        // 不是永远的第 0 页（多标签页下那是另一页的内容）。
        if (!core::addItem(document_, activeTabIndex_, std::move(item),
                           &index)) {
            continue;  // 目标为空/id 冲突：跳过这一条，继续处理后面的
        }
        ++added;

        if (!hasDropPoint) {
            // 没有落点（自动化/调试入口）：不写坐标，交给布局引擎
            // 放到第一个放得下的空位。
            continue;
        }

        // 落点框按「这一项的尺寸」画，所以每一项都按自己的跨度算一次；
        // 后加进来的压住先加的 → 自然形成「最后放下的就在光标下、
        // 先前的被挤到旁边」的效果，与一块一块拖进来完全一致。
        const core::Dropspot spot =
            resolveNewItemDropspot(items[index].tileSize, cursor.x, cursor.y);

        core::AvoidanceRequest request;
        request.columns = gridColumns_;
        request.rows = gridRows_;
        request.items = &items;
        request.dragIndex = index;
        request.targetColumn = spot.column;
        request.targetRow = spot.row;
        request.dragSpanColumns = spot.spanColumns;
        request.dragSpanRows = spot.spanRows;

        const core::TileLayoutResult committed =
            core::calculateAvoidanceLayout(request);
        if (committed.complete()) {
            for (const auto& placement : committed.placements) {
                if (placement.itemIndex < items.size()) {
                    items[placement.itemIndex].setGridPosition(
                        placement.column, placement.row);
                }
            }
        }
        // 布局失败（网格塞满）时不写坐标：交给自动布局把它放到别处，
        // 总比叠在别人身上好。
    }

    if (added == 0) {
        return false;
    }

    // 新磁贴的图标要重新解析，动画状态也早已失效
    clearIconCache();
    animations_.clear();
    stopAnimationTimer();
    fileDropActive_ = false;
    fileDropSpot_.reset();
    KillTimer(hwnd_, kFileDropGuardTimerId);
    recomputeLayout();

    if (documentChangedHandler_) {
        documentChangedHandler_(document_);  // 立即落盘
    }
    return true;
}

bool LauncherWindow::moveItem(const std::size_t fromIndex,
                              const std::size_t toIndex) {
    if (document_.tabs.empty()) {
        return false;
    }
    auto& items = activeItems();

    if (!core::isMeaningfulMove(fromIndex, toIndex, items.size())) {
        return false;
    }

    // 目标格坐标 = 落点磁贴当前占据的位置
    core::Dropspot spot;
    bool foundTarget = false;
    for (const auto& placement : layout_.placements) {
        if (placement.itemIndex == toIndex) {
            spot.column = placement.column;
            spot.row = placement.row;
            foundTarget = true;
            break;
        }
    }
    if (!foundTarget) {
        return false;
    }

    // 被拖项自身的跨度（可能与落点磁贴不同：把 2x2 拖到 1x1 上时，
    // 目标区域按**被拖项**的尺寸算，因为最终是它占在那里）
    for (const auto& placement : layout_.placements) {
        if (placement.itemIndex == fromIndex) {
            spot.spanColumns = placement.spanColumns;
            spot.spanRows = placement.spanRows;
            break;
        }
    }

    return moveItemToSpot(fromIndex, spot);
}

void LauncherWindow::refresh() {
    recomputeLayout();
}

// 按当前网格尺寸重算窗口大小并立即应用。
//
// 与 show() 的区别：**保持窗口当前位置**，只改尺寸 —— 设置窗口里
// 调网格是「边看边调」，窗口不能每输一个数字就跳回鼠标旁边。
// 尺寸没变就不动，避免无意义的 SetWindowPos 抖动。
void LauncherWindow::applyGridGeometry() {
    if (hwnd_ == nullptr) {
        return;
    }
    int width = 0;
    int height = 0;
    windowSizePixels(&width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }

    RECT current{};
    if (GetWindowRect(hwnd_, &current) == FALSE) {
        return;
    }
    if (current.right - current.left == width &&
        current.bottom - current.top == height) {
        return;
    }

    // 保持左上角不动，再夹进工作区（变大后可能顶出屏幕）
    const RECT work = workAreaForWindow(hwnd_);
    int left = current.left;
    int top = current.top;
    if (left + width > work.right) {
        left = work.right - width;
    }
    if (top + height > work.bottom) {
        top = work.bottom - height;
    }
    if (left < work.left) {
        left = work.left;
    }
    if (top < work.top) {
        top = work.top;
    }

    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// 按当前放置策略重新摆放（保持尺寸、可见性、焦点不变）。
void LauncherWindow::applyWindowPlacement() {
    if (hwnd_ == nullptr) {
        return;
    }
    const RECT rect = calculateWindowRect();
    SetWindowPos(hwnd_, HWND_TOPMOST, rect.left, rect.top,
                 rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// ================================================================ 显示控制

void LauncherWindow::show() {
    if (hwnd_ == nullptr) {
        return;
    }

    // 重新计算位置：工作区可能已变化（分辨率切换、任务栏移动）
    const RECT rect = calculateWindowRect();
    SetWindowPos(hwnd_, HWND_TOPMOST,
                 rect.left, rect.top,
                 rect.right - rect.left,
                 rect.bottom - rect.top,
                 SWP_SHOWWINDOW);

    ShowWindow(hwnd_, SW_SHOW);

    // 唤出即把焦点交给搜索框的 Edit 控件：接下来打字直接进查询串
    // （IME 中文、光标、退格全是系统原生行为），这是「普通输入框」的核心。
    // 热键唤出（RegisterHotKey）让本进程获得前台许可，SetForegroundWindow
    // 能成功；即使被系统拒绝（前台锁定），窗口照样显示，用户点一下即可。
    SetForegroundWindow(hwnd_);
    if (searchEdit_ != nullptr) {
        SetFocus(searchEdit_);
    } else {
        SetFocus(hwnd_);
    }

    // 外部点击监视：显示期间才需要。钩子内部有 200ms 静默期，
    // 过滤掉「用鼠标点托盘唤出窗口」那次点击的滞留消息。
    outsideClickMonitor_.start(hwnd_);

    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ================================================================ 搜索模式开关
//
// searchActive_ 只控制界面视觉（搜索框高亮、光标闪烁）。
// 输入能力本身由系统焦点决定 —— 窗口有焦点，IME 就跟着窗口走，
// 不需要这里做任何输入法开关。

void LauncherWindow::activateSearch() {
    if (!searchActive_) {
        searchActive_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void LauncherWindow::deactivateSearch() {
    if (searchActive_) {
        searchActive_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void LauncherWindow::hide() {
    if (hwnd_ == nullptr) {
        return;
    }

    // 先卸外部点击监视：隐藏后不必再监听（省电，也避免隐藏瞬间的误触发）
    outsideClickMonitor_.stop();

    // 外部点击的延迟探测也要停：窗口都收了，再探下去只会重复收起
    KillTimer(hwnd_, kOutsideProbeTimerId);
    outsideProbeActive_ = false;

    // 拖入落点预览随窗口一起消失（下次唤出时若真有拖动，
    // DragEnter 会重新点亮，不会漏）
    KillTimer(hwnd_, kFileDropGuardTimerId);
    fileDropActive_ = false;
    fileDropSpot_.reset();

    ShowWindow(hwnd_, SW_HIDE);
    // 隐藏前确保拖拽状态干净 —— 隐藏后布局可能被外部改变，
    // 残留的拖拽状态在新布局下会产生错位绘制。
    dragActive_ = false;
    dragSource_.reset();
    dropspotPreview_.reset();
    animations_.clear();
    stopAnimationTimer();
    hoveredItem_.reset();
    pressedItem_.reset();
    // 钉子状态：只清「交互中」标志，pinned_ 本身是用户偏好，保留。
    pinHovered_ = false;
    pinPressed_ = false;
    // 搜索状态：清空（含编辑框控件里的文本），下次唤出从干净面板开始。
    // 保留上次的查询会让人困惑（明明是刚打开的窗口，却有磁贴被淡化）。
    clearSearch();
    searchActive_ = false;
    visibleMask_.clear();

    // 隐藏即裁剪工作集：启动器绝大部分时间处于隐藏状态，D2D/DWrite
    // 的位图与字体缓存在后台用不上，让系统把物理页收回去。下次唤出
    // 按需换入（首次绘制多几毫秒软页故障，无感）。任务管理器里看到
    // 的常驻占用会从 ~50MB 掉到个位数 —— 提交内存不变，这是展示层
    // 的账，但对常驻工具来说这才是用户盯着的数字。
    trimWorkingSet();
}

// ================================================================ 钉子

void LauncherWindow::setPinned(const bool pinned) {
    if (pinned_ == pinned) {
        return;
    }
    pinned_ = pinned;

    // 钉住时不应再处于拖拽中途（钉子只会在非拖拽状态被点到），
    // 但保持防御：万一状态不一致，先收干净再切。
    if (pinned_ && dragActive_) {
        dragActive_ = false;
        dragSource_.reset();
        dropspotPreview_.reset();
        animateBackToLayout();
    }

    if (pinnedChangedHandler_) {
        pinnedChangedHandler_(pinned_);
    }
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

bool LauncherWindow::hideIfUnpinned() {
    // 钉住 = 常驻面板，外部点击不收起
    if (pinned_) {
        return false;
    }
    // 模态保护（设置窗口/文件对话框打开中）：同样不收起
    if (modalGuard_) {
        return false;
    }
    if (!isVisible()) {
        return false;
    }
    hide();
    return true;
}

// 模态交互保护：设置窗口打开期间调用 setModalGuard(true)，
// 关闭时 setModalGuard(false)。只影响「点击外面是否收起」，
// 不碰用户的钉子偏好与钉子图标状态。
void LauncherWindow::setModalGuard(const bool active) {
    modalGuard_ = active;
    if (!active) {
        return;
    }
    // 进入保护时收起残留的「外部点击探测」：探测计时器若还在跑，
    // 它会在几百毫秒后判定「点在外部」并尝试隐藏，白白走一趟。
    KillTimer(hwnd_, kOutsideProbeTimerId);
    outsideProbeActive_ = false;
}

// ================================================================ 拖入落点预览

void LauncherWindow::setFileDropTarget(const bool active,
                                       const core::TileSize newItemSize,
                                       const POINTL screenPoint) {
    // ---- 只在「左键真的按着」时才认这一次拖入 ----
    //
    // 真实拖拽必然按着左键；而个别拖拽来源（实测 Directory Opus）在放下
    // 之后会补发一次 DragEnter、却从不发 DragLeave —— 那一次按键已经松开，
    // 据此直接忽略，残影根本没有机会出现。松开后仍在按的情形由看门狗
    // （见 WM_TIMER 的 kFileDropGuardTimerId 分支）每 250ms 兜一次。
    const bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    applyFileDropTarget(active && buttonDown, newItemSize, screenPoint);
}

void LauncherWindow::debugSetFileDropPreview(const int clientX,
                                             const int clientY) {
    if (clientX < 0 || clientY < 0) {
        applyFileDropTarget(false, fileDropSize_, POINTL{});
        return;
    }
    // 客户区 → 屏幕坐标：真实路径拿到的就是屏幕坐标，两条路共用后续逻辑
    POINT point{clientX, clientY};
    if (hwnd_ != nullptr && ClientToScreen(hwnd_, &point) == FALSE) {
        point.x = clientX;
        point.y = clientY;
    }
    POINTL screenPoint{};
    screenPoint.x = point.x;
    screenPoint.y = point.y;
    // 与真实路径的唯一差别：不走「左键按着」的检查（自动化里没有真按键）
    applyFileDropTarget(true, fileDropSize_, screenPoint);
}

void LauncherWindow::applyFileDropTarget(const bool active,
                                         const core::TileSize newItemSize,
                                         const POINTL screenPoint) {
    if (active) {
        SetTimer(hwnd_, kFileDropGuardTimerId, kFileDropGuardPollMs, nullptr);
    } else {
        KillTimer(hwnd_, kFileDropGuardTimerId);
        if (!fileDropActive_ && !fileDropSpot_.has_value()) {
            return;
        }
        fileDropActive_ = false;
        fileDropSpot_.reset();
        // 让位预演撤掉：被挤开的磁贴回到真实布局位置
        animateBackToLayout();
        if (hwnd_ != nullptr) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    // ---- 落点 = 光标所在的那一格 ----
    POINT client{screenPoint.x, screenPoint.y};
    if (hwnd_ != nullptr && ScreenToClient(hwnd_, &client) == FALSE) {
        client.x = screenPoint.x;
        client.y = screenPoint.y;
    }
    const core::Dropspot spot =
        resolveNewItemDropspot(newItemSize, client.x, client.y);

    const bool changed =
        !fileDropActive_ || !fileDropSpot_.has_value() ||
        fileDropSpot_->column != spot.column ||
        fileDropSpot_->row != spot.row ||
        fileDropSpot_->spanColumns != spot.spanColumns ||
        fileDropSpot_->spanRows != spot.spanRows ||
        fileDropSize_ != newItemSize;

    fileDropActive_ = true;
    fileDropSize_ = newItemSize;
    fileDropSpot_ = spot;

    if (changed) {
        // 落点格变了 → 重新算「谁要让位」并播让位动画（只动视觉）。
        // 悬停在同一格上时什么都不做，避免每次 DragOver 都重算一遍。
        previewDropForNewItem(newItemSize, spot);
        if (hwnd_ != nullptr) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
}

void LauncherWindow::toggle() {
    if (isVisible()) {
        hide();
    } else {
        show();
    }
}

bool LauncherWindow::isVisible() const noexcept {
    if (hwnd_ == nullptr) {
        return false;
    }
    return IsWindowVisible(hwnd_) != FALSE;
}

// ================================================================ 调试辅助

void LauncherWindow::debugSetHover(const int clientX, const int clientY) {
    if (clientX < 0 || clientY < 0) {
        hoveredItem_.reset();
    } else {
        hoveredItem_ = hitTestItem(static_cast<float>(clientX),
                                   static_cast<float>(clientY));
    }
    // 立刻重绘一次，让导出拿到最新的悬停态
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }
}

void LauncherWindow::debugSetDrag(const int sourceX, const int sourceY,
                                  const int targetX, const int targetY) {
    if (sourceX < 0 || sourceY < 0 || targetX < 0 || targetY < 0) {
        dragActive_ = false;
        dragSource_.reset();
        dropspotPreview_.reset();
        return;
    }

    const float scale = dpiScale();
    dragSource_ = hitTestItem(static_cast<float>(sourceX) / scale,
                              static_cast<float>(sourceY) / scale);
    dragActive_ = dragSource_.has_value();

    if (dragActive_) {
        // 模拟跟手：光标停在 target 位置，抓取偏移取磁贴中心
        dragCurrentPoint_.x = targetX;
        dragCurrentPoint_.y = targetY;
        dragStartPoint_ = dragCurrentPoint_;

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
                dragTileWidth_ = metrics_.tileWidth(placement.spanColumns);
                dragTileHeight_ = metrics_.tileHeight(placement.spanRows);
                break;
            }
        }
        // 抓取点放在磁贴中心，视觉上最自然
        dragGrabOffsetX_ = dragTileWidth_ / 2.0F;
        dragGrabOffsetY_ = dragTileHeight_ / 2.0F;

        // 计算落点虚线框与挤开预演，并把动画推进到终点 ——
        // 导出的是定格图，不需要中间帧。
        updateDropspotPreview();
        settleAnimations();
        stopAnimationTimer();
    }

    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }
}

// 调试辅助：把当前磁贴布局离屏渲染为 PNG。
//
// 为什么不截窗口：
//   - BitBlt 窗口 DC 对 D2D 无效：HwndRenderTarget 走独立 DXGI 后台缓冲，
//     内容不经 GDI 路径，窗口 DC 上是空的。
//   - PrintWindow 依赖窗口实现 WM_PRINT，D2D 的 HwndRenderTarget 不实现；
//     PW_RENDERFULLCONTENT 是 Win8.1+ API，与项目 Win7 目标冲突。
//
// 做法：新建一个以 WIC 位图为表面的 D2D 渲染目标，把同一套 paintTiles()
// 跑一遍。产出的是渲染器的真实输出，且完全不要求窗口可见 —— 这让视觉
// 回归可以完全自动化。
bool LauncherWindow::debugExportPng(const wchar_t* fileName) {
    if (d2dFactory_.get() == nullptr || dwriteFactory_.get() == nullptr) {
        return false;
    }

    // 尺寸：优先取窗口渲染目标；尚未创建时用客户区兜底
    D2D1_SIZE_F windowSize = D2D1::SizeF(0.0F, 0.0F);
    if (renderTarget_ != nullptr) {
        windowSize = renderTarget_->GetSize();
    }
    if (windowSize.width <= 0.0F || windowSize.height <= 0.0F) {
        RECT client{};
        if (hwnd_ != nullptr && GetClientRect(hwnd_, &client) != FALSE) {
            windowSize = D2D1::SizeF(
                static_cast<float>(client.right - client.left),
                static_cast<float>(client.bottom - client.top));
        }
    }
    const int width = static_cast<int>(windowSize.width);
    const int height = static_cast<int>(windowSize.height);
    if (width <= 0 || height <= 0) {
        return false;
    }

    platform::win32::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) {
        return false;
    }

    platform::win32::ComPtr<IWICBitmap> wicBitmap;
    hr = factory->CreateBitmap(
        static_cast<UINT>(width), static_cast<UINT>(height),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapCacheOnLoad, wicBitmap.put());
    if (FAILED(hr)) {
        return false;
    }

    const D2D1_RENDER_TARGET_PROPERTIES renderProps =
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0F, 96.0F);

    platform::win32::ComPtr<ID2D1RenderTarget> offscreenTarget;
    hr = d2dFactory_->CreateWicBitmapRenderTarget(
        wicBitmap.get(), renderProps, offscreenTarget.put());
    if (FAILED(hr)) {
        return false;
    }

    // ---- 切换绘制上下文到离屏目标 ----
    //
    // paintTiles() 只依赖 renderTarget_ / 三个画刷 / 悬停状态。把它们
    // 临时指向离屏的一套，绘制完再还原 —— 比在每个绘制点加条件分支干净。
    ID2D1RenderTarget* const savedTarget = renderTarget_;
    const auto savedBrush = brush_;
    const auto savedTextBrush = textBrush_;
    const auto savedBorderBrush = borderBrush_;
    const auto savedIcons = std::move(iconCache_);

    // 离屏渲染目标固定 96 DPI（1 DIP = 1 px），而窗口 DPI 是系统真实值。
    // clientHeightDip() 会拿 GetClientRect（此处是未缩放的物理像素）除以
    // dpiScale —— 不把 dpi_ 一并切到 96 的话，高 DPI 机器上底部标签条
    // 会按 1/scale 上移，截出一张「标签条悬在半空」的失真图。
    const int savedDpi = dpi_;
    dpi_ = 96;

    renderTarget_ = offscreenTarget.get();
    brush_ = platform::win32::ComPtr<ID2D1SolidColorBrush>();
    textBrush_ = platform::win32::ComPtr<ID2D1SolidColorBrush>();
    borderBrush_ = platform::win32::ComPtr<ID2D1SolidColorBrush>();
    iconCache_.clear();

    // 清除按下态：导出的是稳态视觉，不应带按压反馈
    const auto savedPressed = pressedItem_;
    pressedItem_.reset();

    bool created = false;
    if (renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White), brush_.put()) == S_OK &&
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White), textBrush_.put()) == S_OK &&
        renderTarget_->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White), borderBrush_.put()) == S_OK) {
        created = true;
    }

    if (created) {
        renderTarget_->BeginDraw();
        renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
        // 与 render() 的窗口底色保持一致 —— 用当前皮肤的窗口背景，
        // 截图才能反映真实观感（此前硬编码深灰，亮色皮肤全被拍糊）。
        const std::uint32_t bg = theme_.windowBackground;
        D2D1_COLOR_F backgroundColor;
        backgroundColor.a = static_cast<float>((bg >> 24) & 0xFFU) / 255.0F;
        backgroundColor.r = static_cast<float>((bg >> 16) & 0xFFU) / 255.0F;
        backgroundColor.g = static_cast<float>((bg >> 8) & 0xFFU) / 255.0F;
        backgroundColor.b = static_cast<float>(bg & 0xFFU) / 255.0F;
        renderTarget_->Clear(backgroundColor);

        paintTiles();

        renderTarget_->EndDraw();
    }

    // ---- 还原绘制上下文 ----
    renderTarget_ = savedTarget;
    brush_ = savedBrush;
    textBrush_ = savedTextBrush;
    borderBrush_ = savedBorderBrush;
    iconCache_ = std::move(savedIcons);
    pressedItem_ = savedPressed;
    dpi_ = savedDpi;

    if (!created) {
        return false;
    }

    // ---- 编码为 PNG ----
    //
    // 输出到 exe 同级的 debug 子目录：相对路径会落到进程当前工作目录，
    // 而工作目录在自动化环境下可能被清理，导致产物"写完就没了"。
    // 这里显式拼接 exe 所在目录，保证产物位置可预期。
    std::wstring outputPath;
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0) {
            std::wstring exeDir(exePath);
            const std::size_t slash = exeDir.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                exeDir.resize(slash + 1);
            }
            outputPath = exeDir + fileName;
        } else {
            outputPath = fileName;
        }
    }

    platform::win32::ComPtr<IWICStream> stream;
    platform::win32::ComPtr<IWICBitmapEncoder> encoder;
    platform::win32::ComPtr<IWICBitmapFrameEncode> frame;
    IPropertyBag2* properties = nullptr;

    bool ok = false;
    do {
        if (FAILED(factory->CreateStream(stream.put()))) break;
        if (FAILED(stream->InitializeFromFilename(outputPath.c_str(),
                                                   GENERIC_WRITE)))
            break;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                         encoder.put())))
            break;
        if (FAILED(encoder->Initialize(stream.get(),
                                       WICBitmapEncoderNoCache)))
            break;
        if (FAILED(encoder->CreateNewFrame(frame.put(), &properties))) break;
        if (FAILED(frame->Initialize(properties))) break;
        if (FAILED(frame->SetSize(static_cast<UINT>(width),
                                  static_cast<UINT>(height))))
            break;

        WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
        if (FAILED(frame->SetPixelFormat(&format))) break;

        // WriteSource 直接从 WIC 位图取像素，无需手工搬运
        if (FAILED(frame->WriteSource(wicBitmap.get(), nullptr))) break;
        if (FAILED(frame->Commit())) break;
        if (FAILED(encoder->Commit())) break;
        ok = true;
    } while (false);

    if (properties != nullptr) {
        properties->Release();
    }
    return ok;
}

}  // namespace ui
}  // namespace ktile
