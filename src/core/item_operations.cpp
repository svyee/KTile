#include "core/item_operations.h"

#include "core/text_normalize.h"

#include <algorithm>
#include <cctype>

namespace ktile {
namespace core {
namespace {

// 取 Tab 的 items 引用。索引非法返回 nullptr。
std::vector<LaunchItem>* itemsAt(ItemsDocument& document,
                                 const std::size_t tabIndex) {
    if (tabIndex >= document.tabs.size()) {
        return nullptr;
    }
    return &document.tabs[tabIndex].items;
}

// id 是否已被任意页面的磁贴占用。id 是跨会话的稳定标识（图标缓存键、
// 启动统计），唯一性必须是全文档级的，不能只看同一页面。
bool documentHasItemId(const ItemsDocument& document, const std::string& id) {
    if (id.empty()) {
        return true;  // 空 id 视为已占用，直接拒绝
    }
    for (const Tab& tab : document.tabs) {
        for (const LaunchItem& item : tab.items) {
            if (item.id == id) {
                return true;
            }
        }
    }
    return false;
}

// 把参数逐条规范化：裁空白、丢空行、条数封顶。
// addItem 与 editItem 共用，避免两条入口规则漂移。
std::vector<std::string> normalizeArguments(
    const std::vector<std::string>& raw) {
    constexpr std::size_t kMaximumArguments = 256;
    std::vector<std::string> arguments;
    arguments.reserve(raw.size());
    for (const std::string& line : raw) {
        if (arguments.size() >= kMaximumArguments) {
            break;
        }
        std::string cleaned =
            truncateUtf8(trimAscii(line), kMaximumTargetLength);
        if (!cleaned.empty()) {
            arguments.push_back(std::move(cleaned));
        }
    }
    return arguments;
}

// 可选字段：空白即「不指定」，等价于清空。
std::optional<std::string> normalizeOptional(const std::string& raw) {
    const std::string value =
        truncateUtf8(trimAscii(raw), kMaximumTargetLength);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::string normalizeItemName(const std::string& rawName) {
    return truncateUtf8(trimAscii(rawName), kMaximumNameLength);
}

bool isUsableItemName(const std::string& rawName) {
    return !normalizeItemName(rawName).empty();
}

bool setItemTileSize(ItemsDocument& document,
                     const std::size_t tabIndex,
                     const std::size_t itemIndex,
                     const TileSize size) {
    auto* items = itemsAt(document, tabIndex);
    if (items == nullptr || itemIndex >= items->size()) {
        return false;
    }

    LaunchItem& item = (*items)[itemIndex];
    if (item.tileSize == size) {
        return false;
    }

    item.tileSize = size;
    // 尺寸变了占用格数也变，清坐标让布局引擎重新安置，避免与邻居重叠。
    item.clearGridPosition();
    return true;
}

bool renameItem(ItemsDocument& document,
                const std::size_t tabIndex,
                const std::size_t itemIndex,
                const std::string& newName) {
    auto* items = itemsAt(document, tabIndex);
    if (items == nullptr || itemIndex >= items->size()) {
        return false;
    }

    const std::string normalized = normalizeItemName(newName);
    if (normalized.empty()) {
        return false;  // 空名字不接受，保持原名
    }

    LaunchItem& item = (*items)[itemIndex];
    if (item.name == normalized) {
        return false;
    }

    item.name = normalized;
    return true;
}

bool removeItem(ItemsDocument& document,
                const std::size_t tabIndex,
                const std::size_t itemIndex) {
    auto* items = itemsAt(document, tabIndex);
    if (items == nullptr || itemIndex >= items->size()) {
        return false;
    }

    items->erase(items->begin() + static_cast<std::ptrdiff_t>(itemIndex));
    return true;
}

bool addItem(ItemsDocument& document,
             const std::size_t tabIndex,
             LaunchItem item,
             std::size_t* outIndex) {
    auto* items = itemsAt(document, tabIndex);
    if (items == nullptr) {
        return false;
    }

    // 目标必须有内容，与 editItem 同一判据
    const std::string target =
        truncateUtf8(trimAscii(item.target), kMaximumTargetLength);
    if (target.empty()) {
        return false;
    }

    const std::string id = truncateUtf8(trimAscii(item.id), kMaximumIdLength);
    if (documentHasItemId(document, id)) {
        return false;
    }

    item.id = id;
    item.name = normalizeItemName(item.name);
    item.target = target;
    item.arguments = normalizeArguments(item.arguments);
    item.workingDirectory = normalizeOptional(item.workingDirectory.value_or(""));
    item.icon = normalizeOptional(item.icon.value_or(""));

    // 新磁贴不带坐标：外部传入的坐标多来自复制粘贴或旧数据，在本机网格
    // 大概率与已有磁贴重叠，交给布局引擎放到第一个空位。
    item.clearGridPosition();

    items->push_back(std::move(item));
    if (outIndex != nullptr) {
        *outIndex = items->size() - 1;
    }
    return true;
}

bool editItem(ItemsDocument& document,
              const std::size_t tabIndex,
              const std::size_t itemIndex,
              const ItemEditFields& fields) {
    auto* items = itemsAt(document, tabIndex);
    if (items == nullptr || itemIndex >= items->size()) {
        return false;
    }

    // 目标必须有内容，没有目标的磁贴什么都启动不了。
    const std::string target = truncateUtf8(trimAscii(fields.target),
                                            kMaximumTargetLength);
    if (target.empty()) {
        return false;
    }

    // 名称允许留空（显示层回落到目标名），但非法 UTF-8 边界要处理
    const std::string name = normalizeItemName(fields.name);

    // 参数：裁空白、丢空行、条数封顶（与 addItem 共用 normalizeArguments）
    std::vector<std::string> arguments = normalizeArguments(fields.arguments);

    // 工作目录/图标：留空即「不指定」，清掉 optional
    const std::string workingDirectory =
        truncateUtf8(trimAscii(fields.workingDirectory), kMaximumTargetLength);
    const std::string icon =
        truncateUtf8(trimAscii(fields.icon), kMaximumTargetLength);

    LaunchItem& item = (*items)[itemIndex];

    // 尺寸改动单独判断：它改变占用格数，必须清坐标让布局引擎重排
    const bool tileSizeChanged =
        fields.tileSize.has_value() && *fields.tileSize != item.tileSize;
    const bool labelModeChanged =
        fields.labelMode.has_value() && *fields.labelMode != item.labelMode;
    const bool iconScaleChanged =
        fields.iconScalePercent.has_value() &&
        *fields.iconScalePercent != item.iconScalePercent;
    const bool tileColorChanged =
        fields.tileColor.has_value() && *fields.tileColor != item.tileColor;

    const bool changed =
        item.name != name ||
        item.type != fields.type ||
        item.target != target ||
        item.arguments != arguments ||
        item.runAsAdministrator != fields.runAsAdministrator ||
        tileSizeChanged ||
        labelModeChanged ||
        iconScaleChanged ||
        tileColorChanged ||
        item.workingDirectory.has_value() != !workingDirectory.empty() ||
        (item.workingDirectory.has_value() &&
         *item.workingDirectory != workingDirectory) ||
        item.icon.has_value() != !icon.empty() ||
        (item.icon.has_value() && *item.icon != icon);
    if (!changed) {
        return false;
    }

    item.name = name;
    item.type = fields.type;
    item.target = target;
    item.arguments = std::move(arguments);
    item.runAsAdministrator = fields.runAsAdministrator;
    if (labelModeChanged) {
        // 只影响这块磁贴画不画标题，位置与尺寸都不动
        item.labelMode = *fields.labelMode;
    }
    if (tileSizeChanged) {
        item.tileSize = *fields.tileSize;
        // 新尺寸可能放不进原位置，清坐标交给布局引擎重新安置。
        item.clearGridPosition();
    }
    if (workingDirectory.empty()) {
        item.workingDirectory.reset();
    } else {
        item.workingDirectory = workingDirectory;
    }
    if (icon.empty()) {
        item.icon.reset();
    } else {
        item.icon = icon;
    }
    if (fields.iconScalePercent.has_value()) {
        // 0 = 跟随全局；否则夹进 50~100（与设置窗口同一范围）
        item.iconScalePercent =
            std::clamp(*fields.iconScalePercent, 0, 100) <= 0
                ? 0
                : std::clamp(*fields.iconScalePercent, 50, 100);
    }
    if (tileColorChanged) {
        // 内层空 = 清除自定义色，回到全局默认 / 皮肤
        item.tileColor = *fields.tileColor;
    }
    return true;
}

}  // namespace core
}  // namespace ktile
