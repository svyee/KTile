#include "core/plugin_layout.h"

#include <algorithm>
#include <cmath>

#include "core/plugin_binding.h"

namespace ktile {
namespace core {
namespace {

constexpr float kPadding = 5.0F;
constexpr float kGap = 3.0F;
constexpr float kMeterHeight = 5.0F;
// 单行文本占用的高度 = 字号 × 该系数。不能取 1.15 / 1.25 这类恰好等于
// 字母高度的值：中文行盒约 1.3em，绘制端又垂直居中，行盒高于矩形会上下
// 裁掉半截字。
constexpr float kTextLineHeight = 1.35F;
// 字号下限，须与绘制端（夹到 7）一致，否则布局算高与实际绘制不符仍会裁切。
constexpr float kMinFontSize = 7.0F;
constexpr int kMaxDepth = 4;
constexpr int kMaxArrayRepeat = 4;

using Locals = std::vector<std::pair<std::string, const PluginValue*>>;

float clampFloat(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

// 按宽度截断，追加省略号。measure 为空时退化为按字符数估算。
std::string fitText(const std::string& text,
                    float maxWidth,
                    float fontSize,
                    const PluginTextMeasure& measure) {
    if (text.empty() || maxWidth <= 0.0F) {
        return text;
    }
    const float width = measure ? measure(text, fontSize) : 0.0F;
    if (!measure || width <= maxWidth) {
        return text;
    }

    const std::string ellipsis = "\xE2\x80\xA6";  // U+2026
    std::string candidate = text;
    while (!candidate.empty()) {
        // 按 UTF-8 码点回退：先去掉最后一个字节，再把剩余的续字节剥掉，
        // 否则截断到半个汉字会画出乱码。
        candidate.resize(candidate.size() - 1);
        while (!candidate.empty() &&
               (static_cast<unsigned char>(candidate.back()) & 0xC0U) == 0x80U) {
            candidate.resize(candidate.size() - 1);
        }
        const std::string attempt = candidate + ellipsis;
        if (measure(attempt, fontSize) <= maxWidth) {
            return attempt;
        }
    }
    return ellipsis;
}

float measureWidth(const std::string& text, float fontSize,
                   const PluginTextMeasure& measure) {
    if (!measure || text.empty()) {
        return 0.0F;
    }
    return measure(text, fontSize);
}

void appendText(PluginDrawList* list,
                const std::string& text,
                PluginTextRole role,
                float fontSize,
                float left,
                float top,
                float width,
                float height,
                const PluginTextMeasure& measure) {
    if (text.empty() || width <= 0.0F || height <= 0.0F) {
        return;
    }
    const std::string fitted = fitText(text, width, fontSize, measure);
    if (fitted.empty()) {
        return;
    }

    PluginDrawCommand command;
    command.kind = PluginDrawKind::Text;
    command.left = left;
    command.top = top;
    command.right = left + width;
    command.bottom = top + height;
    command.role = role;
    command.fontSize = fontSize;
    command.centered = true;
    command.text = fitted;
    list->commands.push_back(std::move(command));
}

void paintLeaf(const PluginManifest& manifest,
               const PluginBindingContext& context,
               const PluginLayoutNode& node,
               float left,
               float top,
               float width,
               float height,
               PluginDrawList* list,
               const PluginTextMeasure& measure) {
    if (width < 4.0F || height < 4.0F) {
        return;
    }

    const PluginSlotsSpec& slots = node.slots;

    // ---- 解析全部槽位文本（空模板/空值都当不存在）----
    const std::string mainText = pluginResolveTemplate(slots.main, context);
    const std::string subText = pluginResolveTemplate(slots.sub, context);
    const std::string footerText = pluginResolveTemplate(slots.footer, context);

    std::string iconId;
    const std::string rawIcon = pluginResolveTemplate(slots.icon, context);
    if (!rawIcon.empty()) {
        if (const std::string* mapped = manifest.iconForRawValue(rawIcon)) {
            iconId = *mapped;
        } else {
            iconId = rawIcon;
        }
    }
    // manifest.icon 只用于无数据/加载中的占位视图；叶子没写 icon 槽就是
    // 明确不想画图标，不能偷偷补一个。

    std::string meterLabel;
    float meterFraction = -1.0F;
    std::uint32_t meterColor = 0;
    if (slots.hasMeter) {
        meterLabel = pluginResolveTemplate(slots.meter.label, context);
        const double maximum = slots.meter.maximum > 0.0 ? slots.meter.maximum : 100.0;
        const double value =
            pluginResolveNumber(slots.meter.value, context, 0.0);
        meterFraction = static_cast<float>(clampFloat(
            static_cast<float>(value / maximum), 0.0F, 1.0F));
        if (!slots.meter.color.empty()) {
            meterColor = pluginParseColor(
                pluginResolveTemplate(slots.meter.color, context), 0);
        }
    }

    // ---- 字号 / 图标尺寸 ----
    // 每项同时受宽高约束：large / wide 的一列只有六十几 DIP 宽，
    // 只按高度算出的字号会明显超出列宽。
    const bool hasIcon = !iconId.empty();
    const bool hasMain = !mainText.empty();
    const bool hasSub = !subText.empty();
    const bool hasFooter = !footerText.empty();
    const bool hasMeter = meterFraction >= 0.0F;
    const bool hasMeterLabel = hasMeter && !meterLabel.empty();

    // 一组尺寸及其堆叠总高度。scale 用于内容比格子高时整体缩小（wide/large
    // 的列宽只有六十几 DIP，图标加三行文本按基准字号放不下，不缩会顶出
    // 磁贴）；withFooter / withSub 用于怎么缩都放不下时逐步丢弃信息量最低
    // 的行。字号有下限，光靠缩放可能永远差一点，必须允许少显示一行。
    struct BlockMetrics {
        float iconSize = 0.0F;
        float mainSize = 0.0F;
        float subSize = 0.0F;
        float footerSize = 0.0F;
        float meterLabelSize = 0.0F;
        float gap = 0.0F;
        float total = 0.0F;
    };
    auto measureBlock = [&](const float scale, const bool withFooter,
                            const bool withSub) {
        BlockMetrics m;
        m.gap = std::max(2.0F, kGap * scale);
        m.iconSize =
            hasIcon ? clampFloat(std::min(width * 0.55F, height * 0.42F) * scale,
                                 8.0F, 44.0F)
                    : 0.0F;
        m.mainSize =
            hasMain ? clampFloat(std::min(height * 0.32F, width * 0.34F) * scale,
                                 7.5F, 26.0F)
                    : 0.0F;
        m.subSize = (withSub && hasSub)
                        ? clampFloat(m.mainSize * 0.62F, kMinFontSize, 12.0F)
                        : 0.0F;
        // 页脚与进度条说明同一视觉层级，字号一致。
        m.footerSize =
            (withFooter && hasFooter)
                ? clampFloat(m.mainSize * 0.50F, kMinFontSize, 10.0F)
                : 0.0F;
        m.meterLabelSize =
            hasMeterLabel ? clampFloat(m.mainSize * 0.50F, kMinFontSize, 10.0F)
                          : 0.0F;

        const float mainHeight = hasMain ? m.mainSize * kTextLineHeight : 0.0F;
        const float subHeight = m.subSize * kTextLineHeight;
        const float footerHeight = m.footerSize * kTextLineHeight;
        const float meterLabelHeight = m.meterLabelSize * kTextLineHeight;
        const float meterHeight =
            hasMeter ? kMeterHeight + (m.meterLabelSize > 0.0F
                                           ? meterLabelHeight + 2.0F
                                           : 0.0F)
                     : 0.0F;

        int pieces = 0;
        if (hasIcon) { ++pieces; }
        if (hasMain) { ++pieces; }
        if (subHeight > 0.0F) { ++pieces; }
        if (footerHeight > 0.0F) { ++pieces; }
        if (hasMeter) { ++pieces; }
        if (pieces == 0) {
            return m;
        }

        m.total = m.iconSize + mainHeight + subHeight + footerHeight + meterHeight;
        if (pieces > 1) {
            m.total += m.gap * static_cast<float>(pieces - 1);
        }
        return m;
    };

    // 在 [0, 1] 上二分找「塞进 height 的最大缩放系数」。不能按 height /
    // total 乘一次：各字号缩到下限后总高度不再线性下降，可能停在刚好差
    // 一点的位置，页脚要么被误丢弃、要么画到磁贴外；二分对带下限的单调
    // 函数是确定的。
    auto fitBlock = [&](const bool withFooter, const bool withSub,
                        BlockMetrics* chosen) {
        const BlockMetrics full = measureBlock(1.0F, withFooter, withSub);
        if (full.total <= height) {
            *chosen = full;
            return;
        }
        const BlockMetrics minimal = measureBlock(0.0F, withFooter, withSub);
        if (minimal.total > height) {
            // 已是最小尺寸仍放不下，交给调用方决定丢弃哪一行。
            *chosen = minimal;
            return;
        }
        float low = 0.0F;
        float high = 1.0F;
        *chosen = minimal;
        for (int step = 0; step < 12; ++step) {
            const float mid = (low + high) * 0.5F;
            const BlockMetrics candidate = measureBlock(mid, withFooter, withSub);
            if (candidate.total <= height) {
                low = mid;
                *chosen = candidate;
            } else {
                high = mid;
            }
        }
    };

    // 优先保留完整内容，实在放不下再依次丢弃页脚、副文本；
    // 宁可少显示一行，也不把内容画到磁贴外面。
    BlockMetrics metrics;
    bool showFooter = hasFooter;
    bool showSub = hasSub;
    fitBlock(showFooter, showSub, &metrics);
    if (metrics.total > height) {
        showFooter = false;
        fitBlock(showFooter, showSub, &metrics);
    }
    if (metrics.total > height) {
        showSub = false;
        fitBlock(showFooter, showSub, &metrics);
    }
    if (metrics.total <= 0.0F) {
        return;
    }

    float cursorY = top + std::max(0.0F, (height - metrics.total) * 0.5F);

    // ---- 图标 ----
    if (hasIcon) {
        PluginDrawCommand command;
        command.kind = PluginDrawKind::Icon;
        const float iconLeft = left + (width - metrics.iconSize) * 0.5F;
        command.left = iconLeft;
        command.top = cursorY;
        command.right = iconLeft + metrics.iconSize;
        command.bottom = cursorY + metrics.iconSize;
        command.icon = iconId;
        // 数据驱动图标可能不在 iconMap，或作者删掉了某条映射，
        // 兜底到 manifest.icon，避免磁贴留下没有图形的空槽。
        command.iconFallback = manifest.icon;
        list->commands.push_back(std::move(command));
        cursorY += metrics.iconSize + metrics.gap;
    }

    // ---- 主文本 ----
    if (hasMain) {
        appendText(list, mainText, PluginTextRole::Main, metrics.mainSize,
                   left, cursorY, width, metrics.mainSize * kTextLineHeight,
                   measure);
        cursorY += metrics.mainSize * kTextLineHeight + metrics.gap;
    }

    // ---- 副文本 ----（放不下时 metrics 已把它压成 0）
    if (metrics.subSize > 0.0F) {
        appendText(list, subText, PluginTextRole::Sub, metrics.subSize,
                   left, cursorY, width, metrics.subSize * kTextLineHeight,
                   measure);
        cursorY += metrics.subSize * kTextLineHeight + metrics.gap;
    }

    // ---- 页脚 ----（放不下时 metrics 已把它压成 0，这里自然不会画）
    if (metrics.footerSize > 0.0F) {
        appendText(list, footerText, PluginTextRole::Footer, metrics.footerSize,
                   left, cursorY, width, metrics.footerSize * kTextLineHeight,
                   measure);
        cursorY += metrics.footerSize * kTextLineHeight + metrics.gap;
    }

    // ---- 进度条 ----
    if (hasMeter) {
        if (metrics.meterLabelSize > 0.0F) {
            appendText(list, meterLabel, PluginTextRole::Footer,
                       metrics.meterLabelSize, left, cursorY, width,
                       metrics.meterLabelSize * kTextLineHeight, measure);
            cursorY += metrics.meterLabelSize * kTextLineHeight + 2.0F;
        }

        PluginDrawCommand command;
        command.kind = PluginDrawKind::Meter;
        command.left = left;
        command.top = cursorY;
        command.right = left + width;
        command.bottom = cursorY + kMeterHeight;
        command.fraction = meterFraction;
        command.color = meterColor;
        list->commands.push_back(std::move(command));
    }
}

void paintNode(const PluginManifest& manifest,
              const PluginBindingContext& context,
              const PluginLayoutNode& node,
              float left,
              float top,
              float width,
              float height,
              int depth,
              PluginDrawList* list,
              const PluginTextMeasure& measure) {
    if (depth > kMaxDepth || width < 2.0F || height < 2.0F) {
        return;
    }

    if (node.kind == PluginLayoutNode::Kind::Leaf) {
        paintLeaf(manifest, context, node, left, top, width, height, list, measure);
        return;
    }

    // ---- 收集子节点（含 repeat 展开）----
    struct Child {
        const PluginLayoutNode* node;
        Locals locals;
    };
    std::vector<Child> children;

    if (node.hasRepeat) {
        const PluginValue* source = pluginResolvePath(context, node.repeatSource);
        if (source != nullptr && source->isArray()) {
            const int limit = std::min<int>(node.repeatMax, kMaxArrayRepeat);
            for (std::size_t index = 0; index < source->elements.size(); ++index) {
                if (static_cast<int>(index) >= limit) {
                    break;
                }
                Locals locals = context.locals != nullptr ? *context.locals : Locals();
                if (!node.repeatMap.empty()) {
                    locals.emplace_back(node.repeatMap, &source->elements[index]);
                }
                for (const PluginLayoutNode& child : node.children) {
                    children.push_back(Child{&child, locals});
                }
            }
        }
    } else {
        for (const PluginLayoutNode& child : node.children) {
            Child entry;
            entry.node = &child;
            if (context.locals != nullptr) {
                entry.locals = *context.locals;
            }
            children.push_back(std::move(entry));
        }
    }

    if (children.empty()) {
        return;
    }

    const bool horizontal = node.horizontal;
    const float available = std::max(0.0F,
        (horizontal ? width : height) - kGap * static_cast<float>(children.size() - 1));

    // 权重：<=0 视为 1
    float totalWeight = 0.0F;
    for (const Child& child : children) {
        totalWeight += child.node->weight > 0 ? static_cast<float>(child.node->weight) : 1.0F;
    }
    if (totalWeight <= 0.0F) {
        totalWeight = static_cast<float>(children.size());
    }

    float cursor = horizontal ? left : top;
    for (const Child& child : children) {
        const float weight = child.node->weight > 0 ? static_cast<float>(child.node->weight) : 1.0F;
        const float extent = available * weight / totalWeight;

        PluginBindingContext childContext = context;
        childContext.locals = child.locals.empty() ? nullptr : &child.locals;

        if (horizontal) {
            paintNode(manifest, childContext, *child.node, cursor, top, extent, height,
                      depth + 1, list, measure);
            cursor += extent + kGap;
        } else {
            paintNode(manifest, childContext, *child.node, left, cursor, width, extent,
                      depth + 1, list, measure);
            cursor += extent + kGap;
        }
    }
}

}  // namespace

PluginDrawList buildPluginDrawList(const PluginManifest& manifest,
                                   const PluginValue& data,
                                   const PluginLayoutNode& layout,
                                   float left,
                                   float top,
                                   float width,
                                   float height,
                                   const PluginTextMeasure& measure) {
    PluginDrawList list;
    if (width <= 0.0F || height <= 0.0F) {
        return list;
    }

    // 内容区内缩，避免文字贴边。
    const float innerLeft = left + kPadding;
    const float innerTop = top + kPadding;
    const float innerWidth = std::max(0.0F, width - kPadding * 2.0F);
    const float innerHeight = std::max(0.0F, height - kPadding * 2.0F);

    PluginBindingContext context;
    context.root = &data;
    context.selectors = &manifest.selectors;

    paintNode(manifest, context, layout, innerLeft, innerTop, innerWidth, innerHeight,
              0, &list, measure);
    return list;
}

}  // namespace core
}  // namespace ktile
