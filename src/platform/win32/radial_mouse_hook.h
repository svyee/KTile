// KTile - Win32 平台层：轮盘菜单鼠标钩子
//
// 职责：
//   1. 用 WH_MOUSE_LL 监听指定鼠标键（右 / 中 / 侧键）的按下/移动/抬起。
//   2. 按下时检查修饰键条件（Ctrl/Shift/Alt），不满足则放行（让 MouseInc 等
//      手势软件正常工作）；满足则吞掉事件并进入跟踪。
//   3. 拖动超过阈值时通知主线程显示轮盘；继续移动更新光标位置。
//   4. 抬起时通知主线程最终位置（由主线程判定命中哪个扇区或取消）。
//   5. 若未超过阈值且触发方式无修饰键，重新注入一次原始点击（让原生右键菜单
//      正常弹出）；有修饰键时不注入（用户明确是想做手势，不弹原生菜单）。
//
// ---------------------------------------------------------------- 为什么用专用线程
//
// WH_MOUSE_LL 的回调运行在**安装钩子的那个线程**上。1.2.0 里它装在 UI 线程，
// 而「显示轮盘」是同步的：查前台进程、建 D2D 资源、解码图标、排标签、首帧绘制
// —— 首次尤其慢。这段时间 UI 线程不泵消息，系统等不及就把事件**静默丢掉**，
// 没有任何通知。
//
// 实测（build/probe_busy.cpp）：钩子线程忙等 600ms，期间从高优先级线程注入的
// 按下事件，3 次实验 3 次都没送到回调。丢掉的若是「抬起」：
//   - 状态机停在 Shown：轮盘一直挂在屏幕上跟着鼠标高亮（「我松手了它还在」）；
//   - 状态机停在 Tracking：之后任意一次 8 像素移动把它弹出来（「单击也弹」）。
// 这就是用户报的那个 bug。
//
// 所以钩子搬进专用线程（与 StarPie 的 MouseHook 一致：独立 _hookThread、
// AboveNormal 优先级、先用 PeekMessage 建好消息队列、WM_QUIT + Join 收尾）。
// 回调只做纯内存操作 + PostMessage，UI 那边慢多久都影响不到事件接收。
//
// 补发点击也**不能在回调里做**：SendInput 在低级钩子回调里调用有死锁风险
// （StarPie 也是丢到线程池去补发的）。这里改成给自己线程 PostThreadMessage，
// 回到消息循环里再补，回调立刻返回。
//
// ---------------------------------------------------------------- 仍然问不出物理按键状态
//
// 「触发键现在物理上还按着吗」在本模块里问不出来，别试：手势开始时我们吞掉了
// 按下事件，而 GetAsyncKeyState 的鼠标键状态要等事件真正派发到目标队列才更新。
// 实测（build/probe_phys.cpp）：同一个实验，钩子放行时 Async 能正确变 DOWN，
// 钩子吞掉后全程回答 up。
//
// 所以「用户还按着吗」只能靠状态机自己记账，外加下面几条兜底对账：
//   1. 再按一次触发键 = 上一次的抬起丢了 → 先发 abort 收掉残影；
//   2. Tracking 有寿命上限（kMaxTrackingMs），过期作废；
//   3. 应用层把窗口真实可见性回灌到 setWheelVisible()，状态机与窗口分家时
//      以窗口为准收掉它。
// 专用线程把「抬起被丢」的概率压到接近零，这几条只兜剩下的意外（系统临时
// 移除钩子、钩子线程被极端抢占等）。
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

// 通知消息（投递到 notifyWindow_ 的窗口过程）
//   kRadialShowMessage    —— 拖动超过阈值，wParam=起点X, lParam=起点Y
//   kRadialMoveMessage    —— 光标移动，wParam=当前X, lParam=当前Y
//   kRadialReleaseMessage —— 按键抬起，wParam=当前X, lParam=当前Y
//   kRadialAbortMessage   —— 这次手势已经作废，请把轮盘收掉（wParam 见
//                            RadialAbortReason）。不带坐标：它要表达的是
//                            「别选中任何东西」，而不是「在某个位置抬起」。
inline constexpr UINT kRadialShowMessage    = WM_APP + 0x71U;
inline constexpr UINT kRadialMoveMessage    = WM_APP + 0x72U;
inline constexpr UINT kRadialReleaseMessage = WM_APP + 0x73U;
inline constexpr UINT kRadialAbortMessage   = WM_APP + 0x74U;

