// KTile - Win32 平台层：启动项目
//
// 把「怎么把一条 LaunchItem 跑起来」收进平台层：
//   - Application 只说「启动这个，要不要管理员」
//   - 拼参数、选 ShellExecute 语义、报错，都在这里
//
// 统一用 ShellExecuteW 而不是 CreateProcessW：
//   ShellExecuteW 天然覆盖四类目标 —— exe 直接跑、文档走文件关联、
//   文件夹开资源管理器、URL 走默认浏览器；.lnk 也能解析。
//   CreateProcessW 只能跑可执行文件，其余三类都要自己分流。
//
#pragma once

#include "core/data_model.h"

#include <Windows.h>

#include <string>

namespace ktile {
namespace platform {
namespace win32 {

// 启动结果。失败时 error 是可直接给 MessageBoxW 展示的描述。
struct LaunchResult {
    bool ok = false;
    std::wstring error;
};

// 以普通权限启动项目。
//
// item.runAsAdministrator 为 true 时自动走 runas（UAC 提权）；
// forceAdministrator 用于右键菜单的「以管理员身份运行」——
// 它无视项目自身的设置，强制提权一次。
LaunchResult launchItem(const core::LaunchItem& item, bool forceAdministrator = false);

// 在资源管理器中打开目标所在的文件夹并选中它。
//
// 「打开文件所在位置」的后端。URL 没有文件位置（调用方应禁用该菜单项）；
// 文件夹类型打开它的上级目录。
LaunchResult revealItemInExplorer(const core::LaunchItem& item);

// 用系统默认程序打开一个文件（例如插件的 radial.json）。
//
// 没有文件关联时用记事本兜底：JSON 是纯文本，记事本在任何 Windows 上都有，
// 而 .json 在精简版系统里确实可能没有关联。用户点「打开 JSON」的诉求是
// 「我要改它」，不是「看看是哪个程序弹出来」—— 所以宁可换程序也要打开。
LaunchResult openPathWithDefaultProgram(const std::wstring& path);

// 在资源管理器中打开一个目录（不选中任何项）。
LaunchResult openFolderInExplorer(const std::wstring& path);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
