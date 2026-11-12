// KTile - 核心数据模型：轮盘菜单实现
//
#include "core/radial_menu.h"

#include <algorithm>
#include <cmath>

namespace ktile {
namespace core {

// --- 抑制键

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

bool suppressionApplies(RadialTriggerModifier trigger) noexcept {
    return trigger == RadialTriggerModifier::None;
}

// --- 黑名单匹配

bool processNameEquals(const std::string& left,
                       const std::string& right) noexcept {
    // 进程名在 Windows 上大小写无关，逐字节折 ASCII 大写即可，不碰 locale。
    // 非 ASCII 字节原样比较，进程名实际都是 ASCII。
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

// --- 几何计算

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

namespace {

// 圆角矩形内部判定（局部坐标，以卡片中心为原点）。
bool insideRoundedRect(float x, float y, float halfWidth, float halfHeight,
                       float corner) noexcept {
    const float ax = (x < 0.0F) ? -x : x;
    const float ay = (y < 0.0F) ? -y : y;
    if (ax > halfWidth || ay > halfHeight) return false;
    if (ax <= halfWidth - corner || ay <= halfHeight - corner) return true;
    const float dx = ax - (halfWidth - corner);
    const float dy = ay - (halfHeight - corner);
    return dx * dx + dy * dy <= corner * corner;
}

}  // namespace

float radialLabelWindowWidth(float cardWidth, float cardHeight, float corner,
                             float angleDeg, float bandCenterOffset,
                             float bandHeight) noexcept {
    if (!(cardWidth > 0.0F) || !(cardHeight > 0.0F) || !(bandHeight > 0.0F)) {
        return 0.0F;
    }
    const float halfWidth = cardWidth * 0.5F;
    const float halfHeight = cardHeight * 0.5F;
    float radius = corner;
    if (!(radius > 0.0F)) radius = 0.0F;
    if (radius > halfWidth) radius = halfWidth;
    if (radius > halfHeight) radius = halfHeight;

    const float rad = angleDeg * 3.14159265358979323846F / 180.0F;
    const float ca = std::cos(rad);
    const float sa = std::sin(rad);

    // 文字带上一行（屏幕空间的水平线）的两端是否都在卡片里。
    // 屏幕坐标相对卡片中心 -> 卡片局部坐标就是反向旋转：D2D 的 Rotation(θ)
    // 在 y 向下的坐标系里是顺时针，逆变换即 (x·cos + y·sin, -x·sin + y·cos)。
    auto rowInside = [&](float u, float v) {
        const float lx = u * ca + v * sa;
        const float ly = -u * sa + v * ca;
        return insideRoundedRect(lx, ly, halfWidth, halfHeight, radius);
    };

    // 圆角矩形是凸的，两端点都在里面就说明整行都在里面，逐行检查两头即可。
    constexpr int kRows = 15;
    auto fits = [&](float halfWindow) {
        for (int i = 0; i < kRows; ++i) {
            const float v = bandCenterOffset - bandHeight * 0.5F +
                            bandHeight * static_cast<float>(i) /
                                static_cast<float>(kRows - 1);
            if (!rowInside(halfWindow, v) || !rowInside(-halfWindow, v)) {
                return false;
            }
        }
        return true;
    };

    if (!fits(0.0F)) return 0.0F;

    // 二分：fits 对 halfWindow 单调（同样的行，窗口越窄越容易满足）。
    // 24 次迭代把误差压到 25/2^24 ≈ 0.000002 DIP，远超绘制精度。
    float lo = 0.0F;
    float hi = halfWidth;
    for (int i = 0; i < 24; ++i) {
        const float mid = (lo + hi) * 0.5F;
        if (fits(mid)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo * 2.0F;
}

// --- 配置校验

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
