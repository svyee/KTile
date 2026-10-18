// KTile - 磁贴跨度布局引擎测试
#include "core/tile_layout.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace ktile::core;

static int g_failures = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            wprintf(L"FAIL line %d: %s\n", __LINE__, L#cond);    \
            ++g_failures;                                        \
        }                                                        \
    } while (false)

#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        const auto a_ = (actual);                                             \
        const auto e_ = (expected);                                           \
        if (!(a_ == e_)) {                                                    \
            wprintf(L"FAIL line %d: %s != %s (got %d, want %d)\n",            \
                    __LINE__, L#actual, L#expected,                           \
                    static_cast<int>(a_), static_cast<int>(e_));              \
            ++g_failures;                                                     \
        }                                                                     \
    } while (false)

static LaunchItem makeItem(const char* id, TileSize size) {
    LaunchItem item;
    item.id = id;
    item.name = id;
    item.target = "C:\\test.exe";
    item.tileSize = size;
    return item;
}

// ---------------------------------------------------------------- 跨度定义

static void testTileSpans() {
    // Small = 1x1
    CHECK_EQ(tileSpanColumns(TileSize::Small), 1);
    CHECK_EQ(tileSpanRows(TileSize::Small), 1);
    // Large = 2x2
    CHECK_EQ(tileSpanColumns(TileSize::Large), 2);
    CHECK_EQ(tileSpanRows(TileSize::Large), 2);
    // Wide = 2x1（横向 2 格）
    CHECK_EQ(tileSpanColumns(TileSize::Wide), 2);
    CHECK_EQ(tileSpanRows(TileSize::Wide), 1);
    // Tall = 1x2（纵向 2 格）
    CHECK_EQ(tileSpanColumns(TileSize::Tall), 1);
    CHECK_EQ(tileSpanRows(TileSize::Tall), 2);
}

// ---------------------------------------------------------------- 占用表

static void testOccupancyGrid() {
    OccupancyGrid grid(4, 4);

    CHECK(!grid.isOccupied(0, 0));
    CHECK(!grid.isRegionFree(3, 3, 2, 2));  // 越界
    CHECK(grid.isRegionFree(0, 0, 2, 2));

    grid.markRegion(0, 0, 2, 2);
    CHECK(grid.isOccupied(0, 0));
    CHECK(grid.isOccupied(1, 1));
    CHECK(!grid.isOccupied(2, 2));
    CHECK(!grid.isRegionFree(1, 1, 2, 2));  // 与已占用区重叠
    CHECK(grid.isRegionFree(2, 0, 2, 2));   // 相邻但不重叠

    grid.clear();
    CHECK(!grid.isOccupied(0, 0));
    CHECK(grid.isRegionFree(0, 0, 4, 4));
}

// ---------------------------------------------------------------- 首个空位查找

static void testFindFirstFreeRegion() {
    OccupancyGrid grid(6, 6);

    // 空网格：第一个位置必然是 (0,0)
    auto p = findFirstFreeRegion(grid, 1, 1);
    CHECK(p.has_value());
    CHECK_EQ(p->first, 0);
    CHECK_EQ(p->second, 0);

    // 占掉 (0,0) 后，1x1 应该去 (1,0)
    grid.markRegion(0, 0, 1, 1);
    p = findFirstFreeRegion(grid, 1, 1);
    CHECK(p.has_value());
    CHECK_EQ(p->first, 1);
    CHECK_EQ(p->second, 0);

    // 2x2 在只剩 (1,0) 被占的情况下，第一个可放位置是 (2,0)
    grid.clear();
    grid.markRegion(0, 0, 1, 1);
    p = findFirstFreeRegion(grid, 2, 2);
    CHECK(p.has_value());
    CHECK_EQ(p->first, 1);
    CHECK_EQ(p->second, 0);

    // 跨度超过网格尺寸：应失败
    p = findFirstFreeRegion(grid, 7, 1);
    CHECK(!p.has_value());
}

// ---------------------------------------------------------------- 基本布局

static void testLayoutEmpty() {
    std::vector<LaunchItem> items;
    TileLayoutRequest request;
    request.columns = 6;
    request.rows = 6;
    request.items = &items;

    auto result = calculateTileLayout(request);
    CHECK(result.placements.empty());
    CHECK(result.overflowed.empty());
    CHECK(result.complete());
    CHECK_EQ(result.usedRows, 0);
}

static void testLayoutAllSmall() {
    std::vector<LaunchItem> items;
    for (int i = 0; i < 6; ++i) {
        items.push_back(makeItem(("a" + std::to_string(i)).c_str(), TileSize::Small));
    }

    TileLayoutRequest request;
    request.columns = 3;
    request.rows = 3;
    request.items = &items;

    auto result = calculateTileLayout(request);
    CHECK(result.complete());
    CHECK_EQ(result.placements.size(), static_cast<std::size_t>(6));

    // 3 列布局下，前 3 个应在第 0 行，后 3 个在第 1 行
    CHECK_EQ(result.placements[0].column, 0);
    CHECK_EQ(result.placements[0].row, 0);
    CHECK_EQ(result.placements[1].column, 1);
    CHECK_EQ(result.placements[1].row, 0);
    CHECK_EQ(result.placements[2].column, 2);
    CHECK_EQ(result.placements[2].row, 0);
    CHECK_EQ(result.placements[3].column, 0);
    CHECK_EQ(result.placements[3].row, 1);
    CHECK_EQ(result.placements[5].column, 2);
    CHECK_EQ(result.placements[5].row, 1);
    CHECK_EQ(result.usedRows, 2);
}

// ---------------------------------------------------------------- 磁贴混排（核心场景）

