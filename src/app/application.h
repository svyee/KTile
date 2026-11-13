// KTile - 应用层：主控制器
//
// 职责：把各层组装成可运行的程序，管理生命周期。
// 不包含具体业务细节（那些在 core / ui 里）。
//
#pragma once

#include "app/plugin_controller.h"
#include "core/data_model.h"
#include "core/radial_menu.h"
#include "infrastructure/filesystem/portable_paths.h"
#include "infrastructure/json/config_store.h"
#include "infrastructure/json/radial_store.h"
#include "infrastructure/radial_plugin_store.h"
#include "platform/win32/drop_target.h"
#include "platform/win32/global_hotkey.h"
#include "platform/win32/radial_mouse_hook.h"
#include "platform/win32/single_instance.h"
#include "platform/win32/startup_registration.h"
#include "platform/win32/tray_icon.h"

#include <Windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ktile {
namespace ui {
class LauncherWindow;
class RadialMenuWindow;
}

namespace app {

// 应用标识
inline constexpr const wchar_t* kApplicationName = L"KTile";

// 主窗口标题（任务栏 / Alt-Tab / 关于对话框标题栏显示的名字）。
// 语言可切换，所以是函数而不是常量（常量会固化成编译期的那一种语言）。
const wchar_t* applicationWindowTitle();

// 版本号。改这里时记得同步 app.rc 里的 VERSIONINFO
// （rc 文件必须保持纯 ASCII，中文版本描述只放 About 对话框）。
inline constexpr const wchar_t* kApplicationVersion = L"1.3.1";
inline constexpr int kApplicationVersionMajor = 1;
inline constexpr int kApplicationVersionMinor = 3;
inline constexpr int kApplicationVersionPatch = 1;
inline constexpr const wchar_t* kSingleInstanceMutexName =
    L"KTile.SingleInstance.{8F3A1C2D-4B5E-4A6F-9C8D-7E6F5A4B3C2D}";
inline constexpr const wchar_t* kStartupRegistryValueName = L"KTile";

// 热键 ID
inline constexpr int kToggleHotkeyId = 1;
// F7 切换前台窗口置顶（同一条 WM_HOTKEY 下游，靠 ID 区分）
inline constexpr int kTopmostHotkeyId = 2;

// 托盘回调消息（消息窗口使用）
inline constexpr UINT WM_TRAYICON_CALLBACK = WM_APP + 100;

// 自动化 / 调试开关集合。只在自动化里用，正常启动一个都不开；
// 汇总成结构体避免 run() 出现一长串看不出含义的 true/false。
struct DebugOptions {
    // --debug-shot：渲染导出 PNG 后立刻退出（见 main.cpp 的说明）
    bool shot = false;
    // --debug-shot=x,y：模拟悬停的客户区坐标（<0 表示不悬停）
    int hoverX = -1;
    int hoverY = -1;
    // --debug-shot=x,y@sx,sy@dx,dy：模拟拖拽的来源与落点（<0 表示不模拟）
    int dragSrcX = -1;
    int dragSrcY = -1;
    int dragDstX = -1;
    int dragDstY = -1;
    // --debug-search=<文本>：预置搜索查询
    std::string search;
    // --debug-settings：打开设置窗口
    bool settings = false;
    // --debug-about：打开「关于」对话框
    bool about = false;
    // --debug-settings-keepalive：打开设置后继续跑主循环
    bool settingsKeepAlive = false;
    // --debug-radial-editor：直接打开「轮盘菜单设置」对话框（供自动化截图）
    bool radialEditor = false;
    // --debug-editor：打开第一个磁贴的「修改项目」对话框
    bool editor = false;
    // --debug-new-item：打开「添加项目」对话框（新建模式）
    bool newItem = false;
    // --debug-drop=x,y：模拟一次「拖入落点」（客户区坐标，<0 表示不模拟）。
    // 单独用时点亮落点预览（供截图）；与 --debug-add 同用时，新项目落在
    // 这一格，走与真实拖放完全相同的下游。
    int dropX = -1;
    int dropY = -1;
    // --debug-add=<路径1;路径2;...>：启动后走一次「拖入添加」流程
    // （与真实拖放共用同一条代码路径，只是不经过 OLE 管道）
    std::vector<std::wstring> addPaths;
    // --debug-opacity=<50..100>：主窗口不透明度（0 = 不改），
    // 验证「分层窗口 + D2D 渲染目标」在真实窗口上是否成立。
    int opacityPercent = 0;
    // --debug-radial：渲染轮盘菜单到 radial-shot.png 后退出（视觉验证）。
    // 可选 =x,y 指定高亮扇区的相对中心偏移（DIP），如 --debug-radial=0,108。
    bool radial = false;
    int radialHoverX = 0;
    int radialHoverY = 0;
    // --debug-radial-plugin=<id>：配合 --debug-radial 渲染插件的菜单
    // 而不是用户的通用轮盘。
    std::string radialPluginId;
    // --debug-radial-scroll=<ms>：配合 --debug-radial，钉住超宽标签的
    // 滚动相位，否则时间相关的滚动让导出截图无法自动比对。
    int radialScrollMs = 0;

