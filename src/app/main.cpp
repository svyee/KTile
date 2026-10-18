// KTile - 应用入口
//
// 里程碑状态：
//   M1 单实例 + 托盘 + 全局热键 + 窗口壳          [完成]
//   M2 磁贴跨度布局 + 四种尺寸 + 真实图标          [完成]
//   M3 JSON 持久化（items.json / config.json）    [完成]
//   M4 拖拽换位 + 右键菜单 + 设置界面              [进行中]
//
// 架构分层：
//     base/           C++17 兼容层（expected / span / algo / 编码转换）
//     core/           领域模型与算法（无平台依赖，可单测）
//     infrastructure/ 持久化（便携路径 / JSON 读写）
//     platform/win32/ Win32 API 封装（RAII + 错误转换）
//     ui/             窗口与 Direct2D 渲染
//     app/            组装与生命周期
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
    // 不声明的话，系统把本程序当 DPI 不感知的老程序：
    //   1. 渲染被 DWM 位图拉伸，150% 缩放下整体模糊
    //   2. 窗口坐标被虚拟化（物理坐标 × 1/scale），而 WH_MOUSE_LL 钩子的
    //      MSLLHOOKSTRUCT.pt 永远是物理坐标 —— 两套坐标系混用，
    //      「点击窗口外部自动隐藏」的窗口归属判定会整体错位
    //
    // 用 SetProcessDPIAware（Vista+，Win7 可用）而非 Per-Monitor 系列：
    // 系统级感知已满足需求 —— 启动器窗口按主显示器 DPI 缩放一次即可，
    // 跨显示器拖动时 WM_DPICHANGED 会触发 updateDpi 重新布局。
    // 渲染侧本就按 dpiScale()（LOGPIXELSX/96）缩放，声明后自动变清晰。
    SetProcessDPIAware();

    // COM / OLE 初始化。
    //
    // 两处依赖：
    //   1. 图标提取走 WIC（COM 组件）
    //   2. 把文件拖进面板要 RegisterDragDrop + IDropTarget（OLE 拖放）
    //
    // 用 OleInitialize 而不是 CoInitializeEx：前者等价于
    // 「CoInitializeEx(APARTMENTTHREADED) + 把 OLE 子系统装上」，
    // 而拖放正需要那部分。它必须在**本线程其他 COM 初始化之前**调用 ——
    // 所以这里的 CoInitializeEx 换成了它。
    // 用 STA 而非 MTA：托盘/Shell/拖放相关 API 都偏好单线程单元。
    const HRESULT comResult = OleInitialize(nullptr);

    ktile::app::Application application;

    // ---- 调试开关：--debug-shot ----
    //
    // 用途：自动化视觉验证。启动后自动显示窗口，把窗口内容渲染到
    // 一张 WIC 位图并写出 PNG，然后立刻退出。
    //
    // 为什么需要它：本机的自动化抓屏链路（PrintWindow / BitBlt 屏幕 DC）
    // 对 WS_EX_NOACTIVATE 的 D2D 窗口不可靠 —— 前者依赖窗口实现 WM_PRINT
    // （D2D 的 HwndRenderTarget 不实现），后者要求窗口真的显示在物理屏幕上
    // 且未被遮挡，在无交互会话里拿不到内容。
    //
    // 由程序自己渲染出图则完全绕开这些限制：拿到的就是 D2D 真实输出。
    const std::wstring command = (commandLine != nullptr) ? commandLine : L"";

    // 开关是否存在（--debug-shoto 这类前缀关系在现有开关里是刻意保留的：
    // --debug-settings-keepalive 必须同时点亮 --debug-settings）
    const auto hasFlag = [&command](const wchar_t* flag) {
        return command.find(std::wstring(flag)) != std::wstring::npos;
    };
    // 取 "--flag=值" 里的值。
    //
    // 与下面 --debug-keys 的「以空格为界」不同：路径里会带空格，
    // 因此这里的边界是「下一个 -- 开关」，命令行末尾的换行也要裁掉。
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
    // --debug-settings：启动后直接打开设置窗口（供自动化检查控件）
    debugOptions.settings = hasFlag(L"--debug-settings");
    // --debug-settings-keepalive：打开设置后不退出，继续跑主循环
    debugOptions.settingsKeepAlive = hasFlag(L"--debug-settings-keepalive");
    // --debug-about：启动后打开「关于」对话框（供自动化检查文案）
    debugOptions.about = hasFlag(L"--debug-about");
    // --debug-editor：启动后打开第一个磁贴的「修改项目」对话框
    debugOptions.editor = hasFlag(L"--debug-editor");
    // --debug-new-item：启动后打开「添加项目」对话框（新建模式）
    debugOptions.newItem = hasFlag(L"--debug-new-item");

    // --debug-add=<路径1;路径2;...>：启动后走一次「拖入添加」
    //
    // 真实的拖放要在交互会话里按住鼠标从资源管理器拖过来，自动化无法复现；
    // 这条开关走的是**与拖放完全相同的下游代码**（addDroppedSources），
    // 因此验证的是「拿到路径之后怎么加」，OLE 那条管道由单测
    // （test_drop_target）覆盖。
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

    // --debug-search=<文本>：启动时预置搜索查询，用于导出「过滤生效」的截图
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

    if (debugOptions.shot) {
        // 可选参数（都在 = 之后给出）：
        //   --debug-shot=x,y                 模拟悬停到 (x,y)
        //   --debug-shot=x,y@sx,sy@dx,dy     模拟悬停 + 拖拽
        //                                     sx,sy=来源磁贴，dx,dy=落点
        //
        // 注意取值时定位到 "--debug-shot=" 本身：命令行里可能还有别的
        // 带 = 的开关（如 --debug-add=...），不能拿全局第一个 '=' 来切。
        const std::wstring args = flagValue(L"--debug-shot=");
        if (!args.empty()) {
            // 按 '@' 拆分为若干坐标段
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