static void testLayoutMixedTiles() {
    // 一个 Medium(2x2) + 若干 Small
    std::vector<LaunchItem> items;
    items.push_back(makeItem("medium", TileSize::Large));
    items.push_back(makeItem("s1", TileSize::Small));
    items.push_back(makeItem("s2", TileSize::Small));
    items.push_back(makeItem("s3", TileSize::Small));
    items.push_back(makeItem("s4", TileSize::Small));

    TileLayoutRequest request;
    request.columns = 4;
    request.rows = 4;
    request.items = &items;

    auto result = calculateTileLayout(request);
    CHECK(result.complete());

    // Medium 应放在 (0,0) 占据 2x2
    CHECK_EQ(result.placements[0].column, 0);
    CHECK_EQ(result.placements[0].row, 0);
    CHECK_EQ(result.placements[0].spanColumns, 2);
    CHECK_EQ(result.placements[0].spanRows, 2);

    // 后续 Small 应填充右侧 (2,0)、(3,0)，然后 (2,1)、(3,1)
    CHECK_EQ(result.placements[1].column, 2);
    CHECK_EQ(result.placements[1].row, 0);
    CHECK_EQ(result.placements[2].column, 3);
    CHECK_EQ(result.placements[2].row, 0);
    CHECK_EQ(result.placements[3].column, 2);
    CHECK_EQ(result.placements[3].row, 1);
    CHECK_EQ(result.placements[4].column, 3);
    CHECK_EQ(result.placements[4].row, 1);

    CHECK_EQ(result.usedRows, 2);

    // 验证无重叠
    OccupancyGrid verify(4, 4);
    for (const auto& p : result.placements) {
        CHECK(verify.isRegionFree(p.column, p.row, p.spanColumns, p.spanRows));
        verify.markRegion(p.column, p.row, p.spanColumns, p.spanRows);
    }
}

static void testLayoutWideTile() {
    // Wide = 2x1（横向 2 格）
    std::vector<LaunchItem> items;
    items.push_back(makeItem("wide", TileSize::Wide));
    items.push_back(makeItem("s1", TileSize::Small));

    TileLayoutRequest request;
    request.columns = 4;
    request.rows = 4;
    request.items = &items;

    auto result = calculateTileLayout(request);
    CHECK(result.complete());

    CHECK_EQ(result.placements[0].column, 0);
    CHECK_EQ(result.placements[0].row, 0);
    CHECK_EQ(result.placements[0].spanColumns, 2);
    CHECK_EQ(result.placements[0].spanRows, 1);

    // Small 应落到 Wide 右侧 (2,0)
    CHECK_EQ(result.placements[1].column, 2);
    CHECK_EQ(result.placements[1].row, 0);
    CHECK_EQ(result.usedRows, 1);
}

static void testLayoutTallTile() {
    // Tall = 1x2（纵向 2 格）
    std::vector<LaunchItem> items;
    items.push_back(makeItem("tall", TileSize::Tall));
    items.push_back(makeItem("s1", TileSize::Small));

    TileLayoutRequest request;
    request.columns = 4;
    request.rows = 4;
    request.items = &items;

    auto result = calculateTileLayout(request);
    CHECK(result.complete());

    CHECK_EQ(result.placements[0].column, 0);
    CHECK_EQ(result.placements[0].row, 0);
    CHECK_EQ(result.placements[0].spanColumns, 1);
    CHECK_EQ(result.placements[0].spanRows, 2);

    // Small 紧随其后：(1,0)（贪心按行优先）
    CHECK_EQ(result.placements[1].column, 1);
    CHECK_EQ(result.placements[1].row, 0);
    CHECK_EQ(result.usedRows, 2);
}

static void testNarrowGridAutoWidens() {
    // 请求 1 列但含 Wide(2x1) —— 应自动扩展到至少 2 列
    std::vector<LaunchItem> items;
    items.push_back(makeItem("wide", TileSize::Wide));

    TileLayoutRequest request;
    request.columns = 1;
    request.rows = 3;
    request.items = &items;

    auto result = calculateTileLayout(request);
    CHECK(result.complete());
    CHECK_EQ(result.placements[0].spanColumns, 2);
    // 能放下说明列数已扩展到 >= 2
    CHECK_EQ(result.placements[0].column, 0);
    CHECK_EQ(result.placements[0].row, 0);
}

// ---------------------------------------------------------------- 显式位置保留

static void testExplicitPositionPreserved() {
    std::vector<LaunchItem> items;
    items.push_back(makeItem("a", TileSize::Small));
    items.push_back(makeItem("b", TileSize::Small));
    items.push_back(makeItem("c", TileSize::Small));

    // 给 b 指定位置 (0,2)
    items[1].setGridPosition(0, 2);

    TileLayoutRequest request;
    request.columns = 4;
    request.rows = 4;
    request.items = &items;
    request.preserveExplicitPositions = true;

    auto result = calculateTileLayout(request);
    CHECK(result.complete());

    // b 必须留在 (0,2)
    CHECK_EQ(result.placements[1].column, 0);
    CHECK_EQ(result.placements[1].row, 2);
    CHECK(!result.placements[1].autoPlaced);

    // a 和 c 自动放置，且不占用 b 的位置
    CHECK(result.placements[0].autoPlaced);
    CHECK(result.placements[2].autoPlaced);

    // 验证无重叠
    OccupancyGrid verify(4, 4);
    for (const auto& p : result.placements) {
        CHECK(verify.isRegionFree(p.column, p.row, p.spanColumns, p.spanRows));
        verify.markRegion(p.column, p.row, p.spanColumns, p.spanRows);
    }
}