    bool any() const noexcept {
        return shot || settings || settingsKeepAlive || radialEditor || about ||
               editor || newItem || !search.empty() || !addPaths.empty() ||
               dragSrcX >= 0 || opacityPercent > 0 || radial ||
               !radialPluginId.empty();
    }
};

class Application {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // 运行主循环。返回进程退出码。
    int run(HINSTANCE instance, const DebugOptions& debug = DebugOptions());

    // ---- 供消息窗口过程调用 ----
    void toggleLauncher();
    void showTrayMenu();
    void handleTrayCommand(UINT commandId);
    // F7：切换前台窗口的置顶状态（WS_EX_TOPMOST 取反）
    void toggleTopmost();

    // 重装手势钩子（reason 只用于写日志）。WH_KEYBOARD_LL 钩子链是 LIFO，
    // Explorer 重启会把它的钩子排到前面使吞 Win 键失效，系统高负载时还会
    // 静默跳过超时钩子；TaskbarCreated 广播、休眠恢复、看门狗都调这里
    // 抢回顺序。非手势热键（普通 RegisterHotKey）不受钩子链影响，直接返回。
    void reinstallGestureHotkey(const char* reason);
    // 重装轮盘鼠标钩子（reason 只用于写日志）。失效原因同上，表现是
    // Ctrl+右键突然没反应。轮盘显示期间不重装：重装会重置状态机，
    // 截断进行中的手势。
    void reinstallRadialHook(const char* reason);
    // 消息窗口定时器回调（日志落盘 / 钩子看门狗）
    void onMessageTimer(UINT_PTR timerId);
    // 插件刷新完成（工作线程 PostMessage 到消息窗口后由主线程调用）
    void onPluginDataReady();
    // 面板可见期间的高频刷新（按插件清单的 visibleRefreshSeconds）
    void onPluginVisibleTick();
    // 休眠恢复：钩子链顺序可能被电源事件打乱，重装保险一次
    void onPowerResume();

    // ---- 轮盘菜单（由消息窗口过程调用） ----
    void onRadialShow(POINT startPoint);
    void onRadialMove(POINT currentPoint);
    void onRadialRelease(POINT currentPoint);
    // 钩子报「这次手势作废」：收起轮盘但不执行任何扇区动作，
    // 用于抬起事件丢失后的对账（见 radial_mouse_hook.h）。
    void onRadialAbort(int reason);
    // 收起轮盘并清理与它绑定的临时状态（动作分发指针、兜底定时器、计时）
    void hideRadialMenu();
private:
    // 各初始化步骤，任一步失败则终止启动
    bool initializeConfig();
    bool initializeWindow();
    bool initializeTray();
    bool initializeHotkey();
    // 注册/注销 F7 置顶热键。被占用等失败只记日志，不影响启动
    bool initializeTopmostHotkey();
    // 托盘提示文字跟随当前唤醒方式（热键 / 手势）刷新
    void updateTrayTooltip();

    // 初始化目录 + 写启动横幅。automation 模式不建会话哨兵
    //（自动化并发多实例会误报「上次异常退出」）。
    void initializeLogging(bool automation);
    // 看门狗：手势钩子丢失就重装，并按周期强制重装一次
    //（系统静默跳过超时钩子且无通知，只能定期抢）。
    void onHookWatchdog();

