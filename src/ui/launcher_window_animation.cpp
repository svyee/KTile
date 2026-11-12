// KTile - UI 层：拖拽动画与落点解析（LauncherWindow 实现之一）
//
// 动画用「偏移量插值」：layout_ 只在真正提交重排时才变，每块磁贴维护
// 视觉偏移 = 渲染位置 − 布局位置，动画只动偏移、结束归零，避免每帧
// 重跑装箱算法（开销随磁贴数增长，且重排会让位置瞬变没有过渡）。
// 拖拽期间其余磁贴原地不动、只画落点虚线框，对齐 Win10 行为；
// 真正的排布推迟到松手提交。预先挤开整个面板的旧做法观感失控，已否决。
//
#include "ui/launcher_window.h"

#include "base/text_convert.h"

#include <algorithm>
#include <cmath>

namespace ktile {
namespace ui {
namespace {

// 挤开动画时长：Win10 磁贴约 200~250ms，取 200ms 平衡手感与响应。
constexpr std::uint32_t kReflowDurationMs = 200;

// 回位动画时长（毫秒）。
constexpr std::uint32_t kReturnDurationMs = 220;

// 动画帧间隔约 60fps。Win7 的 SetTimer 精度约 15.6ms，实际帧率略低，视觉足够。
constexpr UINT kAnimationFrameMs = 16;

}  // namespace

// --- 偏移量读写

void LauncherWindow::currentOffset(const std::size_t itemIndex,
                                   float& offsetX,
                                   float& offsetY) const {
    const auto found = animations_.find(itemIndex);
    if (found == animations_.end()) {
        offsetX = 0.0F;
        offsetY = 0.0F;
        return;
    }
    offsetX = found->second.offsetX;
    offsetY = found->second.offsetY;
}

// --- 动画推进

bool LauncherWindow::advanceAnimations() {
    if (animations_.empty()) {
        return false;
    }

    const std::uint64_t now = GetTickCount64();
    bool stillAnimating = false;

    for (auto& entry : animations_) {
        TileAnimation& animation = entry.second;
        if (animation.settled()) {
            continue;
        }

        const std::uint64_t elapsed = (now >= animation.startTick)
            ? (now - animation.startTick)
            : 0;
        const float rawT = (animation.durationMs == 0)
            ? 1.0F
            : static_cast<float>(elapsed) /
                  static_cast<float>(animation.durationMs);
        const float eased = easeOutCubic(rawT);

        animation.offsetX =
            animation.fromX + (animation.toX - animation.fromX) * eased;
        animation.offsetY =
            animation.fromY + (animation.toY - animation.fromY) * eased;

        if (rawT >= 1.0F) {
            // 收敛到精确终点，避免浮点残差留下亚像素错位
            animation.snapToEnd();
        } else {
            stillAnimating = true;
        }
    }

    return stillAnimating;
}

void LauncherWindow::settleAnimations() {
    for (auto& entry : animations_) {
        entry.second.snapToEnd();
    }
}

// --- 定时器

void LauncherWindow::ensureAnimationTimer() {
    if (hwnd_ == nullptr) {
        return;
    }
    // 已经开着就不要再 SetTimer：重复设置会重置计时，导致掉帧
    if (animationTimerActive_) {
        return;
    }
    SetTimer(hwnd_, kAnimationTimerId, kAnimationFrameMs, nullptr);
    animationTimerActive_ = true;
}

void LauncherWindow::stopAnimationTimer() {
    if (hwnd_ == nullptr || !animationTimerActive_) {
        return;
    }
    KillTimer(hwnd_, kAnimationTimerId);
    animationTimerActive_ = false;
}

// --- 启动动画

void LauncherWindow::startOffsetAnimation(const std::size_t itemIndex,
                                          const float toOffsetX,
                                          const float toOffsetY,
                                          const std::uint32_t durationMs) {
    auto& animation = animations_[itemIndex];

    // 起点取当前偏移，动画可中途打断并平滑改向：
    // 用户快速划过多个落点时不会跳变。
    animation.fromX = animation.offsetX;
    animation.fromY = animation.offsetY;
    animation.toX = toOffsetX;
    animation.toY = toOffsetY;
    animation.startTick = GetTickCount64();
    animation.durationMs = durationMs;

    if (durationMs == 0 ||
        (animation.fromX == toOffsetX && animation.fromY == toOffsetY)) {
        animation.snapToEnd();
        return;
    }
    ensureAnimationTimer();
}

// --- 落点解析

core::Dropspot LauncherWindow::resolveDropspot(const std::size_t dragIndex,
                                               const int clientX,
                                               const int clientY) const {
    core::Dropspot spot;
    if (document_.tabs.empty()) {
        return spot;
    }

    const auto& items = activeItems();
    if (dragIndex >= items.size()) {
        return spot;
    }

    const float scale = dpiScale();
    const float xDip = static_cast<float>(clientX) / scale;
    const float yDip = static_cast<float>(clientY) / scale;
    // 被拖项的跨度：落点区域按被拖项尺寸算（最终是它占在那里）
    std::uint16_t spanColumns = tileSpanColumns(items[dragIndex].tileSize);
    std::uint16_t spanRows = tileSpanRows(items[dragIndex].tileSize);
    std::uint16_t originColumn = 0;
    std::uint16_t originRow = 0;
    bool hasOrigin = false;

    for (const auto& placement : layout_.placements) {
        if (placement.itemIndex != dragIndex) {
            continue;
        }
        spanColumns = placement.spanColumns;
        spanRows = placement.spanRows;
        originColumn = placement.column;
        originRow = placement.row;
        hasOrigin = true;
        break;
    }

    const float step = metrics_.cellSize + metrics_.cellGap;
    const float gridLeft = metrics_.outerPadding;
    const float gridTop = gridOriginY();

    // ---- 命中了哪块磁贴 ----
    // 命中被拖磁贴自己时不吸附回原点：大磁贴小幅拖动时光标仍落在自身
    // 覆盖的另一格上，吸附到自身左上角则落点恒等于原点，表现为拖不动，
    // 应走下面的左上角吸附。
    int column = 0;
    int row = 0;
    bool overTile = false;
    if (const auto hit = hitTestItem(xDip, yDip); hit.has_value() &&
        hit != dragIndex) {
        for (const auto& placement : layout_.placements) {
            if (placement.itemIndex != *hit) {
                continue;
            }
            column = placement.column;
            row = placement.row;
            overTile = true;
            break;
        }
    }

    if (!overTile) {
        // ---- 未命中磁贴：按「磁贴左上角」吸附到最近的格 ----
        // 用「光标 − 抓取偏移」而不是光标本身：抓着磁贴中心拖时按光标
        // 吸附，落点会比视觉位置偏右下约半格。round 让磁贴扫过两格
        // 之间时贴合更近的一侧。
        const float tileLeft = xDip - dragGrabOffsetX_;
        const float tileTop = yDip - dragGrabOffsetY_;
        const float rawColumn = (tileLeft - gridLeft) / step;
        const float rawRow = (tileTop - gridTop) / step;
        column = static_cast<int>(std::lround(rawColumn));
        row = static_cast<int>(std::lround(rawRow));
        if (column < 0) {
            column = 0;
        }
        if (row < 0) {
            row = 0;
        }
    }

    // ---- 按被拖项跨度夹进网格 ----
    // 允许落在最后一个可见格之外的空白带：夹取后贴到最右下能放下的
    // 位置，正是用户拖到右边空白时的期望。
    const int maxColumn = static_cast<int>(gridColumns_) - static_cast<int>(spanColumns);
    const int maxRow = static_cast<int>(gridRows_) - static_cast<int>(spanRows);
    if (maxColumn >= 0 && column > maxColumn) {
        column = maxColumn;
    }
    if (maxRow >= 0 && row > maxRow) {
        row = maxRow;
    }
    if (column < 0) {
        column = 0;
    }
    if (row < 0) {
        row = 0;
    }

    spot.column = static_cast<std::uint16_t>(column);
    spot.row = static_cast<std::uint16_t>(row);
    spot.spanColumns = spanColumns;
    spot.spanRows = spanRows;
    spot.sameAsOrigin = hasOrigin &&
                        spot.column == originColumn && spot.row == originRow;

    // 落点是否压住了别的磁贴（用于视觉上区分「替换」与「填空」）
    for (const auto& placement : layout_.placements) {
        if (placement.itemIndex == dragIndex) {
            continue;
        }
        const bool overlap =
            spot.column < placement.column + placement.spanColumns &&
            placement.column < spot.column + spot.spanColumns &&
            spot.row < placement.row + placement.spanRows &&
            placement.row < spot.row + spot.spanRows;
        if (overlap) {
            spot.overOccupiedCell = true;
            break;
        }
    }

    return spot;
}

// --- 新项目的落点
//
// 新项目的落点：拖文件没有抓取偏移，用户心里的映射是「光标在哪个格子，
// 新图标就长在哪个格子」，直接取光标所在格（向下取整），不四舍五入。
// 预览框与最终落点共用本函数，保证所见即所得。
core::Dropspot LauncherWindow::resolveNewItemDropspot(
    const core::TileSize newItemSize, const int clientX,
    const int clientY) const {
    core::Dropspot spot;
    spot.spanColumns = core::tileSpanColumns(newItemSize);
    spot.spanRows = core::tileSpanRows(newItemSize);

    const float scale = dpiScale();
    const float xDip = static_cast<float>(clientX) / scale;
    const float yDip = static_cast<float>(clientY) / scale;

    const float step = metrics_.cellSize + metrics_.cellGap;
    const float gridLeft = metrics_.outerPadding;
    const float gridTop = gridOriginY();

    int column = static_cast<int>(std::floor((xDip - gridLeft) / step));
    int row = static_cast<int>(std::floor((yDip - gridTop) / step));

    // 夹进网格：拖到边缘外时贴到最右下放得下的位置
    //（标题带那行、最右侧空白带都是合法落点，不必像素级瞄准）
    const int maxColumn =
        static_cast<int>(gridColumns_) - static_cast<int>(spot.spanColumns);
    const int maxRow =
        static_cast<int>(gridRows_) - static_cast<int>(spot.spanRows);
    if (maxColumn >= 0 && column > maxColumn) {
        column = maxColumn;
    }
    if (maxRow >= 0 && row > maxRow) {
        row = maxRow;
    }
    if (column < 0) {
        column = 0;
    }
    if (row < 0) {
        row = 0;
    }

    spot.column = static_cast<std::uint16_t>(column);
    spot.row = static_cast<std::uint16_t>(row);

    // 落点是否压住已有磁贴（决定要不要播让位预演）
    for (const auto& placement : layout_.placements) {
        const bool overlap =
            spot.column < placement.column + placement.spanColumns &&
            placement.column < spot.column + spot.spanColumns &&
            spot.row < placement.row + placement.spanRows &&
            placement.row < spot.row + spot.spanRows;
        if (overlap) {
            spot.overOccupiedCell = true;
            break;
        }
    }
    return spot;
}

void LauncherWindow::previewDropForNewItem(const core::TileSize newItemSize,
                                           const core::Dropspot& spot) {
    if (document_.tabs.empty()) {
        return;
    }

    // 用一份临时副本加一个「还不存在的项」，直接复用拖磁贴的避让算法，
    // 挤开的算法只有一份。
    std::vector<core::LaunchItem> probe = activeItems();
    materializeGridPositions(probe);

    core::LaunchItem placeholder;
    placeholder.tileSize = newItemSize;
    probe.push_back(std::move(placeholder));
    const std::size_t probeIndex = probe.size() - 1;

    core::AvoidanceRequest request;
    request.columns = gridColumns_;
    request.rows = gridRows_;
    request.items = &probe;
    request.dragIndex = probeIndex;
    request.targetColumn = spot.column;
    request.targetRow = spot.row;
    request.dragSpanColumns = spot.spanColumns;
    request.dragSpanRows = spot.spanRows;

    const core::TileLayoutResult preview =
        core::calculateAvoidanceLayout(request);

    std::map<std::size_t, std::pair<std::uint16_t, std::uint16_t>> targets;
    for (const auto& placement : preview.placements) {
        if (placement.itemIndex == probeIndex) {
            continue;
        }
        targets[placement.itemIndex] =
            std::make_pair(placement.column, placement.row);
    }
    applyDisplacementPreview(targets, probeIndex);
}

void LauncherWindow::updateDropspotPreview() {
    if (!dragActive_ || !dragSource_.has_value()) {
        dropspotPreview_.reset();
        return;
    }

    const core::Dropspot spot = resolveDropspot(
        *dragSource_, dragCurrentPoint_.x, dragCurrentPoint_.y);

    const bool changed = !dropspotPreview_.has_value() ||
        dropspotPreview_->column != spot.column ||
        dropspotPreview_->row != spot.row;
    dropspotPreview_ = spot;

    if (changed) {
        // 落点压到已有磁贴播挤开动画，落点是空白则全部回位保持安静，
        // 与 Win10 行为一致。
        previewDisplacement(*dragSource_, spot);
        if (hwnd_ != nullptr) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
}

// --- 挤开预演

void LauncherWindow::applyDisplacementPreview(
    const std::map<std::size_t, std::pair<std::uint16_t, std::uint16_t>>&
        targets,
    const std::size_t skipIndex) {
    const float step = metrics_.cellSize + metrics_.cellGap;

    // 先让「本轮不再需要让位」的磁贴回位，防止鼠标扫过多个落点后飘走
    for (const auto& entry : animations_) {
        if (entry.first == skipIndex || targets.count(entry.first) > 0) {
            continue;
        }
        if (entry.second.offsetX != 0.0F || entry.second.offsetY != 0.0F) {
            startOffsetAnimation(entry.first, 0.0F, 0.0F, kReflowDurationMs);
        }
    }

    // 再给需要让位的磁贴设置目标偏移
    for (const auto& placement : layout_.placements) {
        const auto found = targets.find(placement.itemIndex);
        if (found == targets.end()) {
            continue;
        }
        const float deltaX = static_cast<float>(found->second.first) -
                             static_cast<float>(placement.column);
        const float deltaY = static_cast<float>(found->second.second) -
                             static_cast<float>(placement.row);
        startOffsetAnimation(placement.itemIndex, deltaX * step, deltaY * step,
                             kReflowDurationMs);
    }
}

void LauncherWindow::previewDisplacement(const std::size_t dragIndex,
                                         const core::Dropspot& spot) {
    if (document_.tabs.empty()) {
        return;
    }
    const auto& items = activeItems();
    if (dragIndex >= items.size()) {
        return;
    }

    // 落点未压任何磁贴 → 无需挤开，所有已偏移的磁贴回位
    if (!spot.overOccupiedCell) {
        const std::map<std::size_t, std::pair<std::uint16_t, std::uint16_t>>
            none;
        applyDisplacementPreview(none, dragIndex);
        return;
    }

    // 落点压到磁贴：算避让布局并把「谁该让到哪」翻译成偏移动画，
    // 只动画不改 layout_，真正提交在松手时。与提交路径一样先固化
    // 显式坐标，否则没坐标的磁贴会被当成新来的挤到落点附近，
    // 预演看起来像整个面板都在动。
    std::vector<core::LaunchItem> probe = items;
    materializeGridPositions(probe);

    core::AvoidanceRequest request;
    request.columns = gridColumns_;
    request.rows = gridRows_;
    request.items = &probe;
    request.dragIndex = dragIndex;
    request.targetColumn = spot.column;
    request.targetRow = spot.row;
    request.dragSpanColumns = spot.spanColumns;
    request.dragSpanRows = spot.spanRows;

    const core::TileLayoutResult preview =
        core::calculateAvoidanceLayout(request);

    // 收集本轮需要让位的磁贴及其目标坐标
    std::map<std::size_t, std::pair<std::uint16_t, std::uint16_t>> targets;
    for (const auto& placement : preview.placements) {
        if (placement.itemIndex == dragIndex) {
            continue;
        }
        targets[placement.itemIndex] =
            std::make_pair(placement.column, placement.row);
    }

    applyDisplacementPreview(targets, dragIndex);
}

// --- 回位

void LauncherWindow::animateBackToLayout() {
    // 回位 = 所有在动或已偏移的磁贴偏移归零
    bool anyMoved = false;
    for (auto& entry : animations_) {
        TileAnimation& animation = entry.second;
        if (animation.offsetX != 0.0F || animation.offsetY != 0.0F) {
            anyMoved = true;
        }
        startOffsetAnimation(entry.first, 0.0F, 0.0F, kReturnDurationMs);
    }
    if (!anyMoved) {
        settleAnimations();
        stopAnimationTimer();
    }
}

}  // namespace ui
}  // namespace ktile
