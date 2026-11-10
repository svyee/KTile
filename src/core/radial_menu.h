// KTile - 核心数据模型：轮盘菜单
//
// 设计原则：
//  1. 与 LaunchItem 解耦：轮盘动作只存「怎么执行」，不复用磁贴的数据结构，
//     因为轮盘需要 Shortcut（注入组合键）这种磁贴没有的动作类型。
//  2. 扇区数与动作向量长度一致：空槽位用默认 RadialAction 占位，
//     绘制和命中测试时跳过 type 为默认值且 name 为空的项。
//  3. 仅依赖标准库，不引入任何平台/JSON 细节（保持领域层纯净）。
//  4. 几何计算（角度→扇区、命中测试）是纯函数，可单测。
//
#pragma once

#include "core/data_model.h"   // HotkeyModifier

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ktile {
namespace core {

// ================================================================ 轮盘动作类型
//
// 两类执行机制：
//   - Application/File/Folder/Url：用 ShellExecuteW 直接拉起/打开
//   - Shortcut：用 SendInput 向当前前台窗口注入组合键（如复制=Ctrl+C）
//   - Builtin：内置系统命令（锁屏/音量等），走 system_actions
//
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

// ================================================================ 轮盘动作

struct RadialAction {
    RadialActionType type = RadialActionType::Application;
    std::string name;                          // 显示名（空时由 target 推导）
    std::string target;                        // 程序路径 / 文件 / 网址 / 内置命令 id
    std::vector<std::string> arguments;        // 启动参数（仅 Application）
    std::optional<std::string> icon;           // 自定义图标（相对 data 目录）

    // 仅 Shortcut 类型有效
    std::vector<HotkeyModifier> modifiers;     // 修饰键（Ctrl/Shift/Alt/Win）
    std::string key;                           // 主键名（如 "C", "F4", "Space"）

    RadialAction() = default;

    // 是否为空槽位（未配置动作）
    bool isEmpty() const noexcept {
        return name.empty() && target.empty() && key.empty();
    }
};

// ================================================================ 轮盘方案

// 支持的扇区数
inline constexpr std::uint8_t kMinSectors = 4;
inline constexpr std::uint8_t kMaxSectors = 8;

struct RadialMenu {
    std::string id;
    std::string name;
    std::uint8_t sectorCount = 8;              // 4 / 6 / 8
    std::vector<RadialAction> actions;         // 固定 sectorCount 个

    RadialMenu() = default;
    RadialMenu(std::string id_, std::string name_, std::uint8_t sectors = 8)
        : id(std::move(id_)), name(std::move(name_)),
          sectorCount(sectors), actions(sectors) {}
};

// ================================================================ Per-App 配置

// 进程名 → 轮盘方案的映射。processPattern 为空表示全局默认。
struct RadialProfile {
    std::string processPattern;                // 进程名，如 "chrome.exe"；空 = 全局
    std::string menuId;

    RadialProfile() = default;
    RadialProfile(std::string process_, std::string menuId_)
        : processPattern(std::move(process_)), menuId(std::move(menuId_)) {}
};

// ================================================================ 触发配置

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
    std::uint16_t activationThresholdPx = 8;   // 拖动超过此距离才呼出轮盘
    std::uint16_t cancelRadiusPx = 160;        // 拖出此半径外松手 = 取消
};

// ================================================================ 轮盘总配置

struct RadialConfig {
    std::uint32_t schemaVersion = kCurrentSchemaVersion;
    RadialTriggerConfig trigger;
    std::vector<RadialMenu> menus;
    std::vector<RadialProfile> profiles;

    // 查找指定 id 的菜单，未找到返回 nullptr
    const RadialMenu* findMenu(const std::string& menuId) const noexcept;

    // 根据前台进程名匹配 profile，返回对应的 menu。
    // 优先精确匹配进程名，无匹配时返回空 processPattern 的全局默认菜单。
    const RadialMenu* menuForProcess(const std::string& processName) const noexcept;
};

// ================================================================ 几何计算（纯函数）

// 角度（度）→ 扇区索引。
//
// 约定：0° = 正上方（12 点钟方向），顺时针递增。
// sectorCount 为 4/6/8，扇区 0 的中心在 0°，其余均分 360°。
// 返回值范围 [0, sectorCount)。
//
// 为什么用度数而不是弧度：扇区边界需要 360/N 的精确等分，
// 度数在整数除法和边界判断上更直观，避免浮点弧度的累积误差。
std::uint8_t sectorFromAngle(float angleDeg, std::uint8_t sectorCount) noexcept;

// 命中测试：给定相对轮盘中心的位移 (dx, dy)，返回命中的扇区索引。
//
// - dx/dy 为 DIP 单位，+y 向下（屏幕坐标）。
// - 位移在内圈半径内（中心核圆区域）返回 nullopt。
// - 位移在外圈半径外返回 nullopt（外甩取消区域）。
// - 否则按角度映射到扇区。
//
// 返回 nullopt 表示「不触发任何扇区」（中心核圆或外甩取消）。
std::optional<std::uint8_t> hitTestSector(float dx, float dy,
                                           float innerRadius, float outerRadius,
                                           std::uint8_t sectorCount) noexcept;

// 校验配置：扇区数合法、每个 menu 的 actions.size() == sectorCount、
// profiles 引用的 menuId 存在。
bool validateRadialConfig(const RadialConfig& config) noexcept;

}  // namespace core
}  // namespace ktile
