#include "platform/win32/radial_mouse_hook.h"

#include "base/text_convert.h"
#include "platform/win32/foreground_process.h"

#include <chrono>
#include <cmath>

namespace ktile {
namespace platform {
namespace win32 {

RadialMouseHook* RadialMouseHook::active_ = nullptr;

LRESULT CALLBACK RadialMouseHook::mouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    RadialMouseHook* self = active_;
    if (code < 0 || self == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    const bool swallowed = self->handleMouseEvent(wParam, *event);
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
    // 先收掉上一份配置的钩子线程：配置靠重新 install 生效，两代并存会抢事件。
    uninstall();

    notifyWindow_ = notifyWindow;
    trigger_ = trigger;
    blacklist_ = blacklist;
    state_.store(State::Idle, std::memory_order_relaxed);
    armedTick_ = 0;
    wheelShownThisGesture_ = false;
    // 窗口可见性由应用层随后回灌，先按「没亮」算；若轮盘实际还亮着，
    // 下一次 setWheelVisible 会纠正回来。
    wheelVisible_.store(false, std::memory_order_relaxed);
    stop_.store(false, std::memory_order_relaxed);
    threadId_.store(0, std::memory_order_relaxed);
    installed_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(startMutex_);
        startReady_ = false;
        startOk_ = false;
    }

    // 回调靠 active_ 转发，必须在起线程之前设好（线程一起来就可能收到事件）
    active_ = this;
    thread_ = std::thread(&RadialMouseHook::threadMain, this);

    bool ready = false;
    bool ok = false;
    {
        std::unique_lock<std::mutex> lock(startMutex_);
        ready = startCondition_.wait_for(lock, std::chrono::seconds(5),
                                         [this] { return startReady_; });
        ok = ready && startOk_;
    }
    if (!ok) {
        // 线程没起来（或超时）：收掉，不留半死线程
        stop_.store(true, std::memory_order_relaxed);
        const DWORD tid = threadId_.load(std::memory_order_acquire);
        if (tid != 0) {
            PostThreadMessageW(tid, WM_QUIT, 0, 0);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        active_ = nullptr;
        notifyWindow_ = nullptr;
        return false;
    }
    return true;
}

void RadialMouseHook::uninstall() noexcept {
    stop_.store(true, std::memory_order_relaxed);
    const DWORD tid = threadId_.load(std::memory_order_acquire);
    if (tid != 0) {
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }
    if (thread_.joinable()) {
        // 有界等待：钩子线程收到 WM_QUIT 应立刻退出；万一卡在系统调用里，
        // 也不能拖死本函数所在的 UI 线程。
        const HANDLE handle = static_cast<HANDLE>(thread_.native_handle());
        if (WaitForSingleObject(handle, 2000) == WAIT_OBJECT_0) {
            thread_.join();
        } else {
            thread_.detach();
        }
    }
    if (active_ == this) {
        active_ = nullptr;
    }
    installed_.store(false, std::memory_order_relaxed);
    notifyWindow_ = nullptr;
    blacklist_.clear();
    state_.store(State::Idle, std::memory_order_relaxed);
    wheelVisible_.store(false, std::memory_order_relaxed);
    threadId_.store(0, std::memory_order_relaxed);
}

void RadialMouseHook::threadMain() {
    // 先建本线程消息队列：否则 PostThreadMessage(WM_QUIT) 因队列不存在
    // 而失败，线程会卡死在 GetMessage 里。
    MSG probe{};
    PeekMessageW(&probe, nullptr, 0, 0, PM_NOREMOVE);

    const DWORD tid = GetCurrentThreadId();
    threadId_.store(tid, std::memory_order_release);
    // 略高于 UI 线程：事件须在系统超时前取走，绘制晚几毫秒无所谓。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    hook_ = SetWindowsHookExW(WH_MOUSE_LL, &mouseHookProc, nullptr, 0);
    {
        std::lock_guard<std::mutex> lock(startMutex_);
        startOk_ = hook_ != nullptr;
        startReady_ = true;
    }
    startCondition_.notify_all();

    if (hook_ == nullptr) {
        threadId_.store(0, std::memory_order_release);
        return;
    }
    if (stop_.load(std::memory_order_acquire)) {
        // install() 和 uninstall() 抢跑：装完就被叫停
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
        threadId_.store(0, std::memory_order_release);
        return;
    }
    installed_.store(true, std::memory_order_release);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // 线程消息（无窗口）自己处理：补发点击须回消息循环做，不能在钩子
        // 回调里调 SendInput（会死锁），见头文件说明。
        if (msg.hwnd == nullptr && msg.message == kReinjectMessage) {
            reinjectClick();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    installed_.store(false, std::memory_order_release);
    threadId_.store(0, std::memory_order_release);
}

void RadialMouseHook::requestReinjectClick() const noexcept {
    const DWORD tid = threadId_.load(std::memory_order_acquire);
    if (tid != 0) {
        PostThreadMessageW(tid, kReinjectMessage, 0, 0);
    }
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
    // 选了触发修饰键时抑制键无意义（见 core::suppressionApplies），设置页
    // 此时灰掉抑制键，这里必须同步，避免界面灰了却偷偷生效。
    if (!core::suppressionApplies(trigger_.modifier)) {
        return false;
    }
    // 抑制键是键盘键，未被吞过，GetAsyncKeyState 可信。
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
    // 按下瞬间前台可能还没切到刚点中的窗口，按鼠标所在窗口判。
    const std::wstring process = radialTargetWindow().processName;
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
    // 状态机只有本线程（钩子线程）写，读一次就够；原子是因为 UI 线程的
    // isGestureActive() 也在读。
    const State state = state_.load(std::memory_order_relaxed);

    if (isTriggerDownMessage(message)) {
        // 侧键1/侧键2 共用 WM_XBUTTONDOWN，还得看 mouseData 才知是哪一个
        if (!matchesTriggerButton(event)) {
            return false;  // 另一个侧键：放行给目标窗口
        }
        // 黑名单进程：整条放行。必须在吞事件前判，等窗口显示时再判的话
        // 按下事件已被吞，原生右键菜单弹不出来，用户会以为右键坏了。
        if (foregroundBlocked()) {
            return false;
        }
        // 抑制键按着：整段放行，连状态机都不进。此时用户多半在做
        // Alt+Tab 这类操作，不该吞事件，也不该冒出轮盘挡路。
        if (suppressionActive()) {
            return false;
        }
        // 按键按下
        if (!modifierSatisfied()) {
            // 修饰键不满足：放行，让 MouseInc 等处理
            return false;
        }
        // 满足条件：吞掉事件，进入跟踪。先对账：上一次手势还在跑或轮盘
        // 还亮着，说明那一次的抬起早丢了（见头文件），让主线程收残影，
        // 再当作全新按下，否则状态机带着旧起点继续跑，窗口留在屏幕上。
        if (state != State::Idle || wheelVisible_.load(std::memory_order_relaxed)) {
            PostMessageW(notifyWindow_, kRadialAbortMessage,
                         static_cast<WPARAM>(RadialAbortReason::LostRelease), 0);
        }
        state_.store(State::Tracking, std::memory_order_relaxed);
        armedTick_ = GetTickCount64();
        startPoint_ = event.pt;
        currentPoint_ = event.pt;
        wheelShownThisGesture_ = false;
        return true;  // 吞掉
    }

    if (state != State::Idle) {
        if (message == WM_MOUSEMOVE) {
            currentPoint_ = event.pt;
            const long dx = currentPoint_.x - startPoint_.x;
            const long dy = currentPoint_.y - startPoint_.y;
            const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));

            State next = state;
            if (state == State::Tracking) {
                // 过期判定放在阈值判定之前：跨过阈值的判定一旦赢了，
                // 就会把一个「抬起早丢了」的陈旧起点当成新手势弹出来。
                if (GetTickCount64() - armedTick_ > kMaxTrackingMs) {
                    state_.store(State::Idle, std::memory_order_relaxed);
                    PostMessageW(notifyWindow_, kRadialAbortMessage,
                                 static_cast<WPARAM>(
                                     RadialAbortReason::StaleGesture),
                                 0);
                    return false;
                }
                if (distance >=
                    static_cast<float>(trigger_.activationThresholdPx)) {
                    // 超过阈值：显示轮盘
                    next = State::Shown;
                    state_.store(next, std::memory_order_relaxed);
                    // 从现在起抬起必须吞掉，即使状态机随后被 abort /
                    // 过期打回 Idle。
                    wheelShownThisGesture_ = true;
                    PostMessageW(notifyWindow_, kRadialShowMessage,
                                 static_cast<WPARAM>(startPoint_.x),
                                 static_cast<LPARAM>(startPoint_.y));
                }
            }
            if (next == State::Shown) {
                PostMessageW(notifyWindow_, kRadialMoveMessage,
                             static_cast<WPARAM>(currentPoint_.x),
                             static_cast<LPARAM>(currentPoint_.y));
            }
            // 移动事件必须放行：吞掉 WM_MOUSEMOVE 会让系统不再更新光标
            // 位置，用户感觉鼠标"卡死"。只旁路观察，不改事件流。
            return false;
        }
    }