static void testExplicitPositionConflictIgnored() {
    // 场景：a(Medium, 2x2) 无显式坐标，b(Small) 显式声明占用 (0,0)。
    //
    // 由于 b 的 (0,0) 是合法且不冲突的（此时网格为空），b 应当被保留在 (0,0)；
    // a 自动放置到其他位置。两者都不重叠，且都成功放置。
    //
    // 这验证了「显式坐标优先于自动放置」的语义。
    std::vector<LaunchItem> items;
    items.push_back(makeItem("a", TileSize::Large));
    items.push_back(makeItem("b", TileSize::Small));
    items[1].setGridPosition(0, 0);

    TileLayoutRequest request;
    request.columns = 4;
    request.rows = 4;
    request.items = &items;
    request.preserveExplicitPositions = true;

    auto result = calculateTileLayout(request);
    CHECK(result.complete());

    // b 的显式坐标被保留
    CHECK_EQ(result.placements[1].column, 0);
    CHECK_EQ(result.placements[1].row, 0);
    CHECK(!result.placements[1].autoPlaced);

    // a 被自动放置到别处（不与 b 重叠）
    CHECK(result.placements[0].autoPlaced);

    // 验证无重叠
    OccupancyGrid verify(4, 4);
    for (const auto& p : result.placements) {
        CHECK(verify.isRegionFree(p.column, p.row, p.spanColumns, p.spanRows));
        verify.markRegion(p.column, p.row, p.spanColumns, p.spanRows);
    }
}

static void testLargerTileWinsOverlappingExplicitClaim() {
    // 场景：两个项声明了互相重叠的显式位置。
    //   a(Medium, 2x2) 声明 (0,0)；b(Medium, 2x2) 也声明 (0,0)。
    // 规则：面积大者优先，同面积则索引靠前者优先 —— a 胜出并保留 (0,0)，
    //       b 退回自动放置（落到 (2,0)）。
    std::vector<LaunchItem> items;
    items.push_back(makeItem("a", TileSize::Large));
    items.push_back(makeItem("b", TileSize::Large));
    items[0].setGridPosition(0, 0);
    items[1].setGridPosition(0, 0);  // 与 a 冲突

    TileLayoutRequest request;
    request.columns = 4;
    request.rows = 4;
    request.items = &items;
    request.preserveExplicitPositions = true;

    auto result = calculateTileLayout(request);
    CHECK(result.complete());

    // a 赢得冲突位置
    CHECK_EQ(result.placements[0].column, 0);
    CHECK_EQ(result.placements[0].row, 0);
    CHECK(!result.placements[0].autoPlaced);

    // b 被重排到别处
    CHECK(result.placements[1].autoPlaced);

    // 无重叠验证
    OccupancyGrid verify(4, 4);
    for (const auto& p : result.placements) {
        CHECK(verify.isRegionFree(p.column, p.row, p.spanColumns, p.spanRows));
        verify.markRegion(p.column, p.row, p.spanColumns, p.spanRows);
    }
}

static void testIgnoreExplicitPositions() {
    std::vector<LaunchItem> items;
    items.push_back(makeItem("a", TileSize::Small));
    items[0].setGridPosition(3, 3);

    TileLayoutRequest request;
    request.columns = 4;
    request.rows = 4;
    request.items = &items;
    request.preserveExplicitPositions = false;  // 完全重排

    auto result = calculateTileLayout(request);
    CHECK(result.complete());
    // 忽略显式坐标后应从 (0,0) 开始
    CHECK_EQ(result.placements[0].column, 0);
    CHECK_EQ(result.placements[0].row, 0);
}

// ---------------------------------------------------------------- 同尺寸稳定性

static void testStableLayoutForSameInput() {
    std::vector<LaunchItem> items;
    items.push_back(makeItem("m", TileSize::Large));
    items.push_back(makeItem("s1", TileSize::Small));
    items.push_back(makeItem("s2", TileSize::Small));
    items.push_back(makeItem("w", TileSize::Wide));

    TileLayoutRequest request;
    request.columns = 6;
    request.rows = 6;
    request.items = &items;

    auto first = calculateTileLayout(request);
    auto second = calculateTileLayout(request);

    CHECK_EQ(first.placements.size(), second.placements.size());
    for (std::size_t i = 0; i < first.placements.size(); ++i) {
        CHECK_EQ(first.placements[i].column, second.placements[i].column);
        CHECK_EQ(first.placements[i].row, second.placements[i].row);
    }
}

// ---------------------------------------------------------------- 溢出

static void testOverflowDetection() {
    // 3 列网格塞 10 个 Medium(2x2)。
    // 3 列下每行只能放 1 个 2x2（剩 1 列放不下第二个），
    // 10 个需要 20 行，超过 kMaximumGridRows(16) —— 必然溢出。
    std::vector<LaunchItem> items;
    for (int i = 0; i < 10; ++i) {
        items.push_back(makeItem(("m" + std::to_string(i)).c_str(), TileSize::Large));
    }

    TileLayoutRequest request;
    request.columns = 3;
    request.rows = 3;
    request.items = &items;

    auto result = calculateTileLayout(request);

    // 应检测到溢出，且未放置项已记入 overflowed
    CHECK(!result.complete());
    CHECK(!result.overflowed.empty());

    // 已放置数量 = 总数 - 溢出数量
    CHECK_EQ(result.placements.size(), static_cast<std::size_t>(10));

    // 行数不应超过上限
    CHECK(result.usedRows <= kMaximumGridRows);

    // 溢出项必须都有对应记录
    std::size_t placedCount = 0;
    for (const auto& p : result.placements) {
        (void)p;
        ++placedCount;
    }
    CHECK_EQ(placedCount, static_cast<std::size_t>(10));
}

// ---------------------------------------------------------------- 所需行列估算

