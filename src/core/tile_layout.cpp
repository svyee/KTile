// KTile - 磁贴跨度布局引擎实现
#include "core/tile_layout.h"

#include <algorithm>
#include <utility>

namespace ktile {
namespace core {

// ================================================================ OccupancyGrid

OccupancyGrid::OccupancyGrid(const std::uint16_t columns, const std::uint16_t rows)
    : columns_(columns > 0 ? columns : 1),
      rows_(rows > 0 ? rows : 1),
      cells_(static_cast<std::size_t>(columns_) * static_cast<std::size_t>(rows_), false) {}

bool OccupancyGrid::isOccupied(const std::uint16_t column, const std::uint16_t row) const noexcept {
    // 越界一律视为已占用，避免调用方误判为可放置
    if (column >= columns_ || row >= rows_) {
        return true;
    }
    const std::size_t index =
        static_cast<std::size_t>(row) * static_cast<std::size_t>(columns_) + column;
    return cells_[index];
}

bool OccupancyGrid::isRegionInside(const std::uint16_t column,
                                   const std::uint16_t row,
                                   const std::uint16_t spanColumns,
                                   const std::uint16_t spanRows) const noexcept {
    if (spanColumns == 0 || spanRows == 0) {
        return false;
    }
    // 用乘法前先转 size_t，避免 uint16 溢出
    const std::size_t right = static_cast<std::size_t>(column) + spanColumns;
    const std::size_t bottom = static_cast<std::size_t>(row) + spanRows;
    return right <= static_cast<std::size_t>(columns_) &&
           bottom <= static_cast<std::size_t>(rows_);
}

bool OccupancyGrid::isRegionFree(const std::uint16_t column,
                                 const std::uint16_t row,
                                 const std::uint16_t spanColumns,
                                 const std::uint16_t spanRows) const noexcept {
    if (!isRegionInside(column, row, spanColumns, spanRows)) {
        return false;
    }
    for (std::uint16_t dy = 0; dy < spanRows; ++dy) {
        for (std::uint16_t dx = 0; dx < spanColumns; ++dx) {
            if (isOccupied(static_cast<std::uint16_t>(column + dx),
                           static_cast<std::uint16_t>(row + dy))) {
                return false;
            }
        }
    }
    return true;
}

void OccupancyGrid::markRegion(const std::uint16_t column,
                               const std::uint16_t row,
                               const std::uint16_t spanColumns,
                               const std::uint16_t spanRows) noexcept {
    for (std::uint16_t dy = 0; dy < spanRows; ++dy) {
        for (std::uint16_t dx = 0; dx < spanColumns; ++dx) {
            const std::uint16_t cx = static_cast<std::uint16_t>(column + dx);
            const std::uint16_t cy = static_cast<std::uint16_t>(row + dy);
            if (cx >= columns_ || cy >= rows_) {
                continue;
            }
            const std::size_t index =
                static_cast<std::size_t>(cy) * static_cast<std::size_t>(columns_) + cx;
            cells_[index] = true;
        }
    }
}

void OccupancyGrid::clear() noexcept {
    std::fill(cells_.begin(), cells_.end(), false);
}

// ================================================================ 自由函数

bool canPlaceTile(const OccupancyGrid& grid,
                  const std::uint16_t column,
                  const std::uint16_t row,
                  const std::uint16_t spanColumns,
                  const std::uint16_t spanRows) noexcept {
    return grid.isRegionFree(column, row, spanColumns, spanRows);
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> findFirstFreeRegion(
    const OccupancyGrid& grid,
    const std::uint16_t spanColumns,
    const std::uint16_t spanRows) noexcept {
    if (spanColumns == 0 || spanRows == 0 ||
        spanColumns > grid.columns() || spanRows > grid.rows()) {
        return std::nullopt;
    }
    // 行优先扫描：从上到下、从左到右，取第一个放得下的位置
    const std::uint16_t maxColumn =
        static_cast<std::uint16_t>(grid.columns() - spanColumns);
    const std::uint16_t maxRow = static_cast<std::uint16_t>(grid.rows() - spanRows);
    for (std::uint16_t row = 0; row <= maxRow; ++row) {
        for (std::uint16_t column = 0; column <= maxColumn; ++column) {
            if (grid.isRegionFree(column, row, spanColumns, spanRows)) {
                return std::make_pair(column, row);
            }
        }
    }
    return std::nullopt;
}

std::uint16_t calculateRequiredRows(const std::vector<LaunchItem>& items,
                                    const std::uint16_t columns) noexcept {
    if (items.empty() || columns == 0) {
        return 0;
    }
    // 保守估算：把所有磁贴按跨度面积展开后除以列数，向上取整；
    // 同时不低于任一磁贴的纵向跨度（Tall 1x2 无论列数多少都占 2 行）。
    // 用于给用户一个「至少需要这么多行」的提示，不保证一定能排下。
    std::uint64_t totalCells = 0;
    std::uint16_t widestSpan = 1;
    std::uint16_t tallestSpan = 1;
    for (const auto& item : items) {
        const std::uint16_t sc = tileSpanColumns(item.tileSize);
        const std::uint16_t sr = tileSpanRows(item.tileSize);
        totalCells += static_cast<std::uint64_t>(sc) * sr;
        widestSpan = std::max(widestSpan, sc);
        tallestSpan = std::max(tallestSpan, sr);
    }
    const std::uint16_t effectiveColumns = std::max(columns, widestSpan);
    const std::uint64_t rows =
        (totalCells + effectiveColumns - 1) / effectiveColumns;
    return static_cast<std::uint16_t>(
        std::min<std::uint64_t>(std::max<std::uint64_t>(rows, tallestSpan),
                                kMaximumGridRows));
}

std::uint16_t calculateRequiredColumns(const std::vector<LaunchItem>& items) noexcept {
    std::uint16_t widest = 1;
    for (const auto& item : items) {
        widest = std::max(widest, tileSpanColumns(item.tileSize));
    }
    // 返回「容纳所有磁贴所需的最小列数」= 最宽磁贴的跨度。
    // 注意：这里不保底 4 列 —— 全部是 Small 磁贴时 1 列也是合法的。
    // 界面默认列数由 AppearanceConfig 决定，与此函数无关。
    return widest;
}

// ================================================================ 核心布局算法

namespace {

// 判断显式坐标是否可用（在边界内且与已占用区域不冲突）
bool explicitPositionUsable(const OccupancyGrid& grid,
                            const LaunchItem& item,
                            std::uint16_t* outColumn,
                            std::uint16_t* outRow) noexcept {
    if (!item.hasGridPosition()) {
        return false;
    }
    const std::uint16_t column = *item.gridColumn;
    const std::uint16_t row = *item.gridRow;
    const std::uint16_t spanColumns = tileSpanColumns(item.tileSize);
    const std::uint16_t spanRows = tileSpanRows(item.tileSize);
    if (!grid.isRegionFree(column, row, spanColumns, spanRows)) {
        return false;
    }
    *outColumn = column;
    *outRow = row;
    return true;
}

}  // namespace

TileLayoutResult calculateTileLayout(const TileLayoutRequest& request) {
    TileLayoutResult result;
    if (request.items == nullptr) {
        return result;
    }

    const std::vector<LaunchItem>& items = *request.items;
    if (items.empty()) {
        return result;
    }

    // 网格尺寸裁剪到合法范围，并保证至少能放下最宽的磁贴
    const std::uint16_t requiredColumns = calculateRequiredColumns(items);
    std::uint16_t columns = std::clamp<std::uint16_t>(
        request.columns,
        std::max<std::uint16_t>(kMinimumGridColumns, requiredColumns),
        kMaximumGridColumns);

    std::uint16_t rows = std::clamp<std::uint16_t>(
        request.rows, kMinimumGridRows, kMaximumGridRows);

    // 若行数不足以放下最高的磁贴也会导致冲突，这里保证 rows >= 最大行跨度
    std::uint16_t tallestSpan = 1;
    for (const auto& item : items) {
        tallestSpan = std::max(tallestSpan, tileSpanRows(item.tileSize));
    }
    rows = std::max(rows, tallestSpan);

    OccupancyGrid grid(columns, rows);
    result.placements.reserve(items.size());

    // ------------------------------------------------------------
    // 第一阶段：放置有显式坐标的项（尊重用户拖拽结果）
    //
    // 这样能保证用户手动摆好的位置不会因为其他项的增删而改变，
    // 这是「稳定优先」原则的体现。
    //
    // 冲突处理：多个项声明了重叠的显式位置时，按「面积大者优先，
    // 同面积则索引靠前者优先」的规则裁决，落败者退回自动放置。
    // 这避免了大磁贴被小磁贴挤走这种反直觉的结果。
    // ------------------------------------------------------------
    std::vector<bool> handled(items.size(), false);

    if (request.preserveExplicitPositions) {
        // 按优先级排序：面积降序、索引升序
        std::vector<std::size_t> candidates;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].hasGridPosition()) {
                candidates.push_back(i);
            }
        }
        std::stable_sort(
            candidates.begin(), candidates.end(),
            [&items](const std::size_t left, const std::size_t right) {
                const std::uint32_t leftArea =
                    static_cast<std::uint32_t>(tileSpanColumns(items[left].tileSize)) *
                    static_cast<std::uint32_t>(tileSpanRows(items[left].tileSize));
                const std::uint32_t rightArea =
                    static_cast<std::uint32_t>(tileSpanColumns(items[right].tileSize)) *
                    static_cast<std::uint32_t>(tileSpanRows(items[right].tileSize));
                if (leftArea != rightArea) {
                    return leftArea > rightArea;  // 面积大者优先
                }
                return left < right;  // 索引靠前者优先
            });

