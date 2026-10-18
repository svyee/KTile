// KTile - UI 层：拖拽动画与落点解析
//
// 本文件与 launcher_window.cpp / launcher_window_behavior.cpp 共同构成
// LauncherWindow 实现。拆分目的：拖拽相关的状态机独立成文件，便于维护。
//
// ================================================================
// 设计要点一：为什么用「偏移量插值」而不是「重算布局」
// ================================================================
//
// 部分场景（回位、后续扩展的落位动画）需要磁贴平滑移动，直觉做法是
// 每次变化就重排数组再整体重绘。但那样有两个问题：
//   1. 磁贴位置是瞬变的，没有过渡，看起来是「跳」而不是「挪」
//   2. 每帧重跑二维装箱算法，开销与磁贴数相关，且浮点结果可能微抖
//
// 这里改为：布局结果 layout_ 保持稳定（只在真正提交重排时才变），
// 每块磁贴额外维护一个「视觉偏移」= 渲染位置 − 布局位置。
//   - 位移动画 = 把偏移从当前值动到目标像素差
//   - 「回位」动画 = 把偏移从当前值动回 0
//   - 动画结束偏移归零，绘制结果与静态完全一致
//
// ================================================================
// 设计要点二：为什么拖拽期间「不动任何磁贴」（对齐 Win10 行为）
// ================================================================
//
// 早先的实现是：光标落到哪块磁贴，就把那块磁贴以及所有受牵连的磁贴
// 预先「挤开」到最终位置。实际体验很糟 —— 用户只拖了一块 1x1，
// 整个面板都在动，而且挪走的磁贴又看不出会停在哪，像是界面失控。
//
// 对照 Win10 开始菜单的实际行为，它的规则简单得多：
//   - 拖拽中：被拖的磁贴跟着光标走，其余磁贴**原地不动**
//   - 同时用一个虚线轮廓/色块标出「松手后会放在这里」
//   - 松手：一次性重排，被压住的磁贴顺次腾挪让位
//   - Esc 或拖回原位：取消，什么都不变
//
// 因此现在拖拽期间只做一件事：算落点、画虚线框。
// 真正的排布变化全部推迟到 WM_LBUTTONUP 提交时发生。
//
#include "ui/launcher_window.h"

#include "base/text_convert.h"

#include <algorithm>
#include <cmath>

namespace ktile {
namespace ui {
namespace {

// 挤开动画时长（毫秒）。
// Win10 磁贴约 200~250ms；取 200ms 在手感与响应速度间平衡。
constexpr std::uint32_t kReflowDurationMs = 200;

// 回位动画时长（毫秒）。
constexpr std::uint32_t kReturnDurationMs = 220;

// 动画帧间隔（毫秒）。约 60fps。
// 注意 Win7 的 SetTimer 精度约 15.6ms，实际帧率略低于 60，视觉上足够。
constexpr UINT kAnimationFrameMs = 16;

}  // namespace

// ================================================================ 偏移量读写

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

// ================================================================ 动画推进

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

// ================================================================ 定时器

void LauncherWindow::ensureAnimationTimer() {
    if (hwnd_ == nullptr) {
        return;
    }
    // 已经开着就不要再 SetTimer —— 重复设置会重置计时，导致掉帧
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

// ================================================================ 启动动画

void LauncherWindow::startOffsetAnimation(const std::size_t itemIndex,
                                          const float toOffsetX,
                                          const float toOffsetY,
                                          const std::uint32_t durationMs) {
    auto& animation = animations_[itemIndex];

    // 起点取「当前偏移」，这样动画可以中途被打断并平滑改向 ——
    // 用户快速划过多个落点时不会出现跳变。
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

// ================================================================ 落点解析

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
    // 被拖项的跨度：落点区域按**被拖项**尺寸算（最终是它占在那里）
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

    // ---- 步骤 1：命中了哪块磁贴 ----
    //
    // 注意：命中**被拖磁贴自己**时不能直接吸附回原点 ——
    // 大磁贴（2x2 等）拖动小距离时，光标仍落在自身覆盖的另一格上，
    // 若吸附到自身左上角，落点永远等于原点，表现为「拖不动」。
    // 此时按用户意图应走步骤 2 的「磁贴左上角吸附」。
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
        // ---- 步骤 2：没命中磁贴 —— 按「磁贴左上角」吸附到最近的格 ----
        //
        // 关键：用「光标 − 抓取偏移」（即磁贴当前视觉位置的左上角），
        // 而不是光标本身。抓着磁贴中心拖时，光标在磁贴中央，
        // 若按光标吸附，落点会比磁贴视觉位置偏右下约半格 ——
        // 这正是「虚线框总是跑到右下角」的根源。
        //
        // 用 round（四舍五入）：磁贴扫过两格之间时贴合到更近的一侧。
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

    // ---- 步骤 3：按被拖项跨度夹进网格范围 ----
    //
    // 允许落点落在网格「最后一个可见格」之外的空白带上 ——
    // 夹取后会贴到最右下能放下的位置，这正是用户拖到右边空白时的期望。
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

// ---------------------------------------------------------------- 新项目的落点
//
// 「拖文件进来」和「拖磁贴」是两种手势，落点算法也不同：
//   - 拖磁贴：抓着它的某一处，所以落点要减去「抓取偏移」，还要做四舍五入
//     吸附（详见 resolveDropspot）
//   - 拖文件：没有抓取偏移，用户心里的映射就是「光标在哪个格子，
//     新图标就长在哪个格子」—— 因此直接取光标所在的格（向下取整），
//     不四舍五入、不偏移。预览框与最终落点共用这一个函数，保证所见即所得。
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

    // 夹进网格：往右/下边缘外拖时贴到最右下放得下的位置
    //（标题带那一行、最右侧的空白带都算合法落点，不必像素级瞄准）
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

    // 落点是否压住了已有磁贴（决定要不要播让位预演）
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

    // 用一份临时副本算避让：里面有补上一个「还不存在的项」，
    // 于是可以直接复用拖动磁贴那一套规则 —— 挤开的算法只有一份。
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
        // 落点压到已有磁贴 → 给受影响磁贴播「挤开」动画；
        // 落点是空白 → 所有磁贴回到原位（保持安静）。
        // 这与 Win10 一致：悬停在已有图标上会挤开，悬停在空白上不动。
        previewDisplacement(*dragSource_, spot);
        if (hwnd_ != nullptr) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
}

// ================================================================ 挤开预演

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

    // 落点压到磁贴 → 计算避让布局，把「谁该让到哪」翻译成偏移动画。
    // 只动画，不改 layout_；真正的提交仍在松手时做。
    //
    // 与提交路径（moveItemToSpot）一样，先把「屏幕上就是这么摆的」固化
    // 成显式坐标再算避让 —— 否则没有坐标的磁贴会被当成新来的，
    // 一股脑儿挤到落点附近，预演看起来就像整个面板都在动。
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

// ================================================================ 回位

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