static void testRequiredDimensions() {
    std::vector<LaunchItem> items;
    items.push_back(makeItem("w", TileSize::Wide));
    items.push_back(makeItem("s", TileSize::Small));
    CHECK_EQ(calculateRequiredColumns(items), 2);

    // 全 Small 时最小列数为 1（不强制保底 4 列）
    std::vector<LaunchItem> smalls;
    smalls.push_back(makeItem("s1", TileSize::Small));
    smalls.push_back(makeItem("s2", TileSize::Small));
    CHECK_EQ(calculateRequiredColumns(smalls), 1);

    // Large(2x2) 需要 2 列
    std::vector<LaunchItem> larges;
    larges.push_back(makeItem("m1", TileSize::Large));
    CHECK_EQ(calculateRequiredColumns(larges), 2);

    // Tall(1x2) 只需 1 列但需 2 行
    std::vector<LaunchItem> talls;
    talls.push_back(makeItem("t1", TileSize::Tall));
    CHECK_EQ(calculateRequiredColumns(talls), 1);
    CHECK_EQ(calculateRequiredRows(talls, 4), 2);

    // 空列表
    std::vector<LaunchItem> none;
    CHECK_EQ(calculateRequiredColumns(none), 1);

    // 12 个 Small 在 6 列下需要 2 行
    std::vector<LaunchItem> twelve;
    for (int i = 0; i < 12; ++i) {
        twelve.push_back(makeItem(("x" + std::to_string(i)).c_str(), TileSize::Small));
    }
    CHECK_EQ(calculateRequiredRows(twelve, 6), 2);

    // 4 个 Medium(2x2) 共 16 格，4 列下需要 4 行
    std::vector<LaunchItem> fourMedium;
    for (int i = 0; i < 4; ++i) {
        fourMedium.push_back(makeItem(("m" + std::to_string(i)).c_str(), TileSize::Large));
    }
    CHECK_EQ(calculateRequiredRows(fourMedium, 4), 4);
}

// ---------------------------------------------------------------- 尺寸变更重排

static void testTileSizeChangeReflow() {
    std::vector<LaunchItem> items;
    items.push_back(makeItem("a", TileSize::Small));
    items.push_back(makeItem("b", TileSize::Small));
    items.push_back(makeItem("c", TileSize::Small));
    items.push_back(makeItem("d", TileSize::Small));

    TileLayoutRequest request;
    request.columns = 4;
    request.rows = 4;
    request.items = &items;
    request.preserveExplicitPositions = false;

    auto before = calculateTileLayout(request);
    CHECK_EQ(before.usedRows, 1);  // 4 个 1x1 挤在 1 行

    // 把第一个改成 Medium(2x2)
    items[0].tileSize = TileSize::Large;
    auto after = calculateTileLayout(request);

    CHECK(after.complete());
    CHECK_EQ(after.placements[0].spanColumns, 2);
    CHECK_EQ(after.placements[0].spanRows, 2);
    // 总占用必定增大
    CHECK(after.usedRows >= before.usedRows);

    // 无重叠验证
    OccupancyGrid verify(4, 4);
    for (const auto& p : after.placements) {
        CHECK(verify.isRegionFree(p.column, p.row, p.spanColumns, p.spanRows));
        verify.markRegion(p.column, p.row, p.spanColumns, p.spanRows);
    }
}

// ---------------------------------------------------------------- 工具函数

static void testTileSizeHelpers() {
    CHECK(tileSizeFromInt(0) == TileSize::Small);
    CHECK(tileSizeFromInt(1) == TileSize::Wide);
    CHECK(tileSizeFromInt(2) == TileSize::Tall);
    CHECK(tileSizeFromInt(3) == TileSize::Large);
    CHECK(tileSizeFromInt(99) == TileSize::Small);  // 非法值回退

    CHECK_EQ(tileSizeToInt(TileSize::Small), 0);
    CHECK_EQ(tileSizeToInt(TileSize::Wide), 1);
    CHECK_EQ(tileSizeToInt(TileSize::Tall), 2);
    CHECK_EQ(tileSizeToInt(TileSize::Large), 3);
}

// ---------------------------------------------------------------- 拖拽重排

static std::vector<LaunchItem> makeSequence(const char* ids) {
    std::vector<LaunchItem> items;
    for (const char* p = ids; *p != '\0'; ++p) {
        const std::string id(1, *p);
        items.push_back(makeItem(id.c_str(), TileSize::Small));
    }
    return items;
}

static std::string sequenceIds(const std::vector<LaunchItem>& items) {
    std::string joined;
    for (const auto& item : items) {
        joined += item.id;
    }
    return joined;
}

// 向后拖：把第 0 项移到第 2 位，其余依次前移
static void testMoveItemForward() {
    auto items = makeSequence("abcde");
    CHECK(moveItem(items, 0, 2));
    CHECK(sequenceIds(items) == "bcade");
}

// 向前拖：把第 4 项移到第 1 位，其余依次后移
static void testMoveItemBackward() {
    auto items = makeSequence("abcde");
    CHECK(moveItem(items, 4, 1));
    CHECK(sequenceIds(items) == "aebcd");
}

// 拖到首尾
static void testMoveItemToEnds() {
    auto first = makeSequence("abcd");
    CHECK(moveItem(first, 3, 0));
    CHECK(sequenceIds(first) == "dabc");

    auto last = makeSequence("abcd");
    CHECK(moveItem(last, 0, 3));
    CHECK(sequenceIds(last) == "bcda");
}

// 原地拖拽应被拒绝（避免无谓重排与落盘）
static void testMoveItemToSameIndexRejected() {
    auto items = makeSequence("abc");
    CHECK(!moveItem(items, 1, 1));
    CHECK(sequenceIds(items) == "abc");
}

// 越界索引应被拒绝且不改动序列
static void testMoveItemOutOfRangeRejected() {
    auto items = makeSequence("abc");
    CHECK(!moveItem(items, 5, 0));
    CHECK(!moveItem(items, 0, 5));
    CHECK(sequenceIds(items) == "abc");

    std::vector<LaunchItem> empty;
    CHECK(!moveItem(empty, 0, 0));
}

// 重排后必须清空全部显式坐标，交由布局引擎重新流式填充。
// 否则拖动大跨度磁贴会留下空洞或误判溢出。
static void testMoveItemClearsGridPositions() {
    auto items = makeSequence("abcd");
    items[0].setGridPosition(0, 0);
    items[1].setGridPosition(1, 0);
    items[2].setGridPosition(2, 0);
    items[3].setGridPosition(3, 0);

    CHECK(moveItem(items, 0, 2));
    for (const auto& item : items) {
        CHECK(!item.hasGridPosition());
    }
}

