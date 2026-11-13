// KTile - 应用层：主控制器实现
#include "app/application.h"

#include "base/text_convert.h"
#include "core/builtin_items.h"
#include "core/item_operations.h"
#include "core/tab_operations.h"
#include "infrastructure/i18n.h"
#include "infrastructure/json/config_store.h"
#include "infrastructure/log/app_log.h"
#include "ui/launcher_window.h"
#include "ui/radial_menu_window.h"
#include "platform/win32/follow_launcher.h"
#include "platform/win32/foreground_process.h"
#include "platform/win32/explorer_selection.h"
#include "platform/win32/item_launcher.h"
#include "platform/win32/program_paths.h"
#include "platform/win32/icon_store.h"
#include "platform/win32/key_injector.h"
#include "platform/win32/system_actions.h"
#include "platform/win32/uuid.h"
#include "ui/item_editor_dialog.h"
#include "ui/radial_editor_dialog.h"
#include "ui/about_dialog.h"
#include "ui/tile_context_menu.h"
#include "ui/settings_window.h"
#include "ui/theme.h"

#include <Shlwapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace ktile {
namespace app {

// 主窗口标题：走语言文件，切语言后新建的窗口/对话框即用新名字。
// 定义放在匿名 namespace 之外，否则会与头文件声明构成两个不同实体，
// 调用时出现重载歧义。
const wchar_t* applicationWindowTitle() {
    return infrastructure::i18n::trw("app.windowTitle").c_str();
}

namespace {

// 取词助手：T = 宽字符（Win32 控件），U = UTF-8（数据/日志）。
// 语言表里的值存在字典里，指针在下次切语言前一直有效。
const wchar_t* T(const char* key) {
    return infrastructure::i18n::trw(key).c_str();
}
const std::string& U(const char* key) {
    return infrastructure::i18n::tr(key);
}

// 消息窗口类名（用于接收热键与托盘消息的隐藏窗口）
constexpr const wchar_t* kMessageWindowClassName = L"KTile.MessageWindow";

// 托盘右键菜单命令 ID
constexpr UINT kTrayMenuShowHide = 2001;
constexpr UINT kTrayMenuStartup = 2002;
constexpr UINT kTrayMenuExit = 2003;
constexpr UINT kTrayMenuSettings = 2004;

// 磁贴菜单「移动/复制到其他页面」子菜单的最大条目数。必须与
// tile_context_menu.cpp 的 kMaxTabMenuEntries 一致：app 层按
// 「基址 + 这么多条」解码命令 ID 区间。
constexpr std::size_t kTileMenuTabEntries = 32;
constexpr UINT kTrayMenuAbout = 2005;

// 插件刷新完成消息（工作线程 → 主线程），WM_APP 段避免与系统消息冲突。
// 只带「有更新」信号，数据由主线程向 PluginController 取（快照线程安全）。
constexpr UINT kPluginRefreshMessage = WM_APP + 20;

HICON g_trayIconForMenu = nullptr;  // 避免菜单期间图标被释放

// 消息窗口定时器
constexpr UINT_PTR kLogFlushTimerId = 1;        // 日志落盘
constexpr UINT_PTR kHookWatchdogTimerId = 2;    // 手势钩子看门狗
constexpr UINT_PTR kPluginVisibleTimerId = 3;   // 面板可见时的插件高频刷新
constexpr UINT_PTR kRadialWatchdogTimerId = 4;  // 轮盘可见期间的按键兜底
constexpr UINT kLogFlushIntervalMs = 2000;
constexpr UINT kHookWatchdogIntervalMs = 60 * 1000;
// 轮盘兜底的检查节奏：松手后及时收起，又不至于让 GetAsyncKeyState
// 变成忙轮询。只在轮盘可见期间挂表，其余时间零开销。
constexpr UINT kRadialWatchdogIntervalMs = 250;
// 轮盘最长停留时间：钩子彻底失效（被系统卸载、远程会话切换）时收不到
// 任何事件，只能靠这个上限把窗口收掉；正常选择用不了这么久。
constexpr ULONGLONG kRadialMaxVisibleMs = 10 * 1000;
// 「窗口亮着但钩子已回 Idle」要连续观察到几次才动手，防止和抬起消息抢跑。
constexpr int kRadialMismatchTicksToHide = 2;
// 手势钩子的强制重装周期：系统高负载时会静默跳过超时的钩子回调
//（无任何通知），只能定期重装抢回钩子链顺序。
constexpr ULONGLONG kForceReinstallIntervalMs = 10 * 60 * 1000;

// 操作系统版本（写入启动横幅，排查「只在某台机器复现」时用）。
// 走 ntdll 的 RtlGetVersion：GetVersionEx 在 Win8.1+ 会被兼容性 shim
// 截成 6.2，RtlGetVersion 给的才是真版本（Win7 起就存在）。
std::string osVersionString() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return "unknown";
    }
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto fn = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (fn == nullptr) {
        return "unknown";
    }
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (fn(&info) != 0) {
        return "unknown";
    }
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%lu.%lu.%lu",
             info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
    return buffer;
}

// Explorer 重启时广播的消息 ID（动态注册，不能 constexpr）。
// 钩子链是 LIFO，Explorer 崩溃重启/注销重登后会重新装钩子排到前面，
// 吞 Win 键就失效了；监听这条广播，重启后立刻重装钩子抢回顺序。
UINT taskbarCreatedMessageId() {
    static const UINT id = RegisterWindowMessageW(L"TaskbarCreated");
    return id;
}

// --- 拖入的快捷方式

// .lnk 解析结果。
struct ResolvedShortcut {
    std::wstring target;           // 真实目标路径（空 = 解析失败）
    std::wstring arguments;        // 快捷方式带的启动参数
    std::wstring workingDirectory; // 快捷方式记录的工作目录
};

// 把 .lnk 解析成真实目标再入库：快捷方式指的目标随时可能被移动/删除/
// 卸载，存真实目标使快捷方式删除也不影响启动。解析失败返回空 target，
// 调用方保留原路径（还能双击打开总比拖入凭空失败好）。
ResolvedShortcut resolveShortcut(const std::wstring& lnkPath) {
    constexpr DWORD kMaximumArguments = 2048;
    ResolvedShortcut resolved;

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        link == nullptr) {
        return resolved;
    }
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) &&
        file != nullptr) {
        if (SUCCEEDED(file->Load(lnkPath.c_str(), STGM_READ))) {
            wchar_t path[MAX_PATH] = {};
            wchar_t arguments[kMaximumArguments] = {};
            wchar_t directory[MAX_PATH] = {};
            // RAWPATH：拿快捷方式里存的原始路径。部分系统快捷方式
            // 存的是「%windir%\...」这类带环境变量的写法，必须展开，
            // 否则 inferItemType 与 ShellExecute 都不认。
            link->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH);
            if (path[0] == L'\0') {
                // RAWPATH 为空（部分广告安装的快捷方式只有目标列表）：
                // 再按默认方式要一次，shell 会做完整的解析
                link->GetPath(path, MAX_PATH, nullptr, 0);
            }
            link->GetArguments(arguments, kMaximumArguments);
            link->GetWorkingDirectory(directory, MAX_PATH);
            wchar_t expanded[MAX_PATH] = {};
            if (ExpandEnvironmentStringsW(path, expanded, MAX_PATH) > 0 &&
                expanded[0] != L'\0') {
                resolved.target = expanded;
            } else {
                resolved.target = path;
            }
            wchar_t expandedDir[MAX_PATH] = {};
            if (ExpandEnvironmentStringsW(directory, expandedDir, MAX_PATH) >
                0) {
                resolved.workingDirectory = expandedDir;
            } else {
                resolved.workingDirectory = directory;
            }
            resolved.arguments = arguments;
        }
        file->Release();
    }
    link->Release();
    return resolved;
}

// --- 拖入项的名字