        for (const std::size_t i : candidates) {
            std::uint16_t column = 0;
            std::uint16_t row = 0;
            if (explicitPositionUsable(grid, items[i], &column, &row)) {
                const std::uint16_t spanColumns = tileSpanColumns(items[i].tileSize);
                const std::uint16_t spanRows = tileSpanRows(items[i].tileSize);
                grid.markRegion(column, row, spanColumns, spanRows);

                TilePlacement placement;
                placement.itemIndex = i;
                placement.column = column;
                placement.row = row;
                placement.spanColumns = spanColumns;
                placement.spanRows = spanRows;
                placement.autoPlaced = false;
                result.placements.push_back(placement);
                handled[i] = true;
            }
        }
    }

    // ------------------------------------------------------------
    // 第二阶段：自动放置剩余项（贪心 + 行优先）
    //
    // 保持原有的项目顺序，逐个寻找第一个可容纳位置。
    // 找不到位置时先尝试扩大行数，仍失败则记入 overflow。
    // ------------------------------------------------------------
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (handled[i]) {
            continue;
        }

        const std::uint16_t spanColumns = tileSpanColumns(items[i].tileSize);
        const std::uint16_t spanRows = tileSpanRows(items[i].tileSize);

        auto position = findFirstFreeRegion(grid, spanColumns, spanRows);