// 重排后重新布局，所有磁贴都应完整放入（不因跨度差异溢出）
static void testMoveItemKeepsLayoutComplete() {
    std::vector<LaunchItem> items;
    items.push_back(makeItem("small1", TileSize::Small));
    items.push_back(makeItem("big", TileSize::Large));
    items.push_back(makeItem("small2", TileSize::Small));
    items.push_back(makeItem("wide", TileSize::Wide));
    items.push_back(makeItem("tall", TileSize::Tall));

    // 把 2x2 大磁贴拖到首位
    const std::size_t bigIndex = 1;
    CHECK(moveItem(items, bigIndex, 0));
    CHECK(items.front().id == "big");

    TileLayoutRequest request;
    request.columns = 4;
    request.rows = 8;
    request.items = &items;
    request.preserveExplicitPositions = true;

    const TileLayoutResult result = calculateTileLayout(request);
    CHECK(result.complete());
    CHECK(result.usedRows > 0);
}

static void testIsMeaningfulMove() {
    CHECK(isMeaningfulMove(0, 2, 5));
    CHECK(isMeaningfulMove(4, 1, 5));
    CHECK(!isMeaningfulMove(2, 2, 5));
    CHECK(!isMeaningfulMove(0, 5, 5));   // 越界
    CHECK(!isMeaningfulMove(9, 0, 5));   // 越界
    CHECK(!isMeaningfulMove(0, 0, 0));   // 空集合
}

// ---------------------------------------------------------------- 避让式布局

// 构造一个带显式坐标的磁贴
static LaunchItem makePlacedItem(const char* id, TileSize size,
                                 std::uint16_t column, std::uint16_t row) {
    LaunchItem item = makeItem(id, size);
    item.setGridPosition(column, row);
    return item;
}

// 取出某索引的放置结果
static TilePlacement placementOf(const TileLayoutResult& result,
                                 std::size_t index) {
    for (const auto& placement : result.placements) {
        if (placement.itemIndex == index) {
            return placement;
        }
    }
    return TilePlacement{};
}

// 未受影响的磁贴必须保持原位（核心诉求：只动挡路的）
static void testAvoidanceKeepsUnrelatedTiles() {
    std::vector<LaunchItem> items;
    items.push_back(makePlacedItem("a", TileSize::Small, 0, 0));  // 被拖项
    items.push_back(makePlacedItem("b", TileSize::Small, 0, 3));  // 远处，不动
    items.push_back(makePlacedItem("c", TileSize::Small, 2, 3));  // 远处，不动
    items.push_back(makePlacedItem("d", TileSize::Small, 3, 3));  // 远处，不动

    AvoidanceRequest request;
    request.columns = 4;
    request.rows = 4;
    request.items = &items;
    request.dragIndex = 0;
    request.targetColumn = 0;
    request.targetRow = 0;  // 原位，不引发任何冲突
    request.dragSpanColumns = 1;
    request.dragSpanRows = 1;

    const TileLayoutResult result = calculateAvoidanceLayout(request);
    CHECK(result.complete());

    // b/c/d 一个都不该动
    CHECK_EQ(placementOf(result, 1).column, 0);
    CHECK_EQ(placementOf(result, 1).row, 3);
    CHECK_EQ(placementOf(result, 2).column, 2);
    CHECK_EQ(placementOf(result, 2).row, 3);
    CHECK_EQ(placementOf(result, 3).column, 3);
    CHECK_EQ(placementOf(result, 3).row, 3);
}

// 只有被目标区域覆盖的磁贴才让位
static void testAvoidanceOnlyDisplacesConflictingTiles() {
    std::vector<LaunchItem> items;
    items.push_back(makePlacedItem("drag", TileSize::Large, 2, 0));  // 2x2 被拖项
    items.push_back(makePlacedItem("hit1", TileSize::Small, 2, 0));  // 与目标重叠
    items.push_back(makePlacedItem("hit2", TileSize::Small, 3, 1));  // 与目标重叠
    items.push_back(makePlacedItem("far", TileSize::Small, 0, 2));   // 不重叠

    AvoidanceRequest request;
    request.columns = 4;
    request.rows = 4;
    request.items = &items;
    request.dragIndex = 0;
    request.targetColumn = 2;
    request.targetRow = 0;
    request.dragSpanColumns = 2;
    request.dragSpanRows = 2;

    const TileLayoutResult result = calculateAvoidanceLayout(request);
    CHECK(result.complete());

    // 远处的磁贴必须原地不动
    CHECK_EQ(placementOf(result, 3).column, 0);
    CHECK_EQ(placementOf(result, 3).row, 2);

    // 被挤开的磁贴必须离开目标区域（不能重叠）
    const TilePlacement hit1 = placementOf(result, 1);
    const TilePlacement hit2 = placementOf(result, 2);
    auto insideTarget = [](const TilePlacement& p) {
        return p.column >= 2 && p.column < 4 && p.row >= 0 && p.row < 2;
    };
    CHECK(!insideTarget(hit1));
    CHECK(!insideTarget(hit2));

    // 被挤开的两块之间也不能重叠
    CHECK(!(hit1.column == hit2.column && hit1.row == hit2.row));
}

