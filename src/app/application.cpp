// KTile - 应用层：主控制器实现
#include "app/application.h"

#include "base/text_convert.h"
#include "core/builtin_items.h"
#include "core/item_operations.h"
#include "core/tab_operations.h"
#include "infrastructure/json/config_store.h"
#include "ui/launcher_window.h"
#include "platform/win32/item_launcher.h"
#include "platform/win32/icon_store.h"
#include "platform/win32/uuid.h"
#include "ui/item_editor_dialog.h"
#include "ui/about_dialog.h"
#include "ui/tile_context_menu.h"
#include "ui/settings_window.h"
#include "ui/theme.h"

#include <Shlwapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace ktile {
namespace app {
namespace {

// 消息窗口类名（用于接收热键与托盘消息的隐藏窗口）
constexpr const wchar_t* kMessageWindowClassName = L"KTile.MessageWindow";

// 托盘右键菜单命令 ID
constexpr UINT kTrayMenuShowHide = 2001;
constexpr UINT kTrayMenuStartup = 2002;
constexpr UINT kTrayMenuExit = 2003;
constexpr UINT kTrayMenuSettings = 2004;

// 磁贴菜单「移动/复制到其他页面」子菜单的最大条目数。
// 必须与 tile_context_menu.cpp 的 kMaxTabMenuEntries 一致 ——
// app 层按「基址 + 这么多条」解码命令 ID 区间。
constexpr std::size_t kTileMenuTabEntries = 32;
constexpr UINT kTrayMenuAbout = 2005;

HICON g_trayIconForMenu = nullptr;  // 避免菜单期间图标被释放

// Explorer 重启时广播的消息 ID（动态注册，不能 constexpr）。
//
// 为什么需要它：WH_KEYBOARD_LL 钩子链是 LIFO，后装的先收到事件。
// 正常情况下 KTile 比 Explorer 晚启动，我们的钩子在前，能吞掉 Win 键
// 不让它弹开始菜单。但一旦 Explorer 崩溃重启 / 注销重登，它会重新装
// 钩子排到我们前面 —— 我们的「吞」就失效了，开始菜单和 KTile 会同时弹。
// 监听这条广播，在 Explorer 重启后立刻重装钩子，把顺序抢回来。
UINT taskbarCreatedMessageId() {
    static const UINT id = RegisterWindowMessageW(L"TaskbarCreated");
    return id;
}

// ---------------------------------------------------------------- 拖入的快捷方式

// .lnk 解析结果。
struct ResolvedShortcut {
    std::wstring target;           // 真实目标路径（空 = 解析失败）
    std::wstring arguments;        // 快捷方式带的启动参数
    std::wstring workingDirectory; // 快捷方式记录的工作目录
};

// 把 .lnk 解析成真实目标。
//
// 为什么必须解析：快捷方式只是个「指路牌」，指的目标随时可能被
// 移动/删除/卸载 —— 把 .lnk 本身存成磁贴的 target，等于把磁贴的
// 生命周期拴在一个中间层上。直接存真实目标，快捷方式删了也不影响启动。
//
// 失败（不是合法 lnk / COM 失败）时返回空 target，调用方保留原路径
// —— 宁可存一个还能双击打开的快捷方式，也不能让拖入凭空失败。
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
                // 再按默认方式要一次 —— shell 会做完整的解析
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

// ---------------------------------------------------------------- 拖入项的名字

// 从路径取「文件名去掉扩展名」。
//
// stripExtension=false（文件夹）时整名保留：文件夹名里带点的很常见
// （"vs.2022"、"资料.备份"），一律砍掉点后面会把名字改得面目全非。
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

// 从 URL 取一个「当标题用」的短名。
//
// 规则：去掉协议头、去掉查询串与锚点。剩下的通常就是
// "news.qq.com/rain/a/20261016A01" 这种「一看就知道是哪儿」的串，
// 比整条带 ?utm_source=... 的原始链接干净得多。
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

// 名称留空（或拖入时本来就没名字）时的取名规则。
//
// 两条添加入口共用同一个函数：对话框上写着「名称留空时自动使用目标名称」，
// 拖入时用户没有机会填名字 —— 两处若各写一套，同一个目标会得到两种名字。
//
// 名字跟着**推断出的类型**走，而不是跟着「来源是文件还是链接」走：
// 有些来源会把网址塞进文件格式里（拖网页上的图标就是这样），
// 按类型取名能让两条入口得到一样的结果。
std::string deriveItemName(const std::wstring& target,
                           const core::ItemType type) {
    // 内置命令/系统位置的目标派生不出可读名（"ktile:lock" 的 stem 是
    // "lock"，GUID 路径的 stem 是一串十六进制）—— 查表给出中文名。
    // 正常的「系统项…」入口会在对话框里回填名称，走不到这里；
    // 这是给「手改配置文件 / 手填目标」的防御。
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

// 只把 ASCII 大写字母转小写，用于「Windows 路径不区分大小写」的比较。
// 中文等多字节序列原样保留 —— 逐字节转小写本来也不会碰它们。
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

    // TaskbarCreated 是动态消息 ID，不能进 switch —— 在 switch 前拦截。
    // Explorer 重启后立刻重装手势钩子，把「Win 键先到我们这里」的顺序抢回来。
    if (message == taskbarCreatedMessageId()) {
        if (self != nullptr) {
            self->reinstallGestureHotkey();
        }
        return 0;
    }

    switch (message) {
        case WM_HOTKEY: {
            if (self != nullptr && wParam == static_cast<WPARAM>(kToggleHotkeyId)) {
                self->toggleLauncher();
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
    if (hotkey_.isRegistered()) {
        hotkey_.unregister();
    }
    gestureHotkey_.uninstall();
    tray_.destroy();
}

// ================================================================ 启动流程

int Application::run(const HINSTANCE instance, const DebugOptions& debug) {
    instance_ = instance;

    // 单实例检查：已有实例则激活它并退出。
    //
    // 所有自动化/调试开关都会跳过该检查 —— 验证脚本必须能在
    // 「桌面上还挂着一个旧实例」的情况下照常启动自己那一份
    // （否则自动化会因为拿不到实例权而静默退出，测试全线失败）。
    // 正常的用户启动路径（无任何调试开关）行为不变。
    const bool automationMode = debug.any();

    if (!automationMode &&
        !singleInstance_.tryAcquire(std::wstring(kSingleInstanceMutexName))) {
        // 让已有实例唤出面板。
        //
        // 走消息窗口那条通路（消息内容与热键完全相同），不是直接
        // ShowWindow —— 后者会让窗口「露」出来却仍以为自己隐藏着。
        // 匹配标题必须用 kApplicationWindowTitle（主窗口真实标题），
        // 不能拿 kApplicationName（"KTile"）凑数 —— FindWindowW 是
        // 精确匹配。这条通道在热键被别的程序占用时是唯一的救命
        // 通道，必须真的能用。
        platform::win32::SingleInstance::activateExistingInstance(
            std::wstring(kMessageWindowClassName),
            std::wstring(kApplicationWindowTitle), WM_HOTKEY,
            static_cast<WPARAM>(kToggleHotkeyId));
        return 0;
    }

    if (!initializeConfig()) {
        if (!debug.shot) {
            MessageBoxW(nullptr,
                        L"初始化配置失败。",
                        kApplicationName,
                        MB_OK | MB_ICONERROR);
        }
        return 1;
    }

    if (!initializeWindow()) {
        if (!debug.shot) {
            MessageBoxW(nullptr,
                        L"创建窗口失败。",
                        kApplicationName,
                        MB_OK | MB_ICONERROR);
        }
        return 1;
    }

    // ---- 自动化：走一次「拖入添加」----
    //
    // 放在截图模式之前：debugShot 与 debugAdd 可以同时给，
    // 这样导出的截图里就能看到「刚加进来的磁贴」。
    // 注意 addPaths 为空的**普通拖放**不从这里进来 ——
    // 真实拖放由 OLE 回调驱动（见 initializeWindow 里的 setDropHandler）。
    //
    // --debug-drop=x,y 给出「松手位置」（客户区坐标）时，新磁贴就落在
    // 那一格上，与真实拖放完全同一条下游。
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
    // 只给 --debug-drop 时（不配合 --debug-add）把落点预览点亮，
    // 供截图记录「拖入悬停」那一刻的视觉。
    if (debugDropPoint.has_value() && debug.addPaths.empty() &&
        launcherWindow_ != nullptr) {
        launcherWindow_->debugSetFileDropPreview(debug.dropX, debug.dropY);
    }

    // ---- 调试：主窗口整体不透明度 ----
    //
    // 必须放在窗口创建之后（setWindowOpacityPercent 要改窗口 ex style）、
    // 截图导出之前（导出的离屏图看不出分层合成效果，验证要用真实窗口
    // 的屏幕截图）。
    if (debug.opacityPercent > 0 && launcherWindow_ != nullptr) {
        launcherWindow_->setWindowOpacityPercent(debug.opacityPercent);
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
        return launcherWindow_->debugExportPng(L"debug-shot.png") ? 0 : 3;
    }

    // ---- 调试编辑项目对话框 ----
    //
    // 打开第一个磁贴的编辑对话框并继续跑主循环 —— 供自动化验证
    // 「对话框打开期间主窗口保持可见」「保存落盘」「取消不落盘」。
    if (debug.editor && launcherWindow_ != nullptr &&
        launcherWindow_->tabCount() > 0 && !document_.tabs.empty()) {
        launcherWindow_->show();
        // 取**当前页**的第一项：多标签页下「第 0 页的第 0 项」未必是
        // 用户眼前那块磁贴，拿错了会拍出一张对不上的截图。
        const std::size_t activeTab = launcherWindow_->activeTabIndex();
        const auto& items =
            document_.tabs[activeTab < document_.tabs.size() ? activeTab : 0]
                .items;
        if (!items.empty()) {
            applyItemEdit(launcherWindow_->hwnd(), 0, items[0]);
        }
    }

    // ---- 调试「添加新项目」对话框 ----
    //
    // 与上面的差异只在对话框模式（标题「添加项目」、按钮「添加」），
    // 供自动化拍下新建态的界面并验证「回车即添加」。
    if (debug.newItem && launcherWindow_ != nullptr) {
        launcherWindow_->show();
        applyItemAdd(launcherWindow_->hwnd(), std::wstring());
    }

    if (!initializeTray()) {
        // 托盘失败不致命，仅提示
        MessageBoxW(nullptr,
                    L"创建托盘图标失败，程序仍会运行。",
                    kApplicationName,
                    MB_OK | MB_ICONWARNING);
    }

    // ---- 调试设置窗口 ----
    //
    // debug.settingsKeepAlive = false：打开设置后立刻退出（只验证控件是否建成）
    // debug.settingsKeepAlive = true ：打开设置后继续跑主循环，
    //                                用于验证「点确定不影响主循环」（问题3）
    //
    // 放在托盘（消息窗口）之后：设置里的「唤醒快捷键」会即时热替换，
    // 需要 messageWindow_ 已存在。注意**不能**连同 initializeHotkey 一起
    // 提前 —— 自动化跑在用户真实配置下（本机 Alt+Space 被 CLaunch 占着），
    // 启动注册失败的警告框会把调试入口整个堵死。
    if (debug.settings || debug.settingsKeepAlive) {
        openSettings();
        if (!debug.settingsKeepAlive) {
            return 0;
        }
    }

    // ---- 调试「关于」对话框 ----
    //
    // 打开后立刻退出：自动化只验证控件建成与文案齐全
    // （EnumChildWindows 断言），不需要人看。
    if (debug.about) {
        openAbout();
        return 0;
    }

    if (!initializeHotkey()) {
        // 把「具体是哪个键、被谁占了、去哪儿改」说清楚。
        // 以前只说「可能被其他程序占用」，用户根本不知道该动哪里 ——
        // 本机 Alt+Space 常年被 CLaunch 占着，这条提示是唯一线索。
        std::wstring message =
            L"唤醒快捷键 " +
            platform::win32::formatHotkey(config_.activation.hotkey) +
            L" 注册失败。\n";
        if (!hotkeyErrorText_.empty()) {
            message += hotkeyErrorText_ + L"\n";
        }
        message +=
            L"\n可以直接在「托盘菜单 → 设置 → 唤醒快捷键」里换一个，"
            L"例如「双击 Ctrl」不占用任何组合键。";
        MessageBoxW(nullptr, message.c_str(),
                    kApplicationName,
                    MB_OK | MB_ICONWARNING);
    }

    // 启动期的工作集裁剪由 LauncherWindow 的延迟定时器负责
    //（kStartupTrimTimerId）：初始化之后的首次渲染会把页换回来，
    // 太早裁是白裁。

    return runMessageLoop();
}

bool Application::initializeConfig() {
    // 解析便携路径（exe 同级 data 目录，不可写则降级到 %LOCALAPPDATA%）
    pathsReady_ = paths_.initialize();
    if (!pathsReady_) {
        return false;
    }

    // 告诉图标仓库「自定义图标存在哪」。
    // 必须在加载磁贴之前完成 —— item.icon 里存的是相对 data 的路径
    // （"res\xx.png"），解析依赖这个根目录。
    platform::win32::setIconStoreRoot(paths_.dataDirectory());

    // 皮肤根目录 + 首次运行生成内置预置（已存在的不覆盖）
    ui::setThemesRoot(paths_.dataDirectory());
    ui::ensurePresetThemesExist();

    // 加载配置：文件不存在时使用默认值
    const auto configResult =
        store_.loadConfig(paths_.configFilePath());
    config_ = configResult.value;

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

    return true;
}

void Application::seedSampleDocument() {
    core::ItemsDocument document;
    auto& tab = document.ensureDefaultTab();
    tab.id = "default";
    tab.name = "常用";

    // 示例磁贴：覆盖四种尺寸，用于验证混排效果
    struct SampleItem {
        const char* id;
        const char* name;
        const char* target;
        core::ItemType type;
        core::TileSize size;
    };
    const SampleItem samples[] = {
        {"1",  "记事本",   "C:\\Windows\\System32\\notepad.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"2",  "计算器",   "C:\\Windows\\System32\\calc.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"3",  "浏览器",   "C:\\Program Files\\Internet Explorer\\iexplore.exe",
         core::ItemType::Application, core::TileSize::Large},
        {"4",  "资源管理器", "C:\\Windows\\explorer.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"5",  "命令提示符", "C:\\Windows\\System32\\cmd.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"6",  "用户文档", "C:\\Users",
         core::ItemType::Folder, core::TileSize::Tall},
        {"7",  "控制面板", "C:\\Windows\\System32\\control.exe",
         core::ItemType::Application, core::TileSize::Wide},
        {"8",  "画图",     "C:\\Windows\\System32\\mspaint.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"9",  "截图工具", "C:\\Windows\\System32\\SnippingTool.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"10", "任务管理器", "C:\\Windows\\System32\\Taskmgr.exe",
         core::ItemType::Application, core::TileSize::Wide},
    };

    for (const auto& sample : samples) {
        core::LaunchItem item(sample.id, sample.type, sample.name, sample.target);
        item.tileSize = sample.size;
        tab.items.push_back(std::move(item));
    }

    // 第二个示例页：让「多标签页」这件事在首次运行时就被看见 ——
    // 只给一页的话，用户不会知道面板支持分标。
    // 注意 id 必须全文档唯一（core::addItem 的规则），所以这里另起一组。
    const std::size_t toolsIndex = core::addTab(document, "工具");
    const SampleItem toolSamples[] = {
        {"t1", "注册表编辑器", "C:\\Windows\\regedit.exe",
         core::ItemType::Application, core::TileSize::Small},
        {"t2", "服务", "C:\\Windows\\System32\\services.msc",
         core::ItemType::File, core::TileSize::Small},
        {"t3", "系统信息", "C:\\Windows\\System32\\msinfo32.exe",
         core::ItemType::Application, core::TileSize::Wide},
        {"t4", "磁盘清理", "C:\\Windows\\System32\\cleanmgr.exe",
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

bool Application::initializeWindow() {
    launcherWindow_.reset(new ui::LauncherWindow());

    if (!launcherWindow_->create(instance_,
                                 std::wstring(kApplicationWindowTitle),
                                 document_)) {
        return false;
    }

    // 应用网格配置
    launcherWindow_->setGridSize(config_.appearance.gridColumns,
                                 config_.appearance.gridRows);
    // setGridSize 只改成员，几何必须显式重算——否则窗口仍按默认
    // 8x6 造，配置成更小网格的用户会看到大片空白（实测踩过）。
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
    // 只有拖动会触发 —— 程序按策略摆窗口不写盘，避免自己污染记录。
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

    // 不透明度。必须在 applyThemeFromConfig 之后：磁贴背景的「跟随皮肤」
    // 要读皮肤里的 tileFill，皮肤还没套上时算出来的是旧值。
    // 整窗透明度在窗口创建后即可设置（不需要等渲染目标）。
    launcherWindow_->setWindowOpacityPercent(
        static_cast<int>(config_.appearance.windowOpacityPercent));
    launcherWindow_->setTileOpacityPercent(
        static_cast<int>(config_.appearance.tileOpacityPercent));
    // 全局默认磁贴底色（空 = 跟随皮肤；单项 tileColor 优先）
    launcherWindow_->setDefaultTileColor(config_.appearance.defaultTileColor);

    // ---- 回调：启动项目 ----
    // 真实启动逻辑：普通权限按项目自身的管理员设置决定是否提权；
    // 成功后隐藏窗口（启动器的标准行为），失败弹错并保持窗口可见。
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
        });

    // ---- 回调：钉子状态变更 ----
    // 钉子是用户偏好，立刻落盘，下次启动沿用。
    launcherWindow_->setPinnedChangedHandler([this](const bool pinned) {
        config_.appearance.pinned = pinned;
        persistConfig();
    });

    // ---- 回调：活动标签页变更 ----
    // 「上次在哪一页」也是用户偏好，立刻落盘 —— 面板是随时唤出随时用的
    // 东西，进程被结束（关机、任务管理器）时不会有从容的退出时机，
    // 攒到退出再写多半就丢了。
    launcherWindow_->setActiveTabChangedHandler([this](const std::size_t) {
        if (!launcherWindow_) {
            return;
        }
        config_.appearance.activeTabId = launcherWindow_->activeTabId();
        persistConfig();
    });

    // ---- 回调：磁贴右键菜单 ----
    // UI 层只负责「在哪、点的是哪一块」，菜单内容与数据修改都在这里。
    launcherWindow_->setContextMenuHandler(
        [this](HWND owner, const std::size_t itemIndex, const core::LaunchItem& item) {
            handleItemContextMenu(owner, itemIndex, item);
        });

    // ---- 回调：空白区右键 → 设置 ----
    // 托盘之外的第二个设置入口（托盘图标可能被收进溢出区）。
    launcherWindow_->setSettingsHandler([this]() { openSettings(); });

    // ---- 回调：空白区右键 → 关于 ----
    // 与设置同理由：托盘图标可能被收进溢出区，主窗口给个直达入口。
    launcherWindow_->setAboutHandler([this]() { openAbout(); });

    // ---- 回调：空白区右键 → 添加新项目 ----
    launcherWindow_->setAddItemHandler([this]() {
        if (launcherWindow_) {
            applyItemAdd(launcherWindow_->hwnd(), std::wstring());
        }
    });

    // ---- 拖放接收：把文件/文件夹/链接拖到面板上即可添加 ----
    //
    // 必须在窗口建好之后注册（RegisterDragDrop 要 HWND），
    // 且进程已 OleInitialize（见 main.cpp）。
    // 注册失败不致命：只是拖放这条入口不可用，其余功能照常 ——
    // 因此这里不弹框打扰用户，静默降级。
    //
    // 注意：Windows 的 UIPI 不允许标准权限的拖拽来源（资源管理器、
    // 浏览器）向**以管理员身份运行**的本进程投放拖放，那种情况下光标会
    // 显示「禁止」符号。这不是程序缺陷，是以普通权限启动即可解决的环境问题。
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
            // count == 0 表示这份拖拽数据里没有我们认识的东西（既非文件
            // 也非链接）—— 画了落点框却什么都加不进去是假承诺，不画。
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

// ================================================================ 磁贴右键菜单

void Application::handleItemContextMenu(const HWND owner,
                                        const std::size_t itemIndex,
                                        const core::LaunchItem& item) {
    if (!launcherWindow_) {
        return;
    }

    POINT cursor{};
    GetCursorPos(&cursor);

    // 页面列表：供菜单里的「移动到其他页面」子菜单使用。
    // 页名单向 UTF-16 转换一次即可（菜单里要显示中文页名）。
    std::vector<std::wstring> tabNames;
    for (const core::Tab& tab : launcherWindow_->document().tabs) {
        tabNames.push_back(base::utf8ToWideLossy(tab.name));
    }
    const std::size_t activeTab = launcherWindow_->activeTabIndex();

    const ui::TileMenuCommand command =
        ui::showTileContextMenu(owner, cursor, item, tabNames, activeTab);

    // ---- 「移动到其他页面 / 复制到其他页面」：命令 ID = 各自基址 + 目标页下标。
    // 两个区间必须显式判断边界 —— 只判 >= moveBase 会把复制段也吞进移动。
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

    const platform::win32::LaunchResult result =
        platform::win32::launchItem(item, forceAdministrator);
    if (result.ok) {
        // 启动成功即隐藏 —— 启动器是「唤出 → 启动 → 消失」的轻量工具
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

    // 直接改 UI 层持有的文档，再由它统一广播变更 —— 避免「app 与 ui 各存一份」
    // 导致的状态分裂。
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

    // 与改尺寸同一套路：改 UI 层持有的文档，再广播 + 落盘。
    // 不同之处是这只影响标题画不画，磁贴位置保持不变。
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
    // 保留网格坐标：目标页里若有磁贴占着同一格，布局引擎会就近避让，
    // 比「清掉坐标随便找个空位」更贴近用户的预期。
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

    // ---- 弹对话框（期间主窗口保持可见：owner 关系保证点击判定）----
    // 传全局「默认显示标题」是为了让预览能如实画出「跟随全局设置」那一档的样子
    ui::ItemEditorFields result;
    if (!ui::showItemEditorDialog(
            owner, tabNames, initial.tabIndex, initial,
            launcherWindow_->alwaysShowTileLabels(),
            static_cast<int>(config_.appearance.iconScalePercent),
            config_.appearance.defaultTileColor, &result)) {
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
    // 外层 optional = 「这次编辑要处理颜色」；内层空 = 清除自定义色。
    // 对话框总是回填当前值，所以没动过时这里与 item.tileColor 相等，
    // editItem 会判定为无变化。
    fields.tileColor = std::optional<std::optional<std::uint32_t>>(
        result.tileColor);

    const std::size_t activeTab = launcherWindow_->activeTabIndex();

    core::ItemsDocument updated = launcherWindow_->document();
    if (!core::editItem(updated, activeTab, itemIndex, fields)) {
        return;  // 无实际变化（对话框已挡住空目标）
    }

    // ---- 页面迁移：对话框里选了别的页就把磁贴搬过去 ----
    // 保留网格坐标 —— 布局引擎遇到冲突会就近避让。
    // 注意先编辑再搬移：搬移会改变 itemIndex 的语义。
    if (result.tabIndex != activeTab) {
        (void)core::moveItemToTab(updated, activeTab, itemIndex,
                                  result.tabIndex);
    }

    launcherWindow_->setDocument(std::move(updated));
    // 目标或图标可能刚被换掉，缓存里的旧位图必须作废，
    // 否则磁贴还显示着上一张图标（缓存键只看 id，不看内容）
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

    // ---- 对话框初值：新建态 ----
    //
    // 全空，只把「设置里指定的默认磁贴尺寸」带进去 —— 用户偏好过的东西
    // 不该每次都要重选一遍。
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
        tabNames.push_back(L"常用");
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
    // 名称留空 → 按目标取名。对话框上就是这么承诺的（
    // 「名称留空时自动使用目标名称」），不能让它成为一句空话：
    // 名字为空的磁贴在界面上是不画标题的，用户会以为这块磁贴坏了。
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
        // 对话框已经挡住了空目标，这里剩下的可能只有「id 撞车」这类
        // 几乎不可能的事故 —— 仍要如实报告，不能静默吞掉。
        MessageBoxW(owner, L"添加失败：目标为空或标识冲突。", kApplicationName,
                    MB_OK | MB_ICONWARNING);
        return;
    }

    launcherWindow_->setDocument(std::move(updated));
    launcherWindow_->recomputeLayout();
    launcherWindow_->refresh();

    document_ = launcherWindow_->document();
    persistItems();
}

void Application::addDroppedSources(
    std::vector<platform::win32::DroppedSource> sources,
    const std::optional<POINTL> dropPoint) {
    if (!launcherWindow_ || sources.empty()) {
        return;
    }

    // 已存在的目标（小写化后比较）：拖同一个文件两次不该加出两块 ——
    // 磁贴本来就是一格一个，重复项只会让面板越拖越乱。
    // 去重只在**当前页**内进行。
    //
    // 多标签页的意义正是「同一个程序可以同时放在两页上」
    //（比如「常用」和「开发」各放一个编辑器），按整份文档去重会把
    // 这条最核心的能力直接掐掉。
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

        // .lnk 快捷方式 → 解析出真实目标再入库。
        // 注意 dedup 用的也是解析后的 target：同一个程序，拖它的
        // 快捷方式和拖它本体，应该被识别为同一个东西。
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

        // 类型：拖放回调说是链接就是网址；其余按目标推断
        //（存在的目录 → 文件夹、.exe → 应用……）。
        // 推断不出（路径不存在且没有扩展名）时按文件处理 ——
        // 拖进来的东西十有八九就是文件。
        item.type = (source.kind == platform::win32::DroppedKind::Url)
                        ? core::ItemType::Url
                        : ui::inferItemType(sourceValue)
                              .value_or(core::ItemType::File);

        // 名字跟着类型走（与对话框「添加新项目」共用同一条规则）
        item.name = deriveItemName(sourceValue, item.type);

        // 快捷方式自带的启动参数与工作目录一并继承，
        // 否则「带参数的快捷方式」拖进来启动行为就变了。
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
        // 什么都没加进去 —— 必须说清楚为什么，否则用户只会觉得
        // 「拖了半天没反应，这功能是坏的」。
        const wchar_t* reason = (duplicates > 0)
                                    ? L"这些项目已经在面板里了。"
                                    : L"无法添加：不是可识别的文件、文件夹或链接。";
        MessageBoxW(launcherWindow_->hwnd(), reason, kApplicationName,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    // 安放：直接落在光标所在那一格，压到谁就把谁挤开（与拖动磁贴同一套
    // 规则）。这一步在 UI 层完成 —— 只有它知道网格几何，也只有它能顺带
    // 播让位动画、把排布立刻落盘。
    if (!launcherWindow_->insertNewItemsAt(std::move(fresh), dropPoint)) {
        MessageBoxW(launcherWindow_->hwnd(), L"添加失败：目标为空或标识冲突。",
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

    if (!ui::confirmItemRemoval(owner, displayName)) {
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

// 按配置把皮肤配色与背景图应用到主窗口。
//
// 皮肤名失效（文件被删/改名）时回落默认配色，但**不回写配置**：
// 用户把文件恢复回来后选择仍然有效。背景图同理 —— 文件读不出来
// 就先用纯色背景，路径保留在配置里。
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

    // ---- 设置期间主窗口保持可见（像被钉住一样）----
    //
    // 两个理由：
    //   1. 皮肤/背景图是「所见即所得」的设置项，主窗口必须在场才看得到；
    //   2. 设置窗口是主窗口的 owned 弹窗，用户点它内部的下拉列表时，
    //      下拉列表是**独立顶层窗口**、不在主窗口的 owner 链上，会被
    //      「点外面自动收起」判成外部点击 —— 主窗口当场消失，预览白搭。
    //      setModalGuard 让主窗口在设置期间不响应这类收起（不动钉子偏好）。
    if (!launcherWindow_->isVisible()) {
        launcherWindow_->show();
    }
    launcherWindow_->setModalGuard(true);

    // 实时预览：改了就立刻反映到主窗口，但**不落盘**。
    // 点「确定」才写 config；点「取消」由下面重新套用配置还原。
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
        // 网格决定客户区尺寸：必须立刻改窗口大小，否则超出的磁贴
        // 会被旧尺寸裁掉，看起来像设置没生效。
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

    // ---- 统计当前布局实际占用的行列，用于「缩小网格」的提示 ----
    //
    // 只提示、不阻止：缩小网格不会删磁贴，超出的会被布局引擎重新安置。
    // 但用户需要知道「改小会重排」，否则会以为磁贴丢了。
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
        config_.appearance.backgroundImage,
        config_.activation.startWithWindows,
        core::hotkeyPresetOf(config_.activation.hotkey),
        // 不透明度传「当前实际生效值」：磁贴背景若是「跟随皮肤」（-1），
        // 这里换成皮肤实际的 alpha 百分比，滑块才停在观感对应的位置。
        launcherWindow_->windowOpacityPercent(),
        launcherWindow_->effectiveTileOpacityPercent(),
        static_cast<int>(config_.appearance.iconScalePercent),
        config_.appearance.defaultTileColor,
        columnsInUse,
        rowsInUse,
        &result,
        &preview);

    // 设置窗口已关闭：解除模态保护（无论确定还是取消都要解）。
    launcherWindow_->setModalGuard(false);

    if (!accepted) {
        // 取消：撤销全部实时预览 —— config 未被改动，重新套一次即可。
        //   皮肤 + 背景图 → applyThemeFromConfig 一起还原
        //   标签栏位置 / 标签显示 / 标题默认行为 → 纯状态项
        //   网格 → 改回原行列数并立即恢复窗口尺寸
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

    // ---- 标签栏位置 ----
    //
    // 纯几何变化：窗口尺寸不变、布局不变，切一下位置重画即可。
    if (result.tabBarAtBottom.has_value()) {
        config_.appearance.tabBarAtBottom = *result.tabBarAtBottom;
        launcherWindow_->setTabBarAtBottom(*result.tabBarAtBottom);
        configChanged = true;
    }

    // ---- 窗口出现位置 ----
    //
    // 无法「预览」（窗口不会自己乱动），但确定后立刻按新策略摆一次，
    // 用户关闭设置就能看到结果，不用等下次唤出。
    if (result.windowPlacement.has_value()) {
        config_.appearance.windowPlacement = *result.windowPlacement;
        launcherWindow_->setWindowPlacement(*result.windowPlacement);
        launcherWindow_->applyWindowPlacement();
        configChanged = true;
    }

    // ---- 皮肤 ----
    if (result.themeName.has_value()) {
        config_.appearance.themeName = *result.themeName;
        applyThemeFromConfig();
        configChanged = true;
    }

    // ---- 背景图 ----
    //
    // 空串 = 清除；非空 = 用户新选的图片绝对路径，先复制进
    // themes\backgrounds（哈希命名，保证绿色可搬），再应用。
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
                    error.empty() ? L"未知原因" : error;
                MessageBoxW(launcherWindow_->hwnd(),
                            (L"导入背景图失败：\n" + wideMessage).c_str(),
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
                        (L"设置开机自启失败：\n" + wideMessage).c_str(),
                        kApplicationName,
                        MB_OK | MB_ICONWARNING);
        }
    }

    // ---- 唤醒快捷键 ----
    //
    // 热键/手势可以热替换：先把旧的注销/卸载，再按新配置装一遍。
    // 注册失败（被别的程序占着）时**仍然保存设置** —— 用户选了什么是
    // 用户的意思，程序只负责告诉他这次没生效，下次启动会再试。
    if (result.hotkeyPreset.has_value() &&
        *result.hotkeyPreset != core::hotkeyPresetOf(config_.activation.hotkey)) {
        const core::HotkeyConfig previous = config_.activation.hotkey;
        config_.activation.hotkey =
            core::hotkeyConfigForPreset(*result.hotkeyPreset);
        configChanged = true;
        if (!initializeHotkey()) {
            const std::wstring message =
                L"唤醒快捷键 " +
                platform::win32::formatHotkey(config_.activation.hotkey) +
                L" 没能生效。\n\n" + hotkeyErrorText_ +
                L"\n\n已保留原来的唤醒方式。";
            MessageBoxW(launcherWindow_->hwnd(), message.c_str(),
                        kApplicationName, MB_OK | MB_ICONWARNING);
            // 装不上就把旧的装回去，避免「一个都不能用」——
            // 用户至少还能用熟悉的那个键唤出面板。
            config_.activation.hotkey = previous;
            initializeHotkey();
        }
    }

    // ---- 不透明度 ----
    //
    // 两项都即时生效：整窗透明度改完 ex style 系统立刻重新合成；
    // 磁贴底色在下一帧重绘就变（预览阶段已经在显示，这里只是落盘确认）。
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

    // ---- 图标统一缩放 ----
    // 只改绘制矩形，下一帧重绘即生效，布局与窗口尺寸都不动。
    if (result.iconScalePercent.has_value()) {
        config_.appearance.iconScalePercent = *result.iconScalePercent;
        launcherWindow_->setTileIconScalePercent(*result.iconScalePercent);
        configChanged = true;
    }

    // ---- 全局默认磁贴底色 ----
    //
    // 外层有值 = 用户确实动过（含「恢复跟随皮肤」）；
    // 内层空 = 清空默认色，回到皮肤 tileFill。
    if (result.defaultTileColor.has_value()) {
        config_.appearance.defaultTileColor = *result.defaultTileColor;
        launcherWindow_->setDefaultTileColor(
            config_.appearance.defaultTileColor);
        configChanged = true;
    }

    // ---- 应用网格改动 ----
    //
    // 顺序很重要：先改尺寸，再让窗口重算布局，最后落盘。
    // 若先落盘再重算，中途崩溃会留下「配置是新的、界面还是旧的」的不一致。
    if (gridChanged) {
        launcherWindow_->setGridSize(config_.appearance.gridColumns,
                                     config_.appearance.gridRows);
        launcherWindow_->recomputeLayout();
        // 网格决定客户区尺寸，立即生效 —— 否则要等下次唤出面板才变，
        // 用户会以为设置没保存。
        launcherWindow_->applyGridGeometry();
        launcherWindow_->refresh();
    }

    if (configChanged) {
        persistConfig();
    }

    // ---- 需要重启才能生效的设置 ----
    //
    // 当前三个设置（网格尺寸、标签显示、开机自启）都能即时生效，无需重启。
    // 保留这一分支是为了让后续新增设置的开发者有一个明确的落点：
    // 分组热键、后台常驻策略、右键菜单结构等改动无法在运行时热替换，
    // 届时在 restartPending 里累积，最后统一提示，而不是每项弹一次框。
    if (restartPending) {
        MessageBoxW(launcherWindow_->hwnd(),
                    L"部分设置需要重启 KTile 后生效。\n\n"
                    L"重启后：\n"
                    L"  · 全局热键按新设置注册\n"
                    L"  · 托盘菜单按新设置重建\n\n"
                    L"当前设置已保存，可继续使用。",
                    L"需要重启",
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

    const auto status = tray_.create(
        messageWindow_,
        WM_TRAYICON_CALLBACK,
        L"KTile - Alt+Space 打开",
        icon.get());
    if (!status) {
        return false;
    }

    // 托盘图标创建成功后所有权转移给 TrayIcon（它用 get() 持有）
    // 注意：此处 release 的图标由 TrayIcon 在销毁时通知系统移除，
    // 但 HICON 本身需要存活，故用全局变量持有
    g_trayIconForMenu = icon.release();

    return true;
}

bool Application::initializeHotkey() {
    hotkeyErrorText_.clear();

    const core::HotkeyConfig& config = config_.activation.hotkey;

    // 两种手势只能走低级键盘钩子：RegisterHotKey 表达不了「双击同一个键」
    // 和「单键」，所以这里按 gesture 分成两条互斥的通道。
    // 每次调用都先把另一条通道拆干净 —— 设置里换方式时需要热替换。
    if (config.gesture != core::HotkeyGesture::None) {
        hotkey_.unregister();
        const auto status = gestureHotkey_.install(
            messageWindow_, kToggleHotkeyId, config.gesture);
        if (!status) {
            hotkeyErrorText_ = base::utf8ToWide(status.error().message);
            return false;
        }
        return true;
    }

    gestureHotkey_.uninstall();
    const auto status = hotkey_.registerHotkey(
        messageWindow_, kToggleHotkeyId, config);
    if (!status) {
        hotkeyErrorText_ = base::utf8ToWide(status.error().message);
        return false;
    }
    return true;
}

void Application::reinstallGestureHotkey() {
    const core::HotkeyConfig& config = config_.activation.hotkey;
    if (config.gesture == core::HotkeyGesture::None) {
        return;  // 普通 RegisterHotKey 不走钩子链，无需重装
    }
    if (messageWindow_ == nullptr) {
        return;
    }
    // install 内部会先 uninstall，调用一次即可完成「拆旧 + 装新」。
    // 失败不覆盖 hotkeyErrorText_：这里不是用户主动改热键，没必要
    // 把「暂时没装上」当成用户能处理的错误提示出来，下次唤醒时再试。
    gestureHotkey_.install(messageWindow_, kToggleHotkeyId, config.gesture);
}

// ================================================================ 消息循环

int Application::runMessageLoop() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // 让 IsDialogMessage 之外的按键也能正常分发
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

// ================================================================ 行为

void Application::toggleLauncher() {
    if (!launcherWindow_) {
        return;
    }
    launcherWindow_->toggle();
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
    // owner 给主窗口：About 对话框是它的 owned 弹窗，点对话框内部
    // 不会被「点外面收起」误判（belongsToTarget 走 owner 链）。
    HWND owner = launcherWindow_ ? launcherWindow_->hwnd() : nullptr;
    ui::showAboutDialog(owner, std::wstring(kApplicationWindowTitle),
                        std::wstring(kApplicationVersion));
}

void Application::showTrayMenu() {
    std::vector<platform::win32::TrayMenuItem> menuItems;

    platform::win32::TrayMenuItem showItem;
    showItem.commandId = kTrayMenuShowHide;
    showItem.text = (launcherWindow_ && launcherWindow_->isVisible())
        ? L"隐藏启动器" : L"显示启动器";
    menuItems.push_back(showItem);

    menuItems.push_back(platform::win32::TrayMenuItem::separator());

    platform::win32::TrayMenuItem settingsItem;
    settingsItem.commandId = kTrayMenuSettings;
    settingsItem.text = L"设置...";
    menuItems.push_back(settingsItem);

    menuItems.push_back(platform::win32::TrayMenuItem::separator());

    platform::win32::TrayMenuItem startupItem;
    startupItem.commandId = kTrayMenuStartup;
    startupItem.text = L"开机自动启动";
    startupItem.isChecked = platform::win32::StartupRegistration::isEnabled(
        std::wstring(kStartupRegistryValueName));
    menuItems.push_back(startupItem);

    menuItems.push_back(platform::win32::TrayMenuItem::separator());

    platform::win32::TrayMenuItem aboutItem;
    aboutItem.commandId = kTrayMenuAbout;
    aboutItem.text = L"关于 KTile...";
    menuItems.push_back(aboutItem);

    menuItems.push_back(platform::win32::TrayMenuItem::separator());

    platform::win32::TrayMenuItem exitItem;
    exitItem.commandId = kTrayMenuExit;
    exitItem.text = L"退出";
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
                // 同步到配置并落盘，保证重启后状态一致
                config_.activation.startWithWindows = !currentlyEnabled;
                persistConfig();
            } else {
                const std::string message = status.error().format();
                const std::wstring wideMessage = base::utf8ToWide(message);
                MessageBoxW(nullptr,
                            (L"设置开机自启失败：\n" + wideMessage).c_str(),
                            kApplicationName,
                            MB_OK | MB_ICONWARNING);
            }
            break;
        }

        case kTrayMenuExit:
            if (launcherWindow_) {
                launcherWindow_->hide();
            }
            // 退出前把内存状态刷回磁盘
            persistItems();
            persistConfig();
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
