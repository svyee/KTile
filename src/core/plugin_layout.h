#pragma once

// 插件布局引擎
//
// 输入是 manifest 的受限布局树 + 插件数据，输出一串与渲染后端无关的
// 绘制指令（DIP 坐标）。这样排版逻辑可以脱离 D2D 单测；UI 只需要把
// Text/Icon/Meter 指令翻译成 D2D/DWrite 调用。
//
// 被否决的方案：
//   - 让插件直接返回像素坐标：不同 DPI / 磁贴尺寸下必然错位。
//   - 在 UI 层写一套排版：无法覆盖 large 的 3 天预报等动态结构，也不能单测。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/plugin_model.h"

namespace ktile {
namespace core {

enum class PluginDrawKind : std::uint8_t {
    Text = 0,
    Icon = 1,
    Meter = 2,
};

enum class PluginTextRole : std::uint8_t {
    Main = 0,
    Sub = 1,
    Footer = 2,
};

struct PluginDrawCommand {
    PluginDrawKind kind = PluginDrawKind::Text;

    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;

    // 0 表示使用主题默认色；Meter 的填充色由插件数据提供。
    std::uint32_t color = 0;

    PluginTextRole role = PluginTextRole::Main;
    float fontSize = 12.0F;
    bool centered = true;
    std::string text;

    // Icon
    //   icon         —— 首选图标（相对插件目录的文件路径，如 icons/sun）
    //   iconFallback —— 首选无法解析时使用的兜底（manifest.icon 可为空）
    std::string icon;
    std::string iconFallback;

    // Meter
    float fraction = 0.0F;      // 0..1
    std::string meterLabel;
    float meterLabelSize = 9.0F;

    PluginDrawCommand() = default;
};

struct PluginDrawList {
    std::vector<PluginDrawCommand> commands;

    PluginDrawList() = default;
};

// 文本宽度测量回调（UTF-8 文本 + 字号 → DIP 宽度）。
// 由 UI 注入 DWrite 实现；单测可以给一个按字符数估算的假实现。
using PluginTextMeasure = std::function<float(const std::string&, float)>;

PluginDrawList buildPluginDrawList(const PluginManifest& manifest,
                                   const PluginValue& data,
                                   const PluginLayoutNode& layout,
                                   float left,
                                   float top,
                                   float width,
                                   float height,
                                   const PluginTextMeasure& measure);

}  // namespace core
}  // namespace ktile