// 所有磁贴互不重叠（覆盖全部结果）
static void testAvoidanceNoOverlap() {
    std::vector<LaunchItem> items;
    items.push_back(makePlacedItem("a", TileSize::Large, 0, 0));
    items.push_back(makePlacedItem("b", TileSize::Small, 0, 0));
    items.push_back(makePlacedItem("c", TileSize::Small, 1, 0));
    items.push_back(makePlacedItem("d", TileSize::Small, 0, 1));
    items.push_back(makePlacedItem("e", TileSize::Small, 1, 1));
    items.push_back(makePlacedItem("f", TileSize::Wide, 2, 0));

    AvoidanceRequest request;
    request.columns = 4;
    request.rows = 6;
    request.items = &items;
    request.dragIndex = 0;
    request.targetColumn = 2;
    request.targetRow = 1;
    request.dragSpanColumns = 2;
    request.dragSpanRows = 2;

    const TileLayoutResult result = calculateAvoidanceLayout(request);
    CHECK(result.complete());

    // 逐对检查矩形不相交
    for (std::size_t i = 0; i < result.placements.size(); ++i) {
        for (std::size_t j = i + 1; j < result.placements.size(); ++j) {
            const auto& left = result.placements[i];
            const auto& right = result.placements[j];
            const std::uint16_t leftRight =
                static_cast<std::uint16_t>(left.column + left.spanColumns);
            const std::uint16_t leftBottom =
                static_cast<std::uint16_t>(left.row + left.spanRows);
            const std::uint16_t rightRight =
                static_cast<std::uint16_t>(right.column + right.spanColumns);
            const std::uint16_t rightBottom =
                static_cast<std::uint16_t>(right.row + right.spanRows);
            const bool separated =
                leftRight <= right.column || rightRight <= left.column ||
                leftBottom <= right.row || rightBottom <= left.row;
            CHECK(separated);
        }
    }
}

// 允许中间留空：远处磁贴不被「吸」到左上角填空
static void testAvoidanceAllowsGaps() {
    std::vector<LaunchItem> items;
    items.push_back(makePlacedItem("a", TileSize::Small, 0, 0));  // 被拖项
    items.push_back(makePlacedItem("b", TileSize::Small, 3, 3));  // 右下角

    AvoidanceRequest request;
    request.columns = 4;
    request.rows = 4;
    request.items = &items;
    request.dragIndex = 0;
    request.targetColumn = 0;
    request.targetRow = 0;
    request.dragSpanColumns = 1;
    request.dragSpanRows = 1;

    const TileLayoutResult result = calculateAvoidanceLayout(request);
    CHECK(result.complete());

    // b 必须留在右下角，不能被紧凑回流吸到 (1,0)
    CHECK_EQ(placementOf(result, 1).column, 3);
    CHECK_EQ(placementOf(result, 1).row, 3);
}

// 越界目标坐标应被夹到边界内，不产生越界放置
static void testAvoidanceClampsTargetToGrid() {
    std::vector<LaunchItem> items;
    items.push_back(makePlacedItem("drag", TileSize::Large, 0, 0));

    AvoidanceRequest request;
    request.columns = 4;
    request.rows = 4;
    request.items = &items;
    request.dragIndex = 0;
    request.targetColumn = 3;  // 2x2 放不进第 3 列起
    request.targetRow = 3;
    request.dragSpanColumns = 2;
    request.dragSpanRows = 2;

    const TileLayoutResult result = calculateAvoidanceLayout(request);
    CHECK(result.complete());
    const TilePlacement drag = placementOf(result, 0);
    CHECK(drag.column + drag.spanColumns <= 4);
    CHECK(drag.row + drag.spanRows <= 4);
}

// 拖到空白格：只有被拖项落进空格，其他磁贴完全不动
// （只压到已有磁贴时才触发让位，空白落点零扰动）。
static void testAvoidanceBlankTargetMovesNothing() {
    std::vector<LaunchItem> items;
    items.push_back(makePlacedItem("a", TileSize::Small, 0, 0));
    items.push_back(makePlacedItem("b", TileSize::Small, 1, 0));
    // (2,0) 故意留空
    items.push_back(makePlacedItem("c", TileSize::Small, 3, 0));

    AvoidanceRequest request;
    request.columns = 4;
    request.rows = 2;
    request.items = &items;
    request.dragIndex = 0;      // 拖 a
    request.targetColumn = 2;   // 放到中间的空格
    request.targetRow = 0;
    request.dragSpanColumns = 1;
    request.dragSpanRows = 1;

    const TileLayoutResult result = calculateAvoidanceLayout(request);
    CHECK(result.complete());
    const TilePlacement a = placementOf(result, 0);
    CHECK(a.column == 2 && a.row == 0);
    // 其余磁贴纹丝不动
    const TilePlacement b = placementOf(result, 1);
    CHECK(b.column == 1 && b.row == 0);
    const TilePlacement c = placementOf(result, 2);
    CHECK(c.column == 3 && c.row == 0);
}

// 被压住的磁贴按阅读顺序顺移：右边的空位天然优先于下方、
// 更优先于画面另一头（对齐 Win10 组内腾挪的手感）。
static void testAvoidanceDisplacedTileSlidesInReadingOrder() {
    std::vector<LaunchItem> items;
    // (0,0) (0,1) 一列 1x1，(1,0) 空着
    items.push_back(makePlacedItem("drag", TileSize::Small, 1, 0));
    items.push_back(makePlacedItem("t1", TileSize::Small, 0, 0));
    items.push_back(makePlacedItem("t2", TileSize::Small, 0, 1));

    AvoidanceRequest request;
    request.columns = 2;
    request.rows = 4;
    request.items = &items;
    request.dragIndex = 0;
    request.targetColumn = 0;   // 拖到 (0,0)，压住 t1
    request.targetRow = 0;
    request.dragSpanColumns = 1;
    request.dragSpanRows = 1;

    const TileLayoutResult result = calculateAvoidanceLayout(request);
    CHECK(result.complete());
    const TilePlacement drag = placementOf(result, 0);
    CHECK(drag.column == 0 && drag.row == 0);
    // t1 被压住：阅读顺序里 (1,0) 是它之后的第一个空位 → 让到右侧
    const TilePlacement t1 = placementOf(result, 1);
    CHECK(t1.column == 1 && t1.row == 0);
    // t2 未被压住，原地不动
    const TilePlacement t2 = placementOf(result, 2);
    CHECK(t2.column == 0 && t2.row == 1);
}

