// KTile - Win32 平台层：轮盘目标窗口解析
//
// 供轮盘菜单按窗口归属匹配方案使用。核心是 radialTargetWindow()：光标点
// 命中的窗口优先，回退前台窗口。所用 API（GetCursorPos / WindowFromPoint /
// GetForegroundWindow / OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) /
// QueryFullProcessImageNameW）均在 Win7 SP1 可用范围内。
//
#pragma once

#include <Windows.h>

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

// 获取前台窗口所属进程的可执行文件名（小写，含扩展名，如 "chrome.exe"）。
// 失败（无前台窗口 / 无法打开进程）返回空串。
std::wstring foregroundProcessName();

// 获取前台窗口的窗口类名（小写）。与进程名配合使用：explorer.exe 的
// 文件窗口类是 CabinetWClass，桌面是 Progman / WorkerW，轮盘插件靠它
// 区分同进程的两类界面。取不到返回空串。
std::wstring foregroundWindowClass();

// 轮盘的目标窗口快照：进程名 + 窗口类名（均小写，可都为空）。
// window 保留原始句柄：explorer 多窗口时，查「选中了哪些文件」必须定位
// 到光标下的那一个，只有进程名和类名区分不出来。
struct WindowTarget {
    std::wstring processName;
    std::wstring windowClass;
    // 根窗口自己的进程名。光标落在跨进程子窗口上时 processName 是子窗口
    // 那一方（如微信文章区的 wechatappex.exe），而窗口本身属于 rootProcessName
    // （weixin.exe）。黑名单要两个都认，否则「拉黑微信」对文章区不生效。
    std::wstring rootProcessName;
    HWND window = nullptr;
};

// 按「鼠标所在位置」解析轮盘的目标窗口：光标点命中的根窗口优先，
// 不可见 / 最小化 / 属于本进程（自己的覆盖层）时回退前台窗口。
// 悬停不改变前台，只看 GetForegroundWindow 会把轮盘配给错误的窗口，
// 双窗口场景（此电脑 + 浏览器）尤其明显。
//
// 进程归属取光标下最深的子窗口：宿主窗口里嵌着别的进程时（微信 4.x 把
// 公众号文章作为 WeChatAppEx.exe 的子窗口塞进主窗），光标在文章区与在
// 聊天区会给出不同的进程名，插件才分得清。窗口类名一律取根窗口，保证
// `进程名@窗口类名` 的写法不会因为命中子控件而失配。
WindowTarget radialTargetWindow();

}  // namespace win32
}  // namespace platform
}  // namespace ktile
