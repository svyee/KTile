// KTile - 应用层：主控制器
//
// 职责：把各层组装成可运行的程序，管理生命周期。
// 不包含具体业务细节（那些在 core / ui 里）。
//
#pragma once

#include "core/data_model.h"
#include "infrastructure/filesystem/portable_paths.h"
#include "infrastructure/json/config_store.h"
#include "platform/win32/drop_target.h"
#include "platform/win32/global_hotkey.h"
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
}

namespace app {

// 应用标识
inline constexpr const wchar_t* kApplicationName = L"KTile";
inline constexpr const wchar_t* kApplicationWindowTitle = L"KTile 磁贴快启";

// 版本号。
//   0.9.0 = 功能开发完成、待打包与 Win7 真机验证（M6）；
//   1.0.0 = 正式版。
// 改这里时记得同步 app.rc 里的 VERSIONINFO
//  （rc 文件必须保持纯 ASCII，中文版本描述只放 About 对话框）。
inline constexpr const wchar_t* kApplicationVersion = L"1.0.1";
inline constexpr int kApplicationVersionMajor = 1;
inline constexpr int kApplicationVersionMinor = 0;
inline constexpr int kApplicationVersionPatch = 1;
inline constexpr const wchar_t* kSingleInstanceMutexName =
    L"KTile.SingleInstance.{8F3A1C2D-4B5E-4A6F-9C8D-7E6F5A4B3C2D}";
inline constexpr const wchar_t* kStartupRegistryValueName = L"KTile";

// 热键 ID
inline constexpr int kToggleHotkeyId = 1;

// 托盘回调消息（消息窗口使用）
inline constexpr UINT WM_TRAYICON_CALLBACK = WM_APP + 100;

// 自动化 / 调试开关集合。
//
// 为什么汇总成一个结构体：这些开关都只在自动化里用，正常启动一个都不开。
// 早先它们是一串位置参数，加一个开关就要动 run() 的签名、动 main.cpp 的
// 调用点，调用处还会变成「一长串看不出含义的 true/false」。
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
    // --debug-editor：打开第一个磁贴的「修改项目」对话框
    bool editor = false;
    // --debug-new-item：打开「添加项目」对话框（新建模式）
    bool newItem = false;
    // --debug-drop=x,y：模拟一次「拖入落点」（客户区坐标，<0 表示不模拟）。
    // 单独用时点亮落点预览（供截图）；与 --debug-add 同时用时，新项目就
    // 落在这一格上 —— 走的是与真实拖放完全相同的下游。
    int dropX = -1;
    int dropY = -1;
    // --debug-add=<路径1;路径2;...>：启动后走一次「拖入添加」流程
    // （与真实拖放共用同一条代码路径，只是不经过 OLE 那条管道）
    std::vector<std::wstring> addPaths;
    // --debug-opacity=<50..100>：把主窗口设成指定不透明度（0 = 不改）。
    // 用于验证「分层窗口 + D2D 渲染目标」在真实窗口上是否成立。
    int opacityPercent = 0;

    // 是否有任何开关被打开
    bool any() const noexcept {
        return shot || settings || settingsKeepAlive || about || editor ||
               newItem || !search.empty() || !addPaths.empty() ||
               dragSrcX >= 0 || opacityPercent > 0;
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

    // Explorer 重启广播（TaskbarCreated）触发：重装手势钩子。
    // WH_KEYBOARD_LL 钩子链是 LIFO，Explorer 重启会把它的钩子排到我们
    // 前面，导致「吞 Win 键」失效。收到广播后立刻重装，把顺序抢回来。
    // 非手势热键（普通 RegisterHotKey）不受钩子链影响，直接返回。
    void reinstallGestureHotkey();

private:
    // 各初始化步骤，任一步失败则终止启动
    bool initializeConfig();
    bool initializeWindow();
    bool initializeTray();
    bool initializeHotkey();

    // 持久化：从磁盘加载配置与磁贴数据
    void loadPersistedState();
    // 首次运行或数据缺失时植入示例磁贴（便于用户直观看到界面）
    void seedSampleDocument();
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
    // 把拖进来的东西加进面板（文件/文件夹直接建磁贴；链接建成网址项目）
    // dropPoint = 松手位置的屏幕坐标：新磁贴就落在光标所在那一格，
    // 压住的已有磁贴按避让规则让位（与拖动磁贴同一套规则）。
    // 不给坐标（自动化/调试入口）时交给布局引擎找第一个空位。
    void addDroppedSources(std::vector<platform::win32::DroppedSource> sources,
                           std::optional<POINTL> dropPoint);
    // 删除（弹确认框 + 落盘）
    void applyItemRemoval(HWND owner, std::size_t itemIndex,
                          const core::LaunchItem& item);

    // ---- 设置窗口 ----
    // 打开模态设置窗口，并把用户改动应用到配置与窗口。
    void openSettings();

    // ---- 关于对话框 ----
    // 弹出「关于 KTile」（产品名、版本、联系方式）。
    void openAbout();

    HINSTANCE instance_ = nullptr;
    HWND messageWindow_ = nullptr;

    platform::win32::SingleInstance singleInstance_;
    platform::win32::GlobalHotkey hotkey_;
    // 手势唤醒（双击 Ctrl / 单按 Win 键）。只在选中手势时安装。
    platform::win32::GestureHotkey gestureHotkey_;
    // 唤醒快捷键注册失败的原因（空 = 成功）。供启动提示说明具体是哪个键。
    std::wstring hotkeyErrorText_;
    platform::win32::TrayIcon tray_;

    // 拖放接收器（把文件/文件夹/链接拖到主窗口上即可添加）
    platform::win32::DropTarget dropTarget_;

    std::unique_ptr<ui::LauncherWindow> launcherWindow_;

    // 便携路径与持久化
    infrastructure::PortablePaths paths_;
    infrastructure::ConfigStore store_;
    bool pathsReady_ = false;

    // OLE 是否由本对象初始化（OleInitialize 成功才在析构里 OleUninitialize）。
    // 拖放（RegisterDragDrop）依赖 OLE，因此启动时必须先初始化。
    bool oleInitialized_ = false;

    core::ApplicationConfig config_;
    core::ItemsDocument document_;
};

}  // namespace app
}  // namespace ktile
