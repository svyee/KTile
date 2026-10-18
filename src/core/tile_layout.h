// KTile - 磁贴跨度布局引擎
//
// 这是 KTile 相对 HLaunch 的核心差异点。
//
// HLaunch 使用等分网格：每项固定占 1 格，位置 = index % columns / index / columns。
// KTile 使用跨度网格：每项根据 TileSize 占据 1x1 / 2x2 / 4x2 格，需要二维装箱。
//
// 设计原则：
//  1. 纯算法，零 UI 依赖 —— 可完整单元测试
//  2. 确定性 —— 同样的输入必然产生同样的布局，避免界面抖动
//  3. 稳定优先 —— 已放置的磁贴位置尽量不动，减少重排带来的视觉跳变
//  4. 有序填充 —— 按行优先扫描第一个可容纳位置（贪心）
//
#pragma once

#include "core/data_model.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ktile {
namespace core {

// ================================================================ 网格占用表
//
// 记录哪些小格已被占用。用于跨度布局的空隙检测。
//
class OccupancyGrid {
public:
    OccupancyGrid(std::uint16_t columns, std::uint16_t rows);

    std::uint16_t columns() const noexcept { return columns_; }
    std::uint16_t rows() const noexcept { return rows_; }

    // 指定格是否被占用（越界视为已占用，防止溢出写入）
    bool isOccupied(std::uint16_t column, std::uint16_t row) const noexcept;

    // 指定矩形区域是否全部空闲
    bool isRegionFree(std::uint16_t column,
                      std::uint16_t row,
                      std::uint16_t spanColumns,
                      std::uint16_t spanRows) const noexcept;

    // 指定矩形区域是否在网格边界内
    bool isRegionInside(std::uint16_t column,
                        std::uint16_t row,
                        std::uint16_t spanColumns,
                        std::uint16_t spanRows) const noexcept;

    // 标记区域占用（不做边界检查，调用方需先验证）
    void markRegion(std::uint16_t column,
                    std::uint16_t row,
                    std::uint16_t spanColumns,
                    std::uint16_t spanRows) noexcept;

    // 清空全部占用
    void clear() noexcept;

private:
    std::uint16_t columns_;
    std::uint16_t rows_;
    std::vector<bool> cells_;
};

// ================================================================ 布局结果

struct TilePlacement {
    // 在 items 数组中的索引
    std::size_t itemIndex = 0;
    // 磁贴左上角所在的小格坐标
    std::uint16_t column = 0;
    std::uint16_t row = 0;
    // 磁贴占据的跨度（小格数）
    std::uint16_t spanColumns = 1;
    std::uint16_t spanRows = 1;
    // 是否为自动放置（相对于沿用已有显式坐标）
    bool autoPlaced = false;
};

struct TileLayoutResult {
    // 每个项目的最终放置位置（与输入 items 顺序一一对应）
    std::vector<TilePlacement> placements;

    // 未能放入的项索引（网格容量不足时）
    std::vector<std::size_t> overflowed;

    // 实际占用的最大行数（用于计算窗口高度）
    std::uint16_t usedRows = 0;

    // 全部项是否都成功放入
    bool complete() const noexcept { return overflowed.empty(); }
};

// ================================================================ 布局参数

struct TileLayoutRequest {
    // 网格尺寸（小格）
    std::uint16_t columns = kDefaultGridColumns;
    std::uint16_t rows = kDefaultGridRows;

    // 待布局项（只需提供 tileSize 与已有网格坐标）
    const std::vector<LaunchItem>* items = nullptr;