    // 持久化：从磁盘加载配置与磁贴数据
    void loadPersistedState();
    // 加载轮盘菜单配置（radial.json）
    void loadRadialConfig();
    // 保存轮盘菜单配置
    void persistRadialConfig();
    // 扫描 <exe>/slugin/*/radial.json（按应用轮盘插件）
    void loadRadialPlugins();
    // 取某个轮盘插件的目录绝对路径；id 不存在时退回 slugin 根目录
    std::wstring radialPluginDirectoryOf(const std::string& id) const;
    // 调试用：取某个插件提供的菜单；找不到时退回通用轮盘
    const core::RadialMenu& debugRadialPluginMenu(const std::string& id);
    // 首次运行或数据缺失时植入示例磁贴（便于用户直观看到界面）
    void seedSampleDocument();

    // 跟随启动：把 config_.activation.followLaunch 里的程序逐个拉起，
    // 已在运行（按完整映像路径判定）的跳过，逐条写日志（这是排查
    // 「为什么我的程序没起来」唯一的线索）。条目靠 FollowLaunchEntry::itemId
    // 区分来源：手动添加的只认 target；从磁贴添加的按 id 取磁贴当前值
    //（参数/工作目录/管理员跟着磁贴走），磁贴已删退回添加时的快照。
    void launchFollowStartupItems();

    // 某块磁贴是否已在跟随启动清单里（右键菜单据此打勾）
    bool isFollowingStartup(const std::string& itemId) const;
    // 切换磁贴的跟随状态：未添加则加入（带参数/工作目录/管理员快照），
    // 已添加则移除。落盘但不启动，语义始终是「下次 KTile 启动时」。
    void toggleFollowStartup(const core::LaunchItem& item);
    // 用磁贴当前值刷新跟随清单里的快照（name/target/参数/工作目录/管理员）。
    // 磁贴被删的条目原样保留，删磁贴不会静默清掉开机启动项。
    // 返回是否真的改动了内容。
    bool syncFollowLaunchWithTiles();
    // 在全部标签页里按 id 找磁贴；找不到返回 nullptr（指针指向窗口文档，
    // 调用方当次用完即弃，别存）。
    const core::LaunchItem* findItemById(const std::string& itemId) const;
    // 把当前状态写回磁盘
    void persistItems();
    void persistConfig();

    // 皮肤与背景图：按 config_ 的记载把配色/背景应用到主窗口
    void applyThemeFromConfig();

    // 消息循环
    int runMessageLoop();

    // 行为
    void showLauncher();
    void hideLauncher();

    // 按「当前标签页里最短的 visibleRefreshSeconds」启停前台刷新定时器。
    // 面板隐藏或没有插件需要前台刷新时定时器会被关掉。
    void updatePluginVisibleTimer();

    // ---- 磁贴右键菜单 ----
    // 弹出菜单并分发到具体的动作
    void handleItemContextMenu(HWND owner, std::size_t itemIndex,
                               const core::LaunchItem& item);
    // 启动项目（菜单「运行/打开」与「以管理员身份运行」共用后端）。
    // forceAdministrator 为 true 时无视项目自身的管理员设置强制提权。
    // 失败时弹 MessageBox 报告原因。
    void launchItem(const core::LaunchItem& item, bool forceAdministrator);
    // 调整尺寸（含清坐标重排 + 落盘）
    void applyItemResize(std::size_t itemIndex, core::TileSize newSize);
    // 修改某块磁贴的标题显示方式（只影响标题画不画 + 落盘）
    void applyItemLabelMode(std::size_t itemIndex, core::TileLabelMode mode);
    // 修改项目（完整编辑对话框 + 落盘；支持改所属页面）
    void applyItemEdit(HWND owner, std::size_t itemIndex,
                       const core::LaunchItem& item);
    // 把当前页的某个磁贴搬到目标页（追加到目标页末尾，保留网格坐标）
    void applyItemMoveToTab(std::size_t itemIndex, std::size_t targetTab);
    void applyItemCopyToTab(std::size_t itemIndex, std::size_t targetTab);
    // 添加项目（复用同一个编辑对话框，新建模式 + 落盘）
    void applyItemAdd(HWND owner, const std::wstring& initialTarget);
    // 添加一块插件磁贴（插件来自 data/plugins 的清单，不需要编辑对话框）
    void addPluginItem(const std::string& pluginId);
    // 把拖进来的东西加进面板（文件/文件夹直接建磁贴；链接建成网址项目）。
    // dropPoint = 松手位置屏幕坐标，新磁贴落在光标所在格、压住的磁贴按
    // 避让规则让位；不给坐标（自动化/调试入口）时交给布局引擎找空位。
    void addDroppedSources(std::vector<platform::win32::DroppedSource> sources,
                           std::optional<POINTL> dropPoint);
    // 删除（弹确认框 + 落盘）
    void applyItemRemoval(HWND owner, std::size_t itemIndex,
                          const core::LaunchItem& item);

