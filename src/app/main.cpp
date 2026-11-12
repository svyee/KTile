// KTile - 应用入口
//
// 只做三件事：声明 DPI 感知、初始化 COM/OLE、解析 --debug-* 开关，再交给
// Application::run。分层为 base / core / infrastructure / platform/win32 /
// ui / app。
//
#include "app/application.h"
#include "base/text_convert.h"

#include <Windows.h>
#include <ole2.h>

#include <string>
#include <vector>

int WINAPI wWinMain(HINSTANCE instance,
                    HINSTANCE /*previousInstance*/,
                    LPWSTR commandLine,
                    int /*showCommand*/) {
    // ---- 声明 DPI 感知（必须在创建任何窗口之前）----
    //
    // 不声明则渲染被 DWM 位图拉伸，150% 下整体发虚；窗口坐标也会被虚拟化，
    // 而 WH_MOUSE_LL 钩子的 MSLLHOOKSTRUCT.pt 永远是物理坐标，两套坐标系
    // 混用会让「点击窗口外部自动隐藏」的归属判定整体错位。
    // 用 SetProcessDPIAware（Vista+，Win7 可用）而非 Per-Monitor：启动器按
    // 主显示器 DPI 缩放一次即可，跨屏拖动由 WM_DPICHANGED 触发重新布局。
    SetProcessDPIAware();

    // COM / OLE 初始化。两处依赖：图标提取走 WIC，拖放要 IDropTarget。
    // 用 OleInitialize 而不是 CoInitializeEx，前者额外装上 OLE 子系统，
    // 且必须在本线程其他 COM 初始化之前调用。托盘/Shell/拖放都偏好 STA。
    const HRESULT comResult = OleInitialize(nullptr);

    ktile::app::Application application;

    // ---- 调试开关：--debug-shot ----
    //
    // 启动后自动显示窗口，把内容渲染成 WIC 位图写出 PNG 再退出，用于自动化
    // 视觉验证。外部抓屏对 WS_EX_NOACTIVATE 的 D2D 窗口不可靠：PrintWindow
    // 依赖 WM_PRINT（HwndRenderTarget 不实现），BitBlt 要求窗口真的可见且
    // 未被遮挡。自己渲染绕开这些限制，拿到的就是 D2D 真实输出。
    const std::wstring command = (commandLine != nullptr) ? commandLine : L"";

    // 开关是否存在。hasFlag 是子串匹配：--debug-settings-keepalive 会同时
    // 点亮 --debug-settings，新开关取名时不能让旧开关名成为它的前缀。
    const auto hasFlag = [&command](const wchar_t* flag) {
        return command.find(std::wstring(flag)) != std::wstring::npos;
    };
    // 取 "--flag=值" 里的值。边界是下一个 " --" 而不是空格：路径里会带空格，
    // 末尾的空白也要裁掉。
    const auto flagValue = [&command](const wchar_t* flag) -> std::wstring {
        const std::wstring prefix(flag);
        const std::size_t found = command.find(prefix);
        if (found == std::wstring::npos) {
            return std::wstring();
        }
        std::wstring value = command.substr(found + prefix.size());
        const std::size_t next = value.find(L" --");
        if (next != std::wstring::npos) {
            value.resize(next);
        }
        const std::size_t begin = value.find_first_not_of(L" \t\r\n");
        if (begin == std::wstring::npos) {
            return std::wstring();
        }
        const std::size_t end = value.find_last_not_of(L" \t\r\n");
        return value.substr(begin, end - begin + 1);
    };

    ktile::app::DebugOptions debugOptions;
    debugOptions.shot = hasFlag(L"--debug-shot");
    // 启动后直接打开设置窗口（供自动化检查控件）
    debugOptions.settings = hasFlag(L"--debug-settings");
    // 打开设置后不退出，继续跑主循环
    debugOptions.settingsKeepAlive = hasFlag(L"--debug-settings-keepalive");
    // --debug-wheel-editor：直接打开「轮盘菜单设置」对话框。
    // 不能叫 --debug-radial-editor：hasFlag 是子串匹配，--debug-radial 会命中，
    // 先跑一遍轮盘位图导出就退出了，对话框来不及出现。
    debugOptions.radialEditor = hasFlag(L"--debug-wheel-editor");
    // 启动后打开「关于」对话框（供自动化检查文案）
    debugOptions.about = hasFlag(L"--debug-about");
    // 启动后打开第一个磁贴的「修改项目」对话框
    debugOptions.editor = hasFlag(L"--debug-editor");
    // 启动后打开「添加项目」对话框（新建模式）
    debugOptions.newItem = hasFlag(L"--debug-new-item");

    // --debug-add=<路径1;路径2;...>：走一次「拖入添加」。
    // 自动化无法复现真实 OLE 拖放，这里直接调下游的 addDroppedSources，
    // 验证的是「拿到路径之后怎么加」，OLE 管道由 test_drop_target 覆盖。
    {
        const std::wstring paths = flagValue(L"--debug-add=");
        std::size_t start = 0;
        while (start <= paths.size()) {
            const std::size_t semicolon = paths.find(L';', start);
            const std::wstring token =
                (semicolon == std::wstring::npos)
                    ? paths.substr(start)
                    : paths.substr(start, semicolon - start);
            if (!token.empty()) {
                debugOptions.addPaths.push_back(token);
            }
            if (semicolon == std::wstring::npos) {
                break;
            }
            start = semicolon + 1;
        }
    }

    // --debug-search=<文本>：预置搜索查询，用于导出「过滤生效」的截图
    debugOptions.search =
        ktile::base::wideToUtf8Lossy(flagValue(L"--debug-search="));

    // --debug-opacity=<50..100>：主窗口整体不透明度（验证分层窗口用）
    {
        const std::wstring value = flagValue(L"--debug-opacity=");
        if (!value.empty()) {
            debugOptions.opacityPercent = _wtoi(value.c_str());
        }
    }

    // --debug-drop=x,y：模拟一次「拖入落点」（客户区坐标）
    {
        const std::wstring args = flagValue(L"--debug-drop=");
        const std::size_t comma = args.find(L',');
        if (comma != std::wstring::npos) {
            debugOptions.dropX = _wtoi(args.substr(0, comma).c_str());
            debugOptions.dropY = _wtoi(args.substr(comma + 1).c_str());
        }
    }

    // --debug-radial[=x,y]：把轮盘菜单渲染到 radial-shot.png
    debugOptions.radial = hasFlag(L"--debug-radial");
    {
        const std::wstring args = flagValue(L"--debug-radial=");
        if (!args.empty()) {
            const std::size_t comma = args.find(L',');
            if (comma != std::wstring::npos) {
                debugOptions.radialHoverX =
                    _wtoi(args.substr(0, comma).c_str());
                debugOptions.radialHoverY =
                    _wtoi(args.substr(comma + 1).c_str());
            }
        }
    }

    // --debug-radial-plugin=<id>：配合 --debug-radial，改渲染插件提供的菜单
    debugOptions.radialPluginId = ktile::base::wideToUtf8Lossy(
        flagValue(L"--debug-radial-plugin="));

    // --debug-radial-scroll=<ms>：把超宽标签的滚动进度拨到指定毫秒
    {
        const std::wstring value = flagValue(L"--debug-radial-scroll=");
        if (!value.empty()) {
            debugOptions.radialScrollMs = _wtoi(value.c_str());
        }
    }

    if (debugOptions.shot) {
        // 可选参数都在 = 之后：--debug-shot=x,y 模拟悬停，
        // --debug-shot=x,y@sx,sy@dx,dy 再加拖拽（sx,sy 来源磁贴，dx,dy 落点）。
        // 取值定位到 "--debug-shot=" 本身，命令行里别的开关也带 =。
        const std::wstring args = flagValue(L"--debug-shot=");
        if (!args.empty()) {
            // 按 '@' 拆成若干坐标段
            std::vector<std::wstring> segments;
            std::size_t start = 0;
            while (start <= args.size()) {
                const std::size_t at = args.find(L'@', start);
                if (at == std::wstring::npos) {
                    segments.push_back(args.substr(start));
                    break;
                }
                segments.push_back(args.substr(start, at - start));
                start = at + 1;
            }

            auto parsePair = [](const std::wstring& text, int& outX, int& outY) {
                const std::size_t comma = text.find(L',');
                if (comma == std::wstring::npos) {
                    return false;
                }
                outX = _wtoi(text.substr(0, comma).c_str());
                outY = _wtoi(text.substr(comma + 1).c_str());
                return true;
            };

            if (!segments.empty()) {
                parsePair(segments[0], debugOptions.hoverX, debugOptions.hoverY);
            }
            if (segments.size() >= 2) {
                parsePair(segments[1], debugOptions.dragSrcX,
                          debugOptions.dragSrcY);
            }
            if (segments.size() >= 3) {
                parsePair(segments[2], debugOptions.dragDstX,
                          debugOptions.dragDstY);
            }
        }
    }

    const int exitCode = application.run(instance, debugOptions);

    if (SUCCEEDED(comResult)) {
        OleUninitialize();
    }
    return exitCode;
}
