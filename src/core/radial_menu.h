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

    // 抑制键：按住其中任意一个键时，轮盘不弹出，鼠标事件原样放行给目标窗口。
    // 用途是给 Alt+Tab、Alt+方向这类组合键让路 —— 用户正按着 Alt 操作，
    // 顺手拖了下右键不该被我们吞掉。空列表 = 不抑制。
    std::vector<HotkeyModifier> suppressWhileHeld;
};

// 抑制键是否与触发修饰键撞车。
//
// 同一个键既要求按住（触发）又要求不按住（抑制）等于永远不触发，
// 这种配置在界面上要拦住，从文件里读到时剔除冲突项而不是整份打回默认。
bool suppressionConflictsTrigger(const RadialTriggerConfig& trigger) noexcept;

// 单个修饰键是否就是触发修饰键（判断的是同一个物理键）。
//
// 分开暴露是因为加载期要「逐项剔除」冲突的抑制键，而界面保存前只要一个
// 「整体有没有冲突」的是非答案 —— 两处共用这一条判断，避免各写一遍。
bool modifierIsTriggerModifier(HotkeyModifier held,
                               RadialTriggerModifier trigger) noexcept;

// 外甩取消半径的可用区间（DIP，逻辑像素）。
//
// 下限 142 就是卡片环的外缘：比它更小的半径会把卡片自己判成「已甩出去」，
// 用户会觉得有半个轮盘是坏的。上限只是防呆，别让配置写成一个永远取消不了
// 的天文数字。
inline constexpr std::uint16_t kMinCancelRadiusDip = 142;
inline constexpr std::uint16_t kMaxCancelRadiusDip = 600;

// ================================================================ 轮盘总配置

// radial.json 的 schema 版本。
//
// 刻意**不复用** data_model.h 的 kCurrentSchemaVersion：那个是 items.json /
// config.json 的版本，两者结构毫无关系。共用同一个常量意味着给轮盘加字段
// 会把磁贴配置的版本号一起推上去，下次真要按版本做迁移时就分不清是谁变了。
// 轮盘自己一条版本线：1 = 初版，2 = 多方案 + 按应用白名单，
// 3 = 通用项 + 进程黑名单（按应用差异化交给 slugin 插件）。
inline constexpr std::uint32_t kRadialSchemaVersion = 3;

// 进程名是否命中黑名单（大小写不敏感）。
//
// 抽成自由函数是为了让钩子层也能用：鼠标钩子在「按下」那一瞬间就要决定
// 吞不吞事件，那时手里只有一份黑名单，没有完整的 RadialConfig。
bool isProcessBlockedBy(const std::vector<std::string>& blacklist,
                        const std::string& processName) noexcept;

// 两个进程名是否相等（大小写不敏感）。
//
// 单独暴露是给界面判重用的：黑名单里加第二遍 "Chrome.exe" 应当被拦住，
// 但用户看不出 "chrome.exe" 和 "Chrome.exe" 是一回事，得由代码替他判断。
bool processNameEquals(const std::string& left,
                       const std::string& right) noexcept;

struct RadialConfig {
    std::uint32_t schemaVersion = kRadialSchemaVersion;
    RadialTriggerConfig trigger;

    // 通用轮盘：所有应用共用这一份。按应用差异化交给 slugin 插件目录，
    // 用户侧不再有多方案概念 —— 设置界面只呈现「一份通用配置」。
    RadialMenu menu;

    // 进程黑名单：命中即不弹轮盘（事件全部放行）。只比进程名、大小写不敏感，
    // 刻意不支持通配符 —— `*app*.exe` 这种写法误伤面太大，用户很难预估。
    std::vector<std::string> blacklist;

    // 该进程是否被拉黑。空串（拿不到进程名）不算命中：宁可弹一次，
    // 也别让「查不到进程名」这种异常情况把功能变成静默失效。
    bool isProcessBlocked(const std::string& processName) const noexcept;
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

// 花瓣卡片里一行水平文字可用多宽（DIP）。
//
// 卡片按「宽 cardWidth 沿切向、高 cardHeight 沿径向、圆角 corner」摆放，并像
// 花瓣一样沿径向旋转 angleDeg；卡片里的文字却是水平的。于是同一段文字在不同
// 角度能占的宽度并不一样 —— 正上方最宽（等于卡片宽），斜下方最窄（卡片斜
// 着，短边在挡路）。这个函数返回「文字带（相对卡片中心偏移 bandCenterOffset、
// 高 bandHeight 的水平条）里每一行都完整落在卡片内」的最大宽度。
//
// 用途是给绘制层一个硬边界：文字在这个宽度内排版/滚动，就绝不会画到卡片外。
// 返回 0 表示放不下任何东西（参数非法时也是 0，调用方按「没有窗口」处理）。
float radialLabelWindowWidth(float cardWidth, float cardHeight, float corner,
                             float angleDeg, float bandCenterOffset,
                             float bandHeight) noexcept;

// 校验配置：扇区数合法、actions.size() == sectorCount、黑名单无空项。
bool validateRadialConfig(const RadialConfig& config) noexcept;

}  // namespace core
}  // namespace ktile
