// KTile - Win32 平台层：轮盘菜单鼠标钩子（WH_MOUSE_LL）
//
// 钩子装在专用线程上。低级钩子回调在安装它的线程里执行，而「显示轮盘」要查
// 前台进程、建 D2D 资源、解码图标，首次动辄几百毫秒；装在 UI 线程期间回调
// 会被系统静默丢弃，丢掉「抬起」就留下残影或误弹。回调因此只做纯内存操作
// 加 PostMessage，UI 再慢也影响不到事件接收。
//
// 两条不能碰的约束：
//   - 补发原始点击不能写在回调里。SendInput 在低级钩子回调中有死锁风险，
//     改成 PostThreadMessage 回本线程消息循环再补，回调立刻返回。
//   - 触发键的物理状态问不出来。手势开始时按下事件已被吞掉，而
//     GetAsyncKeyState 要等事件派发到目标队列才更新。只能由状态机记账，
//     靠「重按一次收残影 / Tracking 有寿命上限 / 窗口可见性回灌」兜底。
//
#pragma once

#include "core/radial_menu.h"  // RadialTriggerConfig

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ktile {
namespace platform {
namespace win32 {

// 通知消息（投递到 notifyWindow_ 的窗口过程）：
//   kRadialShowMessage    拖动超过阈值，wParam=起点X, lParam=起点Y
//   kRadialMoveMessage    光标移动，wParam=当前X, lParam=当前Y
//   kRadialReleaseMessage 按键抬起，wParam=当前X, lParam=当前Y
//   kRadialAbortMessage   手势作废，把轮盘收掉（wParam 为 RadialAbortReason）。
//                         不带坐标：语义是「不选中任何东西」。
inline constexpr UINT kRadialShowMessage    = WM_APP + 0x71U;
inline constexpr UINT kRadialMoveMessage    = WM_APP + 0x72U;
inline constexpr UINT kRadialReleaseMessage = WM_APP + 0x73U;
inline constexpr UINT kRadialAbortMessage   = WM_APP + 0x74U;

// 作废原因，写进日志用：都意味着抬起事件没走到钩子，
// 是排查「松开右键后轮盘还赖着」的唯一线索。
enum class RadialAbortReason : WPARAM {
    // 又按下了触发键，而上一次手势还没结束：上一次的抬起丢了
    LostRelease = 1,
    // 按住太久都没越过拖动阈值，按过期处理（见 kMaxTrackingMs）
    StaleGesture = 2,
};

class RadialMouseHook {
public:
    RadialMouseHook() = default;

    RadialMouseHook(const RadialMouseHook&) = delete;
    RadialMouseHook& operator=(const RadialMouseHook&) = delete;

    ~RadialMouseHook();

    // 安装钩子。trigger 为 enabled=false 时等同 uninstall()。
    // blacklist 命中的进程完全放行，既不吞事件也不弹轮盘。
    // 内部起专用钩子线程，本函数等它装好才返回（最多 5 秒）。
    // 只能在 UI 线程调用：会 join 上一次的线程。
    bool install(HWND notifyWindow, const core::RadialTriggerConfig& trigger,
                 const std::vector<std::string>& blacklist);

    void uninstall() noexcept;

    bool isInstalled() const noexcept {
        return installed_.load(std::memory_order_acquire);
    }

    // 状态机是否还处在一次手势里（Tracking / Shown）。
    //
    // 应用层的兜底用它判定「两侧状态不一致」：轮到主线程处理时窗口还亮着、
    // 钩子已回 Idle，说明 kRadialReleaseMessage 没走到（PostMessage 失败、
    // 窗口刚被重建等），可以把窗口收掉，不用干等超时。
    // 它回答的是钩子看到的手势进度，不是物理按键状态，见文件头说明。
    bool isGestureActive() const noexcept {
        return state_.load(std::memory_order_acquire) != State::Idle;
    }

    // 主线程告知「轮盘此刻是否真的亮着」。
    //
    // 状态机和窗口是两套状态，抬起事件丢掉时会分家（窗口亮着、状态已回
    // Idle，或反过来），分家后两边都无法独立发现问题。回灌可见性，
    // 抬起时就能拿它对账。原子写，UI 线程可随时调（钩子线程读）。
    void setWheelVisible(bool visible) noexcept {
        wheelVisible_.store(visible, std::memory_order_release);
    }

    // 供钩子回调使用（文件级单例转发，只在钩子线程上跑）
    bool handleMouseEvent(WPARAM message, const MSLLHOOKSTRUCT& event);

    // 请求补发一次原生点击（线程安全，任意线程可调）。
    //
    // 只用于按下后没越过激活阈值就松手：压根没唤出轮盘，用户是普通右键。
    // 轮盘亮过之后一律不补发：松手就是取消，再补一次点击会让后台窗口
    // 凭空弹出上下文菜单（见 handleMouseEvent）。
    // 真正的 SendInput 在钩子线程的消息循环里做，调用方立刻返回。
    void requestReinjectClick() const noexcept;

private:
    enum class State : std::uint8_t {
        Idle = 0,
        Tracking = 1,   // 按键已按下，未超阈值
        Shown = 2,      // 已超阈值，轮盘已显示
    };

    // 钩子线程专用消息：把「补发原生点击」挪出回调（见文件开头说明）。
    static constexpr UINT kReinjectMessage = WM_APP + 0x75U;

    // 钩子线程主体：建消息队列 → 装钩子 → 泵消息 → 卸载钩子。
    void threadMain();

    HHOOK hook_ = nullptr;   // 只在钩子线程上读写
    HWND notifyWindow_ = nullptr;
    core::RadialTriggerConfig trigger_;
    // 进程黑名单副本：装钩子时拷一份，配置变更靠重新 install 生效。
    // 回调里不能碰应用层对象（跨线程且可能已销毁）。
    std::vector<std::string> blacklist_;

    // 状态机。只有钩子线程写，UI 线程通过 isGestureActive() 读，故用原子。
    std::atomic<State> state_{State::Idle};
    // 下面三个只在钩子线程上用，不需要原子
    POINT startPoint_{};
    POINT currentPoint_{};
    // 按下时刻。只用于「按住太久还没拖」的过期判定，见 kMaxTrackingMs。
    ULONGLONG armedTick_ = 0;

    // 本次按下序列里，轮盘到底亮过没有。
    //
    // abort / 过期判定会把状态机打回 Idle，之后抬起落到「不在跟踪中」分支，
    // 原本会放行一个没有配对按下的右键抬起，Explorer 这类程序照样弹菜单
    // （看起来像「轮盘明明弹出来了，松手却又给我一个右键菜单」）。
    // 记这个事实，抬起时才知道该不该吞。
    bool wheelShownThisGesture_ = false;

    // 主线程回灌的窗口可见性（见 setWheelVisible）
    std::atomic<bool> wheelVisible_{false};
    std::atomic<bool> installed_{false};

    // 线程生命周期
    std::atomic<bool> stop_{false};
    std::atomic<DWORD> threadId_{0};
    std::thread thread_;
    std::mutex startMutex_;
    std::condition_variable startCondition_;
    bool startReady_ = false;   // 受 startMutex_ 保护
    bool startOk_ = false;      // 受 startMutex_ 保护

    // 自身注入的事件签名（避免重复处理自己注入的点击）
    static constexpr ULONG_PTR kInjectSignature = 0x4B545244;  // "KTRD"

    static RadialMouseHook* active_;

    // 低级鼠标钩子回调（文件级静态，转发到 active_ 实例）
    static LRESULT CALLBACK mouseHookProc(int code, WPARAM wParam, LPARAM lParam);

    // 事件是否属于配置的触发键。
    //
    // WM_XBUTTONDOWN / WM_XBUTTONUP 把两个侧键合并成同一个消息，只能靠
    // MSLLHOOKSTRUCT::mouseData 高位字区分：右键/中键比消息类型即可，
    // 侧键必须再比 mouseData，否则配了侧键2 的人按侧键1 也会被吞。
    bool matchesTriggerButton(const MSLLHOOKSTRUCT& event) const noexcept;

    // 这条消息是不是「触发键被按下」。
    //
    // 不能只比 WM_RBUTTONDOWN：快速连点时系统把第二次判成双击
    // （WM_RBUTTONDBLCLK）。漏掉就等于漏吞一次按下，前台窗口收到无配对的
    // 右键按下（Explorer 这类程序会当成拖拽/框选的起点）。
    bool isTriggerDownMessage(WPARAM message) const noexcept;

    // 检查修饰键是否满足配置
    bool modifierSatisfied() const noexcept;

    // 抑制键当前是否按着（按着就不弹轮盘，事件原样放行）。
    //
    // 可以放心用 GetAsyncKeyState：文件头「吞掉事件后问不出物理状态」
    // 的警告只针对鼠标键，抑制键是键盘键，既没装键盘钩子也没吞键盘事件。
    bool suppressionActive() const noexcept;

    // 前台进程是否在黑名单里（是则整条放行，连状态机都不进）。
    bool foregroundBlocked() const noexcept;

    // 把触发键的按下+抬起重新注入一次（恢复原生点击）。
    // 只能在钩子线程、回调之外调用（见 requestReinjectClick）。
    void reinjectClick() const;

    // 当前配置的按键对应的按下/抬起消息
    WPARAM downMessage() const noexcept;
    WPARAM upMessage() const noexcept;

    // Tracking 的寿命上限。正常手势几十到一百毫秒就会越过阈值；停留两秒
    // 还没越过，只能是抬起事件丢了、状态机卡在 Tracking，之后任意一次
    // 小移动会突然弹出轮盘。过期按 Idle 处理，代价是按住超过两秒再拖
    // 这一次不出轮盘（Shown 不受此限）。
    static constexpr ULONGLONG kMaxTrackingMs = 2000;
};

}  // namespace win32
}  // namespace platform
}  // namespace ktile
