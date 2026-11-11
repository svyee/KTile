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
// 钩子回调纪律（与 GestureHotkey / OutsideClickMonitor 一致）：
//   - 只做纯内存操作 + PostMessage，严禁文件 I/O 或任何耗时操作。
//   - 回调在注册线程（消息循环线程）上执行，与窗口过程同线程，无需加锁。
//
// 唯一的例外是黑名单判断：按下瞬间要知道前台进程名才决定吞不吞，这必须
// 同步完成（异步问一句就晚了，事件已经进/不进目标队列了）。它只在按键
// **按下**那一次发生，不是每个 mouse move 都查；QueryFullProcessImageNameW
// 读的是内核里已有的映像路径，微秒级。查不到进程名（提权窗口等）按「不拉黑」
// 处理 —— 宁可弹一次，也别让功能静默失效。
//
#pragma once

#include "core/radial_menu.h"  // RadialTriggerConfig

#include <Windows.h>

#include <string>
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

// 触发键「物理上还按着吗」这件事，在本模块里是**问不出来**的，别试：
//
// 手势开始时我们吞掉了按下事件，而 GetAsyncKeyState / GetKeyState 的鼠标键
// 状态是在事件**通过钩子链、进入目标队列**时才更新的。事件被吞 = 系统从未
// 认为这个键被按下过，于是整个拖动期间它都回答「没按下」。
//
// 1.2.0 就栽在这里：兜底定时器每 120ms 问一次物理状态，答案永远是「松开了」，
// 轮盘刚出现就被强制收起，用户根本来不及选（日志里能看到连续 4 条
// 「抬起事件丢失」）。应用层现在改成用 isGestureActive() + 可见时长上限兜底。
//
// ---- 由此带来的一个真实故障：抬起事件也会丢 ----
//
// 同理，系统会**静默跳过超时的钩子回调**（回调跑在装钩子的线程上，也就是
// UI 线程；UI 线程忙于显示轮盘时，那一段时间里的事件可能不经过我们，而且
// 没有任何通知）。被跳过的是「抬起」时，状态机就停在 Shown/Tracking 上：
//   - 停在 Shown：轮盘一直挂在屏幕上，还跟着鼠标高亮（用户看到的是
//     「我明明松开右键了，一动鼠标轮盘还在」）；
//   - 停在 Tracking：轮盘当时没出来，之后任意一次 8 像素的移动都会突然
//     把它弹出来（用户看到的还是同一句话）。
// 物理状态问不出来，所以**不能靠问**，只能靠三条自愈规则：
//   1. 再按一次触发键 = 铁证上一次的抬起丢了 → 先发 abort 收掉残影；
//   2. Tracking 有寿命上限（kMaxTrackingMs），过期就作废，不让它变成
//      埋在未来的地雷；
//   3. 应用层把窗口真实可见性回灌到 setWheelVisible()，抬起时若「窗口亮着
//      但状态已经回 Idle」，同样发 abort —— 状态机与窗口不一致时，
//      以窗口为准收掉它。

class RadialMouseHook {
public:
    RadialMouseHook() = default;

    RadialMouseHook(const RadialMouseHook&) = delete;
    RadialMouseHook& operator=(const RadialMouseHook&) = delete;

    ~RadialMouseHook();

    // 安装钩子。trigger 为 enabled=false 时等同 uninstall()。
    // blacklist 是进程黑名单（命中即完全放行，既不吞事件也不弹轮盘）。
    bool install(HWND notifyWindow, const core::RadialTriggerConfig& trigger,
                 const std::vector<std::string>& blacklist);

    void uninstall() noexcept;

    bool isInstalled() const noexcept { return hook_ != nullptr; }

    // 状态机是否还处在一次手势里（Tracking / Shown）。
    //
    // 应用层的兜底用它做「两侧状态不一致」的判定：轮到主线程处理时窗口还
    // 亮着、但钩子这边已经回到 Idle，说明抬起事件被我们处理掉了、而
    // kRadialReleaseMessage 没走到（PostMessage 失败、窗口刚被重建等），
    // 这时可以把窗口收掉，不用干等超时。
    //
    // 注意它回答的是「钩子自己看到的手势进度」，不是物理按键状态 ——
    // 见文件开头那段说明。
    bool isGestureActive() const noexcept { return state_ != State::Idle; }

    // 主线程告知「轮盘此刻是否真的亮着」。
    //
    // 为什么需要双向同步：状态机和窗口是两套状态，抬起事件丢掉时它们会分家
    // （窗口亮着、状态已回 Idle，或者反过来）。分家之后光看状态机永远发现
    // 不了「有个轮盘赖在屏幕上」，光看窗口也不知道该不该执行扇区动作。
    // 把窗口可见性回灌进来，抬起时就能拿它做对账。
    //
    // **只允许在装钩子的那个线程调用**（本模块的安装线程就是 UI 线程），
    // 与钩子回调同线程，因此不需要加锁。
    void setWheelVisible(bool visible) noexcept { wheelVisible_ = visible; }

    // 供钩子回调使用（文件级单例转发）
    bool handleMouseEvent(WPARAM message, const MSLLHOOKSTRUCT& event);

private:
    enum class State : std::uint8_t {
        Idle = 0,
        Tracking = 1,   // 按键已按下，未超阈值
        Shown = 2,      // 已超阈值，轮盘已显示
    };

    HHOOK hook_ = nullptr;
    HWND notifyWindow_ = nullptr;
    core::RadialTriggerConfig trigger_;
    // 进程黑名单副本。装钩子时拷一份，配置变更靠重新 install 生效 ——
    // 回调里不能去碰应用层的对象（跨线程且可能已被销毁）。
    std::vector<std::string> blacklist_;

    State state_ = State::Idle;
    POINT startPoint_{};
    POINT currentPoint_{};
    // 按下时刻。只用于「按住太久还没拖」的过期判定，见 kMaxTrackingMs。
    ULONGLONG armedTick_ = 0;
    // 主线程回灌的窗口可见性（见 setWheelVisible）
    bool wheelVisible_ = false;

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

    // 把虚拟键码对应的鼠标按键重新注入（用于阈值内抬起时恢复原生点击）
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
