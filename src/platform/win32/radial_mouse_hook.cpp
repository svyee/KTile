#include "platform/win32/radial_mouse_hook.h"

#include "base/text_convert.h"
#include "platform/win32/foreground_process.h"

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
                              const core::RadialTriggerConfig& trigger,
                              const std::vector<std::string>& blacklist) {
    if (!trigger.enabled) {
        uninstall();
        return true;
    }
    if (hook_ != nullptr) {
        uninstall();
    }

    notifyWindow_ = notifyWindow;
    trigger_ = trigger;
    blacklist_ = blacklist;
    state_ = State::Idle;
    armedTick_ = 0;
    // 全新的一次安装：窗口可见性由应用层随后回灌，这里先按「没亮」算。
    // 如果轮盘这时其实还亮着，应用层下一次 setWheelVisible 会纠正回来。
    wheelVisible_ = false;

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
    blacklist_.clear();
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

bool RadialMouseHook::isTriggerDownMessage(WPARAM message) const noexcept {
    if (message == downMessage()) {
        return true;
    }
    // 双击的第二次按下走的是 *DBLCLK，右键/中键各有一条
    switch (trigger_.button) {
        case core::RadialTriggerButton::Right:  return message == WM_RBUTTONDBLCLK;
        case core::RadialTriggerButton::Middle: return message == WM_MBUTTONDBLCLK;
        case core::RadialTriggerButton::Side1:
        case core::RadialTriggerButton::Side2:  return false;  // 侧键没有双击消息
    }
    return false;
}

bool RadialMouseHook::matchesTriggerButton(const MSLLHOOKSTRUCT& event) const noexcept {
    switch (trigger_.button) {
        case core::RadialTriggerButton::Right:
        case core::RadialTriggerButton::Middle:
            // 右键与中键各有独立的消息类型，消息对上了就是对的键
            return true;
        case core::RadialTriggerButton::Side1:
            return HIWORD(event.mouseData) == XBUTTON1;
        case core::RadialTriggerButton::Side2:
            return HIWORD(event.mouseData) == XBUTTON2;
    }
    return false;
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

bool RadialMouseHook::suppressionActive() const noexcept {
    // 选了触发修饰键，抑制键就没有意义（见 core::suppressionApplies）。
    // 设置页在那种情况下会把抑制键整组灰掉，这里必须同步 —— 界面灰了却
    // 偷偷生效，是最难排查的那类问题。
    if (!core::suppressionApplies(trigger_.modifier)) {
        return false;
    }
    // 见头文件说明：抑制键是键盘键，没被我们吞过，GetAsyncKeyState 可信。
    for (core::HotkeyModifier modifier : trigger_.suppressWhileHeld) {
        int vk = 0;
        switch (modifier) {
            case core::HotkeyModifier::Alt:     vk = VK_MENU;    break;
            case core::HotkeyModifier::Control: vk = VK_CONTROL; break;
            case core::HotkeyModifier::Shift:   vk = VK_SHIFT;   break;
            case core::HotkeyModifier::Win:     vk = VK_LWIN;    break;
        }
        if (vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0) {
            return true;
        }
    }
    return false;
}

bool RadialMouseHook::foregroundBlocked() const noexcept {
    if (blacklist_.empty()) {
        return false;  // 常见情况，连进程都不查
    }
    const std::wstring process = foregroundProcessName();
    if (process.empty()) {
        return false;  // 查不到就按「不拉黑」处理，别让功能静默失效
    }
    return core::isProcessBlockedBy(blacklist_, base::wideToUtf8Lossy(process));
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

    const WPARAM upMsg = upMessage();

    if (isTriggerDownMessage(message)) {
        // 侧键1/侧键2 共用 WM_XBUTTONDOWN，还得看 mouseData 才知是哪一个
        if (!matchesTriggerButton(event)) {
            return false;  // 另一个侧键：放行给目标窗口
        }
        // 黑名单进程：整条放行。必须在这里判，不能等窗口显示时再判 ——
        // 那时按下事件已经被吞掉，原生右键菜单弹不出来，用户会觉得
        // 「这个应用里右键坏了」。
        if (foregroundBlocked()) {
            return false;
        }
        // 抑制键按着：整段放行，连状态机都不进。
        // 用户此刻大概率在做 Alt+Tab / Alt+方向 这类操作，顺手拖了下右键，
        // 我们既不该吞事件，也不该冒出一个轮盘挡在半路上。
        if (suppressionActive()) {
            return false;
        }
        // 按键按下
        if (!modifierSatisfied()) {
            // 修饰键不满足：放行，让 MouseInc 等处理
            return false;
        }
        // 满足条件：吞掉事件，进入跟踪
        //
        // 先对账：如果上一次手势还「在跑」，或者轮盘还亮着，说明那一次
        // 抬起到我们手上的路断了（见头文件「由此带来的一个真实故障」）。
        // 让主线程把残影收掉，再当作全新的一次按下 —— 否则状态机会带着
        // 上一次的起点继续跑，窗口也会留在屏幕上跟着鼠标走。
        if (state_ != State::Idle || wheelVisible_) {
            PostMessageW(notifyWindow_, kRadialAbortMessage,
                         static_cast<WPARAM>(RadialAbortReason::LostRelease), 0);
        }
        state_ = State::Tracking;
        armedTick_ = GetTickCount64();
        startPoint_ = event.pt;
        currentPoint_ = event.pt;
        return true;  // 吞掉
    }

    if (state_ != State::Idle) {
        if (message == WM_MOUSEMOVE) {
            currentPoint_ = event.pt;
            const long dx = currentPoint_.x - startPoint_.x;
            const long dy = currentPoint_.y - startPoint_.y;
            const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));

            if (state_ == State::Tracking) {
                // 过期判定放在阈值判定之前：跨过阈值的判定一旦赢了，
                // 就会把一个「抬起早丢了」的陈旧起点当成新手势弹出来。
                if (GetTickCount64() - armedTick_ > kMaxTrackingMs) {
                    state_ = State::Idle;
                    PostMessageW(notifyWindow_, kRadialAbortMessage,
                                 static_cast<WPARAM>(
                                     RadialAbortReason::StaleGesture),
                                 0);
                    return false;
                }
                if (distance >=
                    static_cast<float>(trigger_.activationThresholdPx)) {
                    // 超过阈值：显示轮盘
                    state_ = State::Shown;
                    PostMessageW(notifyWindow_, kRadialShowMessage,
                                 static_cast<WPARAM>(startPoint_.x),
                                 static_cast<LPARAM>(startPoint_.y));
                }
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
            // 同上：抬起也要确认是配置的那一个侧键（另一个侧键的抬起
            // 不能当成手势结束，否则会把状态机提前打回 Idle）
            if (!matchesTriggerButton(event)) {
                return false;
            }
            // 按键抬起
            currentPoint_ = event.pt;
            if (state_ == State::Shown) {
                // 已显示轮盘：通知主线程判定扇区
                PostMessageW(notifyWindow_, kRadialReleaseMessage,
                             static_cast<WPARAM>(currentPoint_.x),
                             static_cast<LPARAM>(currentPoint_.y));
            } else if (wheelVisible_) {
                // 状态机回 Idle 了、窗口却还亮着（中间被 abort / 重装钩子
                // 重置过）。这时候绝不能注入原生点击 —— 用户看到的是一次
                // 结束了的轮盘操作，再弹一个右键菜单等于凭空多一次点击。
                PostMessageW(notifyWindow_, kRadialAbortMessage,
                             static_cast<WPARAM>(RadialAbortReason::LostRelease), 0);
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
