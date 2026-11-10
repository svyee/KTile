// KTile - 核心数据模型：轮盘菜单实现
//
#include "core/radial_menu.h"

#include <algorithm>
#include <cmath>

namespace ktile {
namespace core {

// ================================================================ 配置查找

const RadialMenu* RadialConfig::findMenu(const std::string& menuId) const noexcept {
    for (const auto& menu : menus) {
        if (menu.id == menuId) {
            return &menu;
        }
    }
    return nullptr;
}

const RadialMenu* RadialConfig::menuForProcess(
        const std::string& processName) const noexcept {
    // 优先精确匹配进程名
    for (const auto& profile : profiles) {
        if (!profile.processPattern.empty() &&
            profile.processPattern == processName) {
            const RadialMenu* menu = findMenu(profile.menuId);
            if (menu) return menu;
        }
    }
    // 回退到全局默认（processPattern 为空的 profile）
    for (const auto& profile : profiles) {
        if (profile.processPattern.empty()) {
            const RadialMenu* menu = findMenu(profile.menuId);
            if (menu) return menu;
        }
    }
    // 都没有则返回第一个菜单作为兜底
    if (!menus.empty()) return &menus.front();
    return nullptr;
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
    for (const auto& menu : config.menus) {
        if (menu.id.empty()) return false;
        if (menu.sectorCount < kMinSectors || menu.sectorCount > kMaxSectors)
            return false;
        if (menu.actions.size() != menu.sectorCount) return false;
    }
    for (const auto& profile : config.profiles) {
        if (profile.menuId.empty()) return false;
        if (!config.findMenu(profile.menuId)) return false;
    }
    if (config.trigger.activationThresholdPx < 1) return false;
    if (config.trigger.cancelRadiusPx <= config.trigger.activationThresholdPx)
        return false;
    return true;
}

}  // namespace core
}  // namespace ktile