// 取「文件名去掉扩展名」。文件夹（stripExtension=false）整名保留：
// 文件夹名带点的很常见（"vs.2022"），砍掉点后面会把名字改得面目全非。
std::wstring fileNameStem(const std::wstring& path, const bool stripExtension) {
    const std::size_t slash = path.find_last_of(L"\\/");
    std::wstring name =
        (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    if (!stripExtension) {
        return name;
    }
    const std::size_t dot = name.find_last_of(L'.');
    // dot == 0 是 ".gitignore" 这种整名都是扩展名的文件，保留原名
    if (dot != std::wstring::npos && dot > 0) {
        name.resize(dot);
    }
    return name;
}

// 从 URL 取标题用短名：去协议头、查询串与锚点，剩下通常是
// 一看就知道是哪儿的串，比带 ?utm_source=... 的原始链接干净。
std::wstring urlDisplayName(const std::wstring& url) {
    std::wstring value = url;
    const std::size_t scheme = value.find(L"://");
    if (scheme != std::wstring::npos) {
        value = value.substr(scheme + 3);
    }
    const std::size_t cut = value.find_first_of(L"?#");
    if (cut != std::wstring::npos) {
        value.resize(cut);
    }
    if (value.empty()) {
        value = url;
    }
    constexpr std::size_t kMaximumUrlNameLength = 60;
    if (value.size() > kMaximumUrlNameLength) {
        value.resize(kMaximumUrlNameLength);
    }
    return value;
}

// 名称留空时的取名规则，两条添加入口（对话框、拖入）共用，保证同一
// 目标得到同一名字。名字跟着推断出的类型走而不是来源：有些来源会把
// 网址塞进文件格式里（拖网页上的图标就是这样）。
std::string deriveItemName(const std::wstring& target,
                           const core::ItemType type) {
    // 内置命令/系统位置的目标派生不出可读名（stem 是 "lock" 或一串
    // 十六进制），查表给中文名。正常入口会在对话框回填名称走不到这里，
    // 这是给手改配置/手填目标的防御。
    if (!core::builtinIdFromTarget(target).empty()) {
        const core::BuiltinItem* item =
            core::findBuiltinItem(core::builtinIdFromTarget(target));
        if (item != nullptr) {
            return base::wideToUtf8Lossy(item->name);
        }
        return std::string();
    }
    if (!target.empty() && target.front() == L':') {
        // shell 命名空间路径（"::{...}"）：问系统要显示名
        SHFILEINFOW info{};
        if (SHGetFileInfoW(target.c_str(), FILE_ATTRIBUTE_NORMAL, &info,
                           sizeof(info), SHGFI_DISPLAYNAME) != 0) {
            return base::wideToUtf8Lossy(info.szDisplayName);
        }
    }
    switch (type) {
        case core::ItemType::Url:
            return base::wideToUtf8Lossy(urlDisplayName(target));
        case core::ItemType::Folder:
            return base::wideToUtf8Lossy(
                fileNameStem(target, /*stripExtension=*/false));
        default:
            return base::wideToUtf8Lossy(
                fileNameStem(target, /*stripExtension=*/true));
    }
}

// 只把 ASCII 大写字母转小写，用于「Windows 路径不区分大小写」的比较；
// 逐字节转小写本来也不会碰中文等多字节序列。
std::string asciiLower(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

// 消息窗口过程
LRESULT CALLBACK messageWindowProc(HWND hwnd, UINT message,
                                   WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
    }

    auto* self = reinterpret_cast<Application*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // TaskbarCreated 是动态消息 ID，不能进 switch，在 switch 前拦截；
    // Explorer 重启后立刻重装手势与轮盘钩子。
    if (message == taskbarCreatedMessageId()) {
        if (self != nullptr) {
            self->reinstallGestureHotkey("Explorer 重启（TaskbarCreated）");
            self->reinstallRadialHook("Explorer 重启（TaskbarCreated）");
        }
        return 0;
    }

    switch (message) {
        case WM_TIMER:
            if (self != nullptr) {
                self->onMessageTimer(wParam);
                return 0;
            }
            break;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC ||
                wParam == PBT_APMRESUMESUSPEND) {
                if (self != nullptr) {
                    self->onPowerResume();
                }
            }
            return TRUE;

        case kPluginRefreshMessage:
            if (self != nullptr) {
                self->onPluginDataReady();
            }
            return 0;

        case WM_HOTKEY: {
            if (self != nullptr && wParam == static_cast<WPARAM>(kToggleHotkeyId)) {
                self->toggleLauncher();
                return 0;
            }
            if (self != nullptr && wParam == static_cast<WPARAM>(kTopmostHotkeyId)) {
                self->toggleTopmost();
                return 0;
            }
            break;
        }

        case WM_TRAYICON_CALLBACK: {
            if (self != nullptr) {
                if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                    self->toggleLauncher();
                    return 0;
                }
                if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
                    self->showTrayMenu();
                    return 0;
                }
            }
            break;
        }

        case WM_COMMAND: {
            if (self != nullptr) {
                self->handleTrayCommand(LOWORD(wParam));
                return 0;
            }
            break;
        }

        case platform::win32::kRadialShowMessage: {
            if (self != nullptr) {
                POINT p = {static_cast<LONG>(wParam), static_cast<LONG>(lParam)};
                self->onRadialShow(p);
                return 0;
            }
            break;
        }
        case platform::win32::kRadialMoveMessage: {
            if (self != nullptr) {
                POINT p = {static_cast<LONG>(wParam), static_cast<LONG>(lParam)};
                self->onRadialMove(p);
                return 0;
            }
            break;
        }
        case platform::win32::kRadialReleaseMessage: {
            if (self != nullptr) {
                POINT p = {static_cast<LONG>(wParam), static_cast<LONG>(lParam)};
                self->onRadialRelease(p);
                return 0;
            }
            break;
        }
        case platform::win32::kRadialAbortMessage: {
            if (self != nullptr) {
                self->onRadialAbort(static_cast<int>(wParam));
                return 0;
            }
            break;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

Application::Application() = default;

Application::~Application() {
    // 先停插件工作线程：窗口销毁后它投递的消息只会静默失败，
    // 且线程仍可能在读写缓存文件，有序退出更干净。
    pluginController_.shutdown();
    if (hotkey_.isRegistered()) {
        hotkey_.unregister();
    }
    gestureHotkey_.uninstall();
    tray_.destroy();
    // 兜底覆盖调试开关的提前返回路径（正常退出已在 runMessageLoop 之后
    // shutdown 过日志）。
    if (loggingReady_) {
        infrastructure::applog::shutdown();
        loggingReady_ = false;
    }
}

// --- 启动流程

int Application::run(const HINSTANCE instance, const DebugOptions& debug) {
    instance_ = instance;

    // 单实例检查：已有实例则激活它并退出。所有自动化/调试开关都跳过
    // 该检查，验证脚本要能在「桌面上还挂着旧实例」时照常启动自己那一份；
    // 正常用户路径行为不变。
    const bool automationMode = debug.any();

    if (!automationMode &&
        !singleInstance_.tryAcquire(std::wstring(kSingleInstanceMutexName))) {
        // 让已有实例唤出面板：走消息窗口通路（消息内容与热键完全相同），
        // 不能直接 ShowWindow，否则窗口露出来却仍以为自己在隐藏。
        // 标题必须用 applicationWindowTitle()（主窗口真实标题）精确匹配，
        // 不能拿 kApplicationName 凑数。热键被别的程序占用时这是唯一通道。
        platform::win32::SingleInstance::activateExistingInstance(
            std::wstring(kMessageWindowClassName),
            std::wstring(applicationWindowTitle()), WM_HOTKEY,
            static_cast<WPARAM>(kToggleHotkeyId));
        return 0;
    }

    if (!initializeConfig()) {
        if (!debug.shot) {
            MessageBoxW(nullptr,
                        T("app.error.initConfig"),
                        kApplicationName,
                        MB_OK | MB_ICONERROR);
        }
        return 1;
    }

    // 日志初始化紧跟配置（需要 data 目录路径）；此后所有关键环节都留痕。
    initializeLogging(automationMode);

    // 扫描 slugin/ 下的按应用轮盘插件。
    //
    // 刻意放在日志就绪之后而不是 initializeConfig 里：插件写坏时的
    // 告警是用户排查插件问题的唯一线索，initializeConfig 阶段日志
    // 还没有可落盘的地方。
    loadRadialPlugins();

    if (!initializeWindow()) {
        if (!debug.shot) {
            MessageBoxW(nullptr,
                        T("app.error.createWindow"),
                        kApplicationName,
                        MB_OK | MB_ICONERROR);
        }
        return 1;
    }

    // ---- 自动化：走一次「拖入添加」，放在截图模式之前，导出的截图里
    // 能看到刚加进来的磁贴。普通拖放不从这里进来（走 OLE 回调）；
    // --debug-drop=x,y 给出松手位置时新磁贴落在那一格，与真实拖放
    // 完全同一条下游。
    std::optional<POINTL> debugDropPoint;
    if (launcherWindow_ != nullptr && debug.dropX >= 0 && debug.dropY >= 0) {
        POINT point{debug.dropX, debug.dropY};
        if (ClientToScreen(launcherWindow_->hwnd(), &point) != FALSE) {
            POINTL screenPoint{};
            screenPoint.x = point.x;
            screenPoint.y = point.y;
            debugDropPoint = screenPoint;
        }
    }
    if (!debug.addPaths.empty()) {
        std::vector<platform::win32::DroppedSource> sources;
        sources.reserve(debug.addPaths.size());
        for (const std::wstring& path : debug.addPaths) {
            platform::win32::DroppedSource source;
            source.kind = platform::win32::DroppedKind::Path;
            source.value = path;
            sources.push_back(std::move(source));
        }
        addDroppedSources(std::move(sources), debugDropPoint);
    }
    // 只给 --debug-drop 时不配合 --debug-add，点亮落点预览供截图。
    if (debugDropPoint.has_value() && debug.addPaths.empty() &&
        launcherWindow_ != nullptr) {
        launcherWindow_->debugSetFileDropPreview(debug.dropX, debug.dropY);
    }

    // ---- 调试：主窗口不透明度。放在窗口创建之后（要改 ex style）、
    // 截图导出之前（离屏图看不出分层合成效果，须用真实窗口截图）。
    if (debug.opacityPercent > 0 && launcherWindow_ != nullptr) {
        launcherWindow_->setWindowOpacityPercent(debug.opacityPercent);
    }

    // ---- 调试截图：轮盘菜单 ----
    if (debug.radial) {
        auto radialWindow = std::make_unique<ui::RadialMenuWindow>();
        if (!radialWindow->create(instance_)) {
            return 3;
        }
        const core::RadialMenu& menu = debug.radialPluginId.empty()
                                          ? radialConfig_.menu
                                          : debugRadialPluginMenu(debug.radialPluginId);
        // 以屏幕中心为基准，保证完整可见
        POINT center{};
        center.x = GetSystemMetrics(SM_CXSCREEN) / 2;
        center.y = GetSystemMetrics(SM_CYSCREEN) / 2;
        radialWindow->show(menu, center,
                           static_cast<int>(radialConfig_.trigger.cancelRadiusPx));
        // 可选：按相对中心的偏移（DIP）模拟光标，高亮对应扇区
        if (debug.radialHoverX != 0 || debug.radialHoverY != 0) {
            POINT cursor = center;
            cursor.x += debug.radialHoverX;
            cursor.y += debug.radialHoverY;
            radialWindow->updateCursor(cursor);
        }
        // 可选：钉住滚动相位（默认 0 = 标签开头），让导出结果可复现
        radialWindow->setMarqueePhaseForDebug(
            static_cast<float>(debug.radialScrollMs));
        const bool ok = radialWindow->debugExportPng(L"radial-shot.png");
        radialWindow->hide();
        return ok ? 0 : 4;
    }

    // ---- 调试截图模式：渲染导出后直接退出 ----
    if (debug.shot) {
        if (!launcherWindow_) {
            return 2;
        }
        // 可选：先按坐标模拟悬停，再导出
        launcherWindow_->debugSetHover(debug.hoverX, debug.hoverY);
        // 可选：预置搜索查询（导出「过滤生效」的截图）
        if (!debug.search.empty()) {
            launcherWindow_->setSearchQuery(debug.search);
        }
        // 可选：模拟拖拽视觉反馈
        launcherWindow_->debugSetDrag(debug.dragSrcX, debug.dragSrcY,
                                      debug.dragDstX, debug.dragDstY);

        // 插件数据是异步刷新的，等一次否则截图里只有占位，无法验证布局。
        // document_ 在 create() 时已被 move 进窗口，必须读窗口当前文档。
        const core::ItemsDocument& shotDocument = launcherWindow_->document();
        const std::size_t shotTab = launcherWindow_->activeTabIndex();
        if (shotTab < shotDocument.tabs.size()) {
            pluginController_.refreshItems(shotDocument.tabs[shotTab].items,
                                           true);
            for (int attempt = 0; attempt < 40; ++attempt) {
                bool loading = false;
                for (const core::LaunchItem& item :
                     shotDocument.tabs[shotTab].items) {
                    if (item.type == core::ItemType::Plugin &&
                        pluginController_.viewFor(item).loading) {
                        loading = true;
                        break;
                    }
                }
                if (!loading) {
                    break;
                }
                Sleep(50);
            }
        }

        // 调试截图直接 return 不经过正常退出流程，先刷日志，
        // 否则插件刷新失败信息会随进程结束一起丢掉。
        infrastructure::applog::flush();
        return launcherWindow_->debugExportPng(L"debug-shot.png") ? 0 : 3;
    }

    // ---- 调试编辑项目对话框：打开第一个磁贴的编辑对话框并继续跑
    // 主循环，供自动化验证对话框打开、保存落盘、取消不落盘。
    if (debug.editor && launcherWindow_ != nullptr &&
        launcherWindow_->tabCount() > 0 && !document_.tabs.empty()) {
        launcherWindow_->show();
        // 取当前页的第一项：多标签页下第 0 页的第 0 项未必是用户
        // 眼前那块磁贴。
        const std::size_t activeTab = launcherWindow_->activeTabIndex();
        const auto& items =
            document_.tabs[activeTab < document_.tabs.size() ? activeTab : 0]
                .items;
        if (!items.empty()) {
            applyItemEdit(launcherWindow_->hwnd(), 0, items[0]);
        }
    }

    // ---- 调试「添加新项目」对话框：与上面差异只在对话框模式
    //（标题「添加项目」、按钮「添加」），验证「回车即添加」。
    if (debug.newItem && launcherWindow_ != nullptr) {
        launcherWindow_->show();
        applyItemAdd(launcherWindow_->hwnd(), std::wstring());
    }

    if (!initializeTray()) {
        // 托盘失败不致命，仅提示
        MessageBoxW(nullptr,
                    T("app.error.trayIcon"),
                    kApplicationName,
                    MB_OK | MB_ICONWARNING);
    }

    // 定时器挂在托盘的消息窗口上：窗口建好之前 show() 的入口（如
    // --debug-editor 先显示面板再开对话框）不生效，这里补一次。
    updatePluginVisibleTimer();

    // ---- 调试设置窗口 ----
    //
    // keepAlive = false 打开设置后立刻退出（只验证控件建成）；
    // true 继续跑主循环。放在托盘（消息窗口）之后：设置里的唤醒快捷键
    // 会即时热替换，需要 messageWindow_ 已存在；不能连同 initializeHotkey
    // 一起提前，自动化跑在用户真实配置下，注册失败警告框会堵死调试入口。
    if (debug.settings || debug.settingsKeepAlive) {
        openSettings();
        if (!debug.settingsKeepAlive) {
            return 0;
        }
    }

    // ---- 调试「轮盘菜单设置」对话框：平时藏在设置 → 轮盘 →
    // 「编辑扇区动作…」后面，自动化拍版面得先切页再点按钮，给个直达入口。
    if (debug.radialEditor) {
        ui::showRadialEditorWindow(
            launcherWindow_ != nullptr ? launcherWindow_->hwnd() : nullptr,
            &radialConfig_);
        return 0;
    }

    // ---- 调试「关于」对话框：打开即退出，自动化只断言控件建成与文案。
    if (debug.about) {
        openAbout();
        return 0;
    }

    if (!initializeHotkey()) {
    // 把「具体是哪个键、被谁占了、去哪儿改」说清楚。错误详情与处置建议
    // 分开放（正文/语言文件），翻译时不用把动态内容塞进句子里。
        std::wstring message = infrastructure::i18n::formatW(
            "app.error.hotkeyRegister",
            {platform::win32::formatHotkey(config_.activation.hotkey)});
        if (!hotkeyErrorText_.empty()) {
            message = hotkeyErrorText_ + L"\n\n" + message;
        }
        MessageBoxW(nullptr, message.c_str(),
                    kApplicationName,
                    MB_OK | MB_ICONWARNING);
    }

    // F7 置顶热键注册失败不提示框：被占用只记日志，面板唤醒不能被它连累。
    initializeTopmostHotkey();

    // 安装轮盘菜单鼠标钩子（Ctrl+右键拖动等）
    if (radialConfig_.trigger.enabled) {
        if (radialHook_.install(messageWindow_, radialConfig_.trigger,
                                radialConfig_.blacklist)) {
            infrastructure::applog::info("轮盘菜单钩子已安装");
            lastRadialInstallTick_ = GetTickCount64();
        } else {
            infrastructure::applog::error("轮盘菜单钩子安装失败");
        }
    }

    // 启动期的工作集裁剪由 LauncherWindow 的延迟定时器负责
    //（kStartupTrimTimerId）：初始化后的首次渲染会把页换回来，太早裁是白裁。

    // 日志落盘（2s）与手势钩子看门狗（60s）挂在消息窗口上：
    // 主窗口隐藏期间消息循环照常泵送。
    SetTimer(messageWindow_, kLogFlushTimerId, kLogFlushIntervalMs, nullptr);
    SetTimer(messageWindow_, kHookWatchdogTimerId, kHookWatchdogIntervalMs,
             nullptr);

    // 跟随启动放在消息循环之前、所有初始化之后：窗口、托盘、热键就绪，
    // 拉起外部程序不会拖慢自己的启动流程。自动化/调试开关下不执行，
    // 避免反复启动几十次干扰截图与断言。
    if (!automationMode) {
        launchFollowStartupItems();
    }

    const int exitCode = runMessageLoop();

    KillTimer(messageWindow_, kLogFlushTimerId);
    KillTimer(messageWindow_, kHookWatchdogTimerId);
    KillTimer(messageWindow_, kPluginVisibleTimerId);
    KillTimer(messageWindow_, kRadialWatchdogTimerId);
    pluginVisibleTimerSeconds_ = 0;
    infrastructure::applog::markSessionEnd();
    infrastructure::applog::info("正常退出");
    infrastructure::applog::shutdown();
    loggingReady_ = false;
    return exitCode;
}

void Application::initializeLogging(const bool automation) {
    loggingReady_ = infrastructure::applog::initialize(
        paths_.dataDirectory() + L"\\logs");
    if (!loggingReady_) {
        return;  // 目录不可写：程序照常跑，只是没有日志
    }
    if (!automation) {
        infrastructure::applog::markSessionStart();
    }

    // 手势状态机的事件上报：sink 里只做内存缓冲（applog::info 不落盘），
    // 满足钩子回调「零文件 I/O」的硬约束。
    gestureHotkey_.setEventSink(
        [](const char* message) { infrastructure::applog::info(message); });

    // 启动横幅：一次给齐排查所需的全部上下文
    std::string banner = "===== KTile 启动 v";
    banner += base::wideToUtf8Lossy(std::wstring(kApplicationVersion));
    banner += " | Windows " + osVersionString();
    banner += paths_.isPortable() ? " | 便携模式" : " | 降级模式(AppData)";
    banner += automation ? " | 自动化" : "";
    banner += " =====";
    infrastructure::applog::info(banner);

    const core::HotkeyConfig& hotkey = config_.activation.hotkey;
    std::string summary = "唤醒方式: ";
    summary += base::wideToUtf8Lossy(platform::win32::formatHotkey(hotkey));
    summary += config_.activation.startWithWindows ? " | 开机自启: 开"
                                                   : " | 开机自启: 关";
    infrastructure::applog::info(summary);

    // 语言单独一行：排查「界面怎么是英文/中文」时第一个要看的就是这里。
    if (effectiveLanguage_.empty()) {
        infrastructure::applog::warn(
            "界面语言：lang 目录下没有可用语言文件，界面将显示 key");
    } else {
        infrastructure::applog::info(
            "界面语言: " + effectiveLanguage_ + "（配置值 " +
            config_.language + "）");
    }
}

void Application::onMessageTimer(const UINT_PTR timerId) {
    if (timerId == kLogFlushTimerId) {
        infrastructure::applog::flush();
    } else if (timerId == kHookWatchdogTimerId) {
        onHookWatchdog();
    } else if (timerId == kPluginVisibleTimerId) {
        onPluginVisibleTick();
    } else if (timerId == kRadialWatchdogTimerId) {
        onRadialWatchdog();
    }
}

// 轮盘可见期间的兜底：触发键已经松开就强制收起。低级钩子回调会被系统
// 静默跳过（回调超时，无任何通知），「抬起」事件一旦丢失，无焦点无
// Esc 通道的轮盘窗口只能靠这里对账收掉，代价是一次 GetAsyncKeyState。
void Application::onRadialWatchdog() {
    if (messageWindow_ == nullptr || !radialWindow_ ||
        !radialWindow_->isVisible()) {
        KillTimer(messageWindow_, kRadialWatchdogTimerId);
        radialWatchdogMismatchTicks_ = 0;
        return;
    }

    // 判据一：钩子没了。收不到任何鼠标事件，窗口不会再有机会被收起。
    if (!radialHook_.isInstalled()) {
        infrastructure::applog::warn("轮盘菜单：鼠标钩子已失效，强制收起");
        hideRadialMenu();
        return;
    }

    // 判据二：两侧状态不一致，窗口还亮着、钩子却已回 Idle，说明抬起
    // 事件没送到（PostMessage 失败、消息窗口重建等）。连续两次才收，
    // 避免和正常路径上的抬起消息抢跑。
    if (!radialHook_.isGestureActive()) {
        if (++radialWatchdogMismatchTicks_ >= kRadialMismatchTicksToHide) {
            infrastructure::applog::warn("轮盘菜单：手势已结束但窗口未收，强制收起");
            hideRadialMenu();
        }
        return;
    }
    radialWatchdogMismatchTicks_ = 0;

    // 判据三：可见时长上限。不能去问 GetAsyncKeyState「触发键还按着吗」：
    // 手势开始时按下事件被吞掉，它要等事件进入目标队列才更新，整个拖动
    // 期间都回答「已松开」，按它判断轮盘刚出现就会被收掉。
    const ULONGLONG now = GetTickCount64();
    if (radialShownTick_ != 0 && now - radialShownTick_ >= kRadialMaxVisibleMs) {
        infrastructure::applog::warn("轮盘菜单：停留超过 10 秒，强制收起");
        hideRadialMenu();
    }
}

void Application::hideRadialMenu() {
    if (radialWindow_) {
        radialWindow_->hide();
    }
    activeRadialMenu_ = nullptr;
    radialShownTick_ = 0;
    radialWatchdogMismatchTicks_ = 0;
    // 窗口已经不在屏幕上了，同步告诉钩子，免得它拿「还亮着」去做出错误判断
    radialHook_.setWheelVisible(false);
    if (messageWindow_ != nullptr) {
        KillTimer(messageWindow_, kRadialWatchdogTimerId);
    }
}

void Application::onPluginVisibleTick() {
    if (!launcherWindow_ || !launcherWindow_->isVisible()) {
        // 面板已经隐藏（或正在隐藏）：立刻停表，隐藏期间不再刷新。
        updatePluginVisibleTimer();
        return;
    }
    const core::ItemsDocument& document = launcherWindow_->document();
    const std::size_t index = launcherWindow_->activeTabIndex();
    if (index < document.tabs.size()) {
        // 逐项 TTL 决定谁真的发请求：1 秒节拍只是最小公分母。
        pluginController_.refreshItems(document.tabs[index].items, false, true);
    }
}

void Application::updatePluginVisibleTimer() {
    if (messageWindow_ == nullptr) {
        return;
    }

    int seconds = 0;
    if (launcherWindow_ && launcherWindow_->isVisible()) {
        const core::ItemsDocument& document = launcherWindow_->document();
        const std::size_t index = launcherWindow_->activeTabIndex();
        if (index < document.tabs.size()) {
            seconds = pluginController_.foregroundRefreshSeconds(
                document.tabs[index].items);
        }
    }

    if (seconds == pluginVisibleTimerSeconds_) {
        return;
    }
    KillTimer(messageWindow_, kPluginVisibleTimerId);
    pluginVisibleTimerSeconds_ = seconds;
    if (seconds > 0) {
        SetTimer(messageWindow_, kPluginVisibleTimerId,
                 static_cast<UINT>(seconds) * 1000, nullptr);
    }
}

void Application::onPluginDataReady() {
    if (launcherWindow_) {
        InvalidateRect(launcherWindow_->hwnd(), nullptr, FALSE);
    }
}

void Application::onHookWatchdog() {
    if (messageWindow_ == nullptr) {
        return;
    }

    // 轮盘钩子先检查：它和手势钩子是两条独立的钩子链，
    // 不能因为「没配手势」就整段跳过。
    if (radialConfig_.trigger.enabled) {
        const bool menuVisible = radialWindow_ && radialWindow_->isVisible();
        if (!radialHook_.isInstalled()) {
            infrastructure::applog::error("看门狗：轮盘钩子丢失，立即重装");
            reinstallRadialHook("看门狗（钩子丢失）");
        } else if (GetTickCount64() - lastRadialInstallTick_ >=
                       kForceReinstallIntervalMs &&
                   !menuVisible) {
            reinstallRadialHook("看门狗（定期重装）");
        }
    }

    const core::HotkeyConfig& config = config_.activation.hotkey;
    if (config.gesture == core::HotkeyGesture::None) {
        return;
    }
    if (!gestureHotkey_.isInstalled()) {
        infrastructure::applog::error("看门狗：手势钩子丢失，立即重装");
        reinstallGestureHotkey("看门狗（钩子丢失）");
        return;
    }
    // 系统静默跳过超时钩子且无通知，定期重装抢回钩子链顺序。
    // 手势进行中不装：重装会重置状态机。
    const ULONGLONG elapsed =
        GetTickCount64() - lastGestureInstallTick_;
    if (elapsed >= kForceReinstallIntervalMs && !gestureHotkey_.isMidGesture()) {
        reinstallGestureHotkey("看门狗（定期重装）");
    }
}

void Application::reinstallRadialHook(const char* reason) {
    if (!radialConfig_.trigger.enabled || messageWindow_ == nullptr) {
        return;
    }
    // 轮盘显示期间不动钩子：install 内部先 uninstall，会重置状态机，
    // 截断用户手里正按着的那次拖动。等下一次看门狗或事件再收尾。
    if (radialWindow_ && radialWindow_->isVisible()) {
        return;
    }
    infrastructure::applog::info(std::string("重装轮盘钩子（") +
                                 (reason != nullptr ? reason : "?") + "）");
    radialHook_.uninstall();
    if (radialHook_.install(messageWindow_, radialConfig_.trigger,
                                radialConfig_.blacklist)) {
        lastRadialInstallTick_ = GetTickCount64();
    } else {
        infrastructure::applog::error("重装轮盘钩子失败（下次看门狗再试）");
    }
}

void Application::onPowerResume() {
    infrastructure::applog::info("系统从休眠/睡眠恢复");
    reinstallGestureHotkey("休眠恢复");
    reinstallRadialHook("休眠恢复");
}

bool Application::initializeConfig() {
    // 解析便携路径（exe 同级 data 目录，不可写则降级到 %LOCALAPPDATA%）
    pathsReady_ = paths_.initialize();
    if (!pathsReady_) {
        return false;
    }

    // 告诉图标仓库「自定义图标存在哪」，必须在加载磁贴之前完成：
    // item.icon 里存相对 data 的路径（"res\xx.png"），解析依赖这个根目录。
    platform::win32::setIconStoreRoot(paths_.dataDirectory());

    // 皮肤根目录 + 首次运行生成内置预置（已存在的不覆盖）
    ui::setThemesRoot(paths_.dataDirectory());
    ui::ensurePresetThemesExist();

    // 加载配置：文件不存在时使用默认值
    const auto configResult =
        store_.loadConfig(paths_.configFilePath());
    config_ = configResult.value;

    // 界面语言必须赶在第一个窗口/第一段文案之前定下，否则窗口标题、
    // 托盘菜单、示例磁贴名字一半中文一半英文。日志此时还没初始化，
    // 只落「生效语言」状态，日志在 initializeLogging 里补。
    effectiveLanguage_ = infrastructure::i18n::initialize(
        paths_.langDirectory(), config_.language);

    // 加载磁贴数据
    const auto itemsResult =
        store_.loadItems(paths_.itemsFilePath());
    document_ = itemsResult.value;

    // 首次运行（或数据被清空）时植入示例磁贴并立即落盘
    if (!itemsResult.fromFile || document_.totalItemCount() == 0) {
        seedSampleDocument();
        persistItems();
    }

    // 首次运行时把默认配置写出来，方便用户手工编辑
    if (!configResult.fromFile) {
        // 对齐托盘状态：以注册表实际情况为准
        config_.activation.startWithWindows =
            platform::win32::StartupRegistration::isEnabled(
                std::wstring(kStartupRegistryValueName));
        persistConfig();
    }

    // 加载轮盘菜单配置
    loadRadialConfig();

    return true;
}

void Application::seedSampleDocument() {
    core::ItemsDocument document;
    auto& tab = document.ensureDefaultTab();
    tab.id = "default";
    tab.name = U("app.firstRun.tabFavorites");

    // 示例磁贴：覆盖四种尺寸，用于验证混排效果
    struct SampleItem {
        const char* id;
        const char* name;
        const char* target;
        core::ItemType type;
        core::TileSize size;
    };
    const SampleItem samples[] = {
        {"1",  U("app.firstRun.notepad").c_str(),   "C:\\Windows\\System32\\notepad.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"2",  U("app.firstRun.calculator").c_str(),   "C:\\Windows\\System32\\calc.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"3",  U("app.firstRun.browser").c_str(),   "C:\\Program Files\\Internet Explorer\\iexplore.exe",
         core::ItemType::Application, core::TileSize::Large},
        {"4",  U("app.firstRun.explorer").c_str(), "C:\\Windows\\explorer.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"5",  U("app.firstRun.cmd").c_str(), "C:\\Windows\\System32\\cmd.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"6",  U("app.firstRun.documents").c_str(), "C:\\Users",
         core::ItemType::Folder, core::TileSize::Tall},
        {"7",  U("app.firstRun.controlPanel").c_str(), "C:\\Windows\\System32\\control.exe",
         core::ItemType::Application, core::TileSize::Wide},
        {"8",  U("app.firstRun.paint").c_str(),     "C:\\Windows\\System32\\mspaint.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"9",  U("app.firstRun.snipping").c_str(), "C:\\Windows\\System32\\SnippingTool.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"10", U("app.firstRun.taskManager").c_str(), "C:\\Windows\\System32\\Taskmgr.exe",
         core::ItemType::Application, core::TileSize::Wide},
    };

    for (const auto& sample : samples) {
        core::LaunchItem item(sample.id, sample.type, sample.name, sample.target);
        item.tileSize = sample.size;
        tab.items.push_back(std::move(item));
    }

    // 第二个示例页：让「多标签页」首次运行时就被看见。
    // id 必须全文档唯一（core::addItem 的规则），另起一组。
    const std::size_t toolsIndex = core::addTab(document, U("app.firstRun.tabTools"));
    const SampleItem toolSamples[] = {
        {"t1", U("app.firstRun.regedit").c_str(), "C:\\Windows\\regedit.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"t2", U("app.firstRun.services").c_str(), "C:\\Windows\\System32\\services.msc",
         core::ItemType::File, core::TileSize::Small},
        {"t3", U("app.firstRun.systemInfo").c_str(), "C:\\Windows\\System32\\msinfo32.exe",
         core::ItemType::Application, core::TileSize::Wide},
        {"t4", U("app.firstRun.diskCleanup").c_str(), "C:\\Windows\\System32\\cleanmgr.exe",
         core::ItemType::Application, core::TileSize::Small},
    };
    for (const auto& sample : toolSamples) {
        core::LaunchItem item(sample.id, sample.type, sample.name, sample.target);
        item.tileSize = sample.size;
        document.tabs[toolsIndex].items.push_back(std::move(item));
    }

    document_ = std::move(document);
}

void Application::persistItems() {
    if (!pathsReady_) {
        return;
    }
    // 保存失败不阻断使用（可能只是磁盘写保护），静默接受
    (void)store_.saveItems(paths_.itemsFilePath(), document_);
}

void Application::persistConfig() {
    if (!pathsReady_) {
        return;
    }
    (void)store_.saveConfig(paths_.configFilePath(), config_);
}

// --- 跟随启动

const core::LaunchItem* Application::findItemById(
    const std::string& itemId) const {
    if (launcherWindow_ == nullptr || itemId.empty()) {
        return nullptr;
    }
    for (const core::Tab& tab : launcherWindow_->document().tabs) {
        for (const core::LaunchItem& item : tab.items) {
            if (item.id == itemId) {
                return &item;
            }
        }
    }
    return nullptr;
}

bool Application::isFollowingStartup(const std::string& itemId) const {
    if (itemId.empty()) {
        return false;
    }
    for (const core::FollowLaunchEntry& entry :
         config_.activation.followLaunch) {
        if (entry.itemId == itemId) {
            return true;
        }
    }
    return false;
}

void Application::toggleFollowStartup(const core::LaunchItem& item) {
    if (!launcherWindow_ || item.id.empty()) {
        return;
    }
    std::vector<core::FollowLaunchEntry>& entries =
        config_.activation.followLaunch;
    const auto existing = std::find_if(
        entries.begin(), entries.end(),
        [&item](const core::FollowLaunchEntry& entry) {
            return entry.itemId == item.id;
        });
    const std::string label = item.name.empty() ? item.target : item.name;

    if (existing != entries.end()) {
        entries.erase(existing);
        infrastructure::applog::info("跟随启动：从清单移除 " + label);
        persistConfig();
        return;
    }

    if (entries.size() >= core::kMaximumFollowLaunchItems) {
        const std::wstring message = infrastructure::i18n::formatW(
            "settings.followLaunch.full",
            {std::to_wstring(core::kMaximumFollowLaunchItems)});
        MessageBoxW(launcherWindow_->hwnd(), message.c_str(), kApplicationName,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    // 快照：磁贴被删之后靠它继续启动。参数、工作目录、管理员权限一并
    // 带走，少一样就是另一种运行结果。
    core::FollowLaunchEntry entry;
    entry.itemId = item.id;
    entry.name = label;
    entry.target = item.target;
    entry.arguments = item.arguments;
    entry.workingDirectory = item.workingDirectory;
    entry.runAsAdministrator = item.runAsAdministrator;
    entries.push_back(std::move(entry));

    infrastructure::applog::info("跟随启动：加入 " + label);
    persistConfig();
}

bool Application::syncFollowLaunchWithTiles() {
    if (!launcherWindow_) {
        return false;
    }
    bool changed = false;
    for (core::FollowLaunchEntry& entry : config_.activation.followLaunch) {
        if (!entry.fromTile()) {
            continue;
        }
        const core::LaunchItem* tile = findItemById(entry.itemId);
        if (tile == nullptr) {
            // 磁贴已被删除：条目保留、快照不动。删磁贴不等于不要开机
            // 启动，静默清掉会让用户下次开机少一个程序却找不到原因。
            continue;
        }
        const std::string name = tile->name.empty() ? tile->target : tile->name;
        if (entry.name == name && entry.target == tile->target &&
            entry.arguments == tile->arguments &&
            entry.workingDirectory == tile->workingDirectory &&
            entry.runAsAdministrator == tile->runAsAdministrator) {
            continue;
        }
        entry.name = name;
        entry.target = tile->target;
        entry.arguments = tile->arguments;
        entry.workingDirectory = tile->workingDirectory;
        entry.runAsAdministrator = tile->runAsAdministrator;
        changed = true;
    }
    return changed;
}

void Application::launchFollowStartupItems() {
    const std::vector<core::FollowLaunchEntry>& entries =
        config_.activation.followLaunch;
    if (entries.empty()) {
        return;
    }
    infrastructure::applog::info("跟随启动：" + std::to_string(entries.size()) +
                                 " 项待处理");

    for (const core::FollowLaunchEntry& entry : entries) {
        // 两种来源在此合流：磁贴条目按 id 取当前值，取不到（磁贴已删）
        // 或手动条目用快照，后面只剩一条路径。
        core::LaunchItem item;
        bool fromLiveTile = false;
        if (entry.fromTile()) {
            const core::LaunchItem* tile = findItemById(entry.itemId);
            if (tile != nullptr) {
                item = *tile;
                fromLiveTile = true;
            }
        }
        if (!fromLiveTile) {
            item.type = core::ItemType::Application;
            item.name = entry.name;
            item.target = entry.target;
            item.arguments = entry.arguments;
            item.workingDirectory = entry.workingDirectory;
            item.runAsAdministrator = entry.runAsAdministrator;
        }

        const std::string label = item.name.empty() ? item.target : item.name;

        // 只有可执行目标才谈得上「启动后常驻」。插件磁贴没有可执行文件，
        // 右键菜单已禁用，这里再挡一次（用户添加后可能改了磁贴类型）。
        if (item.type != core::ItemType::Application) {
            infrastructure::applog::warn("跟随启动：跳过 " + label +
                                         "（不是可执行程序）");
            continue;
        }

        // 路径解析与磁贴同一条规则：相对路径以 exe 目录为基准。
        const std::wstring target = platform::win32::resolveProgramRelativePath(
            base::utf8ToWideLossy(item.target));
        if (target.empty()) {
            continue;
        }

        // 已在运行就不重复启动：开机时这些程序常已被系统或用户拉起过，
        // 再开一次托盘里会出现两份。
        if (platform::win32::isProgramRunning(target)) {
            infrastructure::applog::info("跟随启动：跳过 " + label +
                                         "（已在运行）");
            continue;
        }

        // 交给 launchItem 的 target 换成解析后的绝对路径：它自己也会解析
        // 同样的结果，但失败提示里不会出现看不出落点的相对路径。
        item.target = base::wideToUtf8Lossy(target);

        const platform::win32::LaunchResult result =
            platform::win32::launchItem(item, false);
        if (result.ok) {
            infrastructure::applog::info("跟随启动：" + label + " 已启动");
        } else {
            infrastructure::applog::warn(
                "跟随启动：" + label + " 启动失败 - " +
                base::wideToUtf8Lossy(result.error));
        }
    }
}

void Application::loadRadialConfig() {
    if (!pathsReady_) return;
    const std::wstring path = paths_.dataDirectory() + L"\\radial.json";
    const auto result = radialStore_.load(path);
    radialConfig_ = result.value;
    if (!result.fromFile) {
        // 首次运行：写一份默认配置
        persistRadialConfig();
    }
}

void Application::loadRadialPlugins() {
    // 落点策略与磁贴插件一致：便携模式跟随 exe，否则跟随 data 目录，
    // 保持「整个目录可搬移」的约定。
    const std::wstring root =
        paths_.isPortable() ? paths_.executableDirectory() + L"\\slugin"
                            : paths_.dataDirectory() + L"\\slugin";
    radialPluginStore_.discover(root);

    // 重扫清空并重建插件列表，而 activeRadialMenu_ 正指着其中某个
    // plugin->menu；轮盘还亮着时指针悬空（选择动作会读到已释放内存），
    // 正常路径「打开设置」意味着拖动早已结束，这里把理论上不可能变成一定安全。
    if (radialWindow_ && radialWindow_->isVisible()) {
        hideRadialMenu();
    }
    const auto& plugins = radialPluginStore_.plugins();
    if (plugins.empty()) {
        return;
    }
    int loaded = 0;
    for (const auto& plugin : plugins) {
        if (plugin.valid) {
            ++loaded;
        } else {
            // 插件写坏了不能让用户以为是「功能失灵」，至少在日志里留下原因
            infrastructure::applog::warn(
                "轮盘插件「" + plugin.id + "」未加载：" + plugin.error);
        }
    }
    infrastructure::applog::info("轮盘插件：加载 " + std::to_string(loaded) +
                                " / " + std::to_string(plugins.size()) + " 个");
}

std::wstring Application::radialPluginDirectoryOf(const std::string& id) const {    for (const core::RadialPlugin& plugin : radialPluginStore_.plugins()) {
        if (plugin.id == id && !plugin.directory.empty()) {
            return base::utf8ToWideLossy(plugin.directory);
        }
    }
    // id 找不到（或调用方想看根目录）：退回 slugin 根，至少能打开个地方
    return radialPluginStore_.rootDirectory();
}

const core::RadialMenu& Application::debugRadialPluginMenu(const std::string& id) {
    for (const core::RadialPlugin& plugin : radialPluginStore_.plugins()) {
        if (plugin.id == id) {
            return plugin.menu;
        }
    }
    infrastructure::applog::warn("--debug-radial-plugin：找不到插件「" + id +
                                "」，改用通用轮盘");
    return radialConfig_.menu;
}

void Application::persistRadialConfig() {
    if (!pathsReady_) return;
    const std::wstring path = paths_.dataDirectory() + L"\\radial.json";
    (void)radialStore_.save(path, radialConfig_);
}

// --- 轮盘菜单

void Application::onRadialShow(POINT startPoint) {
    const ULONGLONG showStartTick = GetTickCount64();
    // 黑名单在钩子按下时已拦掉，这里不再判。匹配顺序：slugin 插件 →
    // 用户配置的通用轮盘。插件优先是有意的：插件是「这个应用该有哪些
    // 动作」的专门答案，通用轮盘是兜底，反过来插件永远轮不到。
    const core::RadialMenu* menu = &radialConfig_.menu;
    platform::win32::WindowTarget target;
    {
        // 目标按鼠标所在窗口取（悬停不改前台，只看前台会配错窗口）。
        target = platform::win32::radialTargetWindow();
        if (!target.processName.empty()) {
            const core::RadialPlugin* plugin = core::findPluginForProcess(
                radialPluginStore_.plugins(),
                base::wideToUtf8Lossy(target.processName),
                base::wideToUtf8Lossy(target.windowClass));
            if (plugin != nullptr && plugin->valid) {
                menu = &plugin->menu;
            }
        }
    }

    // 带 when 条件的菜单（explorer / 桌面插件的「有选中 / 无选中」两套）：
    // 按光标下窗口的文件选中状态过滤成真正要显示的一份。shell 查询失败
    // 与非 shell 目标都按无选中处理；过滤结果不合法时沿用原菜单。
    bool anyConditional = false;
    for (const core::RadialAction& action : menu->actions) {
        if (action.when != core::RadialActionWhen::Always) {
            anyConditional = true;
            break;
        }
    }
    if (anyConditional) {
        std::optional<bool> selection;
        if (platform::win32::isShellSelectionTarget(target.processName,
                                                    target.windowClass)) {
            selection = platform::win32::shellWindowHasSelection(target.window);
        }
        std::optional<core::RadialMenu> filtered =
            core::selectionFilteredMenu(*menu, selection.value_or(false));
        if (filtered.has_value()) {
            shownRadialMenu_ = std::move(*filtered);
            menu = &shownRadialMenu_;
        }
    }

    activeRadialMenu_ = menu;

    infrastructure::applog::info(
        "轮盘菜单：显示 起点 " + std::to_string(startPoint.x) + "," +
        std::to_string(startPoint.y) + " 扇区 " +
        std::to_string(menu->sectorCount) + " 动作 " +
        std::to_string(menu->actions.size()));

    if (!radialWindow_) {
        radialWindow_ = std::make_unique<ui::RadialMenuWindow>();
        if (!radialWindow_->create(instance_)) {
            radialWindow_.reset();
            return;
        }
        radialWindow_->setActionHandler(
            [this](std::uint8_t sectorIndex) {
                if (activeRadialMenu_ == nullptr) return;
                if (sectorIndex >= activeRadialMenu_->actions.size()) return;
                executeRadialAction(activeRadialMenu_->actions[sectorIndex]);
            });
    }
    radialWindow_->show(*menu, startPoint,
                        static_cast<int>(radialConfig_.trigger.cancelRadiusPx));
    // 告诉钩子窗口真的亮了。钩子与窗口是两套状态，抬起事件丢失时会
    // 分家，钩子靠这个判断「该收还是不收」（见 radial_mouse_hook.h）。
    radialHook_.setWheelVisible(true);
    radialShownTick_ = GetTickCount64();
    radialWatchdogMismatchTicks_ = 0;
    // 挂上兜底定时器（钩子失效 / 状态不一致 / 停留过久三种收尾）
    if (messageWindow_ != nullptr) {
        SetTimer(messageWindow_, kRadialWatchdogTimerId,
                 kRadialWatchdogIntervalMs, nullptr);
    }

    // 显示这一段跑在装钩子的线程上，太久会让系统跳过这期间的钩子回调
    //（抬起事件就是这么丢的）。超过半秒留记录，出现「松开右键轮盘还在」
    // 时可直接对号。
    const ULONGLONG elapsed = GetTickCount64() - showStartTick;
    if (elapsed >= 500) {
        infrastructure::applog::warn(
            "轮盘菜单：显示耗时 " + std::to_string(elapsed) +
            " ms，期间的鼠标事件可能被系统跳过");
    }
}

void Application::onRadialMove(POINT currentPoint) {
    if (radialWindow_ && radialWindow_->isVisible()) {
        radialWindow_->updateCursor(currentPoint);
    }
}

void Application::onRadialRelease(POINT currentPoint) {
    if (messageWindow_ != nullptr) {
        KillTimer(messageWindow_, kRadialWatchdogTimerId);
    }
    radialShownTick_ = 0;
    radialWatchdogMismatchTicks_ = 0;
    if (!radialWindow_ || !radialWindow_->isVisible()) {
        activeRadialMenu_ = nullptr;
        radialHook_.setWheelVisible(false);
        return;
    }
    // 窗口自行判定扇区并触发动作，然后隐藏。这里不再补发原生右键：
    // 以前「没碰扇区、中心空档松手」当作手抖单击还给前台，结果是用户
    // 明明想取消，桌面却跟着冒出上下文菜单，且松手位置不同就时有时无。
    // 轮盘亮过之后这次按下的结果就交给轮盘；真想点右键的那一次
    //（压根没拖动、没唤出轮盘）在钩子层（未超激活阈值）就已补发。
    radialWindow_->releaseAt(currentPoint);
    activeRadialMenu_ = nullptr;
    radialHook_.setWheelVisible(false);
}

// 钩子报「这次手势作废」。触发原因不是用户操作失误，而是抬起事件没走到
// 钩子里（系统静默跳过超时的低级钩子回调），表现为轮盘赖在屏幕上跟着
// 鼠标高亮或突然弹出。收掉并记日志，这是唯一能证明「不是用户手抖」的证据。
// 不执行任何扇区动作：再补一次快捷键等于凭空多按了一下。
void Application::onRadialAbort(int reason) {
    const char* why = "未知";
    switch (static_cast<platform::win32::RadialAbortReason>(reason)) {
        case platform::win32::RadialAbortReason::LostRelease:
            why = "上一次手势的抬起事件丢失";
            break;
        case platform::win32::RadialAbortReason::StaleGesture:
            why = "按住超时仍未拖动";
            break;
    }
    if (radialWindow_ && radialWindow_->isVisible()) {
        infrastructure::applog::warn(std::string("轮盘菜单：") + why +
                                     "，收起轮盘");
    } else {
        // 窗口早就收了：说明这只是状态机清理，记 info 就够，别当作告警
        infrastructure::applog::info(std::string("轮盘手势作废（") + why + "）");
    }
    hideRadialMenu();
}

void Application::executeRadialAction(const core::RadialAction& action) {
    if (action.isEmpty()) return;

    switch (action.type) {
        case core::RadialActionType::Application:
        case core::RadialActionType::File:
        case core::RadialActionType::Folder:
        case core::RadialActionType::Url: {
            // 转成 LaunchItem 后复用 item_launcher
            core::LaunchItem item;
            item.type = static_cast<core::ItemType>(action.type);
            item.name = action.name;
            item.target = action.target;
            item.arguments = action.arguments;
            if (action.icon.has_value()) {
                item.icon = *action.icon;
            }
            platform::win32::launchItem(item, false);
            break;
        }
        case core::RadialActionType::Shortcut: {
            platform::win32::injectShortcut(action.modifiers, action.key);
            break;
        }
        case core::RadialActionType::Builtin: {
            std::wstring id = base::utf8ToWide(action.target);
            platform::win32::runBuiltinAction(id);
            break;
        }
    }
}

bool Application::initializeWindow() {
    launcherWindow_.reset(new ui::LauncherWindow());

    if (!launcherWindow_->create(instance_,
                                 std::wstring(applicationWindowTitle()),
                                 document_)) {
        return false;
    }

    // 应用网格配置
    launcherWindow_->setGridSize(config_.appearance.gridColumns,
                                 config_.appearance.gridRows);
    // setGridSize 只改成员，几何必须显式重算，否则窗口仍按默认 8x6 造，
    // 配置成更小网格的用户会看到大片空白。
    launcherWindow_->recomputeLayout();
    launcherWindow_->applyGridGeometry();

    // 应用标签显示偏好（设置窗口可改，需在首次显示前生效）
    launcherWindow_->setShowTileLabels(config_.appearance.showTileLabels);
    launcherWindow_->setAlwaysShowTileLabels(
        config_.appearance.alwaysShowTileLabels);
    launcherWindow_->setTabBarAtBottom(config_.appearance.tabBarAtBottom);
    // 图标统一缩放（首次显示前生效）
    launcherWindow_->setTileIconScalePercent(
        static_cast<int>(config_.appearance.iconScalePercent));

    // 窗口出现位置：策略 + 上次拖动坐标（「上一次的位置」用）。
    // 必须在首次 show() 之前设好，否则第一次唤出会按默认策略摆。
    launcherWindow_->setWindowPlacement(
        config_.appearance.windowPlacement);
    launcherWindow_->setLastWindowPosition(config_.appearance.lastWindowX,
                                           config_.appearance.lastWindowY,
                                           config_.appearance.lastWindowPositionValid);
    // 用户拖动标题带后记住坐标（下次「上一次的位置」用）。
    // 只有拖动会写盘，程序按策略摆窗口不记，避免自己污染记录。
    launcherWindow_->setWindowMovedHandler(
        [this](const int x, const int y) {
            if (config_.appearance.lastWindowX == x &&
                config_.appearance.lastWindowY == y &&
                config_.appearance.lastWindowPositionValid) {
                return;
            }
            config_.appearance.lastWindowX = x;
            config_.appearance.lastWindowY = y;
            config_.appearance.lastWindowPositionValid = true;
            persistConfig();
        });

    // 应用皮肤与背景图（背景位图在渲染目标就绪后自动建立）
    applyThemeFromConfig();

    // 不透明度必须在 applyThemeFromConfig 之后：磁贴背景的「跟随皮肤」
    // 要读皮肤里的 tileFill，皮肤没套上时算出来的是旧值。
    launcherWindow_->setWindowOpacityPercent(
        static_cast<int>(config_.appearance.windowOpacityPercent));
    launcherWindow_->setTileOpacityPercent(
        static_cast<int>(config_.appearance.tileOpacityPercent));
    // 全局默认磁贴底色（空 = 跟随皮肤；单项 tileColor 优先）
    launcherWindow_->setDefaultTileColor(config_.appearance.defaultTileColor);

    // ---- 回调：启动项目 ----
    // 普通权限按项目自身的管理员设置决定是否提权；成功后隐藏窗口
    //（启动器「唤出 → 启动 → 消失」的标准行为），失败弹错并保持可见。
    launcherWindow_->setLaunchHandler([this](const core::LaunchItem& item) {
        launchItem(item, false);
    });

    // ---- 回调：关闭窗口 ----
    launcherWindow_->setCloseHandler([this]() {
        if (launcherWindow_) {
            launcherWindow_->hide();
        }
    });

    // ---- 回调：数据变更 ----
    launcherWindow_->setDocumentChangedHandler(
        [this](const core::ItemsDocument& document) {
            document_ = document;
            persistItems();  // 立即落盘，避免崩溃丢失排序/尺寸调整
            // 磁贴增删可能改变了「本页有没有需要前台刷新的插件」。
            updatePluginVisibleTimer();
        });

    // ---- 回调：钉子状态变更 ----
    // 钉子是用户偏好，立刻落盘，下次启动沿用。
    launcherWindow_->setPinnedChangedHandler([this](const bool pinned) {
        config_.appearance.pinned = pinned;
        persistConfig();
    });

    // ---- 回调：活动标签页变更 ----
    // 「上次在哪一页」也是用户偏好，立刻落盘：面板随时唤出随时用，
    // 进程被结束（关机、任务管理器）时不会有从容的退出时机。
    launcherWindow_->setActiveTabChangedHandler([this](const std::size_t tabIndex) {
        if (!launcherWindow_) {
            return;
        }
        config_.appearance.activeTabId = launcherWindow_->activeTabId();
        persistConfig();

        // 面板开着时切页才触发懒加载刷新；隐藏时的切页（如设置窗口
        // 改默认页）不触发，避免无谓开销。
        if (launcherWindow_->isVisible()) {
            const core::ItemsDocument& document = launcherWindow_->document();
            if (tabIndex < document.tabs.size()) {
                pluginController_.refreshItems(document.tabs[tabIndex].items,
                                               false);
            }
        }
        // 新页可能挂着「每秒刷新」的插件：前台节拍跟着当前页走。
        updatePluginVisibleTimer();
    });

    // ---- 回调：磁贴右键菜单 ----
    // UI 层只负责「在哪、点的是哪一块」，菜单内容与数据修改都在这里。
    launcherWindow_->setContextMenuHandler(
        [this](HWND owner, const std::size_t itemIndex, const core::LaunchItem& item) {
            handleItemContextMenu(owner, itemIndex, item);
        });

    // ---- 回调：空白区右键 → 设置 ----
    // 托盘之外的第二个入口（托盘图标可能被收进溢出区）。
    launcherWindow_->setSettingsHandler([this]() { openSettings(); });

    // ---- 回调：空白区右键 → 关于 ----
    // 理由同上。
    launcherWindow_->setAboutHandler([this]() { openAbout(); });

    // ---- 回调：空白区右键 → 添加新项目 ----
    launcherWindow_->setAddItemHandler([this]() {
        if (launcherWindow_) {
            applyItemAdd(launcherWindow_->hwnd(), std::wstring());
        }
    });

    // ---- 插件系统 ----
    //
    // 初始化放在窗口建好之后：刷新完成的回投需要已存在的窗口句柄。
    // 插件目录便携模式放 exe 同级，exe 目录不可写时跟随 data 目录，
    // 保证「整个目录可搬移」。
    const std::wstring pluginDirectory =
        paths_.isPortable() ? paths_.executableDirectory() + L"\\plugin"
                            : paths_.dataDirectory() + L"\\plugin";
    pluginController_.initialize(
        pluginDirectory, paths_.dataDirectory(), [this]() {
            // 工作线程不碰 UI：只投一个「有更新」信号，绘制留在主线程。
            if (messageWindow_ != nullptr) {
                PostMessageW(messageWindow_, kPluginRefreshMessage, 0, 0);
            }
        });
    // initialize() 内部已完成首次发现，这里不重复扫描。

    launcherWindow_->setPluginViewProvider(
        [this](const core::LaunchItem& item) {
            return pluginController_.viewFor(item);
        });
    launcherWindow_->setPluginCatalogProvider([this]() {
        return pluginController_.catalog();
    });
    launcherWindow_->setAddPluginHandler(
        [this](const std::string& pluginId) {
            addPluginItem(pluginId);
        });
    // 面板唤出 → 只刷新当前页的插件（命中未过期缓存时直接跳过）。
    launcherWindow_->setPanelShownHandler([this]() {
        if (!launcherWindow_) {
            return;
        }
        const core::ItemsDocument& document = launcherWindow_->document();
        const std::size_t index = launcherWindow_->activeTabIndex();
        if (index < document.tabs.size()) {
            pluginController_.refreshItems(document.tabs[index].items, false);
        }
        // 面板可见期间按插件的 visibleRefreshSeconds 持续刷新；
        // 没有插件需要高频刷新时这里什么都不做（不加定时器）。
        updatePluginVisibleTimer();
    });
    launcherWindow_->setPanelHiddenHandler([this]() {
        pluginController_.cancelPending();
        // 隐藏即停表：后台不刷新，这是「前台刷新间隔」的另一半语义。
        updatePluginVisibleTimer();
    });

    // ---- 拖放接收：把文件/文件夹/链接拖到面板上即可添加 ----
    //
    // 必须在窗口建好之后注册（RegisterDragDrop 要 HWND），且进程已
    // OleInitialize（见 main.cpp）。注册失败不致命，拖放入口不可用而已，
    // 静默降级不弹框。Windows 的 UIPI 不允许标准权限的拖拽来源
    //（资源管理器、浏览器）向以管理员身份运行的本进程投放拖放，光标
    // 显示「禁止」，以普通权限启动即可解决。
    const bool dropRegistered = dropTarget_.registerForWindow(
        launcherWindow_->hwnd(),
        [this](std::vector<platform::win32::DroppedSource> sources,
               POINTL point) {
            addDroppedSources(std::move(sources), point);
        },
        [this](const bool entered, const std::size_t count, POINTL point) {
            if (!launcherWindow_) {
                return;
            }
            // count == 0 表示这份拖拽数据里没有认识的东西（既非文件
            // 也非链接），画了落点框却什么都加不进去是假承诺，不画。
            launcherWindow_->setFileDropTarget(
                entered && count > 0, config_.appearance.defaultTileSize,
                point);
        });
    (void)dropRegistered;

    // 应用已保存的钉子状态（跨会话记住）
    launcherWindow_->setPinned(config_.appearance.pinned);

    // 回到上次所在的那一页（页已被删除时按 id 找不到，停在第一页即可）
    launcherWindow_->setActiveTabById(config_.appearance.activeTabId);

    return true;
}

// --- 磁贴右键菜单

void Application::handleItemContextMenu(const HWND owner,
                                        const std::size_t itemIndex,
                                        const core::LaunchItem& item) {
    if (!launcherWindow_) {
        return;
    }

    POINT cursor{};
    GetCursorPos(&cursor);

    // 页面列表供「移动到其他页面」子菜单使用；页名转 UTF-16 一次
    //（菜单要显示中文页名）。
    std::vector<std::wstring> tabNames;
    for (const core::Tab& tab : launcherWindow_->document().tabs) {
        tabNames.push_back(base::utf8ToWideLossy(tab.name));
    }
    const std::size_t activeTab = launcherWindow_->activeTabIndex();

    const ui::TileMenuCommand command =
        ui::showTileContextMenu(owner, cursor, item, tabNames, activeTab,
                                isFollowingStartup(item.id));

    // 命令 ID = 各自基址 + 目标页下标。必须显式判断边界：
    // 只判 >= moveBase 会把复制段也吞进移动。
    const auto moveBase = static_cast<std::size_t>(ui::TileMenuCommand::MoveToTabBase);
    const auto copyBase = static_cast<std::size_t>(ui::TileMenuCommand::CopyToTabBase);
    const auto commandId = static_cast<std::size_t>(command);
    if (commandId >= moveBase &&
        commandId < moveBase + kTileMenuTabEntries) {
        applyItemMoveToTab(itemIndex, commandId - moveBase);
        return;
    }
    if (commandId >= copyBase &&
        commandId < copyBase + kTileMenuTabEntries) {
        applyItemCopyToTab(itemIndex, commandId - copyBase);
        return;
    }

    switch (command) {
        case ui::TileMenuCommand::Run:
            launchItem(item, false);
            break;

        case ui::TileMenuCommand::RunAsAdministrator:
            // 菜单项强制提权：无视项目自身的管理员设置
            launchItem(item, true);
            break;

        case ui::TileMenuCommand::OpenFileLocation: {
            const platform::win32::LaunchResult result =
                platform::win32::revealItemInExplorer(item);
            if (!result.ok) {
                MessageBoxW(owner, result.error.c_str(), kApplicationName,
                            MB_OK | MB_ICONWARNING);
            }
            // 辅助类动作：查看位置后窗口保持打开
            break;
        }

        case ui::TileMenuCommand::ToggleFollowStartup:
            // 勾选切换：加入或移除，都不启动，语义是「下次 KTile 启动」。
            toggleFollowStartup(item);
            break;

        case ui::TileMenuCommand::ResizeSmall:
        case ui::TileMenuCommand::ResizeWide:
        case ui::TileMenuCommand::ResizeTall:
        case ui::TileMenuCommand::ResizeLarge: {
            const core::TileSize newSize =
                (command == ui::TileMenuCommand::ResizeWide) ? core::TileSize::Wide
                : (command == ui::TileMenuCommand::ResizeTall) ? core::TileSize::Tall
                : (command == ui::TileMenuCommand::ResizeLarge) ? core::TileSize::Large
                                                                : core::TileSize::Small;
            applyItemResize(itemIndex, newSize);
            break;
        }

        case ui::TileMenuCommand::LabelFollowGlobal:
        case ui::TileMenuCommand::LabelAlways:
        case ui::TileMenuCommand::LabelOnHover: {
            const core::TileLabelMode mode =
                (command == ui::TileMenuCommand::LabelAlways)
                    ? core::TileLabelMode::Always
                    : (command == ui::TileMenuCommand::LabelOnHover)
                          ? core::TileLabelMode::OnHover
                          : core::TileLabelMode::FollowGlobal;
            applyItemLabelMode(itemIndex, mode);
            break;
        }

        case ui::TileMenuCommand::Edit:
            applyItemEdit(owner, itemIndex, item);
            break;

        case ui::TileMenuCommand::Remove:
            applyItemRemoval(owner, itemIndex, item);
            break;

        case ui::TileMenuCommand::None:
        default:
            break;
    }
}

void Application::launchItem(const core::LaunchItem& item,
                             const bool forceAdministrator) {
    if (!launcherWindow_) {
        return;
    }

    // 插件磁贴点击 = 立即刷新（忽略 TTL），面板保持打开；
    // 缓存里的旧数据在刷新期间继续显示，不会闪成空白。
    if (item.type == core::ItemType::Plugin) {
        pluginController_.requestRefresh(item, true);
        return;
    }

    const platform::win32::LaunchResult result =
        platform::win32::launchItem(item, forceAdministrator);
    if (result.ok) {
        // 启动成功即隐藏：启动器是「唤出 → 启动 → 消失」的轻量工具
        launcherWindow_->hide();
        return;
    }

    // 失败保持窗口可见，让用户能接着操作（比如右键改路径）
    MessageBoxW(launcherWindow_->hwnd(), result.error.c_str(),
                kApplicationName, MB_OK | MB_ICONWARNING);
}

void Application::applyItemResize(const std::size_t itemIndex,
                                  const core::TileSize newSize) {
    if (!launcherWindow_) {
        return;
    }

    // 直接改 UI 层持有的文档再由它统一广播变更，
    // 避免「app 与 ui 各存一份」的状态分裂。
    if (!launcherWindow_->setItemTileSize(itemIndex, newSize)) {
        return;  // 尺寸没变，不做任何事
    }

    document_ = launcherWindow_->document();
    persistItems();
}

void Application::applyItemLabelMode(const std::size_t itemIndex,
                                     const core::TileLabelMode mode) {
    if (!launcherWindow_) {
        return;
    }

    // 与改尺寸同一套路：改 UI 层持有的文档再广播 + 落盘，
    // 只影响标题画不画，磁贴位置不变。
    if (!launcherWindow_->setItemLabelMode(itemIndex, mode)) {
        return;
    }

    document_ = launcherWindow_->document();
    persistItems();
}

void Application::applyItemMoveToTab(const std::size_t itemIndex,
                                    const std::size_t targetTab) {
    if (!launcherWindow_) {
        return;
    }

    const std::size_t activeTab = launcherWindow_->activeTabIndex();
    core::ItemsDocument updated = launcherWindow_->document();
    // 保留网格坐标：目标页冲突时布局引擎就近避让，
    // 比清掉坐标随便找空位更贴用户预期。
    if (!core::moveItemToTab(updated, activeTab, itemIndex, targetTab)) {
        return;  // 同页 / 下标越界：什么都不做
    }

    launcherWindow_->setDocument(std::move(updated));
    launcherWindow_->recomputeLayout();
    launcherWindow_->refresh();

    document_ = launcherWindow_->document();
    persistItems();
}

void Application::applyItemCopyToTab(const std::size_t itemIndex,
                                     const std::size_t targetTab) {
    if (!launcherWindow_) {
        return;
    }

    const std::size_t activeTab = launcherWindow_->activeTabIndex();
    core::ItemsDocument updated = launcherWindow_->document();

    // 复制件必须是全新身份：生成新 id（失败回落 fallback-tick-seq，
    // 与手工添加项目同一套兜底）。
    std::string newId = platform::win32::createGuidString();
    if (newId.empty()) {
        newId = "copy-" + std::to_string(GetTickCount64()) + "-" +
                std::to_string(itemIndex);
    }
    if (!core::copyItemToTab(updated, activeTab, itemIndex, targetTab,
                             newId)) {
        return;  // 同页 / 下标越界：什么都不做
    }

    launcherWindow_->setDocument(std::move(updated));
    launcherWindow_->recomputeLayout();
    launcherWindow_->refresh();

    document_ = launcherWindow_->document();
    persistItems();
}

void Application::applyItemEdit(const HWND owner,
                                const std::size_t itemIndex,
                                const core::LaunchItem& item) {
    if (!launcherWindow_) {
        return;
    }

    // ---- 组装对话框的初始值（UTF-8 → UTF-16）----
    ui::ItemEditorFields initial;
    initial.name = base::utf8ToWideLossy(item.name);
    initial.type = item.type;
    initial.target = base::utf8ToWideLossy(item.target);
    initial.arguments.reserve(item.arguments.size());
    for (const std::string& argument : item.arguments) {
        initial.arguments.push_back(base::utf8ToWideLossy(argument));
    }
    if (item.workingDirectory.has_value()) {
        initial.workingDirectory = base::utf8ToWideLossy(*item.workingDirectory);
    }
    if (item.icon.has_value()) {
        initial.icon = base::utf8ToWideLossy(*item.icon);
    }
    initial.runAsAdministrator = item.runAsAdministrator;
    initial.tileSize = item.tileSize;
    initial.labelMode = item.labelMode;
    initial.iconScalePercent = item.iconScalePercent;
    // 本项自己的底色（空 = 跟随设置里的全局默认 / 皮肤）
    initial.tileColor = item.tileColor;
    // 初值 = 当前所在页：用户在哪一页上右键，改的就是哪一页的东西。
    initial.tabIndex = launcherWindow_->activeTabIndex();

    std::vector<std::wstring> tabNames;
    tabNames.reserve(document_.tabs.size());
    for (const core::Tab& tab : document_.tabs) {
        tabNames.push_back(base::utf8ToWideLossy(tab.name));
    }

    // ---- 插件磁贴：给对话框一个「按真实磁贴效果渲染」的回调 ----
    //
    // 插件磁贴没有目标图标，预览包成回调走主窗口的渲染路径，
    // 避免在编辑对话框里再实现一套（两套必然逐渐漂移）。
    ui::PluginPreviewRenderer pluginPreview;
    if (item.type == core::ItemType::Plugin) {
        const core::PluginView view = pluginController_.viewFor(item);
        pluginPreview = [this, view](HDC dc, const RECT& tile,
                                     core::TileSize size) {
            return launcherWindow_ != nullptr &&
                   launcherWindow_->renderPluginPreview(dc, tile, view, size);
        };
    }

    // ---- 弹对话框（期间主窗口保持可见：owner 关系保证点击判定）----
    // 传全局「默认显示标题」让预览如实画出「跟随全局设置」那一档。
    ui::ItemEditorFields result;
    if (!ui::showItemEditorDialog(
            owner, tabNames, initial.tabIndex, initial,
            launcherWindow_->alwaysShowTileLabels(),
            static_cast<int>(config_.appearance.iconScalePercent),
            config_.appearance.defaultTileColor, &result,
            ui::ItemEditorMode::Edit, std::move(pluginPreview))) {
        return;  // 用户取消
    }

    // ---- 转回 UTF-8 交给核心层 ----
    core::ItemEditFields fields;
    fields.name = base::wideToUtf8Lossy(result.name);
    fields.type = result.type;
    fields.target = base::wideToUtf8Lossy(result.target);
    fields.arguments.reserve(result.arguments.size());
    for (const std::wstring& argument : result.arguments) {
        fields.arguments.push_back(base::wideToUtf8Lossy(argument));
    }
    fields.workingDirectory = base::wideToUtf8Lossy(result.workingDirectory);
    fields.icon = base::wideToUtf8Lossy(result.icon);
    fields.runAsAdministrator = result.runAsAdministrator;
    fields.tileSize = result.tileSize;
    fields.labelMode = result.labelMode;
    fields.iconScalePercent = result.iconScalePercent;
    // 外层 optional = 这次编辑要处理颜色；内层空 = 清除自定义色。
    // 对话框总是回填当前值，没动过时与 item.tileColor 相等，
    // editItem 会判定为无变化。
    fields.tileColor = std::optional<std::optional<std::uint32_t>>(
        result.tileColor);

    const std::size_t activeTab = launcherWindow_->activeTabIndex();

    core::ItemsDocument updated = launcherWindow_->document();
    if (!core::editItem(updated, activeTab, itemIndex, fields)) {
        return;  // 无实际变化（对话框已挡住空目标）
    }

    // ---- 页面迁移：选了别的页就把磁贴搬过去，保留网格坐标（冲突时
    // 就近避让）。先编辑再搬移，搬移会改变 itemIndex 的语义。
    if (result.tabIndex != activeTab) {
        (void)core::moveItemToTab(updated, activeTab, itemIndex,
                                  result.tabIndex);
    }

    launcherWindow_->setDocument(std::move(updated));
    // 目标或图标可能刚被换掉，缓存键只看 id 不看内容，
    // 旧位图必须作废，否则磁贴还显示上一张图标。
    launcherWindow_->invalidateIconCache();
    launcherWindow_->recomputeLayout();
    launcherWindow_->refresh();

    document_ = launcherWindow_->document();
    persistItems();
}

void Application::applyItemAdd(const HWND owner,
                               const std::wstring& initialTarget) {
    if (!launcherWindow_) {
        return;
    }

    // ---- 对话框初值：新建态。全空，只带设置里指定的默认磁贴尺寸，
    // 用户偏好过的东西不必每次重选。
    ui::ItemEditorFields initial;
    initial.type = core::ItemType::Application;  // 目标推断失败时的兜底
    initial.target = initialTarget;
    initial.tileSize = config_.appearance.defaultTileSize;
    initial.labelMode = core::TileLabelMode::FollowGlobal;
    // 默认加进当前所在的那一页（对话框里可以改）
    initial.tabIndex = launcherWindow_->activeTabIndex();

    std::vector<std::wstring> tabNames;
    tabNames.reserve(document_.tabs.size());
    for (const core::Tab& tab : document_.tabs) {
        tabNames.push_back(base::utf8ToWideLossy(tab.name));
    }
    if (tabNames.empty()) {
        tabNames.push_back(infrastructure::i18n::trw("app.firstRun.tabFavorites").c_str());
    }

    // ---- 复用「修改项目」那套对话框（新建模式）----
    ui::ItemEditorFields result;
    if (!ui::showItemEditorDialog(
            owner, tabNames, initial.tabIndex, initial,
            launcherWindow_->alwaysShowTileLabels(),
            static_cast<int>(config_.appearance.iconScalePercent),
            config_.appearance.defaultTileColor, &result,
            ui::ItemEditorMode::Create)) {
        return;  // 用户取消
    }

    // ---- 组装新磁贴 ----
    core::LaunchItem item;
    item.id = platform::win32::createGuidString();
    item.type = result.type;  // 对话框已按目标推断并回填
    // 名称留空按目标取名，这是对话框「名称留空时自动使用目标名称」
    // 承诺过的行为：名字为空的磁贴不画标题，用户会以为磁贴坏了。
    item.name = result.name.empty()
                    ? deriveItemName(result.target, result.type)
                    : base::wideToUtf8Lossy(result.name);
    item.target = base::wideToUtf8Lossy(result.target);
    item.arguments.reserve(result.arguments.size());
    for (const std::wstring& argument : result.arguments) {
        item.arguments.push_back(base::wideToUtf8Lossy(argument));
    }
    if (!result.workingDirectory.empty()) {
        item.workingDirectory = base::wideToUtf8Lossy(result.workingDirectory);
    }
    if (!result.icon.empty()) {
        item.icon = base::wideToUtf8Lossy(result.icon);
    }
    item.runAsAdministrator = result.runAsAdministrator;
    item.tileSize = result.tileSize;
    item.labelMode = result.labelMode;
    item.tileColor = result.tileColor;

    core::ItemsDocument updated = launcherWindow_->document();
    const std::size_t tabIndex =
        (result.tabIndex < updated.tabs.size()) ? result.tabIndex : 0;

    std::size_t newIndex = 0;
    if (!core::addItem(updated, tabIndex, std::move(item), &newIndex)) {
        // 对话框已挡住空目标，剩下的可能只有 id 撞车这类几乎不可能的
        // 事故，仍要如实报告不能静默吞掉。
        MessageBoxW(owner, T("app.error.addItem"), kApplicationName,
                    MB_OK | MB_ICONWARNING);
        return;
    }

    launcherWindow_->setDocument(std::move(updated));
    launcherWindow_->recomputeLayout();
    launcherWindow_->refresh();

    document_ = launcherWindow_->document();
    persistItems();
}

void Application::addPluginItem(const std::string& pluginId) {
    if (!launcherWindow_ || pluginId.empty()) {
        return;
    }

    const core::PluginManifest* manifest =
        pluginController_.store().find(pluginId);
    if (manifest == nullptr) {
        return;  // 清单已被删除 / 扫描失败：不添加一块永远画不出的磁贴
    }

    core::LaunchItem item;
    item.id = platform::win32::createGuidString();
    if (item.id.empty()) {
        item.id = "plugin-" + pluginId + "-" +
                  std::to_string(GetTickCount64());
    }
    item.type = core::ItemType::Plugin;
    item.name = manifest->name;
    // target 只是插件 id 的冗余记录：启动路径按 Plugin 类型走刷新而不是
    // ShellExecute；保留它也让用户手工编辑 JSON 时一眼可读。
    item.target = pluginId;
    item.pluginId = pluginId;
    item.tileSize = config_.appearance.defaultTileSize;

    core::ItemsDocument updated = launcherWindow_->document();
    const std::size_t tabIndex = launcherWindow_->activeTabIndex();
    if (!core::addItem(updated, tabIndex, std::move(item), nullptr)) {
        return;  // id 撞车等极小概率事件：静默放弃，不弹框打断
    }

    launcherWindow_->setDocument(std::move(updated));
    launcherWindow_->recomputeLayout();
    launcherWindow_->refresh();

    document_ = launcherWindow_->document();
    persistItems();

    // 刚加进来的插件磁贴没有缓存，排一次刷新（命中 TTL 时控制器自己跳过）。
    const core::ItemsDocument& current = launcherWindow_->document();
    if (tabIndex < current.tabs.size()) {
        pluginController_.refreshItems(current.tabs[tabIndex].items, false);
    }
}

void Application::addDroppedSources(
    std::vector<platform::win32::DroppedSource> sources,
    const std::optional<POINTL> dropPoint) {
    if (!launcherWindow_ || sources.empty()) {
        return;
    }

    // 已存在的目标（小写化后比较）只在当前页内去重：拖同一个文件两次
    // 不该加出两块，磁贴一格一个，重复项只会让面板越拖越乱。
    // 跨页不去重：「同一个程序放两页」正是多标签页的核心能力。
    const std::size_t activeTab = launcherWindow_->activeTabIndex();
    const std::size_t activeTabCount = launcherWindow_->tabCount();
    std::vector<std::string> existingTargets;
    if (activeTabCount > 0) {
        const auto& document = launcherWindow_->document();
        const std::size_t index =
            (activeTab < document.tabs.size()) ? activeTab : 0;
        for (const core::LaunchItem& item : document.tabs[index].items) {
            existingTargets.push_back(asciiLower(item.target));
        }
    }

    std::vector<core::LaunchItem> fresh;
    std::size_t duplicates = 0;
    for (const platform::win32::DroppedSource& source : sources) {
        std::wstring sourceValue = source.value;

        // .lnk 解析出真实目标再入库。去重也用解析后的 target：
        // 拖同一个程序的快捷方式和拖它本体应识别为同一个东西。
        std::wstring linkArguments;
        std::wstring linkWorkingDirectory;
        if (source.kind == platform::win32::DroppedKind::Path &&
            _wcsicmp(PathFindExtensionW(sourceValue.c_str()), L".lnk") == 0) {
            const ResolvedShortcut resolved = resolveShortcut(sourceValue);
            if (!resolved.target.empty()) {
                sourceValue = resolved.target;
                linkArguments = resolved.arguments;
                linkWorkingDirectory = resolved.workingDirectory;
            }
        }

        const std::string target = base::wideToUtf8Lossy(sourceValue);
        const std::string lowered = asciiLower(target);

        bool alreadyThere = false;
        for (const std::string& existing : existingTargets) {
            if (existing == lowered) {
                alreadyThere = true;
                break;
            }
        }
        if (alreadyThere) {
            ++duplicates;
            continue;
        }

        core::LaunchItem item;
        item.id = platform::win32::createGuidString();
        item.target = target;
        item.tileSize = config_.appearance.defaultTileSize;
        item.labelMode = core::TileLabelMode::FollowGlobal;

        // 类型：链接就是网址，其余按目标推断（存在的目录 → 文件夹、
        // .exe → 应用）；推断不出（路径不存在且无扩展名）按文件处理，
        // 拖进来的东西十有八九就是文件。
        item.type = (source.kind == platform::win32::DroppedKind::Url)
                        ? core::ItemType::Url
                        : ui::inferItemType(sourceValue)
                              .value_or(core::ItemType::File);

        // 名字跟着类型走（与对话框「添加新项目」共用同一条规则）
        item.name = deriveItemName(sourceValue, item.type);

        // 快捷方式自带的启动参数与工作目录一并继承，
        // 否则带参数的快捷方式拖进来启动行为就变了。
        if (!linkArguments.empty()) {
            item.arguments.push_back(
                base::wideToUtf8Lossy(linkArguments));
        }
        if (!linkWorkingDirectory.empty()) {
            item.workingDirectory =
                base::wideToUtf8Lossy(linkWorkingDirectory);
        }

        // 同一批里也可能有重复（多选时把同一个文件拖了两次）
        existingTargets.push_back(lowered);
        fresh.push_back(std::move(item));
    }

    if (fresh.empty()) {
        // 什么都没加进去时必须说清楚为什么，否则用户只会觉得功能是坏的。
        const wchar_t* reason = (duplicates > 0)
                                    ? T("app.info.alreadyAdded")
                                    : T("app.error.dropUnknown");
        MessageBoxW(launcherWindow_->hwnd(), reason, kApplicationName,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    // 安放：直接落在光标所在那一格，压到谁就把谁挤开（与拖动磁贴同一套
    // 规则）。在 UI 层完成，只有它知道网格几何，还能顺带播让位动画、
    // 立即落盘。
    if (!launcherWindow_->insertNewItemsAt(std::move(fresh), dropPoint)) {
        MessageBoxW(launcherWindow_->hwnd(), T("app.error.addItem"),
                    kApplicationName, MB_OK | MB_ICONWARNING);
        return;
    }

    document_ = launcherWindow_->document();
}

void Application::applyItemRemoval(const HWND owner,
                                   const std::size_t itemIndex,
                                   const core::LaunchItem& item) {
    if (!launcherWindow_) {
        return;
    }

    const std::wstring displayName =
        base::utf8ToWideLossy(item.name.empty() ? item.target : item.name);

    // 删磁贴不动跟随清单：删磁贴不等于不要开机启动，
    // 确认框里把这层说清楚，别让用户下次开机才发现。
    if (!ui::confirmItemRemoval(owner, displayName,
                                isFollowingStartup(item.id))) {
        return;
    }

    core::ItemsDocument updated = launcherWindow_->document();
    const std::size_t activeTab = launcherWindow_->activeTabIndex();
    if (!core::removeItem(updated, activeTab, itemIndex)) {
        return;
    }

    launcherWindow_->setDocument(std::move(updated));
    launcherWindow_->recomputeLayout();
    launcherWindow_->refresh();

    document_ = launcherWindow_->document();
    persistItems();
}

// 按配置把皮肤配色与背景图应用到主窗口。皮肤名失效（文件被删/改名）时
// 回落默认配色但不回写配置，用户恢复文件后选择仍有效；背景图读不出来
// 先用纯色，路径保留在配置里。
void Application::applyThemeFromConfig() {
    if (launcherWindow_ == nullptr) {
        return;
    }

    ui::ThemeColors theme;
    const std::wstring themeName =
        base::utf8ToWide(config_.appearance.themeName);
    if (!themeName.empty()) {
        ui::loadThemeByName(themeName, &theme);
    }
    launcherWindow_->setTheme(theme);

    if (config_.appearance.backgroundImage.empty()) {
        launcherWindow_->setBackgroundImage(std::wstring());
    } else {
        launcherWindow_->setBackgroundImage(ui::resolveBackgroundImagePath(
            base::utf8ToWide(config_.appearance.backgroundImage)));
    }
}

void Application::openSettings() {
    if (!launcherWindow_) {
        return;
    }

    // 设置期间主窗口保持可见（像被钉住一样）。皮肤/背景图是所见即所得
    // 的设置项，主窗口必须在场才看得到；且设置窗口内部的下拉列表是
    // 独立顶层窗口、不在主窗口 owner 链上，会被「点外面收起」判成外部
    // 点击让主窗口消失。setModalGuard 让主窗口期间不响应收起（不动钉子偏好）。
    if (!launcherWindow_->isVisible()) {
        launcherWindow_->show();
    }
    launcherWindow_->setModalGuard(true);

    // 磁贴来源的跟随条目先按磁贴当前值刷一次：设置窗口显示的就是
    // 待会儿真正会启动的东西，省得用户对着旧名字猜。
    if (syncFollowLaunchWithTiles()) {
        persistConfig();
    }

    // 实时预览：改了立刻反映到主窗口但不落盘，点「确定」才写 config，
    // 点「取消」由后面重新套用配置还原。
    ui::SettingsPreviewHooks preview;
    preview.themeChanged = [this](const std::string& themeName) {
        ui::ThemeColors theme;
        const std::wstring wideName = base::utf8ToWide(themeName);
        if (!wideName.empty()) {
            ui::loadThemeByName(wideName, &theme);
        }
        launcherWindow_->setTheme(theme);
    };
    preview.tabBarAtBottomChanged = [this](const bool atBottom) {
        launcherWindow_->setTabBarAtBottom(atBottom);
    };
    preview.backgroundChanged = [this](const std::wstring& absolutePath) {
        // setBackgroundImage 的语义正好对上：空串 = 撤掉背景，
        // 非空 = 直接解码这张图（预览阶段不复制进 data 目录）。
        launcherWindow_->setBackgroundImage(absolutePath);
    };
    preview.gridChanged = [this](const std::uint16_t columns,
                                 const std::uint16_t rows) {
        if (columns == launcherWindow_->gridColumns() &&
            rows == launcherWindow_->gridRows()) {
            return;  // 输入过程中的中间值，没必要重建
        }
        launcherWindow_->setGridSize(columns, rows);
        launcherWindow_->recomputeLayout();
        // 网格决定客户区尺寸：必须立刻改窗口大小，否则超出的磁贴被旧
        // 尺寸裁掉，看起来像设置没生效。
        launcherWindow_->applyGridGeometry();
    };
    preview.showTileLabelsChanged = [this](const bool showLabels) {
        launcherWindow_->setShowTileLabels(showLabels);
    };
    preview.alwaysShowTitleChanged = [this](const bool alwaysShow) {
        launcherWindow_->setAlwaysShowTileLabels(alwaysShow);
    };
    preview.windowOpacityChanged = [this](const int percent) {
        launcherWindow_->setWindowOpacityPercent(percent);
    };
    preview.tileOpacityChanged = [this](const int percent) {
        launcherWindow_->setTileOpacityPercent(percent);
    };
    preview.iconScaleChanged = [this](const int percent) {
        launcherWindow_->setTileIconScalePercent(percent);
    };
    preview.defaultTileColorChanged = [this](
        std::optional<std::uint32_t> color) {
        launcherWindow_->setDefaultTileColor(color);
    };

    // 统计当前布局实际占用的行列，给「缩小网格」的提示。只提示不阻止：
    // 超出的磁贴会被布局引擎重新安置，但要告诉用户「改小会重排」。
    std::uint16_t columnsInUse = 0;
    std::uint16_t rowsInUse = 0;
    for (const auto& placement : launcherWindow_->layout().placements) {
        const auto right = static_cast<std::uint16_t>(
            placement.column + placement.spanColumns);
        const auto bottom = static_cast<std::uint16_t>(
            placement.row + placement.spanRows);
        if (right > columnsInUse) {
            columnsInUse = right;
        }
        if (bottom > rowsInUse) {
            rowsInUse = bottom;
        }
    }

    // ---- 插件管理页回调 ----
    //
    // 设置窗口不直接碰磁盘：安装/删除转发给 PluginController，
    // 由它重新扫描清单并通知界面刷新，这里只做「模型 → 显示字段」翻译。
    ui::SettingsPluginHooks pluginHooks;
    pluginHooks.list = [this]() {
        std::vector<ui::SettingsPluginInfo> rows;
        for (const core::PluginManifest& manifest :
             pluginController_.store().manifests()) {
            ui::SettingsPluginInfo info;
            info.id = manifest.id;
            info.name = manifest.name;
            switch (manifest.source.kind) {
                case core::PluginSourceKind::Builtin:
                    info.sourceLabel = U("plugin.source.local");
                    break;
                case core::PluginSourceKind::Http:
                    info.sourceLabel = U("plugin.source.network");
                    break;
                case core::PluginSourceKind::Command:
                    info.sourceLabel = U("plugin.source.command");
                    break;
            }
            info.refreshSeconds = manifest.refreshSeconds;
            info.visibleRefreshSeconds = manifest.visibleRefreshSeconds;
            rows.push_back(std::move(info));
        }
        return rows;
    };
    pluginHooks.install = [this](const std::wstring& manifestPath,
                                 std::string* error) {
        return pluginController_.installPlugin(manifestPath, error);
    };
    pluginHooks.remove = [this](const std::string& id, std::string* error) {
        return pluginController_.removePlugin(id, error);
    };
    pluginHooks.openFolder = [this](const std::string& id) {
        pluginController_.openPluginFolder(id);
    };
    pluginHooks.addTile = [this](const std::string& id) {
        addPluginItem(id);
    };

    // ---- 轮盘插件页回调 ----
    //
    // 这一页只读：轮盘插件是「目录 + JSON + 图标」的形态，界面能做的
    // 最有用的事是展示加载了哪些、为什么被跳过，并提供打开 JSON 的入口。
    ui::SettingsRadialPluginHooks radialPluginHooks;
    radialPluginHooks.list = [this]() {
        std::vector<ui::SettingsRadialPluginInfo> rows;
        for (const core::RadialPlugin& plugin : radialPluginStore_.plugins()) {
            ui::SettingsRadialPluginInfo info;
            info.id = plugin.id;
            info.name = plugin.name;
            for (std::size_t i = 0; i < plugin.processes.size(); ++i) {
                if (i > 0) {
                    info.processes += ", ";
                }
                info.processes += plugin.processes[i];
            }
            info.sectorCount = static_cast<int>(plugin.menu.sectorCount);
            info.actionCount = static_cast<int>(plugin.menu.actions.size());
            info.valid = plugin.valid;
            info.error = plugin.error;
            rows.push_back(std::move(info));
        }
        return rows;
    };
    radialPluginHooks.rescan = [this]() {
        // 磁盘是真相：扫描一次，下一次 list() 就会反映增删改的结果。
        loadRadialPlugins();
    };
    radialPluginHooks.openManifest = [this](const std::string& id) {
        const std::wstring manifest =
            radialPluginDirectoryOf(id) + L"\\radial.json";
        const platform::win32::LaunchResult result =
            platform::win32::openPathWithDefaultProgram(manifest);
        if (!result.ok) {
            MessageBoxW(launcherWindow_->hwnd(), result.error.c_str(),
                        T("app.radialPluginTitle"), MB_OK | MB_ICONERROR);
        }
    };
    radialPluginHooks.openFolder = [this](const std::string& id) {
        const std::wstring directory = radialPluginDirectoryOf(id);
        if (directory.empty()) {
            return;
        }
        const platform::win32::LaunchResult result =
            platform::win32::openFolderInExplorer(directory);
        if (!result.ok) {
            MessageBoxW(launcherWindow_->hwnd(), result.error.c_str(),
                        T("app.radialPluginTitle"), MB_OK | MB_ICONERROR);
        }
    };
    radialPluginHooks.rootDirectory = [this]() {
        return radialPluginStore_.rootDirectory();
    };

    ui::SettingsResult result;
    const bool accepted = ui::showSettingsWindow(
        launcherWindow_->hwnd(),
        config_.appearance.gridColumns,
        config_.appearance.gridRows,
        config_.appearance.showTileLabels,
        config_.appearance.alwaysShowTileLabels,
        config_.appearance.tabBarAtBottom,
        config_.appearance.windowPlacement,
        config_.appearance.themeName,
        config_.language,
        config_.activation.startWithWindows,
        config_.activation.topmostHotkeyEnabled,
        config_.activation.followLaunch,
        core::hotkeyPresetOf(config_.activation.hotkey),
        // 不透明度传当前实际生效值：磁贴背景「跟随皮肤」(-1) 时换成
        // 皮肤实际的 alpha 百分比，滑块才停在观感对应的位置。
        launcherWindow_->windowOpacityPercent(),
        launcherWindow_->effectiveTileOpacityPercent(),
        static_cast<int>(config_.appearance.iconScalePercent),
        config_.appearance.defaultTileColor,
        radialConfig_,
        columnsInUse,
        rowsInUse,
        &result,
        &preview,
        &pluginHooks,
        &radialPluginHooks);

    // 设置窗口已关闭：解除模态保护（无论确定还是取消都要解）。
    launcherWindow_->setModalGuard(false);

    if (!accepted) {
    // 取消：config 未被改动，重新套用配置即可撤销全部实时预览
    //（皮肤/背景由 applyThemeFromConfig 还原，网格改回原行列数并恢复窗口尺寸）。
        applyThemeFromConfig();
        launcherWindow_->setTabBarAtBottom(config_.appearance.tabBarAtBottom);
        launcherWindow_->setShowTileLabels(config_.appearance.showTileLabels);
        launcherWindow_->setAlwaysShowTileLabels(
            config_.appearance.alwaysShowTileLabels);
        launcherWindow_->setGridSize(config_.appearance.gridColumns,
                                     config_.appearance.gridRows);
        launcherWindow_->recomputeLayout();
        launcherWindow_->applyGridGeometry();
        launcherWindow_->setWindowOpacityPercent(
            static_cast<int>(config_.appearance.windowOpacityPercent));
        launcherWindow_->setTileOpacityPercent(
            static_cast<int>(config_.appearance.tileOpacityPercent));
        launcherWindow_->setTileIconScalePercent(
            static_cast<int>(config_.appearance.iconScalePercent));
        launcherWindow_->setDefaultTileColor(
            config_.appearance.defaultTileColor);
        return;
    }

    bool configChanged = false;
    bool gridChanged = false;
    // 需要重启才能生效的改动（当前没有，保留为扩展点）
    bool restartPending = false;

    // ---- 网格尺寸 ----
    if (result.gridColumns.has_value() || result.gridRows.has_value()) {
        const std::uint16_t columns =
            result.gridColumns.value_or(config_.appearance.gridColumns);
        const std::uint16_t rows =
            result.gridRows.value_or(config_.appearance.gridRows);

        config_.appearance.gridColumns = columns;
        config_.appearance.gridRows = rows;
        configChanged = true;
        gridChanged = true;
    }

    // ---- 显示标签 ----
    if (result.showTileLabels.has_value()) {
        config_.appearance.showTileLabels = *result.showTileLabels;
        launcherWindow_->setShowTileLabels(*result.showTileLabels);
        configChanged = true;
    }

    // ---- 标题默认显示（不勾选则鼠标移入才显示）----
    if (result.alwaysShowTileLabels.has_value()) {
        config_.appearance.alwaysShowTileLabels = *result.alwaysShowTileLabels;
        launcherWindow_->setAlwaysShowTileLabels(
            *result.alwaysShowTileLabels);
        configChanged = true;
    }

    // 标签栏位置：纯几何变化，窗口尺寸与布局不变，切位置重画即可。
    if (result.tabBarAtBottom.has_value()) {
        config_.appearance.tabBarAtBottom = *result.tabBarAtBottom;
        launcherWindow_->setTabBarAtBottom(*result.tabBarAtBottom);
        configChanged = true;
    }

    // 窗口出现位置无法预览，确定后立刻按新策略摆一次，
    // 用户关闭设置就能看到结果。
    if (result.windowPlacement.has_value()) {
        config_.appearance.windowPlacement = *result.windowPlacement;
        launcherWindow_->setWindowPlacement(*result.windowPlacement);
        launcherWindow_->applyWindowPlacement();
        configChanged = true;
    }

    // 界面语言放在最前：后面的提示框要当场用新文案。已画好的窗口不会
    // 自己改字：托盘菜单每次右键都重建、立刻生效；面板空状态在下次重绘
    // 时取词，补一次 refresh；窗口标题要补 SetWindowTextW，否则任务栏
    // 还留着旧语言的名字。
    if (result.language.has_value()) {
        config_.language = *result.language;
        const std::string effective =
            infrastructure::i18n::resolveLanguage(*result.language);
        if (infrastructure::i18n::setLanguage(effective)) {
            effectiveLanguage_ = infrastructure::i18n::languageCode();
        }
        configChanged = true;
        updateTrayTooltip();
        if (launcherWindow_ && launcherWindow_->hwnd() != nullptr) {
            SetWindowTextW(launcherWindow_->hwnd(), applicationWindowTitle());
        }
        launcherWindow_->refresh();
    }

    // ---- 皮肤 ----
    if (result.themeName.has_value()) {
        config_.appearance.themeName = *result.themeName;
        applyThemeFromConfig();
        configChanged = true;
    }

    // 空串 = 清除；非空先复制进 themes\backgrounds（哈希命名，保证绿色可搬），
    // 再应用。
    if (result.backgroundImage.has_value()) {
        if (result.backgroundImage->empty()) {
            config_.appearance.backgroundImage.clear();
        } else {
            std::wstring error;
            const std::wstring stored = ui::importBackgroundImage(
                base::utf8ToWide(*result.backgroundImage), &error);
            if (!stored.empty()) {
                config_.appearance.backgroundImage =
                    base::wideToUtf8(stored);
            } else {
                const std::wstring wideMessage =
                    error.empty() ? T("app.unknownReason") : error;
                MessageBoxW(launcherWindow_->hwnd(),
                            (T("app.error.importBackground") + wideMessage).c_str(),
                            kApplicationName,
                            MB_OK | MB_ICONWARNING);
                // 导入失败不清旧图：保持现状，用户可重试
            }
        }
        applyThemeFromConfig();
        configChanged = true;
    }

    // ---- 开机自启 ----
    if (result.startWithWindows.has_value()) {
        const auto status = platform::win32::StartupRegistration::setEnabled(
            std::wstring(kStartupRegistryValueName), *result.startWithWindows);
        if (status) {
            config_.activation.startWithWindows = *result.startWithWindows;
            configChanged = true;
        } else {
            const std::wstring wideMessage =
                base::utf8ToWide(status.error().format());
            MessageBoxW(launcherWindow_->hwnd(),
                        (T("app.error.startup") + wideMessage).c_str(),
                        kApplicationName,
                        MB_OK | MB_ICONWARNING);
        }
    }

    // ---- F7 置顶热键 ----
    if (result.topmostHotkey.has_value()) {
        config_.activation.topmostHotkeyEnabled = *result.topmostHotkey;
        // 失败只记日志：热键被占用时界面照常保存，用户可自行排查
        initializeTopmostHotkey();
        configChanged = true;
    }

    // 只落盘不立即执行：语义是「下次 KTile 启动时一并拉起」，
    // 点「添加」就当场跑起来会让整理清单变成误触区。
    if (result.followLaunch.has_value()) {
        config_.activation.followLaunch = *result.followLaunch;
        configChanged = true;
    }

    // 热键/手势可热替换：先拆旧再装新。注册失败（被别的程序占着）仍
    // 保存设置：用户选了什么是用户的意思，程序只负责告诉他这次没生效；
    // 装不上时回滚旧配置，保证至少有一个键能唤出面板。
    if (result.hotkeyPreset.has_value() &&
        *result.hotkeyPreset != core::hotkeyPresetOf(config_.activation.hotkey)) {
        const core::HotkeyConfig previous = config_.activation.hotkey;
        config_.activation.hotkey =
            core::hotkeyConfigForPreset(*result.hotkeyPreset);
        configChanged = true;
        infrastructure::applog::info(
            "设置变更：唤醒方式 " +
            base::wideToUtf8Lossy(platform::win32::formatHotkey(previous)) +
            " -> " +
            base::wideToUtf8Lossy(
                platform::win32::formatHotkey(config_.activation.hotkey)));
        if (!initializeHotkey()) {
            const std::wstring message = infrastructure::i18n::formatW(
                "app.error.hotkeyApply",
                {platform::win32::formatHotkey(config_.activation.hotkey),
                 hotkeyErrorText_});
            MessageBoxW(launcherWindow_->hwnd(), message.c_str(),
                        kApplicationName, MB_OK | MB_ICONWARNING);
            // 装不上就把旧的装回去，避免「一个都不能用」，
            // 用户至少还能用熟悉的那个键唤出面板。
            config_.activation.hotkey = previous;
            initializeHotkey();
        }
        // 设置里换了唤醒方式（或回滚到旧的）后同步托盘提示。
        updateTrayTooltip();
    }

    // 不透明度两项都即时生效：整窗改完 ex style 系统立刻重新合成；
    // 磁贴底色下一帧重绘就变（预览阶段已在显示，这里只是落盘确认）。
    if (result.windowOpacityPercent.has_value()) {
        config_.appearance.windowOpacityPercent = *result.windowOpacityPercent;
        launcherWindow_->setWindowOpacityPercent(*result.windowOpacityPercent);
        configChanged = true;
    }
    if (result.tileOpacityPercent.has_value()) {
        config_.appearance.tileOpacityPercent = *result.tileOpacityPercent;
        launcherWindow_->setTileOpacityPercent(*result.tileOpacityPercent);
        configChanged = true;
    }

    // 图标统一缩放只改绘制矩形，下一帧重绘即生效，布局与窗口尺寸不动。
    if (result.iconScalePercent.has_value()) {
        config_.appearance.iconScalePercent = *result.iconScalePercent;
        launcherWindow_->setTileIconScalePercent(*result.iconScalePercent);
        configChanged = true;
    }

    // 外层有值 = 用户确实动过（含「恢复跟随皮肤」）；
    // 内层空 = 清空默认色，回到皮肤 tileFill。
    if (result.defaultTileColor.has_value()) {
        config_.appearance.defaultTileColor = *result.defaultTileColor;
        launcherWindow_->setDefaultTileColor(
            config_.appearance.defaultTileColor);
        configChanged = true;
    }

    // 整份替换而不是逐字段合并：轮盘方案与按应用规则是嵌套结构，
    // 用户可能删掉方案、改扇区数，逐字段合并表达不了这些变化。
    // 换完必须重装钩子，触发键可能一起变了，旧钩子仍按旧配置吞键；
    // 配置在显示时才被读取，无需重启。
    if (result.radial.has_value()) {
        radialConfig_ = *result.radial;
        radialHook_.uninstall();
        lastRadialInstallTick_ = 0;
        if (radialConfig_.trigger.enabled && messageWindow_ != nullptr) {
            if (radialHook_.install(messageWindow_, radialConfig_.trigger,
                                radialConfig_.blacklist)) {
                infrastructure::applog::info("设置变更：轮盘触发方式已重装钩子");
                lastRadialInstallTick_ = GetTickCount64();
            } else {
                infrastructure::applog::error("设置变更：轮盘钩子重装失败");
            }
        }
        persistRadialConfig();
        updateTrayTooltip();
    }

    // 应用网格改动，顺序：先改尺寸，再重算布局，最后落盘。
    // 若先落盘再重算，中途崩溃会留下「配置是新的、界面还是旧的」的不一致。
    if (gridChanged) {
        launcherWindow_->setGridSize(config_.appearance.gridColumns,
                                     config_.appearance.gridRows);
        launcherWindow_->recomputeLayout();
        // 网格决定客户区尺寸，立即生效，否则要等下次唤出面板才变。
        launcherWindow_->applyGridGeometry();
        launcherWindow_->refresh();
    }

    if (configChanged) {
        persistConfig();
    }

    // 当前所有设置都能即时生效，此分支保留为扩展点：后续无法运行时
    // 热替换的设置（分组热键、后台常驻策略、右键菜单结构等）在
    // restartPending 里累积，最后统一提示，而不是每项弹一次框。
    if (restartPending) {
        MessageBoxW(launcherWindow_->hwnd(),
                    T("app.restart.message"),
                    T("app.restart.title"),
                    MB_OK | MB_ICONINFORMATION);
    }
}

bool Application::initializeTray() {
    // 创建隐藏消息窗口接收托盘与热键消息
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(WNDCLASSEXW);
    windowClass.lpfnWndProc = &messageWindowProc;
    windowClass.hInstance = instance_;
    windowClass.lpszClassName = kMessageWindowClassName;

    const ATOM atom = RegisterClassExW(&windowClass);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    messageWindow_ = CreateWindowExW(
        0,
        kMessageWindowClassName,
        L"KTile Message Window",
        0, 0, 0, 0, 0,
        HWND_MESSAGE,  // 只收消息，不显示
        nullptr,
        instance_,
        this);

    if (messageWindow_ == nullptr) {
        return false;
    }

    // 加载图标（无内嵌资源时用纯色兜底图标）
    platform::win32::UniqueHIcon icon = platform::win32::loadApplicationIcon(
        instance_, platform::win32::kApplicationIconResourceId);
    if (!icon.valid()) {
        icon = platform::win32::createFallbackIcon(16, RGB(0x2D, 0x7D, 0xD2));
    }

    platform::win32::TrayIcon::ActivateHandler activateHandler = [this]() {
        toggleLauncher();
    };
    tray_.setActivateHandler(std::move(activateHandler));
    tray_.setCommandHandler([this](const UINT commandId) {
        handleTrayCommand(commandId);
    });

    // 提示文字必须跟随配置：选 Win 键 / 双击 Ctrl 时不能再写死 Alt+Space。
    const std::wstring tooltip = L"KTile - " +
        platform::win32::formatHotkey(config_.activation.hotkey) + infrastructure::i18n::trw("app.tray.tooltipSuffix");

    const auto status = tray_.create(
        messageWindow_,
        WM_TRAYICON_CALLBACK,
        tooltip,
        icon.get());
    if (!status) {
        return false;
    }

    // 图标所有权转移给 TrayIcon（它用 get() 持有，销毁时通知系统移除）。
    // HICON 要活到菜单显示期间，用全局变量持有。
    g_trayIconForMenu = icon.release();

    return true;
}

void Application::updateTrayTooltip() {
    // 与启动时同一套文案：唤醒方式变化后立即刷新，避免提示与实际不符。
    const std::wstring tooltip = L"KTile - " +
        platform::win32::formatHotkey(config_.activation.hotkey) + infrastructure::i18n::trw("app.tray.tooltipSuffix");
    tray_.setTooltip(tooltip);
}

bool Application::initializeHotkey() {
    hotkeyErrorText_.clear();

    const core::HotkeyConfig& config = config_.activation.hotkey;

    // 两种手势只能走低级键盘钩子：RegisterHotKey 表达不了「双击同一个键」
    // 和「单键」，按 gesture 分成两条互斥通道。每次先拆另一条通道，
    // 支持设置里换方式时的热替换。
    if (config.gesture != core::HotkeyGesture::None) {
        hotkey_.unregister();
        const auto status = gestureHotkey_.install(
            messageWindow_, kToggleHotkeyId, config.gesture);
        if (!status) {
            hotkeyErrorText_ = base::utf8ToWide(status.error().message);
            infrastructure::applog::error(
                "手势钩子安装失败: " + status.error().message);
            return false;
        }
        lastGestureInstallTick_ = GetTickCount64();
        return true;
    }

    gestureHotkey_.uninstall();
    const auto status = hotkey_.registerHotkey(
        messageWindow_, kToggleHotkeyId, config);
    if (!status) {
        hotkeyErrorText_ = base::utf8ToWide(status.error().message);
        infrastructure::applog::error("热键注册失败: " +
                                      status.error().message);
        return false;
    }
    infrastructure::applog::info(
        "热键已注册: " +
        base::wideToUtf8Lossy(platform::win32::formatHotkey(config)));
    return true;
}

bool Application::initializeTopmostHotkey() {
    topmostHotkey_.unregister();
    if (!config_.activation.topmostHotkeyEnabled || messageWindow_ == nullptr) {
        return true;
    }
    core::HotkeyConfig config;
    config.modifiers.clear();  // 裸键 F7
    config.key = "F7";
    const auto status =
        topmostHotkey_.registerHotkey(messageWindow_, kTopmostHotkeyId, config);
    if (!status) {
        infrastructure::applog::error("置顶热键注册失败: " +
                                      status.error().message);
        return false;
    }
    infrastructure::applog::info("置顶热键已注册: F7");
    return true;
}

void Application::toggleTopmost() {
    const HWND window = GetForegroundWindow();
    if (window == nullptr) {
        return;
    }
    // 自己的面板/对话框不参与：热键落在本进程窗口上没有意义，还会把
    // 面板钉死在最前。
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == GetCurrentProcessId()) {
        return;
    }
    const bool topmost =
        (GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    SetWindowPos(window, topmost ? HWND_NOTOPMOST : HWND_TOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    wchar_t title[128] = {};
    GetWindowTextW(window, title, 128);
    infrastructure::applog::info(std::string(topmost ? "取消置顶: " : "置顶: ") +
                                 base::wideToUtf8Lossy(title));
}

void Application::reinstallGestureHotkey(const char* reason) {
    const core::HotkeyConfig& config = config_.activation.hotkey;
    if (config.gesture == core::HotkeyGesture::None) {
        return;  // 普通 RegisterHotKey 不走钩子链，无需重装
    }
    if (messageWindow_ == nullptr) {
        return;
    }
    // install 内部会先 uninstall，一次完成「拆旧 + 装新」。失败不覆盖
    // hotkeyErrorText_：这里不是用户主动改热键，不必把「暂时没装上」
    // 当成用户能处理的错误提示出来，下次唤醒时再试。
    infrastructure::applog::info(std::string("重装手势钩子（") +
                                 (reason != nullptr ? reason : "?") + "）");
    const auto status = gestureHotkey_.install(
        messageWindow_, kToggleHotkeyId, config.gesture);
    if (status) {
        lastGestureInstallTick_ = GetTickCount64();
    } else {
        infrastructure::applog::error("重装手势钩子失败: " +
                                      status.error().message);
    }
}

// --- 消息循环

int Application::runMessageLoop() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // 让 IsDialogMessage 之外的按键也能正常分发
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

// --- 行为

void Application::toggleLauncher() {
    if (!launcherWindow_) {
        return;
    }
    launcherWindow_->toggle();
    infrastructure::applog::info(launcherWindow_->isVisible()
                                     ? "面板：显示"
                                     : "面板：隐藏");
}

void Application::showLauncher() {
    if (launcherWindow_) {
        launcherWindow_->show();
    }
}

void Application::hideLauncher() {
    if (launcherWindow_) {
        launcherWindow_->hide();
    }
}

void Application::openAbout() {
    // owner 给主窗口：About 是它的 owned 弹窗，点对话框内部不会被
    // 「点外面收起」误判（belongsToTarget 走 owner 链）。
    HWND owner = launcherWindow_ ? launcherWindow_->hwnd() : nullptr;
    ui::showAboutDialog(owner, std::wstring(applicationWindowTitle()),
                        std::wstring(kApplicationVersion));
}

void Application::showTrayMenu() {
    std::vector<platform::win32::TrayMenuItem> menuItems;

    platform::win32::TrayMenuItem showItem;
    showItem.commandId = kTrayMenuShowHide;
    showItem.text = (launcherWindow_ && launcherWindow_->isVisible())
        ? T("app.tray.hideLauncher") : T("app.tray.showLauncher");
    menuItems.push_back(showItem);

    menuItems.push_back(platform::win32::TrayMenuItem::separator());

    platform::win32::TrayMenuItem settingsItem;
    settingsItem.commandId = kTrayMenuSettings;
    settingsItem.text = T("app.tray.settings");
    menuItems.push_back(settingsItem);

    menuItems.push_back(platform::win32::TrayMenuItem::separator());

    platform::win32::TrayMenuItem startupItem;
    startupItem.commandId = kTrayMenuStartup;
    startupItem.text = T("app.tray.startup");
    startupItem.isChecked = platform::win32::StartupRegistration::isEnabled(
        std::wstring(kStartupRegistryValueName));
    menuItems.push_back(startupItem);

    menuItems.push_back(platform::win32::TrayMenuItem::separator());

    platform::win32::TrayMenuItem aboutItem;
    aboutItem.commandId = kTrayMenuAbout;
    aboutItem.text = T("app.tray.about");
    menuItems.push_back(aboutItem);

    menuItems.push_back(platform::win32::TrayMenuItem::separator());

    platform::win32::TrayMenuItem exitItem;
    exitItem.commandId = kTrayMenuExit;
    exitItem.text = T("app.tray.exit");
    menuItems.push_back(exitItem);

    POINT cursor{};
    GetCursorPos(&cursor);

    // 托盘菜单需要显式触发
    tray_.showContextMenu(menuItems, cursor);
}

void Application::handleTrayCommand(const UINT commandId) {
    switch (commandId) {
        case kTrayMenuShowHide:
            toggleLauncher();
            break;

        case kTrayMenuSettings:
            openSettings();
            break;

        case kTrayMenuAbout:
            openAbout();
            break;

        case kTrayMenuStartup: {
            const bool currentlyEnabled =
                platform::win32::StartupRegistration::isEnabled(
                    std::wstring(kStartupRegistryValueName));
            const auto status = platform::win32::StartupRegistration::setEnabled(
                std::wstring(kStartupRegistryValueName), !currentlyEnabled);

            if (status) {
                config_.activation.startWithWindows = !currentlyEnabled;
                persistConfig();
            } else {
                const std::string message = status.error().format();
                const std::wstring wideMessage = base::utf8ToWide(message);
                MessageBoxW(nullptr,
                            (T("app.error.startup") + wideMessage).c_str(),
                            kApplicationName,
                            MB_OK | MB_ICONWARNING);
            }
            break;
        }

        case kTrayMenuExit:
            infrastructure::applog::info("用户从托盘菜单退出");
            if (launcherWindow_) {
                launcherWindow_->hide();
            }
            persistItems();
            persistConfig();
            persistRadialConfig();
            tray_.destroy();
            if (messageWindow_ != nullptr) {
                DestroyWindow(messageWindow_);
                messageWindow_ = nullptr;
            }
            break;

        default:
            break;
    }
}

}  // namespace app
}  // namespace ktile