        // 当前网格放不下：尝试按需扩展行数（上限 kMaximumGridRows）
        if (!position && rows < kMaximumGridRows) {
            const std::uint16_t extendedRows = std::min<std::uint16_t>(
                static_cast<std::uint16_t>(rows + spanRows), kMaximumGridRows);
            if (extendedRows > rows) {
                OccupancyGrid extended(columns, extendedRows);
                // 重建占用：把已放置项复制过去
                for (const auto& placed : result.placements) {
                    extended.markRegion(placed.column, placed.row,
                                        placed.spanColumns, placed.spanRows);
                }
                grid = extended;
                rows = extendedRows;
                position = findFirstFreeRegion(grid, spanColumns, spanRows);
            }
        }

        if (!position) {
            // 网格确实装不下，记入溢出（界面会提示用户扩大网格）
            result.overflowed.push_back(i);
            continue;
        }

        const std::uint16_t column = position->first;
        const std::uint16_t row = position->second;
        grid.markRegion(column, row, spanColumns, spanRows);

        TilePlacement placement;
        placement.itemIndex = i;
        placement.column = column;
        placement.row = row;
        placement.spanColumns = spanColumns;
        placement.spanRows = spanRows;
        placement.autoPlaced = true;
        result.placements.push_back(placement);
    }

    // ------------------------------------------------------------
    // 第三阶段：保持输出与输入索引对应（便于调用方直接按索引取值）
    // ------------------------------------------------------------
    std::vector<TilePlacement> byIndex(items.size());
    std::vector<bool> filled(items.size(), false);
    for (const auto& placement : result.placements) {
        if (placement.itemIndex < items.size()) {
            byIndex[placement.itemIndex] = placement;
            filled[placement.itemIndex] = true;
        }
    }

    // 未放置的项也补一个默认占位（跨度正确，坐标 0,0，标记 autoPlaced）
    // 这样调用方总能按索引安全访问，无需额外判空。
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!filled[i]) {
            TilePlacement placeholder;
            placeholder.itemIndex = i;
            placeholder.column = 0;
            placeholder.row = 0;
            placeholder.spanColumns = tileSpanColumns(items[i].tileSize);
            placeholder.spanRows = tileSpanRows(items[i].tileSize);
            placeholder.autoPlaced = true;
            byIndex[i] = placeholder;
        }
    }
    result.placements = std::move(byIndex);

    // 计算实际占用的行数
    std::uint16_t usedRows = 0;
    for (const auto& placement : result.placements) {
        if (!filled[placement.itemIndex]) {
            continue;
        }
        const std::uint16_t bottom =
            static_cast<std::uint16_t>(placement.row + placement.spanRows);
        usedRows = std::max(usedRows, bottom);
    }
    result.usedRows = usedRows;

    return result;
}