    // 抬起单独处理，不放在 `state != Idle` 里面：状态机随时可能被 abort、
    // 过期、重装钩子打回 Idle，而这次按下确实唤出过轮盘
    // （wheelShownThisGesture_）。落到「不在跟踪中」分支会原样放行，前台
    // 窗口收到无配对按下的抬起照样弹菜单（Explorer 等），症状是轮盘刚
    // 消失右键菜单又冒出来，且时有时无。只要轮盘亮过，抬起一律吞掉。
    if (message == upMsg && matchesTriggerButton(event)) {
        currentPoint_ = event.pt;
        const bool shownNow = state == State::Shown;
        const bool wheelWasUp = wheelShownThisGesture_ ||
                                wheelVisible_.load(std::memory_order_relaxed);
        wheelShownThisGesture_ = false;

        if (shownNow) {
            // 已显示轮盘：通知主线程判定扇区
            PostMessageW(notifyWindow_, kRadialReleaseMessage,
                         static_cast<WPARAM>(currentPoint_.x),
                         static_cast<LPARAM>(currentPoint_.y));
        } else if (wheelWasUp) {
            // 轮盘亮过但状态机已回 Idle（被 abort / 重装钩子重置）：让主
            // 线程收残影，不注入原生点击，避免凭空多弹一次右键菜单。
            PostMessageW(notifyWindow_, kRadialAbortMessage,
                         static_cast<WPARAM>(RadialAbortReason::LostRelease), 0);
        } else if (state == State::Idle) {
            // 没在跟踪且轮盘没亮过：不是本次手势，放行。开头已拦自身
            // 注入的事件，这里不会自激。
            return false;
        } else {
            // 未超阈值：恢复原生点击。SendInput 在低级钩子回调里有死锁
            // 风险（原始输入线程正等回调返回），投消息回消息循环再补。
            if (trigger_.modifier == core::RadialTriggerModifier::None) {
                requestReinjectClick();
            }
            // 有修饰键时不注入（用户是想做手势，不弹原生菜单）
        }
        state_.store(State::Idle, std::memory_order_relaxed);
        return true;  // 吞掉抬起
    }

    return false;
}

}  // namespace win32
}  // namespace platform
}  // namespace ktile
