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
    // 先把上一份配置的线程收干净：状态机、配置、黑名单都靠「重新 install」
    // 生效，两代同时活着会互相抢事件。
    uninstall();

    notifyWindow_ = notifyWindow;
    trigger_ = trigger;
    blacklist_ = blacklist;
    state_.store(State::Idle, std::memory_order_relaxed);
    armedTick_ = 0;
    wheelShownThisGesture_ = false;
    // 全新的一次安装：窗口可见性由应用层随后回灌，这里先按「没亮」算。
    // 如果轮盘这时其实还亮着，应用层下一次 setWheelVisible 会纠正回来。
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
        // 线程没起来（或超时）：把它收掉，别留一个半死不活的线程
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
        // 有界等待：钩子线程正常情况下收到 WM_QUIT 立刻退出；万一它卡在某个
        // 系统调用里，也不能把 UI 线程（本函数从 UI 线程调用）一起拖死。
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
    // 先建好本线程的消息队列：否则 install() 收尾时 PostThreadMessage(WM_QUIT)
    // 会因为「队列还不存在」而直接失败，线程就会一直卡在 GetMessage 里。
    MSG probe{};
    PeekMessageW(&probe, nullptr, 0, 0, PM_NOREMOVE);

    const DWORD tid = GetCurrentThreadId();
    threadId_.store(tid, std::memory_order_release);
    // 比 UI 线程略高一点：事件必须在系统超时之前被取走，绘制晚几毫秒无所谓。
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
        // 线程消息（无窗口）自己处理：补发点击必须回到消息循环里做，
        // 不能在钩子回调里调 SendInput（会死锁）。见头文件说明。
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
    // 状态机只有本线程（钩子线程）写，读一次就够；原子是因为 UI 线程的
    // isGestureActive() 也在读。
    const State state = state_.load(std::memory_order_relaxed);

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
        // 抬起到我们手上的路断了（见头文件「为什么用专用线程」）。
        // 让主线程把残影收掉，再当作全新的一次按下 —— 否则状态机会带着
        // 上一次的起点继续跑，窗口也会留在屏幕上跟着鼠标走。
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
                    // 从这一刻起，这一次按下的结果就交给轮盘了：抬起必须吞掉，
                    // 即使随后状态机被 abort / 过期打回 Idle 也一样。
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
            // 移动事件必须放行：低级鼠标钩子吞掉 WM_MOUSEMOVE 会导致系统
            // 不再更新光标位置，用户会感觉鼠标"卡死"。我们只旁路观察位置，
            // 不修改事件流。
            return false;
        }
    }

    // 抬起单独处理，不放在 `state != Idle` 里面。
    //
    // 为什么：状态机随时可能被 abort、被过期判定、被重装钩子打回 Idle，
    // 而这一次按下确实唤出过轮盘（wheelShownThisGesture_）。那时抬起落到
    // 「不在跟踪中」那一支上会原样放行 —— 前台窗口收到一个没有配对按下的
    // 右键抬起，Explorer 这类程序照样弹上下文菜单。用户看到的就是
    // 「轮盘刚消失，右键菜单又冒出来」，而且时有时无，全看那一次有没有被
    // 打回 Idle。只要轮盘亮过，抬起就一律吞掉。
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
            // 轮盘亮过，但状态机已经回了 Idle（中间被 abort / 重装钩子
            // 重置过）。让主线程收掉残影，绝不注入原生点击 —— 用户做的是
            // 一次轮盘操作，再弹一个右键菜单等于凭空多一次点击。
            PostMessageW(notifyWindow_, kRadialAbortMessage,
                         static_cast<WPARAM>(RadialAbortReason::LostRelease), 0);
        } else if (state == State::Idle) {
            // 既没在跟踪、轮盘也没亮过：这不是我们管的那一次手势，放行。
            // （函数开头已经拦掉自身注入的事件，这里不会自激。）
            return false;
        } else {
            // 未超阈值：恢复原生点击。
            // 不在这里直接 SendInput：SendInput 在低级钩子回调里调用有
            // 死锁风险（原始输入线程正等着我们这个回调返回）。给自己投
            // 个消息，回到消息循环里再补。
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