// ================================================================ 拖拽重排

bool moveItem(std::vector<LaunchItem>& items,
              const std::size_t fromIndex,
              const std::size_t toIndex) noexcept {
    if (items.empty()) {
        return false;
    }
    if (fromIndex >= items.size() || toIndex >= items.size()) {
        return false;
    }
    if (fromIndex == toIndex) {
        return false;
    }

    // 抽出再插入：用 rotate 一次完成，避免反复 erase/insert 的拷贝开销。
    // [first, middle) 与 [middle, last) 交换 —— 对单个元素即「移动」语义。
    if (fromIndex < toIndex) {
        std::rotate(items.begin() + static_cast<std::ptrdiff_t>(fromIndex),
                    items.begin() + static_cast<std::ptrdiff_t>(fromIndex + 1),
                    items.begin() + static_cast<std::ptrdiff_t>(toIndex + 1));
    } else {
        std::rotate(items.begin() + static_cast<std::ptrdiff_t>(toIndex),
                    items.begin() + static_cast<std::ptrdiff_t>(fromIndex),
                    items.begin() + static_cast<std::ptrdiff_t>(fromIndex + 1));
    }

    // 清空显式坐标，让布局引擎重新流式填充。理由见头文件注释：
    // 保留坐标会因跨度差异留下空洞或误判溢出。
    for (auto& item : items) {
        item.clearGridPosition();
    }
    return true;
}

bool isMeaningfulMove(const std::size_t fromIndex,
                      const std::size_t toIndex,
                      const std::size_t itemCount) noexcept {
    if (itemCount == 0) {
        return false;
    }
    if (fromIndex >= itemCount || toIndex >= itemCount) {
        return false;
    }
    return fromIndex != toIndex;
}

// ================================================================ 避让式布局

