// KTile - Win32 平台层：explorer / 桌面文件选中状态查询
//
// 轮盘弹出前问一句「光标下的 shell 视图有没有选中文件」，据此切换插件
// 里带 when 条件的两套动作。路径与 StarPie 的 ActionExecutor 一致：
// CLSID_ShellWindows 枚举所有 shell 视图，按窗口句柄匹配后取
// Document.SelectedItems().Count。Count 只需要一位比特，不取具体路径。
#pragma once

#include <optional>
#include <string>

#include <Windows.h>

namespace ktile {
namespace platform {
namespace win32 {

// 目标窗口是否属于可查询选中状态的 shell 视图：explorer 的文件窗口
// （CabinetWClass）或桌面（Progman / WorkerW）。其他进程 / 类名直接返回
// false，调用方不必发起 COM 查询。
bool isShellSelectionTarget(const std::wstring& processName,
                            const std::wstring& windowClass);

// 查询目标窗口当前是否选中了文件。返回值语义：
//   nullopt     查不到对应的 shell 视图（窗口已关 / 非标准视图 / COM 失败）
//   true/false  找到了视图，选中的文件数大于 / 等于零
// 查询失败时调用方应按「无选中」处理：宁可显示基础动作，不报错不卡顿。
std::optional<bool> shellWindowHasSelection(HWND targetWindow);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
