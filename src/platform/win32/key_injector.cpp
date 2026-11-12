#include "platform/win32/key_injector.h"

#include "platform/win32/global_hotkey.h"  // keyNameToVirtualKey

#include <Windows.h>

#include <vector>

namespace ktile {
namespace platform {
namespace win32 {

namespace {

// 修饰键 → 虚拟键码
WORD modifierToVk(core::HotkeyModifier modifier) noexcept {
    switch (modifier) {
        case core::HotkeyModifier::Control: return VK_CONTROL;
        case core::HotkeyModifier::Shift:   return VK_SHIFT;
        case core::HotkeyModifier::Alt:     return VK_MENU;
        case core::HotkeyModifier::Win:     return VK_LWIN;
    }
    return 0;
}

// 单个按键的按下或抬起
void keyEvent(WORD vk, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

}  // namespace

void injectShortcut(const std::vector<core::HotkeyModifier>& modifiers,
                    const std::string& key) {
    const WORD mainVk = keyNameToVirtualKey(key);
    if (mainVk == 0) return;  // 无法识别的主键

    // 只注入当前未被物理按下的修饰键。轮盘默认 Ctrl+右键触发，用户松手
    // 时 Ctrl 往往还按着；若再注入 Ctrl 按下+抬起，系统认为 Ctrl 已放开
    // 而物理上还按着，之后的 Ctrl+C 会失效。跳过已按住的修饰键就不会
    // 破坏用户的物理按键状态。
    std::vector<WORD> pressedByUs;
    pressedByUs.reserve(modifiers.size());
    for (const auto& mod : modifiers) {
        const WORD vk = modifierToVk(mod);
        if (vk == 0) continue;
        if ((GetAsyncKeyState(vk) & 0x8000) != 0) continue;  // 已按住，跳过
        keyEvent(vk, true);
        pressedByUs.push_back(vk);
    }

    // 主键按下 → 抬起
    keyEvent(mainVk, true);
    keyEvent(mainVk, false);

    // 逆序释放注入过的修饰键
    for (auto it = pressedByUs.rbegin(); it != pressedByUs.rend(); ++it) {
        keyEvent(*it, false);
    }
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