namespace {

// 就近搜索候选位置。
//
// 按「距离目标区域中心的曼哈顿距离」升序枚举候选格，保证第一个可用位置
// 就是视觉上最近的那个。距离相同时，按「行优先」稳定排序（先上后下、
// 先左后右），让结果可预测、不随机跳。
struct CandidateSite {
    std::uint16_t column = 0;
    std::uint16_t row = 0;
    std::uint32_t distance = 0;
};

// 生成候选位置列表（不排序，调用方排序）
void collectCandidateSites(const std::uint16_t columns,
                           const std::uint16_t rows,
                           const std::uint16_t spanColumns,
                           const std::uint16_t spanRows,
                           const std::uint16_t centerColumn,
                           const std::uint16_t centerRow,
                           std::vector<CandidateSite>& out) {
    out.clear();
    if (spanColumns == 0 || spanRows == 0 ||
        spanColumns > columns || spanRows > rows) {
        return;
    }
    const std::uint16_t maxColumn =
        static_cast<std::uint16_t>(columns - spanColumns);
    const std::uint16_t maxRow = static_cast<std::uint16_t>(rows - spanRows);

    for (std::uint16_t row = 0; row <= maxRow; ++row) {
        for (std::uint16_t column = 0; column <= maxColumn; ++column) {
            // 用位置中心到目标中心的曼哈顿距离衡量「远近」
            const std::uint16_t siteCenterColumn = static_cast<std::uint16_t>(
                column + (spanColumns - 1) / 2);
            const std::uint16_t siteCenterRow = static_cast<std::uint16_t>(
                row + (spanRows - 1) / 2);
            const std::uint32_t dx = (siteCenterColumn > centerColumn)
                ? static_cast<std::uint32_t>(siteCenterColumn - centerColumn)
                : static_cast<std::uint32_t>(centerColumn - siteCenterColumn);
            const std::uint32_t dy = (siteCenterRow > centerRow)
                ? static_cast<std::uint32_t>(siteCenterRow - centerRow)
                : static_cast<std::uint32_t>(centerRow - siteCenterRow);

            CandidateSite site;
            site.column = column;
            site.row = row;
            // 纵向距离加权更高：磁贴横向排布更常见，上下挪动视觉干扰更大，
            // 因此「左/右挪一格」比「上/下挪一格」更优先。
            site.distance = dx * 2 + dy * 3;
            out.push_back(site);
        }
    }
}

// 就近寻找可放置位置。找到返回 true 并写出坐标。
bool findNearestFreeRegion(const OccupancyGrid& grid,
                           const std::uint16_t spanColumns,
                           const std::uint16_t spanRows,
                           const std::uint16_t centerColumn,
                           const std::uint16_t centerRow,
                           std::uint16_t* outColumn,
                           std::uint16_t* outRow) {
    std::vector<CandidateSite> sites;
    collectCandidateSites(grid.columns(), grid.rows(), spanColumns, spanRows,
                          centerColumn, centerRow, sites);
    if (sites.empty()) {
        return false;
    }

    std::stable_sort(sites.begin(), sites.end(),
                     [](const CandidateSite& left, const CandidateSite& right) {
                         if (left.distance != right.distance) {
                             return left.distance < right.distance;
                         }
                         if (left.row != right.row) {
                             return left.row < right.row;  // 行优先
                         }
                         return left.column < right.column;
                     });

    for (const auto& site : sites) {
        if (grid.isRegionFree(site.column, site.row, spanColumns, spanRows)) {
            *outColumn = site.column;
            *outRow = site.row;
            return true;
        }
    }
    return false;
}

// 为「被挤开」的磁贴找一个最不突兀的落点。
//
// 为什么不是单纯按阅读顺序扫：
//   纯阅读顺序（从左到右、从上到下）有个反直觉的后果 —— 一个贴着右边缘的
//   磁贴被挤时，即使它左边、上边紧挨着就是空位，也会被甩到「下一行的最左边」。
//   这种大跨度位移视觉上非常跳，用户很难预料。
//
// 因此改成「由近及远、同轴优先」的螺旋式搜索：
//   对每个步长 d = 1, 2, 3, ...，依次尝试同一行向右 / 向左、
//   同一列向上 / 向下的位置，命中即返回。
//
// 这样保证两个性质：
//   1. 位移尽可能小   —— 步长递增，绝不会「旁边有位置却跳到远处」
//   2. 方向有优先级   —— 同样距离下，右 > 左 > 上 > 下
//      （向右优先对齐 Win10 磁贴腾挪手感；左右整体优先于上下，
//        因为横向排布更常见，上下挪动视觉干扰更大）
//
// 四方向全部探完仍未命中，才退到阅读顺序回卷做最终兜底。
bool findBestFreeRegionForDisplaced(const OccupancyGrid& grid,
                                    const std::uint16_t spanColumns,
                                    const std::uint16_t spanRows,
                                    const std::uint16_t fromColumn,
                                    const std::uint16_t fromRow,
                                    std::uint16_t* outColumn,
                                    std::uint16_t* outRow) {
    const std::uint16_t columns = grid.columns();
    const std::uint16_t rows = grid.rows();
    if (spanColumns == 0 || spanRows == 0 ||
        spanColumns > columns || spanRows > rows) {
        return false;
    }
    const std::uint16_t maxColumn =
        static_cast<std::uint16_t>(columns - spanColumns);
    const std::uint16_t maxRow = static_cast<std::uint16_t>(rows - spanRows);

    // 起点夹到可放置范围内（原坐标可能因为 span>1 而越界）
    const std::uint16_t centerColumn =
        (fromColumn > maxColumn) ? maxColumn : fromColumn;
    const std::uint16_t centerRow =
        (fromRow > maxRow) ? maxRow : fromRow;

    // 步长上限：保证能覆盖到网格最远处
    const std::uint16_t maxStep = static_cast<std::uint16_t>(
        maxColumn > maxRow ? maxColumn : maxRow);

    for (std::uint16_t step = 1; step <= maxStep; ++step) {
        // ---- 同一行向右（最高优先） ----
        if (static_cast<std::uint32_t>(centerColumn) + step <= maxColumn) {
            const std::uint16_t column =
                static_cast<std::uint16_t>(centerColumn + step);
            if (grid.isRegionFree(column, centerRow, spanColumns, spanRows)) {
                *outColumn = column;
                *outRow = centerRow;
                return true;
            }
        }

        // ---- 同一行向左 ----
        if (step <= centerColumn) {
            const std::uint16_t column =
                static_cast<std::uint16_t>(centerColumn - step);
            if (grid.isRegionFree(column, centerRow, spanColumns, spanRows)) {
                *outColumn = column;
                *outRow = centerRow;
                return true;
            }
        }

        // ---- 同一列向上 ----
        if (step <= centerRow) {
            const std::uint16_t row =
                static_cast<std::uint16_t>(centerRow - step);
            if (grid.isRegionFree(centerColumn, row, spanColumns, spanRows)) {
                *outColumn = centerColumn;
                *outRow = row;
                return true;
            }
        }

        // ---- 同一列向下 ----
        if (static_cast<std::uint32_t>(centerRow) + step <= maxRow) {
            const std::uint16_t row =
                static_cast<std::uint16_t>(centerRow + step);
            if (grid.isRegionFree(centerColumn, row, spanColumns, spanRows)) {
                *outColumn = centerColumn;
                *outRow = row;
                return true;
            }
        }
    }

    // ---- 兜底：阅读顺序回卷（同一行/列全被堵死时才可能走到这里） ----
    for (int pass = 0; pass < 2; ++pass) {
        for (std::uint16_t row = 0; row <= maxRow; ++row) {
            for (std::uint16_t column = 0; column <= maxColumn; ++column) {
                const bool isAfter =
                    (row > centerRow) ||
                    (row == centerRow && column > centerColumn);
                if ((pass == 0) != isAfter) {
                    continue;
                }
                if (grid.isRegionFree(column, row, spanColumns, spanRows)) {
                    *outColumn = column;
                    *outRow = row;
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

TileLayoutResult calculateAvoidanceLayout(const AvoidanceRequest& request) {
    TileLayoutResult result;
    if (request.items == nullptr) {
        return result;
    }

    const std::vector<LaunchItem>& items = *request.items;
    if (items.empty() || request.dragIndex >= items.size()) {
        return result;
    }

    // 网格尺寸：保证能放下最宽/最高的磁贴
    std::uint16_t requiredColumns = 1;
    std::uint16_t requiredRows = 1;
    for (const auto& item : items) {
        requiredColumns = std::max(requiredColumns,
                                   tileSpanColumns(item.tileSize));
        requiredRows = std::max(requiredRows, tileSpanRows(item.tileSize));
    }
    std::uint16_t columns = std::clamp<std::uint16_t>(
        request.columns,
        std::max<std::uint16_t>(kMinimumGridColumns, requiredColumns),
        kMaximumGridColumns);
    std::uint16_t rows = std::clamp<std::uint16_t>(
        request.rows, kMinimumGridRows, kMaximumGridRows);
    rows = std::max(rows, requiredRows);

    OccupancyGrid grid(columns, rows);

    // ---- 步骤 1：被拖项先落位，占据目标区域 ----
    //
    // 先占位（而不是最后占），这样后续磁贴的避让都是相对于「A 已经在这里」
    // 来算的，语义更直观。
    const std::uint16_t dragSpanColumns =
        std::max<std::uint16_t>(1, request.dragSpanColumns);
    const std::uint16_t dragSpanRows =
        std::max<std::uint16_t>(1, request.dragSpanRows);

    std::uint16_t dragColumn = request.targetColumn;
    std::uint16_t dragRow = request.targetRow;
    // 目标越界时夹到边界内
    if (dragColumn + dragSpanColumns > columns) {
        dragColumn = static_cast<std::uint16_t>(
            columns > dragSpanColumns ? columns - dragSpanColumns : 0);
    }
    if (dragRow + dragSpanRows > rows) {
        dragRow = static_cast<std::uint16_t>(
            rows > dragSpanRows ? rows - dragSpanRows : 0);
    }
    grid.markRegion(dragColumn, dragRow, dragSpanColumns, dragSpanRows);

    {
        TilePlacement placement;
        placement.itemIndex = request.dragIndex;
        placement.column = dragColumn;
        placement.row = dragRow;
        placement.spanColumns = dragSpanColumns;
        placement.spanRows = dragSpanRows;
        placement.autoPlaced = false;
        result.placements.push_back(placement);
    }

    // ---- 步骤 2：其余磁贴按「冻结原坐标 → 冲突才让位」处理 ----
    //
    // 被拖项占据目标格后，只有与目标区域重叠的磁贴需要让位。
    // 让位规则：按阅读顺序（从左到右、从上到下）从自己位置之后
    // 找第一个放得下的空位 —— 右边的空位天然优先于下方，
    // 更优先于跳到画面另一头（Win10 组内腾挪的手感）。
    // 全部读完放不下才回卷/扩行。未受影响的磁贴坐标完全不动。

    auto overlapArea = [&](const std::size_t index) -> std::uint32_t {
        const auto& item = items[index];
        if (!item.hasGridPosition()) {
            return 0;
        }
        const std::uint16_t sc = tileSpanColumns(item.tileSize);
        const std::uint16_t sr = tileSpanRows(item.tileSize);
        const std::uint16_t c0 = *item.gridColumn;
        const std::uint16_t r0 = *item.gridRow;
        const std::uint16_t left = std::max(c0, dragColumn);
        const std::uint16_t top = std::max(r0, dragRow);
        const std::uint16_t right = std::min<std::uint16_t>(
            static_cast<std::uint16_t>(c0 + sc),
            static_cast<std::uint16_t>(dragColumn + dragSpanColumns));
        const std::uint16_t bottom = std::min<std::uint16_t>(
            static_cast<std::uint16_t>(r0 + sr),
            static_cast<std::uint16_t>(dragRow + dragSpanRows));
        if (left >= right || top >= bottom) {
            return 0;
        }
        return static_cast<std::uint32_t>(right - left) *
               static_cast<std::uint32_t>(bottom - top);
    };

    // 冲突面积大的先处理，减少连环挤压；无冲突的按索引稳定排序
    std::vector<std::size_t> order;
    order.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != request.dragIndex) {
            order.push_back(i);
        }
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](const std::size_t left, const std::size_t right) {
                         const std::uint32_t leftOverlap = overlapArea(left);
                         const std::uint32_t rightOverlap = overlapArea(right);
                         if (leftOverlap != rightOverlap) {
                             return leftOverlap > rightOverlap;
                         }
                         return left < right;
                     });

    std::vector<bool> placed(items.size(), false);
    placed[request.dragIndex] = true;

    // 第一轮：不在目标区域内的磁贴原地冻结
    for (const std::size_t i : order) {
        const auto& item = items[i];
        const std::uint16_t sc = tileSpanColumns(item.tileSize);
        const std::uint16_t sr = tileSpanRows(item.tileSize);
        if (!item.hasGridPosition()) {
            continue;
        }
        const std::uint16_t c = *item.gridColumn;
        const std::uint16_t r = *item.gridRow;
        if (c + sc <= columns && r + sr <= rows &&
            grid.isRegionFree(c, r, sc, sr)) {
            grid.markRegion(c, r, sc, sr);
            TilePlacement placement;
            placement.itemIndex = i;
            placement.column = c;
            placement.row = r;
            placement.spanColumns = sc;
            placement.spanRows = sr;
            placement.autoPlaced = false;
            result.placements.push_back(placement);
            placed[i] = true;
        }
    }

    // 第二轮：为「被压住」的磁贴按阅读顺序向后找位置
    for (const std::size_t i : order) {
        if (placed[i]) {
            continue;
        }
        const auto& item = items[i];
        const std::uint16_t sc = tileSpanColumns(item.tileSize);
        const std::uint16_t sr = tileSpanRows(item.tileSize);

        // 扫描起点 = 自己原来的位置（原地已被占，从「之后」找）
        std::uint16_t centerColumn = dragColumn;
        std::uint16_t centerRow = dragRow;
        if (item.hasGridPosition()) {
            centerColumn = *item.gridColumn;
            centerRow = *item.gridRow;
        }

        std::uint16_t column = 0;
        std::uint16_t row = 0;
        bool found = findBestFreeRegionForDisplaced(
            grid, sc, sr, centerColumn, centerRow, &column, &row);

        // 放不下就扩行（与紧凑布局一致的行为）
        while (!found && rows < kMaximumGridRows) {
            const std::uint16_t extendedRows = std::min<std::uint16_t>(
                static_cast<std::uint16_t>(rows + sr), kMaximumGridRows);
            if (extendedRows <= rows) {
                break;
            }
            OccupancyGrid extended(columns, extendedRows);
            for (const auto& existing : result.placements) {
                extended.markRegion(existing.column, existing.row,
                                    existing.spanColumns, existing.spanRows);
            }
            grid = extended;
            rows = extendedRows;
            found = findBestFreeRegionForDisplaced(
                grid, sc, sr, centerColumn, centerRow, &column, &row);
        }

        if (!found) {
            result.overflowed.push_back(i);
            continue;
        }

        grid.markRegion(column, row, sc, sr);
        TilePlacement placement;
        placement.itemIndex = i;
        placement.column = column;
        placement.row = row;
        placement.spanColumns = sc;
        placement.spanRows = sr;
        placement.autoPlaced = true;
        result.placements.push_back(placement);
        placed[i] = true;
    }

    // 第二轮 B：无坐标的磁贴兜底就近放置
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (placed[i] || items[i].hasGridPosition()) {
            continue;
        }
        const auto& item = items[i];
        const std::uint16_t sc = tileSpanColumns(item.tileSize);
        const std::uint16_t sr = tileSpanRows(item.tileSize);

        std::uint16_t column = 0;
        std::uint16_t row = 0;
        bool found = findNearestFreeRegion(grid, sc, sr, dragColumn,
                                           dragRow, &column, &row);
        if (!found) {
            result.overflowed.push_back(i);
            continue;
        }
        grid.markRegion(column, row, sc, sr);
        TilePlacement placement;
        placement.itemIndex = i;
        placement.column = column;
        placement.row = row;
        placement.spanColumns = sc;
        placement.spanRows = sr;
        placement.autoPlaced = true;
        result.placements.push_back(placement);
        placed[i] = true;
    }

    // ---- 步骤 3：输出按输入索引对齐 ----
    std::vector<TilePlacement> byIndex(items.size());
    std::vector<bool> filled(items.size(), false);
    for (const auto& placement : result.placements) {
        if (placement.itemIndex < items.size()) {
            byIndex[placement.itemIndex] = placement;
            filled[placement.itemIndex] = true;
        }
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!filled[i]) {
            TilePlacement placeholder;
            placeholder.itemIndex = i;
            placeholder.spanColumns = tileSpanColumns(items[i].tileSize);
            placeholder.spanRows = tileSpanRows(items[i].tileSize);
            placeholder.autoPlaced = true;
            byIndex[i] = placeholder;
        }
    }
    result.placements = std::move(byIndex);

    // 实际占用行数
    std::uint16_t usedRows = 0;
    for (const auto& placement : result.placements) {
        if (!filled[placement.itemIndex]) {
            continue;
        }
        const std::uint16_t bottom = static_cast<std::uint16_t>(
            placement.row + placement.spanRows);
        usedRows = std::max(usedRows, bottom);
    }
    result.usedRows = usedRows;

    return result;
}

}  // namespace core
}  // namespace ktile
