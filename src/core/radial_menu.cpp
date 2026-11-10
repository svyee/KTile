// KTile - 核心数据模型：轮盘菜单实现
//
#include "core/radial_menu.h"

#include <algorithm>
#include <cmath>

namespace ktile {
namespace core {

// ================================================================ 抑制键

bool modifierIsTriggerModifier(HotkeyModifier held,
                              RadialTriggerModifier trigger) noexcept {
    switch (trigger) {
        case RadialTriggerModifier::Ctrl:
            return held == HotkeyModifier::Control;
        case RadialTriggerModifier::Shift:
            return held == HotkeyModifier::Shift;
        case RadialTriggerModifier::Alt:
            return held == HotkeyModifier::Alt;
        case RadialTriggerModifier::None:
            return false;  // 没有触发修饰键，任何抑制键都不冲突
    }
    return false;
}

bool suppressionConflictsTrigger(const RadialTriggerConfig& trigger) noexcept {
    for (HotkeyModifier held : trigger.suppressWhileHeld) {
        if (modifierIsTriggerModifier(held, trigger.modifier)) {
            return true;
        }
    }
    return false;
}

// ================================================================ 黑名单匹配

bool processNameEquals(const std::string& left,
                       const std::string& right) noexcept {
    // 进程名在 Windows 上大小写无关，逐字节折 ASCII 大写即可，不碰 locale。
    // 非 ASCII 字节原样比较 —— 进程名实际都是 ASCII。
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(left[i]);
        const unsigned char b = static_cast<unsigned char>(right[i]);
        const unsigned char la = (a >= 'A' && a <= 'Z') ? a + 32 : a;
        const unsigned char lb = (b >= 'A' && b <= 'Z') ? b + 32 : b;
        if (la != lb) return false;
    }
    return true;
}

bool isProcessBlockedBy(const std::vector<std::string>& blacklist,
                        const std::string& processName) noexcept {
    if (processName.empty()) return false;
    for (const auto& blocked : blacklist) {
        if (processNameEquals(blocked, processName)) return true;
    }
    return false;
}

bool RadialConfig::isProcessBlocked(const std::string& processName) const noexcept {
    return isProcessBlockedBy(blacklist, processName);
}

// ================================================================ 几何计算

std::uint8_t sectorFromAngle(float angleDeg, std::uint8_t sectorCount) noexcept {
    if (sectorCount < kMinSectors) sectorCount = kMinSectors;
    if (sectorCount > kMaxSectors) sectorCount = kMaxSectors;

    // 归一化到 [0, 360)
    float angle = std::fmod(angleDeg, 360.0F);
    if (angle < 0.0F) angle += 360.0F;

    const float sectorWidth = 360.0F / static_cast<float>(sectorCount);
    // 偏移半个扇区，使扇区 0 的中心在 0°（正上方）
    const float shifted = angle + sectorWidth * 0.5F;
    float idx = std::floor(shifted / sectorWidth);
    if (idx >= static_cast<float>(sectorCount)) idx = 0.0F;
    return static_cast<std::uint8_t>(idx);
}

std::optional<std::uint8_t> hitTestSector(float dx, float dy,
                                           float innerRadius, float outerRadius,
                                           std::uint8_t sectorCount) noexcept {
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (distance < innerRadius) return std::nullopt;   // 中心核圆
    if (distance >= outerRadius) return std::nullopt;  // 外甩取消

    // 屏幕坐标：+y 向下。要让正上方为 0°、顺时针递增，
    // 需要用 atan2(dx, -dy)：dy 为负（向上）时 -dy 为正，atan2(0, +) = 0。
    const float angleRad = std::atan2(dx, -dy);
    const float angleDeg = angleRad * 180.0F / 3.14159265358979323846F;
    return sectorFromAngle(angleDeg, sectorCount);
}

// ================================================================ 配置校验

bool validateRadialConfig(const RadialConfig& config) noexcept {
    const RadialMenu& menu = config.menu;
    if (menu.sectorCount < kMinSectors || menu.sectorCount > kMaxSectors)
        return false;
    if (menu.actions.size() != menu.sectorCount) return false;
    // 黑名单里出现空串没有意义（空进程名永远匹配不上），视为非法配置
    for (const auto& blocked : config.blacklist) {
        if (blocked.empty()) return false;
    }
    if (config.trigger.activationThresholdPx < 1) return false;
    if (config.trigger.cancelRadiusPx <= config.trigger.activationThresholdPx)
        return false;
    // 抑制键与触发修饰键撞车 = 永远弹不出来，属于非法配置
    if (suppressionConflictsTrigger(config.trigger)) return false;
    return true;
}

}  // namespace core
}  // namespace ktile