// 作废原因。写进日志用 —— 这三种情况都意味着「抬起事件没走到钩子里」，
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
    // blacklist 是进程黑名单（命中即完全放行，既不吞事件也不弹轮盘）。
    //
    // 内部起一个专用钩子线程，本函数等它装好才返回（最多 5 秒）。
    // 只能在 UI 线程调用：会 join 上一次的线程。
    bool install(HWND notifyWindow, const core::RadialTriggerConfig& trigger,
                 const std::vector<std::string>& blacklist);

    void uninstall() noexcept;

    bool isInstalled() const noexcept {
        return installed_.load(std::memory_order_acquire);
    }

    // 状态机是否还处在一次手势里（Tracking / Shown）。
    //
    // 应用层的兜底用它做「两侧状态不一致」的判定：轮到主线程处理时窗口还
    // 亮着、但钩子这边已经回到 Idle，说明抬起事件被我们处理掉了、而
    // kRadialReleaseMessage 没走到（PostMessage 失败、窗口刚被重建等），
    // 这时可以把窗口收掉，不用干等超时。
    //
    // 注意它回答的是「钩子自己看到的手势进度」，不是物理按键状态 ——
    // 见文件开头那段说明。
    bool isGestureActive() const noexcept {
        return state_.load(std::memory_order_acquire) != State::Idle;
    }

    // 主线程告知「轮盘此刻是否真的亮着」。
    //
    // 为什么需要双向同步：状态机和窗口是两套状态，抬起事件丢掉时它们会分家
    // （窗口亮着、状态已回 Idle，或者反过来）。分家之后光看状态机永远发现
    // 不了「有个轮盘赖在屏幕上」，光看窗口也不知道该不该执行扇区动作。
    // 把窗口可见性回灌进来，抬起时就能拿它做对账。
    //
    // 线程安全：原子写，UI 线程可随时调（钩子线程读）。
    void setWheelVisible(bool visible) noexcept {
        wheelVisible_.store(visible, std::memory_order_release);
    }

    // 供钩子回调使用（文件级单例转发，只在钩子线程上跑）
    bool handleMouseEvent(WPARAM message, const MSLLHOOKSTRUCT& event);

    // 请求补发一次原生点击（线程安全，任意线程可调）。
    //
    // 用在两处：钩子自己在「按下没超阈值」时补发；应用层在「轮盘亮过但
    // 全程没碰任何扇区、又在中心松手」时补发（见 releaseAt 的返回值）。
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
    // 进程黑名单副本。装钩子时拷一份，配置变更靠重新 install 生效 ——
    // 回调里不能去碰应用层的对象（跨线程且可能已被销毁）。
    std::vector<std::string> blacklist_;

    // 状态机。只有钩子线程写，UI 线程通过 isGestureActive() 读，故用原子。
    std::atomic<State> state_{State::Idle};
    // 下面三个只在钩子线程上用，不需要原子
    POINT startPoint_{};
    POINT currentPoint_{};
    // 按下时刻。只用于「按住太久还没拖」的过期判定，见 kMaxTrackingMs。
    ULONGLONG armedTick_ = 0;

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
    // WM_XBUTTONDOWN / WM_XBUTTONUP 把侧键1 与侧键2 合并成同一个消息，
    // 只能靠 MSLLHOOKSTRUCT::mouseData 的高位字区分。因此右键/中键比消息
    // 类型即可，侧键还必须比 mouseData —— 否则配了侧键2 的人按侧键1 也会
    // 被吞掉并呼出轮盘。
    bool matchesTriggerButton(const MSLLHOOKSTRUCT& event) const noexcept;

    // 这条消息是不是「触发键被按下」。
    //
    // 不能只比 WM_RBUTTONDOWN：快速连点两次时系统把第二次判定成双击，消息
    // 变成 WM_RBUTTONDBLCLK。漏掉它就等于漏掉一次按下 —— 而且那次按下没被
    // 吞掉，前台窗口会收到一个没有配对的右键按下（Explorer 这类程序会把它
    // 当成拖拽/框选的起点）。
    bool isTriggerDownMessage(WPARAM message) const noexcept;

    // 检查修饰键是否满足配置
    bool modifierSatisfied() const noexcept;

    // 抑制键当前是否按着（按着就不弹轮盘，事件原样放行）。
    //
    // 这里**可以**放心用 GetAsyncKeyState —— 文件开头那条「吞掉事件后
    // 问不出物理状态」的警告只针对鼠标键：抑制键是键盘键，我们既没装
    // 键盘钩子、也没吞任何键盘事件，它的异步状态是准确的。
    bool suppressionActive() const noexcept;

    // 前台进程是否在黑名单里（是则整条放行，连状态机都不进）。
    bool foregroundBlocked() const noexcept;

    // 把触发键的按下+抬起重新注入一次（恢复原生点击）。
    // **只能在钩子线程、回调之外调用**（见 requestReinjectClick）。
    void reinjectClick() const;

    // 当前配置的按键对应的按下/抬起消息
    WPARAM downMessage() const noexcept;
    WPARAM upMessage() const noexcept;

    // Tracking 的寿命上限。
    //
    // 一次正常手势从按下到越过阈值是几十一百毫秒的事（阈值最小 4 像素）。
    // 停留两秒还没越过，只能是「抬起事件丢了、状态机卡在 Tracking」——
    // 那种状态平时毫无表现，却在之后任意一次小移动里突然弹出轮盘，是最
    // 容易让人以为「这软件抽风」的形态。过期后按 Idle 处理：这一次手势
    // 不成立，用户重新按一次就好。
    //
    // 代价：按住右键思考两秒以上再拖，这一次不会出轮盘。两秒留得足够宽 ——
    // 真要看扇区内容也是先拖出来、在轮盘上停着看（那是 Shown，不受此限）。
    static constexpr ULONGLONG kMaxTrackingMs = 2000;
};

}  // namespace win32
}  // namespace platform
}  // namespace ktile
