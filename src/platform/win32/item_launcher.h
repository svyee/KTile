// KTile - Win32 平台层：启动项目
//
// 拼参数、选 ShellExecute 语义、报错都收在这里，Application 层只说
// 「启动这个，要不要管理员」。统一用 ShellExecuteW：天然覆盖 exe、
// 文档关联、文件夹、URL 四类目标，.lnk 也能解析；CreateProcessW 只能
// 跑可执行文件，其余三类要自己分流。
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
// forceAdministrator 用于右键菜单的「以管理员身份运行」，
// 它无视项目自身的设置，强制提权一次。
LaunchResult launchItem(const core::LaunchItem& item, bool forceAdministrator = false);

// 在资源管理器中打开目标所在的文件夹并选中它。
//
// 「打开文件所在位置」的后端。URL 没有文件位置（调用方应禁用该菜单项）；
// 文件夹类型打开它的上级目录。
LaunchResult revealItemInExplorer(const core::LaunchItem& item);

// 用系统默认程序打开一个文件（例如插件的 radial.json）。
// 没有文件关联时用记事本兜底：JSON 是纯文本，记事本任何 Windows 都有，
// 而 .json 在精简版系统里可能没有关联；用户要的是能改它，宁可换程序
// 也要打开。
LaunchResult openPathWithDefaultProgram(const std::wstring& path);

// 在资源管理器中打开一个目录（不选中任何项）。
LaunchResult openFolderInExplorer(const std::wstring& path);

}  // namespace win32
}  // namespace platform
}  // namespace ktile