    // 是否保留已有显式坐标（拖拽后固定位置）
    // true  = 尊重 item 里已存的 gridColumn/gridRow
    // false = 完全重新自动布局（如切换网格行列数时）
    bool preserveExplicitPositions = true;
};

// ================================================================ 核心 API

// 计算磁贴布局。纯函数，不修改输入。
TileLayoutResult calculateTileLayout(const TileLayoutRequest& request);

// 寻找能容纳指定跨度的第一个空闲位置（行优先扫描）。
// 返回 nullopt 表示放不下。
std::optional<std::pair<std::uint16_t, std::uint16_t>> findFirstFreeRegion(
    const OccupancyGrid& grid,
    std::uint16_t spanColumns,
    std::uint16_t spanRows) noexcept;

// 判断某位置是否可放置指定跨度的磁贴（边界内且无冲突）
bool canPlaceTile(const OccupancyGrid& grid,
                  std::uint16_t column,
                  std::uint16_t row,
                  std::uint16_t spanColumns,
                  std::uint16_t spanRows) noexcept;

// 计算为容纳所有磁贴所需的最小行数
std::uint16_t calculateRequiredRows(const std::vector<LaunchItem>& items,
                                    std::uint16_t columns) noexcept;

// 计算为容纳所有磁贴所需的最小列数（取最大跨度，至少为 4 以放下 Wide）
std::uint16_t calculateRequiredColumns(const std::vector<LaunchItem>& items) noexcept;

// ================================================================ 拖拽重排
//
// 拖拽采用「插入式重排」而不是「自由定位」：
//   把 fromIndex 处的项从序列中抽出，插入到 toIndex 处，其余项依次顺移。
//   重排后清空所有项的显式坐标，让布局引擎重新自动填充 —— 这样不会因为
//   跨度差异留下空洞，视觉上也更符合「列表重新流式排布」的直觉。
//
// 之所以不保留显式坐标：拖动一个 2x2 到一排 1x1 中间时，若强行保留
// 原有坐标，其余 1x1 会被挤到奇怪的位置或直接被判溢出。清空让引擎重排
// 可保证结果总是紧凑且确定的。
//
// 返回 false 表示索引非法（越界或 from == to），调用方应忽略本次拖拽。
bool moveItem(std::vector<LaunchItem>& items,
              std::size_t fromIndex,
              std::size_t toIndex) noexcept;

// 判断某个拖拽落点是否会产生实际变化（用于避免无谓的重排与落盘）
bool isMeaningfulMove(std::size_t fromIndex,
                      std::size_t toIndex,
                      std::size_t itemCount) noexcept;

// ================================================================ 拖拽落点
//
// 「落点」= 被拖项松手后将占据的格子区域，而不是「光标下那块磁贴」。
// 两者在多数时候一致，但有两种情况必须区分：
//   1. 面板右侧/下方的空白带：那里没有磁贴，但用户明确想把图标放过去
//   2. 被拖项跨度大于落点磁贴：2x2 落到 1x1 上时，占的是更大的一块
//
// 因此落点用「矩形 + 被拖项跨度」描述，由 UI 层的 resolveDropspot 求出，
// 再由 calculateAvoidanceLayout 把它翻译成最终排布。
struct Dropspot {
    // 目标格坐标（磁贴左上角所在小格）
    std::uint16_t column = 0;
    std::uint16_t row = 0;

    // 被拖项要占据的跨度
    std::uint16_t spanColumns = 1;
    std::uint16_t spanRows = 1;

    // 该落点是否与某个现有磁贴重合（重合时显示为「替换/挤开」语义）
    bool overOccupiedCell = false;

    // 该落点是否与原位置相同（相同则松手等价于取消）
    bool sameAsOrigin = false;
};

// ================================================================ 避让式布局
//
// 拖拽落定后需要回答一个问题：「把 A 放到目标格，其他磁贴会怎样？」
//
// 早期实现复用了 calculateTileLayout 的「紧凑回流」策略 —— 全部磁贴
// 从左上角重新流式填充。这在拖拽场景下有三个反直觉的后果：
//   1. 空位总被挤到右下角，用户想留个洞是不可能的
//   2. 明明没碰到某块磁贴，它也被挪走了（大面积位移）
//   3. 被拖项的跨度差异会让「谁该让位」变得难以预测
//
// 现在改为「最小扰动」策略：
//   - 被拖项直接占据目标格（含空白格，所以往空白处拖也成立）
//   - 所有被它压住的磁贴**只让出自己占的那一块**，不改变相对次序
//   - 让位磁贴按「阅读顺序顺移」：从自己位置之后，往右找、再往下找，
//     最后才回卷到上方 —— 右边的空位天然优先（Win10 组内腾挪的手感）
//   - 未与目标区域重叠的磁贴坐标完全不动，中间的空格得以保留
//
// 这样拖 A 时只有真正挡住 A 的磁贴会动，且动得最少，所见即所得。

struct AvoidanceRequest {
    // 网格尺寸（小格）
    std::uint16_t columns = kDefaultGridColumns;
    std::uint16_t rows = kDefaultGridRows;

    // 待布局项（按显示顺序）
    const std::vector<LaunchItem>* items = nullptr;

    // 被拖拽项的索引（该项会被放到 targetColumn/targetRow）
    std::size_t dragIndex = 0;

    // 被拖项的目标格坐标
    std::uint16_t targetColumn = 0;
    std::uint16_t targetRow = 0;

    // 被拖项的跨度（由调用方给出，避免再次查询 tileSize）
    std::uint16_t dragSpanColumns = 1;
    std::uint16_t dragSpanRows = 1;
};

// 计算避让式布局。纯函数，不修改输入。
TileLayoutResult calculateAvoidanceLayout(const AvoidanceRequest& request);

}  // namespace core
}  // namespace ktile
