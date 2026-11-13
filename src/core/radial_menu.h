// KTile - 核心数据模型：轮盘菜单
//
// 与 LaunchItem 解耦：轮盘需要 Shortcut（注入组合键）这类磁贴没有的动作。
// 扇区数与 actions.size() 恒等，空槽位用默认 RadialAction 占位，绘制和
// 命中测试跳过空槽位。仅依赖标准库；几何计算是纯函数，可单测。
//
#pragma once

#include "core/data_model.h"   // HotkeyModifier

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ktile {
namespace core {

// --- 轮盘动作类型
//
// 执行机制：Application/File/Folder/Url 用 ShellExecuteW 直接拉起/打开；
// Shortcut 用 SendInput 向前台窗口注入组合键；Builtin 走 system_actions。
enum class RadialActionType : std::uint8_t {
    Application = 0,
    File        = 1,
    Folder      = 2,
    Url         = 3,
    Shortcut    = 4,   // 注入快捷键（修饰键 + 主键）
    Builtin     = 5,   // 内置系统命令（复用 ktile: 前缀）
};

inline const char* radialActionTypeKey(RadialActionType type) noexcept {
    switch (type) {
        case RadialActionType::Application: return "application";
        case RadialActionType::File:        return "file";
        case RadialActionType::Folder:      return "folder";
        case RadialActionType::Url:         return "url";
        case RadialActionType::Shortcut:    return "shortcut";
        case RadialActionType::Builtin:     return "builtin";
    }
    return "application";
}

inline RadialActionType radialActionTypeFromKey(const std::string& key,
                                                bool* recognized = nullptr) noexcept {
    if (recognized) *recognized = true;
    if (key == "application") return RadialActionType::Application;
    if (key == "file")        return RadialActionType::File;
    if (key == "folder")      return RadialActionType::Folder;
    if (key == "url")         return RadialActionType::Url;
    if (key == "shortcut")    return RadialActionType::Shortcut;
    if (key == "builtin")     return RadialActionType::Builtin;
    if (recognized) *recognized = false;
    return RadialActionType::Application;
}

// --- 轮盘动作

struct RadialAction {
    RadialActionType type = RadialActionType::Application;
    std::string name;                          // 显示名（空时由 target 推导）
    // 预置项的文案 key（如 "action.copy"），非空时显示层优先翻译它。
    // 译名不能存进 name：写进配置就固化了，切语言或首次生成早于语言
    // 加载时会显示成 key 原文。
    std::string nameKey;
    std::string target;                        // 程序路径 / 文件 / 网址 / 内置命令 id
    std::vector<std::string> arguments;        // 启动参数（仅 Application）
    std::optional<std::string> icon;           // 自定义图标（相对 data 目录）

    // 仅 Shortcut 类型有效
    std::vector<HotkeyModifier> modifiers;     // 修饰键（Ctrl/Shift/Alt/Win）
    std::string key;                           // 主键名（如 "C", "F4", "Space"）

    RadialAction() = default;

    // 是否为空槽位（未配置动作）
    bool isEmpty() const noexcept {
        return name.empty() && nameKey.empty() && target.empty() && key.empty();
    }
};

// --- 轮盘方案

// 支持的扇区数
inline constexpr std::uint8_t kMinSectors = 4;
inline constexpr std::uint8_t kMaxSectors = 8;

struct RadialMenu {
    std::string id;
    std::string name;
    std::string nameKey;                       // 预置菜单名 key，语义同 nameKey
    std::uint8_t sectorCount = 8;              // 4 / 6 / 8
    std::vector<RadialAction> actions;         // 固定 sectorCount 个

    RadialMenu() = default;
    RadialMenu(std::string id_, std::string name_, std::uint8_t sectors = 8)
        : id(std::move(id_)), name(std::move(name_)),
          sectorCount(sectors), actions(sectors) {}
};

// --- 触发配置

enum class RadialTriggerButton : std::uint8_t {
    Right  = 0,
    Middle = 1,
    Side1  = 2,
    Side2  = 3,
};

inline const char* radialTriggerButtonKey(RadialTriggerButton button) noexcept {
    switch (button) {
        case RadialTriggerButton::Right:  return "right";
        case RadialTriggerButton::Middle: return "middle";
        case RadialTriggerButton::Side1:  return "side1";
        case RadialTriggerButton::Side2:  return "side2";
    }
    return "right";
}

inline RadialTriggerButton radialTriggerButtonFromKey(const std::string& key) noexcept {
    if (key == "middle") return RadialTriggerButton::Middle;
    if (key == "side1")  return RadialTriggerButton::Side1;
    if (key == "side2")  return RadialTriggerButton::Side2;
    return RadialTriggerButton::Right;
}

enum class RadialTriggerModifier : std::uint8_t {
    None  = 0,   // 纯按键
    Ctrl  = 1,
    Shift = 2,
    Alt   = 3,
};

inline const char* radialTriggerModifierKey(RadialTriggerModifier modifier) noexcept {
    switch (modifier) {
        case RadialTriggerModifier::None:  return "none";
        case RadialTriggerModifier::Ctrl:  return "ctrl";
        case RadialTriggerModifier::Shift: return "shift";
        case RadialTriggerModifier::Alt:   return "alt";
    }
    return "none";
}

inline RadialTriggerModifier radialTriggerModifierFromKey(const std::string& key) noexcept {
    if (key == "ctrl")  return RadialTriggerModifier::Ctrl;
    if (key == "shift") return RadialTriggerModifier::Shift;
    if (key == "alt")   return RadialTriggerModifier::Alt;
    return RadialTriggerModifier::None;
}

struct RadialTriggerConfig {
    bool enabled = true;
    RadialTriggerButton button = RadialTriggerButton::Right;
    RadialTriggerModifier modifier = RadialTriggerModifier::Ctrl;  // 默认 Ctrl+右键
    // 拖动超过此距离才呼出轮盘（物理像素）。15 而不是 8：按右键带出几个
    // 像素很常见，8 的窗口太窄，普通单击也会拽出轮盘；15 仍远小于卡片环
    // 内半径（47 DIP），正常选择不受影响。
    std::uint16_t activationThresholdPx = 15;
    std::uint16_t cancelRadiusPx = 160;        // 拖出此半径外松手 = 取消

    // 抑制键：按住其中任意一个时轮盘不弹出，事件原样放行，给 Alt+Tab
    // 这类组合键让路。空列表 = 不抑制。
    std::vector<HotkeyModifier> suppressWhileHeld;
};

// 抑制键是否与触发修饰键撞车：同一个键既必须按又必须不按，永远不触发。
// 界面上要拦住这种配置；从文件读到时剔除冲突项而不是整份打回默认。
bool suppressionConflictsTrigger(const RadialTriggerConfig& trigger) noexcept;

// 单个修饰键是否就是触发修饰键（同一个物理键）。
// 加载期逐项剔除冲突抑制键、界面保存前判整体冲突，两处共用这一条判断。
bool modifierIsTriggerModifier(HotkeyModifier held,
                               RadialTriggerModifier trigger) noexcept;

// 抑制键只有触发修饰键为「无」时才参与判断：选了修饰键，不按它本身就是
// 不触发，再叠抑制键等于同一个键必须按又必须不按。设置页灰掉抑制键与
// 钩子运行期忽略它必须走同一条规则，否则会出现看不见却生效的抑制。
bool suppressionApplies(RadialTriggerModifier trigger) noexcept;

// 外甩取消半径的可用区间（DIP）。下限 142 是卡片环外缘：更小的半径会把
// 卡片自己判成「已甩出去」，半个轮盘看起来是坏的。上限只是防呆。
inline constexpr std::uint16_t kMinCancelRadiusDip = 142;
inline constexpr std::uint16_t kMaxCancelRadiusDip = 600;

// --- 轮盘总配置

// radial.json 的 schema 版本，刻意不复用 data_model.h 的 kCurrentSchemaVersion：
// 两者结构无关，共用常量会分不清哪边推进了版本。
// 1 = 初版，2 = 多方案 + 按应用白名单，3 = 通用项 + 进程黑名单，
// 4 = 激活阈值默认值 8 → 15（见 radial_store 里那条唯一的迁移）。
inline constexpr std::uint32_t kRadialSchemaVersion = 4;

// 进程名是否命中黑名单（大小写不敏感）。抽成自由函数供钩子层用：
// 钩子在按下瞬间就要决定吞不吞事件，手里只有黑名单，没有完整 RadialConfig。
bool isProcessBlockedBy(const std::vector<std::string>& blacklist,
                        const std::string& processName) noexcept;

// 两个进程名是否相等（大小写不敏感）。界面判重用：
// 黑名单里加第二遍 "Chrome.exe" 要拦住，用户看不出大小写是同一回事。
bool processNameEquals(const std::string& left,
                       const std::string& right) noexcept;

struct RadialConfig {
    std::uint32_t schemaVersion = kRadialSchemaVersion;
    RadialTriggerConfig trigger;

    // 通用轮盘：所有应用共用这一份。按应用差异化交给 slugin 插件目录，
    // 设置界面只呈现「一份通用配置」。
    RadialMenu menu;

    // 进程黑名单：命中即不弹轮盘，事件全部放行。只比进程名、大小写不敏感，
    // 不支持通配符（`*app*.exe` 误伤面大，用户难预估）。
    std::vector<std::string> blacklist;

    // 该进程是否被拉黑。空串（拿不到进程名）不算命中：宁可多弹一次，
    // 不让功能静默失效。
    bool isProcessBlocked(const std::string& processName) const noexcept;
};

// --- 几何计算（纯函数）

// 角度（度）→ 扇区索引。0° = 正上方，顺时针递增；sectorCount 为 4/6/8，
// 扇区 0 中心在 0°，其余均分 360°，返回值在 [0, sectorCount)。
// 用度数不用弧度：扇区边界要 360/N 的精确等分，整数除法与边界判断更直观，
// 避免浮点弧度的累积误差。
std::uint8_t sectorFromAngle(float angleDeg, std::uint8_t sectorCount) noexcept;

// 命中测试：相对轮盘中心的位移 (dx, dy) → 扇区索引。dx/dy 为 DIP，
// +y 向下（屏幕坐标）。位移在内圈半径内（中心核圆）或外圈半径外
// （外甩取消区）返回 nullopt，即不触发任何扇区；否则按角度映射到扇区。
std::optional<std::uint8_t> hitTestSector(float dx, float dy,
                                           float innerRadius, float outerRadius,
                                           std::uint8_t sectorCount) noexcept;

// 花瓣卡片里一行水平文字可用多宽（DIP）。卡片沿径向旋转而文字水平，
// 可占宽度随角度变化（正上方最宽，斜下方最窄）。返回偏移 bandCenterOffset、
// 高 bandHeight 的文字带内每行都完整落在卡片内的最大宽度，供绘制层当硬边界；
// 返回 0 表示放不下任何东西（参数非法也是 0）。
float radialLabelWindowWidth(float cardWidth, float cardHeight, float corner,
                             float angleDeg, float bandCenterOffset,
                             float bandHeight) noexcept;

// 校验配置：扇区数合法、actions.size() == sectorCount、黑名单无空项。
bool validateRadialConfig(const RadialConfig& config) noexcept;

}  // namespace core
}  // namespace ktile