    // ---- 设置窗口 ----
    // 打开模态设置窗口，并把用户改动应用到配置与窗口。
    void openSettings();

    // ---- 轮盘菜单 ----
    // 执行轮盘扇区动作
    void executeRadialAction(const core::RadialAction& action);
    // 轮盘可见期间的兜底：触发键已物理松开但「抬起」事件丢了，就强制收起
    void onRadialWatchdog();

    // ---- 关于对话框 ----
    // 弹出「关于 KTile」（产品名、版本、联系方式）。
    void openAbout();

    HINSTANCE instance_ = nullptr;
    HWND messageWindow_ = nullptr;

    platform::win32::SingleInstance singleInstance_;
    platform::win32::GlobalHotkey hotkey_;
    // F7 置顶热键（与唤醒热键互不相干，各自独立注册/注销）
    platform::win32::GlobalHotkey topmostHotkey_;
    // 手势唤醒（双击 Ctrl / 单按 Win 键）。只在选中手势时安装。
    platform::win32::GestureHotkey gestureHotkey_;
    // 唤醒快捷键注册失败的原因（空 = 成功）。供启动提示说明具体是哪个键。
    std::wstring hotkeyErrorText_;
    platform::win32::TrayIcon tray_;

    // 拖放接收器（把文件/文件夹/链接拖到主窗口上即可添加）
    platform::win32::DropTarget dropTarget_;

    std::unique_ptr<ui::LauncherWindow> launcherWindow_;

    // 插件发现 / 懒加载刷新 / 缓存。没有插件时只是空转。
    PluginController pluginController_;
    // 前台刷新定时器当前生效的秒数（0 = 未启用），
    // 避免每次显示/切页都 KillTimer + SetTimer 一遍。
    int pluginVisibleTimerSeconds_ = 0;

    // 便携路径与持久化
    infrastructure::PortablePaths paths_;
    infrastructure::ConfigStore store_;
    bool pathsReady_ = false;

    // OLE 是否由本对象初始化（OleInitialize 成功才在析构里 OleUninitialize）。
    // 拖放（RegisterDragDrop）依赖 OLE，启动时必须先初始化。
    bool oleInitialized_ = false;

    // 运行日志是否已初始化（析构时据此决定是否 shutdown 日志模块）
    bool loggingReady_ = false;
    // 上一次安装/重装手势钩子的时刻（看门狗定期重装用）
    ULONGLONG lastGestureInstallTick_ = 0;
    ULONGLONG lastRadialInstallTick_ = 0;

    core::ApplicationConfig config_;
    core::ItemsDocument document_;

    // 本次启动实际生效的界面语言（"zh-Hans" / "en"）。config_.language
    // 可能是 "auto"，日志要记的是解析后的结果，不是配置值。
    std::string effectiveLanguage_;

    // ---- 轮盘菜单 ----
    core::RadialConfig radialConfig_;
    infrastructure::RadialStore radialStore_;
    // slugin/<id>/radial.json 提供的按应用轮盘。只读，启动时（以及用户点
    // 「重新扫描」时）加载一次。
    infrastructure::RadialPluginStore radialPluginStore_;
    platform::win32::RadialMouseHook radialHook_;
    std::unique_ptr<ui::RadialMenuWindow> radialWindow_;
    // 轮盘当前显示的方案（显示期间保持引用，避免悬空）
    const core::RadialMenu* activeRadialMenu_ = nullptr;
    // when 条件过滤后的显示副本。指向插件或用户配置的原菜单无需拷贝，
    // 只有过滤发生时才重写这里。
    core::RadialMenu shownRadialMenu_;
    // 轮盘出现时刻（兜底的可见时长上限用）；0 表示当前没有轮盘在显示
    ULONGLONG radialShownTick_ = 0;
    // 「窗口亮着但钩子已回 Idle」的连续观察次数
    int radialWatchdogMismatchTicks_ = 0;
};

}  // namespace app
}  // namespace ktile