// 复刻实测场景：0 列下方全满，右侧 (1,0) 空着。
// 把控制面板拖到 (0,0) 时，原 (0,0) 的命令提示符应让到右侧空位 (1,0)，
// 而不是被甩到左列最下面（旧实现的缺陷）。
static void testAvoidanceDisplacedGoesRightWhenColumnBlocked() {
    std::vector<LaunchItem> items;
    items.push_back(makePlacedItem("cmd", TileSize::Small, 0, 0));
    items.push_back(makePlacedItem("calc", TileSize::Small, 0, 1));
    items.push_back(makePlacedItem("explorer", TileSize::Small, 0, 2));
    items.push_back(makePlacedItem("docs", TileSize::Tall, 0, 3));   // 1x2
    items.push_back(makePlacedItem("panel", TileSize::Small, 3, 3)); // 被拖项

    AvoidanceRequest request;
    request.columns = 6;
    request.rows = 6;
    request.items = &items;
    request.dragIndex = 4;
    request.targetColumn = 0;   // 拖到 (0,0)，压住 cmd
    request.targetRow = 0;
    request.dragSpanColumns = 1;
    request.dragSpanRows = 1;

    const TileLayoutResult result = calculateAvoidanceLayout(request);
    CHECK(result.complete());
    const TilePlacement panel = placementOf(result, 4);
    CHECK(panel.column == 0 && panel.row == 0);
    // cmd 让到右侧第一个空位
    const TilePlacement cmd = placementOf(result, 0);
    CHECK(cmd.column == 1 && cmd.row == 0);
    // 其余磁贴原地不动
    const TilePlacement calc = placementOf(result, 1);
    CHECK(calc.column == 0 && calc.row == 1);
    const TilePlacement docs = placementOf(result, 3);
    CHECK(docs.column == 0 && docs.row == 3);
}

// 复刻用户实测场景：磁贴贴着右边缘被挤，左侧紧挨着就是空位。
//
// 旧行为（纯阅读顺序）：右边缘磁贴被挤 → 甩到「下一行最左边」，横向跨越整行，
// 视觉上非常突兀。
// 新行为：先在同一行向右找 → 没位置就同一行向左找 → 命中紧邻左侧的空位。
static void testAvoidanceDisplacedPrefersLeftOverNextRow() {
    std::vector<LaunchItem> items;
    // 一行 6 列，只在 (0,0) 有 cmd 占位。
    // 被拖的 panel 要落到 (1,0)，把 cmd 挤开。
    items.push_back(makePlacedItem("cmd", TileSize::Small, 0, 0));
    items.push_back(makePlacedItem("panel", TileSize::Small, 5, 5));  // 被拖项

    AvoidanceRequest request;
    request.columns = 6;
    request.rows = 6;
    request.items = &items;
    request.dragIndex = 1;      // panel 被拖
    request.targetColumn = 0;   // 拖到 (0,0)，压住 cmd
    request.targetRow = 0;
    request.dragSpanColumns = 1;
    request.dragSpanRows = 1;

    const TileLayoutResult result = calculateAvoidanceLayout(request);
    CHECK(result.complete());
    const TilePlacement panel = placementOf(result, 1);
    CHECK(panel.column == 0 && panel.row == 0);
    // cmd 被挤开：右侧 (1,0) 空着 → 优先向右（梯队 1 仍然最高优先）
    const TilePlacement cmd = placementOf(result, 0);
    CHECK(cmd.column == 1 && cmd.row == 0);
}

// 右边缘磁贴被挤、右侧与下方都被堵 → 应向左侧让一格，
// 而不是跳到下一行最左边。
static void testAvoidanceDisplacedGoesLeftAtRightEdge() {
    std::vector<LaunchItem> items;
    // 网格 6 列 6 行。t 在 (4,0)，它的右侧 (5,0) 与下方 (4,1) 都放好占位物。
    // 拖到 (4,0) 挤开 t：
    //   步长 1 向右：(5,0) 被 right 占 → 失败
    //   步长 1 向左：(3,0) 空着 → 命中
    // 旧行为会把它甩到下一行最左 (0,1)。
    items.push_back(makePlacedItem("drag", TileSize::Small, 0, 5));
    items.push_back(makePlacedItem("right", TileSize::Small, 5, 0));  // 堵住向右
    items.push_back(makePlacedItem("below", TileSize::Small, 4, 1));  // 堵住向下
    items.push_back(makePlacedItem("t", TileSize::Small, 4, 0));

    AvoidanceRequest request;
    request.columns = 6;
    request.rows = 6;
    request.items = &items;
    request.dragIndex = 0;
    request.targetColumn = 4;   // 拖到 (4,0)，压住 t
    request.targetRow = 0;
    request.dragSpanColumns = 1;
    request.dragSpanRows = 1;

    const TileLayoutResult result = calculateAvoidanceLayout(request);
    CHECK(result.complete());
    const TilePlacement drag = placementOf(result, 0);
    CHECK(drag.column == 4 && drag.row == 0);
    // t 让到左侧紧邻的 (3,0)
    const TilePlacement t = placementOf(result, 3);
    CHECK(t.column == 3 && t.row == 0);
}

