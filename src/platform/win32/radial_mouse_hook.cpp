#include "platform/win32/radial_mouse_hook.h"

#include <cmath>

namespace ktile {
namespace platform {
namespace win32 {

RadialMouseHook* RadialMouseHook::active_ = nullptr;

LRESULT CALLBACK RadialMouseHook::mouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || active_ == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    const bool swallowed = active_->handleMouseEvent(wParam, *event);
    if (swallowed) {
        return 1;  // 吞掉事件
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

RadialMouseHook::~RadialMouseHook() {
    uninstall();
}

bool RadialMouseHook::install(HWND notifyWindow,
                              const core::RadialTriggerConfig& trigger) {
    if (!trigger.enabled) {
        uninstall();
        return true;
    }
    if (hook_ != nullptr) {
        uninstall();
    }

    notifyWindow_ = notifyWindow;
    trigger_ = trigger;
    state_ = State::Idle;

    hook_ = SetWindowsHookExW(WH_MOUSE_LL, &mouseHookProc, nullptr, 0);
    if (hook_ != nullptr) {
        active_ = this;
        return true;
    }
    notifyWindow_ = nullptr;
    return false;
}

void RadialMouseHook::uninstall() noexcept {
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (active_ == this) {
        active_ = nullptr;
    }
    notifyWindow_ = nullptr;
    state_ = State::Idle;
}

WPARAM RadialMouseHook::downMessage() const noexcept {
    switch (trigger_.button) {
        case core::RadialTriggerButton::Right:  return WM_RBUTTONDOWN;
        case core::RadialTriggerButton::Middle: return WM_MBUTTONDOWN;
        case core::RadialTriggerButton::Side1:  return WM_XBUTTONDOWN;
        case core::RadialTriggerButton::Side2:  return WM_XBUTTONDOWN;
    }
    return WM_RBUTTONDOWN;
}

WPARAM RadialMouseHook::upMessage() const noexcept {
    switch (trigger_.button) {
        case core::RadialTriggerButton::Right:  return WM_RBUTTONUP;
        case core::RadialTriggerButton::Middle: return WM_MBUTTONUP;
        case core::RadialTriggerButton::Side1:  return WM_XBUTTONUP;
        case core::RadialTriggerButton::Side2:  return WM_XBUTTONUP;
    }
    return WM_RBUTTONUP;
}

bool RadialMouseHook::modifierSatisfied() const noexcept {
    if (trigger_.modifier == core::RadialTriggerModifier::None) {
        return true;  // 无修饰键要求，直接满足
    }
    int vk = 0;
    switch (trigger_.modifier) {
        case core::RadialTriggerModifier::Ctrl:  vk = VK_CONTROL; break;
        case core::RadialTriggerModifier::Shift: vk = VK_SHIFT;   break;
        case core::RadialTriggerModifier::Alt:   vk = VK_MENU;    break;
        default: return true;
    }
    // 高位为 1 表示当前按下
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void RadialMouseHook::reinjectClick() const {
    // 重新注入一次按下+抬起，让原生菜单正常弹出。
    // 用 dwExtraInfo 标记，钩子回调看到签名就放行。
    INPUT downInput{};
    downInput.type = INPUT_MOUSE;
    downInput.mi.dwExtraInfo = kInjectSignature;

    INPUT upInput = downInput;

    switch (trigger_.button) {
        case core::RadialTriggerButton::Right:
            downInput.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
            upInput.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            break;
        case core::RadialTriggerButton::Middle:
            downInput.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
            upInput.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
            break;
        case core::RadialTriggerButton::Side1:
            downInput.mi.dwFlags = MOUSEEVENTF_XDOWN;
            downInput.mi.mouseData = XBUTTON1;
            upInput.mi.dwFlags = MOUSEEVENTF_XUP;
            upInput.mi.mouseData = XBUTTON1;
            break;
        case core::RadialTriggerButton::Side2:
            downInput.mi.dwFlags = MOUSEEVENTF_XDOWN;
            downInput.mi.mouseData = XBUTTON2;
            upInput.mi.dwFlags = MOUSEEVENTF_XUP;
            upInput.mi.mouseData = XBUTTON2;
            break;
    }

    INPUT inputs[2] = {downInput, upInput};
    SendInput(2, inputs, sizeof(INPUT));
}

bool RadialMouseHook::handleMouseEvent(WPARAM message, const MSLLHOOKSTRUCT& event) {
    // 自身注入的事件必须放行，否则会死循环
    if (event.dwExtraInfo == kInjectSignature) {
        return false;
    }

    const WPARAM downMsg = downMessage();
    const WPARAM upMsg = upMessage();

    if (message == downMsg) {
        // 按键按下
        if (!modifierSatisfied()) {
            // 修饰键不满足：放行，让 MouseInc 等处理
            return false;
        }
        // 满足条件：吞掉事件，进入跟踪
        state_ = State::Tracking;
        startPoint_ = event.pt;
        currentPoint_ = event.pt;
        triggerMessage_ = message;
        return true;  // 吞掉
    }

    if (state_ != State::Idle) {
        if (message == WM_MOUSEMOVE) {
            currentPoint_ = event.pt;
            const long dx = currentPoint_.x - startPoint_.x;
            const long dy = currentPoint_.y - startPoint_.y;
            const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));

            if (state_ == State::Tracking &&
                distance >= static_cast<float>(trigger_.activationThresholdPx)) {
                // 超过阈值：显示轮盘
                state_ = State::Shown;
                PostMessageW(notifyWindow_, kRadialShowMessage,
                             static_cast<WPARAM>(startPoint_.x),
                             static_cast<LPARAM>(startPoint_.y));
            }
            if (state_ == State::Shown) {
                PostMessageW(notifyWindow_, kRadialMoveMessage,
                             static_cast<WPARAM>(currentPoint_.x),
                             static_cast<LPARAM>(currentPoint_.y));
            }
            // 移动事件必须放行：低级鼠标钩子吞掉 WM_MOUSEMOVE 会导致系统
            // 不再更新光标位置，用户会感觉鼠标"卡死"。我们只旁路观察位置，
            // 不修改事件流。
            return false;
        }

        if (message == upMsg) {
            // 按键抬起
            currentPoint_ = event.pt;
            if (state_ == State::Shown) {
                // 已显示轮盘：通知主线程判定扇区
                PostMessageW(notifyWindow_, kRadialReleaseMessage,
                             static_cast<WPARAM>(currentPoint_.x),
                             static_cast<LPARAM>(currentPoint_.y));
            } else {
                // 未超阈值：恢复原生点击
                if (trigger_.modifier == core::RadialTriggerModifier::None) {
                    reinjectClick();
                }
                // 有修饰键时不注入（用户是想做手势，不弹原生菜单）
            }
            state_ = State::Idle;
            return true;  // 吞掉抬起
        }
    }

    return false;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