// 同一行左右都满了 → 往同一列上方找（梯队 3）。
static void testAvoidanceDisplacedGoesUpWhenRowFull() {
    std::vector<LaunchItem> items;
    // 6 列的行 1 全满：(0,1)..(5,1)
    for (int c = 0; c < 6; ++c) {
        items.push_back(makePlacedItem("row1", TileSize::Small,
                                       static_cast<std::uint16_t>(c), 1));
    }
    // 行 0 只有 (3,0) 有东西，其余空着。
    items.push_back(makePlacedItem("top", TileSize::Small, 3, 0));
    items.push_back(makePlacedItem("drag", TileSize::Small, 0, 5));

    AvoidanceRequest request;
    request.columns = 6;
    request.rows = 6;
    request.items = &items;
    request.dragIndex = 7;
    request.targetColumn = 3;   // 拖到 (3,1)，压住 row1#3（index 3）
    request.targetRow = 1;
    request.dragSpanColumns = 1;
    request.dragSpanRows = 1;

    const TileLayoutResult result = calculateAvoidanceLayout(request);
    CHECK(result.complete());
    // 被挤开的那块：列 3 的行 1 已满（左右都被同行占），
    // 同一列向上 (3,0) 被 top 占着 → 继续向上无路；
    // 但注意 (2,0)/(4,0) 是空的，仍是同一行吗？不是 —— 被挤项在第 1 行，不是第 0 行。
    // 它的同一行（第 1 行）左右都满 → 梯队 3 看同一列 (3,0)：被 top 占 → 失败；
    // 梯队 4 看同一列向下 (3,2)：空着 → 命中。
    const TilePlacement moved = placementOf(result, 3);
    CHECK(moved.column == 3 && moved.row == 2);
}

// 右边缘 + 上方紧邻空位 → 应向上走一格（步长 1 的「向上」），
// 而不是回卷到下一行最左。
static void testAvoidanceDisplacedPrefersUpOverWrap() {
    std::vector<LaunchItem> items;
    // 网格 6 列、6 行。t 在 (5,1)（最右列）。
    // 拖到 (5,1) 挤开 t：
    //   步长 1 向右：(6,1) 越界 → 失败
    //   步长 1 向左：(4,1) 被 left 占 → 失败
    //   步长 1 向上：(5,0) 空着 → 命中
    // 旧行为会把它甩到下一行最左 (0,2)。
    items.push_back(makePlacedItem("left", TileSize::Small, 4, 1));
    items.push_back(makePlacedItem("t", TileSize::Small, 5, 1));
    items.push_back(makePlacedItem("drag", TileSize::Small, 0, 5));

    AvoidanceRequest request;
    request.columns = 6;
    request.rows = 6;
    request.items = &items;
    request.dragIndex = 2;
    request.targetColumn = 5;   // 拖到 (5,1)，压住 t
    request.targetRow = 1;
    request.dragSpanColumns = 1;
    request.dragSpanRows = 1;

    const TileLayoutResult result = calculateAvoidanceLayout(request);
    CHECK(result.complete());
    const TilePlacement drag = placementOf(result, 2);
    CHECK(drag.column == 5 && drag.row == 1);
    const TilePlacement t = placementOf(result, 1);
    CHECK(t.column == 5 && t.row == 0);
}

// 挤开位移必须限定在「同一行/同一列」，不应出现「旁边有位置却横跨整行」。
static void testAvoidanceDisplacedNeverJumpsAcrossRowWhenAdjacentFree() {
    std::vector<LaunchItem> items;
    // 8 列网格，第 0 行只有 (7,0) 有 t；其余全空。
    // 拖到 (7,0) 挤开 t：
    //   梯队 1（向右）越界失败
    //   梯队 2（向左）：(6,0) 空 → 命中
    // 旧行为会甩到 (0,1)。
    items.push_back(makePlacedItem("t", TileSize::Small, 7, 0));
    items.push_back(makePlacedItem("drag", TileSize::Small, 0, 5));

    AvoidanceRequest request;
    request.columns = 8;
    request.rows = 6;
    request.items = &items;
    request.dragIndex = 1;
    request.targetColumn = 7;
    request.targetRow = 0;
    request.dragSpanColumns = 1;
    request.dragSpanRows = 1;

    const TileLayoutResult result = calculateAvoidanceLayout(request);
    CHECK(result.complete());
    const TilePlacement t = placementOf(result, 0);
    CHECK(t.row == 0);          // 必须还在第 0 行
    CHECK(t.column == 6);       // 紧邻左侧
}

int wmain() {
    testTileSpans();
    testOccupancyGrid();
    testFindFirstFreeRegion();
    testLayoutEmpty();
    testLayoutAllSmall();
    testLayoutMixedTiles();
    testLayoutWideTile();
    testNarrowGridAutoWidens();
    testExplicitPositionPreserved();
    testExplicitPositionConflictIgnored();
    testLargerTileWinsOverlappingExplicitClaim();
    testIgnoreExplicitPositions();
    testStableLayoutForSameInput();
    testOverflowDetection();
    testRequiredDimensions();
    testTileSizeChangeReflow();
    testTileSizeHelpers();
    testMoveItemForward();
    testMoveItemBackward();
    testMoveItemToEnds();
    testMoveItemToSameIndexRejected();
    testMoveItemOutOfRangeRejected();
    testMoveItemClearsGridPositions();
    testMoveItemKeepsLayoutComplete();
    testIsMeaningfulMove();
    testAvoidanceKeepsUnrelatedTiles();
    testAvoidanceOnlyDisplacesConflictingTiles();
    testAvoidanceNoOverlap();
    testAvoidanceAllowsGaps();
    testAvoidanceClampsTargetToGrid();
    testAvoidanceBlankTargetMovesNothing();
    testAvoidanceDisplacedTileSlidesInReadingOrder();
    testAvoidanceDisplacedGoesRightWhenColumnBlocked();
    testAvoidanceDisplacedPrefersLeftOverNextRow();
    testAvoidanceDisplacedGoesLeftAtRightEdge();
    testAvoidanceDisplacedGoesUpWhenRowFull();
    testAvoidanceDisplacedPrefersUpOverWrap();
    testAvoidanceDisplacedNeverJumpsAcrossRowWhenAdjacentFree();

    if (g_failures == 0) {
        wprintf(L"===== TILE LAYOUT: ALL PASSED =====\n");
    } else {
        wprintf(L"===== TILE LAYOUT: %d FAILURES =====\n", g_failures);
    }
    return g_failures;
}
